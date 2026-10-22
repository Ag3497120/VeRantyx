// In-tree Vulkan 1.0 frontend. It exposes the implemented transfer and compute subset through the shared Metal backend.
#include "vx_vulkan.h"
#include "gpu_ir.h"
#include "gpu_ir_metal.h"
#include "gpu_ir_msl.h"
#include "spirv.h"
#include "spirv_ir.h"
#include "ui.h"
#include <stdalign.h>
#include <stdatomic.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/sysctl.h>

#define VX_VK_MAX_DESCRIPTOR_VALUES \
    (GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS + GPU_IR_METAL_MAX_COMPUTE_TEXTURES + \
     GPU_IR_METAL_MAX_COMPUTE_SAMPLERS)
#define VX_VK_MAX_IMAGE_LAYERS 256u
#define VX_VK_MAX_SAMPLER_ALLOCATION_COUNT 4096u
#define VX_VK_MAX_DRAW_INDIRECT_COUNT 4096u
// The 30 Metal buffer arguments are shared by uniform and storage descriptors.
#define VX_VK_MAX_PER_STAGE_UNIFORM_BUFFERS 12u
#define VX_VK_MAX_PER_STAGE_STORAGE_BUFFERS \
    (GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS - VX_VK_MAX_PER_STAGE_UNIFORM_BUFFERS)
#define VX_VK_MAX_UNIFORM_BUFFER_DYNAMIC_DESCRIPTORS 8u
#define VX_VK_MAX_STORAGE_BUFFER_DYNAMIC_DESCRIPTORS 4u

_Static_assert(sizeof(VkApplicationInfo) == 48, "VkApplicationInfo ABI layout");
_Static_assert(sizeof(VkInstanceCreateInfo) == 64, "VkInstanceCreateInfo ABI layout");
_Static_assert(sizeof(VkWin32SurfaceCreateInfoKHR) == 40, "VkWin32SurfaceCreateInfoKHR ABI layout");
_Static_assert(sizeof(VkSurfaceCapabilitiesKHR) == 52, "VkSurfaceCapabilitiesKHR ABI layout");
_Static_assert(sizeof(VkSurfaceFormatKHR) == 8, "VkSurfaceFormatKHR ABI layout");
_Static_assert(sizeof(VkSwapchainCreateInfoKHR) == 104, "VkSwapchainCreateInfoKHR ABI layout");
_Static_assert(sizeof(VkPresentInfoKHR) == 64, "VkPresentInfoKHR ABI layout");
_Static_assert(sizeof(VkAllocationCallbacks) == 48, "VkAllocationCallbacks ABI layout");
_Static_assert(sizeof(VkExtensionProperties) == 260, "VkExtensionProperties ABI layout");
_Static_assert(sizeof(VkLayerProperties) == 520, "VkLayerProperties ABI layout");
_Static_assert(sizeof(VkPhysicalDeviceFeatures) == 220, "VkPhysicalDeviceFeatures ABI layout");
_Static_assert(offsetof(VkPhysicalDeviceFeatures, shaderSampledImageArrayDynamicIndexing) == 34 * sizeof(VkBool32),
               "VkPhysicalDeviceFeatures sampled-image indexing feature offset");
_Static_assert(offsetof(VkPhysicalDeviceFeatures, shaderStorageBufferArrayDynamicIndexing) == 35 * sizeof(VkBool32),
               "VkPhysicalDeviceFeatures storage-buffer indexing feature offset");
_Static_assert(offsetof(VkPhysicalDeviceFeatures, shaderStorageImageArrayDynamicIndexing) == 36 * sizeof(VkBool32),
               "VkPhysicalDeviceFeatures storage-image indexing feature offset");
_Static_assert(sizeof(VkPhysicalDeviceLimits) == 504, "VkPhysicalDeviceLimits ABI layout");
_Static_assert(sizeof(VkPhysicalDeviceProperties) == 824, "VkPhysicalDeviceProperties ABI layout");
_Static_assert(sizeof(VkQueueFamilyProperties) == 24, "VkQueueFamilyProperties ABI layout");
_Static_assert(sizeof(VkPhysicalDeviceMemoryProperties) == 520, "VkPhysicalDeviceMemoryProperties ABI layout");
_Static_assert(sizeof(VkDeviceQueueCreateInfo) == 40, "VkDeviceQueueCreateInfo ABI layout");
_Static_assert(sizeof(VkDeviceCreateInfo) == 72, "VkDeviceCreateInfo ABI layout");
_Static_assert(sizeof(VkMemoryRequirements) == 24, "VkMemoryRequirements ABI layout");
_Static_assert(sizeof(VkMemoryAllocateInfo) == 32, "VkMemoryAllocateInfo ABI layout");
_Static_assert(sizeof(VkMappedMemoryRange) == 40, "VkMappedMemoryRange ABI layout");
_Static_assert(sizeof(VkBufferCreateInfo) == 56, "VkBufferCreateInfo ABI layout");
_Static_assert(sizeof(VkImageCreateInfo) == 88, "VkImageCreateInfo ABI layout");
_Static_assert(sizeof(VkImageViewCreateInfo) == 80, "VkImageViewCreateInfo ABI layout");
_Static_assert(sizeof(VkAttachmentDescription) == 36, "VkAttachmentDescription ABI layout");
_Static_assert(sizeof(VkAttachmentReference) == 8, "VkAttachmentReference ABI layout");
_Static_assert(sizeof(VkSubpassDescription) == 72, "VkSubpassDescription ABI layout");
_Static_assert(sizeof(VkSubpassDependency) == 28, "VkSubpassDependency ABI layout");
_Static_assert(sizeof(VkRenderPassCreateInfo) == 64, "VkRenderPassCreateInfo ABI layout");
_Static_assert(sizeof(VkFramebufferCreateInfo) == 64, "VkFramebufferCreateInfo ABI layout");
_Static_assert(sizeof(VkRenderPassBeginInfo) == 64, "VkRenderPassBeginInfo ABI layout");
_Static_assert(sizeof(VkRect2D) == 16, "VkRect2D ABI layout");
_Static_assert(sizeof(VkClearValue) == 16, "VkClearValue ABI layout");
_Static_assert(sizeof(VkSamplerCreateInfo) == 80, "VkSamplerCreateInfo ABI layout");
_Static_assert(sizeof(VkImageSubresource) == 12, "VkImageSubresource ABI layout");
_Static_assert(sizeof(VkImageSubresourceLayers) == 16, "VkImageSubresourceLayers ABI layout");
_Static_assert(sizeof(VkImageSubresourceRange) == 20, "VkImageSubresourceRange ABI layout");
_Static_assert(sizeof(VkClearColorValue) == 16, "VkClearColorValue ABI layout");
_Static_assert(sizeof(VkSubresourceLayout) == 40, "VkSubresourceLayout ABI layout");
_Static_assert(sizeof(VkBufferImageCopy) == 56, "VkBufferImageCopy ABI layout");
_Static_assert(sizeof(VkImageCopy) == 68, "VkImageCopy ABI layout");
_Static_assert(sizeof(VkImageMemoryBarrier) == 72, "VkImageMemoryBarrier ABI layout");
_Static_assert(sizeof(VkFormatProperties) == 12, "VkFormatProperties ABI layout");
_Static_assert(sizeof(VkImageFormatProperties) == 32, "VkImageFormatProperties ABI layout");
_Static_assert(sizeof(VkCommandPoolCreateInfo) == 24, "VkCommandPoolCreateInfo ABI layout");
_Static_assert(sizeof(VkCommandBufferAllocateInfo) == 32, "VkCommandBufferAllocateInfo ABI layout");
_Static_assert(sizeof(VkCommandBufferBeginInfo) == 32, "VkCommandBufferBeginInfo ABI layout");
_Static_assert(sizeof(VkBufferCopy) == 24, "VkBufferCopy ABI layout");
_Static_assert(sizeof(VkDispatchIndirectCommand) == 12, "VkDispatchIndirectCommand ABI layout");
_Static_assert(sizeof(VkDrawIndirectCommand) == 16, "VkDrawIndirectCommand ABI layout");
_Static_assert(sizeof(VkDrawIndexedIndirectCommand) == 20, "VkDrawIndexedIndirectCommand ABI layout");
_Static_assert(sizeof(VkShaderModuleCreateInfo) == 40, "VkShaderModuleCreateInfo ABI layout");
_Static_assert(sizeof(VkDescriptorSetLayoutBinding) == 24, "VkDescriptorSetLayoutBinding ABI layout");
_Static_assert(sizeof(VkDescriptorSetLayoutCreateInfo) == 32, "VkDescriptorSetLayoutCreateInfo ABI layout");
_Static_assert(sizeof(VkPipelineLayoutCreateInfo) == 48, "VkPipelineLayoutCreateInfo ABI layout");
_Static_assert(sizeof(VkPipelineShaderStageCreateInfo) == 48, "VkPipelineShaderStageCreateInfo ABI layout");
_Static_assert(sizeof(VkSpecializationMapEntry) == 16, "VkSpecializationMapEntry ABI layout");
_Static_assert(sizeof(VkComputePipelineCreateInfo) == 96, "VkComputePipelineCreateInfo ABI layout");
_Static_assert(sizeof(VkGraphicsPipelineCreateInfo) == 144, "VkGraphicsPipelineCreateInfo ABI layout");
_Static_assert(offsetof(VkGraphicsPipelineCreateInfo, pStages) == 24, "VkGraphicsPipelineCreateInfo stages offset");
_Static_assert(offsetof(VkGraphicsPipelineCreateInfo, layout) == 104, "VkGraphicsPipelineCreateInfo layout offset");
_Static_assert(sizeof(VkDescriptorPoolCreateInfo) == 40, "VkDescriptorPoolCreateInfo ABI layout");
_Static_assert(sizeof(VkDescriptorSetAllocateInfo) == 40, "VkDescriptorSetAllocateInfo ABI layout");
_Static_assert(sizeof(VkDescriptorBufferInfo) == 24, "VkDescriptorBufferInfo ABI layout");
_Static_assert(sizeof(VkDescriptorImageInfo) == 24, "VkDescriptorImageInfo ABI layout");
_Static_assert(sizeof(VkSamplerCreateInfo) == 80, "VkSamplerCreateInfo ABI layout");
_Static_assert(sizeof(VkWriteDescriptorSet) == 64, "VkWriteDescriptorSet ABI layout");
_Static_assert(sizeof(VkCopyDescriptorSet) == 56, "VkCopyDescriptorSet ABI layout");
_Static_assert(sizeof(VkSubmitInfo) == 72, "VkSubmitInfo ABI layout");
_Static_assert(sizeof(VkFenceCreateInfo) == 24, "VkFenceCreateInfo ABI layout");
_Static_assert(sizeof(VkSemaphoreCreateInfo) == 24, "VkSemaphoreCreateInfo ABI layout");
_Static_assert(sizeof(VkMemoryBarrier) == 24, "VkMemoryBarrier ABI layout");
_Static_assert(sizeof(VkBufferMemoryBarrier) == 56, "VkBufferMemoryBarrier ABI layout");

enum {
    VX_VK_INSTANCE_MAGIC = 0x56584b49u,
    VX_VK_SURFACE_MAGIC = 0x56585346u,
    VX_VK_SWAPCHAIN_MAGIC = 0x56585357u,
    VX_VK_PHYSICAL_DEVICE_MAGIC = 0x56585044u,
    VX_VK_DEVICE_MAGIC = 0x56584456u,
    VX_VK_QUEUE_MAGIC = 0x56585155u,
    VX_VK_MEMORY_MAGIC = 0x56584d45u,
    VX_VK_BUFFER_MAGIC = 0x56584246u,
    VX_VK_IMAGE_MAGIC = 0x5658494du,
    VX_VK_IMAGE_VIEW_MAGIC = 0x56584956u,
    VX_VK_RENDER_PASS_MAGIC = 0x56585250u,
    VX_VK_FRAMEBUFFER_MAGIC = 0x56584642u,
    VX_VK_COMMAND_POOL_MAGIC = 0x56584350u,
    VX_VK_COMMAND_BUFFER_MAGIC = 0x56584342u,
    VX_VK_FENCE_MAGIC = 0x56584645u,
    VX_VK_SEMAPHORE_MAGIC = 0x56585345u,
    VX_VK_SAMPLER_MAGIC = 0x56585341u,
    VX_VK_SHADER_MODULE_MAGIC = 0x5658534du,
    VX_VK_DESCRIPTOR_LAYOUT_MAGIC = 0x5658444cu,
    VX_VK_PIPELINE_LAYOUT_MAGIC = 0x5658504cu,
    VX_VK_PIPELINE_MAGIC = 0x56585049u,
    VX_VK_DESCRIPTOR_POOL_MAGIC = 0x56584450u,
    VX_VK_DESCRIPTOR_SET_MAGIC = 0x56584453u,
    VX_VK_QUEUE_FAMILY = 0,
    VX_VK_BUFFER_ALIGNMENT = 16,
    VX_VK_MAX_DESCRIPTOR_SETS = 4,
    VX_VK_MAX_PUSH_CONSTANT_SIZE = GPU_IR_METAL_MAX_PUSH_CONSTANT_SIZE,
    VX_VK_COMMAND_INITIAL = 0,
    VX_VK_COMMAND_RECORDING,
    VX_VK_COMMAND_EXECUTABLE,
    VX_VK_COMMAND_PENDING,
    VX_VK_COMMAND_INVALID
};

static int vk_storage_descriptor_type(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

static int vk_uniform_descriptor_type(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
}

static int vk_dynamic_buffer_descriptor_type(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
           type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

static int vk_buffer_descriptor_type(VkDescriptorType type) {
    return vk_uniform_descriptor_type(type) || vk_storage_descriptor_type(type);
}

static int vk_descriptor_type_supported(VkDescriptorType type) {
    return vk_buffer_descriptor_type(type) || type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
           type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == VK_DESCRIPTOR_TYPE_SAMPLER ||
           type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

static int vk_image_descriptor_type(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
           type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

static int vk_sampler_descriptor_type(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_SAMPLER || type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

static int vk_descriptor_image_info_type(VkDescriptorType type) {
    return vk_image_descriptor_type(type) || type == VK_DESCRIPTOR_TYPE_SAMPLER;
}

static int vk_shader_stage_mask(VkShaderStageFlags flags, uint32_t *stage_mask) {
    const VkShaderStageFlags supported = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
        VK_SHADER_STAGE_COMPUTE_BIT;
    if (!flags || (flags & ~supported) || !stage_mask) return 0;
    uint32_t mask = 0;
    if (flags & VK_SHADER_STAGE_VERTEX_BIT) mask |= GPU_IR_STAGE_VERTEX;
    if (flags & VK_SHADER_STAGE_FRAGMENT_BIT) mask |= GPU_IR_STAGE_FRAGMENT;
    if (flags & VK_SHADER_STAGE_COMPUTE_BIT) mask |= GPU_IR_STAGE_COMPUTE;
    *stage_mask = mask;
    return 1;
}

static VkShaderStageFlags vk_stage_flags_for_gpu(uint32_t stage_mask) {
    VkShaderStageFlags flags = 0;
    if (stage_mask & GPU_IR_STAGE_VERTEX) flags |= VK_SHADER_STAGE_VERTEX_BIT;
    if (stage_mask & GPU_IR_STAGE_FRAGMENT) flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (stage_mask & GPU_IR_STAGE_COMPUTE) flags |= VK_SHADER_STAGE_COMPUTE_BIT;
    return flags;
}

typedef struct {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    PFN_vkEnumerateInstanceVersion enumerate_instance_version;
    PFN_vkEnumerateInstanceExtensionProperties enumerate_instance_extension_properties;
    PFN_vkEnumerateInstanceLayerProperties enumerate_instance_layer_properties;
    PFN_vkEnumerateDeviceExtensionProperties enumerate_device_extension_properties;
    PFN_vkEnumerateDeviceLayerProperties enumerate_device_layer_properties;
    PFN_vkCreateInstance create_instance;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices;
    PFN_vkGetPhysicalDeviceFeatures get_physical_device_features;
    PFN_vkGetPhysicalDeviceProperties get_physical_device_properties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_physical_device_queue_family_properties;
    PFN_vkGetPhysicalDeviceMemoryProperties get_physical_device_memory_properties;
    PFN_vkGetPhysicalDeviceFormatProperties get_physical_device_format_properties;
    PFN_vkGetPhysicalDeviceImageFormatProperties get_physical_device_image_format_properties;
    PFN_vkCreateDevice create_device;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
} vx_vk_dispatch;

typedef struct vx_vk_buffer vx_vk_buffer;
typedef struct vx_vk_image vx_vk_image;
typedef struct vx_vk_swapchain vx_vk_swapchain;
typedef struct vx_vk_pipeline vx_vk_pipeline;
typedef struct vx_vk_pipeline_layout vx_vk_pipeline_layout;
typedef struct vx_vk_sampler vx_vk_sampler;
typedef struct vx_vk_buffer_use {
    VkCommandBuffer command_buffer;
    struct vx_vk_buffer_use *next;
} vx_vk_buffer_use;
typedef struct {
    vx_vk_buffer *buffer;
    vx_vk_buffer_use *use;
} vx_vk_command_buffer_buffer_use;
typedef struct vx_vk_image_use {
    VkCommandBuffer command_buffer;
    struct vx_vk_image_use *next;
} vx_vk_image_use;
typedef struct {
    vx_vk_image *image;
    vx_vk_image_use *use;
} vx_vk_command_buffer_image_use;
typedef struct vx_vk_pipeline_use {
    VkCommandBuffer command_buffer;
    struct vx_vk_pipeline_use *next;
} vx_vk_pipeline_use;
typedef struct {
    vx_vk_pipeline *pipeline;
    vx_vk_pipeline_use *use;
} vx_vk_command_buffer_pipeline_use;
typedef struct vx_vk_pipeline_layout_use {
    VkCommandBuffer command_buffer;
    struct vx_vk_pipeline_layout_use *next;
} vx_vk_pipeline_layout_use;
typedef struct {
    vx_vk_pipeline_layout *layout;
    vx_vk_pipeline_layout_use *use;
} vx_vk_command_buffer_pipeline_layout_use;

struct VkPhysicalDevice_T {
    const vx_vk_dispatch *dispatch;
    uint32_t magic;
    VkInstance owner;
    uint32_t max_sampler_anisotropy;
    uint8_t storage_image_supported;
};

struct VkQueue_T {
    const vx_vk_dispatch *dispatch;
    uint32_t magic;
    VkDevice device;
    uint32_t family_index;
};

struct VkDevice_T {
    const vx_vk_dispatch *dispatch;
    uint32_t magic;
    VkPhysicalDevice physical_device;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator;
    uint8_t lost;
    uint8_t shader_sampled_image_array_dynamic_indexing;
    uint8_t shader_storage_buffer_array_dynamic_indexing;
    uint8_t shader_storage_image_array_dynamic_indexing;
    uint8_t sampler_anisotropy_enabled;
    uint8_t maintenance1_enabled;
    uint8_t swapchain_enabled;
    uint8_t multi_draw_indirect_enabled;
    uint32_t sampler_count;
    pthread_mutex_t sync_mutex;
    pthread_cond_t sync_condition;
    struct VkQueue_T queue;
};

struct VkInstance_T {
    const vx_vk_dispatch *dispatch;  // Vulkan dispatchable-handle ABI: dispatch pointer is the first word.
    uint32_t magic;
    uint32_t api_version;
    uint8_t surface_enabled, win32_surface_enabled;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator;
    gpu_ir_metal_device *metal;
    struct VkPhysicalDevice_T physical_device;
};

typedef struct vx_vk_surface {
    uint32_t magic;
    VkInstance instance;
    HANDLE hwnd;
    vx_vk_swapchain *active_swapchain;
    uint32_t swapchain_count;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator;
} vx_vk_surface;

typedef struct {
    uint32_t magic;
    VkDevice device;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator, mapped;
    uint32_t bound_buffers, bound_images;
    uint64_t size, mapped_offset, mapped_size;
    gpu_ir_resource *resource;
} vx_vk_memory;

struct vx_vk_buffer {
    uint32_t magic;
    VkDevice device;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator, externally_alive, bound;
    uint32_t refs;
    uint32_t usage;
    uint64_t size;
    vx_vk_memory *memory;
    uint64_t memory_offset;
    vx_vk_buffer_use *command_buffer_uses;
};

struct vx_vk_image {
    uint32_t magic;
    VkDevice device;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator, bound, externally_alive, swapchain_owned;
    uint32_t refs;
    VkFormat format;
    VkImageLayout current_layouts[VX_VK_MAX_IMAGE_LAYERS];
    uint32_t usage, width, height, array_layers, bytes_per_texel, gpu_format;
    VkImageTiling tiling;
    uint64_t row_pitch, array_pitch, size, alignment;
    vx_vk_memory *memory;
    uint64_t memory_offset;
    gpu_ir_resource *resource;
    gpu_ir_resource *backing_resource;
    gpu_ir_resource **layer_resources;
    vx_vk_swapchain *swapchain;
    uint32_t swapchain_index;
    vx_vk_image_use *command_buffer_uses;
};

struct vx_vk_swapchain {
    uint32_t magic;
    VkDevice device;
    vx_vk_surface *surface;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator, retired, externally_alive, surface_registered;
    uint32_t image_count, next_image;
    uint32_t width, height;
    VkFormat format;
    VkImage *image_handles;
    vx_vk_image **images;
    uint8_t *acquired;
    void *metal_layer;
};

typedef struct {
    uint32_t magic;
    VkDevice device;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator, externally_alive;
    uint32_t refs;
    vx_vk_image *image;
    VkFormat format;
    VkImageViewType view_type;
    VkComponentMapping components;
    VkImageSubresourceRange subresource_range;
} vx_vk_image_view;

typedef struct {
    uint32_t magic;
    VkDevice device;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator, externally_alive;
    uint32_t refs;
    VkAttachmentDescription attachment;
} vx_vk_render_pass;

typedef struct {
    uint32_t magic;
    VkDevice device;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator, externally_alive;
    vx_vk_render_pass *render_pass;
    vx_vk_image_view *view;
    uint32_t width, height, layer;
} vx_vk_framebuffer;

struct vx_vk_sampler {
    uint32_t magic;
    VkDevice device;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator, externally_alive;
    uint32_t refs;
    void *metal_state;
};

typedef struct {
    vx_vk_image *image;
    VkImageLayout old_layout, new_layout;
    uint32_t base_array_layer, array_layer_count;
    uint8_t transition;
} vx_vk_image_layout_op;

typedef struct {
    vx_vk_image *image;
    uint32_t array_layer;
    VkImageLayout layout;
} vx_vk_image_layout_state;

typedef struct {
    uint32_t magic;
    VkDevice device;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator;
    uint32_t flags, family_index;
    VkCommandBuffer *buffers;
    uint32_t buffer_count, buffer_capacity;
} vx_vk_command_pool;

typedef struct vx_vk_descriptor_set vx_vk_descriptor_set;

typedef struct {
    vx_vk_descriptor_set *set;
    uint64_t generation;
} vx_vk_descriptor_set_version;

struct VkCommandBuffer_T {
    const vx_vk_dispatch *dispatch;
    uint32_t magic;
    VkDevice device;
    vx_vk_command_pool *pool;
    uint32_t state, usage_flags;
    int record_error;
    uint8_t render_pass_active;
    vx_vk_image *render_pass_image;
    uint32_t render_pass_layer, render_pass_width, render_pass_height;
    VkImageLayout render_pass_attachment_layout, render_pass_final_layout;
    vx_vk_pipeline *compute_pipeline;
    vx_vk_pipeline *graphics_pipeline;
    vx_vk_buffer *index_buffer;
    VkDeviceSize index_buffer_offset;
    VkIndexType index_type;
    vx_vk_buffer *vertex_buffers[GPU_IR_MAX_VERTEX_BUFFERS];
    VkDeviceSize vertex_buffer_offsets[GPU_IR_MAX_VERTEX_BUFFERS];
    VkViewport dynamic_viewport;
    VkRect2D dynamic_scissor;
    float dynamic_blend_constants[4];
    uint8_t dynamic_viewport_set, dynamic_scissor_set, dynamic_blend_constants_set;
    vx_vk_pipeline_layout *bound_layout;
    vx_vk_pipeline_layout *push_constant_layout;
    gpu_ir_resource *push_constant_resource;
    VkDescriptorSet bound_sets[VX_VK_MAX_DESCRIPTOR_SETS];
    uint32_t bound_dynamic_offsets[VX_VK_MAX_DESCRIPTOR_SETS][VX_VK_MAX_DESCRIPTOR_VALUES];
    vx_vk_descriptor_set_version *used_descriptor_sets;
    uint32_t used_descriptor_set_count, used_descriptor_set_capacity;
    vx_vk_command_buffer_buffer_use *used_buffers;
    uint32_t used_buffer_count, used_buffer_capacity;
    vx_vk_command_buffer_image_use *used_images;
    uint32_t used_image_count, used_image_capacity;
    vx_vk_command_buffer_pipeline_use *used_pipelines;
    uint32_t used_pipeline_count, used_pipeline_capacity;
    vx_vk_command_buffer_pipeline_layout_use *used_pipeline_layouts;
    uint32_t used_pipeline_layout_count, used_pipeline_layout_capacity;
    vx_vk_image_layout_op *image_layout_ops;
    uint32_t image_layout_op_count, image_layout_op_capacity;
    gpu_ir_stream stream;
};

typedef struct {
    uint32_t magic;
    VkDevice device;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator, signaled;
} vx_vk_fence;

typedef struct {
    uint32_t magic;
    VkDevice device;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator, signaled;
} vx_vk_semaphore;

typedef struct {
    uint32_t magic;
    VkDevice device;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator, externally_alive;
    uint32_t refs, binding_count, descriptor_count;
    VkDescriptorSetLayoutBinding *bindings;
} vx_vk_descriptor_layout;

struct vx_vk_pipeline_layout {
    uint32_t magic;
    VkDevice device;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator, externally_alive;
    uint32_t refs, set_layout_count;
    vx_vk_descriptor_layout **set_layouts;
    gpu_ir_pipeline_layout *gpu_layout;
    vx_vk_pipeline_layout_use *command_buffer_uses;
};

typedef struct {
    uint32_t magic;
    VkDevice device;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator, externally_alive;
    uint32_t refs;
    spirv_module module;
} vx_vk_shader_module;

struct vx_vk_pipeline {
    uint32_t magic;
    VkDevice device;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator, externally_alive;
    uint32_t refs, set_layout_count, bind_point, topology;
    vx_vk_descriptor_layout **set_layouts;
    vx_vk_pipeline_layout *layout;
    gpu_ir_pipeline *gpu_pipeline;
    gpu_ir_pipeline_layout *reflected_layout;
    VkFormat color_format;
    VkViewport viewport;
    VkRect2D scissor;
    uint8_t dynamic_viewport, dynamic_scissor, dynamic_blend_constants;
    vx_vk_pipeline_use *command_buffer_uses;
};

typedef struct {
    uint32_t magic;
    VkDevice device;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator, externally_alive, allow_free_sets;
    uint32_t refs, max_sets, allocated_sets;
    uint32_t uniform_capacity, uniform_used, dynamic_uniform_capacity, dynamic_uniform_used;
    uint32_t storage_capacity, storage_used, dynamic_storage_capacity, dynamic_storage_used;
    uint32_t sampled_image_capacity, sampled_image_used;
    uint32_t storage_image_capacity, storage_image_used;
    uint32_t sampler_capacity, sampler_used;
    uint32_t combined_image_sampler_capacity, combined_image_sampler_used;
    VkDescriptorSet *sets;
    uint32_t set_capacity;
} vx_vk_descriptor_pool;

typedef struct {
    vx_vk_buffer *buffer;
    vx_vk_image *image;
    vx_vk_sampler *sampler;
    uint64_t offset, range;
    VkImageLayout image_layout;
    uint32_t component_mapping;
    uint8_t written, whole_size;
} vx_vk_descriptor_value;

struct vx_vk_descriptor_set {
    uint32_t magic;
    VkDevice device;
    vx_vk_descriptor_pool *pool;
    vx_vk_descriptor_layout *layout;
    VkAllocationCallbacks allocator;
    uint8_t has_allocator, externally_alive;
    uint32_t refs;
    uint64_t generation;
    uint32_t uniform_descriptor_count, dynamic_uniform_descriptor_count;
    uint32_t storage_descriptor_count, dynamic_storage_descriptor_count;
    uint32_t sampled_image_descriptor_count, storage_image_descriptor_count;
    uint32_t sampler_descriptor_count, combined_image_sampler_descriptor_count;
    vx_vk_descriptor_value *values;
};

static const vx_vk_dispatch g_vk_dispatch;

static int instance_is_valid(VkInstance instance) {
    return instance && instance->dispatch == &g_vk_dispatch && instance->magic == VX_VK_INSTANCE_MAGIC;
}

static int physical_device_is_valid(VkPhysicalDevice physical_device) {
    return physical_device && physical_device->dispatch == &g_vk_dispatch &&
        physical_device->magic == VX_VK_PHYSICAL_DEVICE_MAGIC && instance_is_valid(physical_device->owner) &&
        physical_device == &physical_device->owner->physical_device && physical_device->owner->metal;
}

static int device_is_valid(VkDevice device) {
    return device && device->dispatch == &g_vk_dispatch && device->magic == VX_VK_DEVICE_MAGIC &&
        physical_device_is_valid(device->physical_device);
}

static int queue_is_valid(VkQueue queue) {
    return queue && queue->dispatch == &g_vk_dispatch && queue->magic == VX_VK_QUEUE_MAGIC &&
        device_is_valid(queue->device) && queue == &queue->device->queue;
}

static int allocator_is_valid(const VkAllocationCallbacks *allocator) {
    return !allocator || (allocator->pfnAllocation && allocator->pfnFree);
}

static void *object_allocate(const VkAllocationCallbacks *allocator, size_t size, size_t alignment,
                             VkSystemAllocationScope scope) {
    void *memory = allocator ? allocator->pfnAllocation(allocator->pUserData, size, alignment, scope) :
        calloc(1, size);
    if (memory && allocator) memset(memory, 0, size);
    return memory;
}

static void object_free(const VkAllocationCallbacks *allocator, void *memory) {
    if (!memory) return;
    if (allocator) allocator->pfnFree(allocator->pUserData, memory);
    else free(memory);
}

static vx_vk_memory *memory_from_handle(VkDeviceMemory handle, VkDevice device) {
    vx_vk_memory *memory = (vx_vk_memory *)(uintptr_t)handle;
    return memory && memory->magic == VX_VK_MEMORY_MAGIC && memory->device == device ? memory : NULL;
}

static vx_vk_buffer *buffer_from_handle(VkBuffer handle, VkDevice device) {
    vx_vk_buffer *buffer = (vx_vk_buffer *)(uintptr_t)handle;
    return buffer && buffer->magic == VX_VK_BUFFER_MAGIC && buffer->externally_alive &&
        buffer->device == device ? buffer : NULL;
}

static void buffer_retain(vx_vk_buffer *buffer) {
    if (buffer && buffer->refs < UINT32_MAX) buffer->refs++;
}

static void buffer_release(vx_vk_buffer *buffer) {
    if (!buffer || !buffer->refs || --buffer->refs) return;
    VkAllocationCallbacks allocator = buffer->allocator;
    int has_allocator = buffer->has_allocator;
    buffer->magic = 0;
    object_free(has_allocator ? &allocator : NULL, buffer);
}

static void image_retain(vx_vk_image *image) {
    if (image && image->refs < UINT32_MAX) image->refs++;
}

static void image_release(vx_vk_image *image) {
    if (!image || !image->refs || --image->refs) return;
    if (image->memory && image->memory->bound_images) image->memory->bound_images--;
    VkAllocationCallbacks allocator = image->allocator;
    int has_allocator = image->has_allocator;
    if (image->layer_resources) {
        for (uint32_t i = 0; i < image->array_layers; i++)
            gpu_ir_resource_release(image->layer_resources[i]);
        object_free(has_allocator ? &allocator : NULL, image->layer_resources);
    } else {
        gpu_ir_resource_release(image->resource);
    }
    gpu_ir_resource_release(image->backing_resource);
    image->magic = 0;
    object_free(has_allocator ? &allocator : NULL, image);
}

static gpu_ir_resource *image_layer_resource(const vx_vk_image *image, uint32_t layer) {
    if (!image || layer >= image->array_layers) return NULL;
    return image->layer_resources ? image->layer_resources[layer] : image->resource;
}

static void sampler_retain(vx_vk_sampler *sampler) {
    if (sampler && sampler->refs < UINT32_MAX) sampler->refs++;
}

static void sampler_release(vx_vk_sampler *sampler) {
    if (!sampler || !sampler->refs || --sampler->refs) return;
    gpu_ir_metal_sampler_release(sampler->metal_state);
    VkAllocationCallbacks allocator = sampler->allocator;
    int has_allocator = sampler->has_allocator;
    sampler->magic = 0;
    object_free(has_allocator ? &allocator : NULL, sampler);
}

static int sampler_slot_reserve(VkDevice device) {
    pthread_mutex_lock(&device->sync_mutex);
    int reserved = device->sampler_count < VX_VK_MAX_SAMPLER_ALLOCATION_COUNT;
    if (reserved) device->sampler_count++;
    pthread_mutex_unlock(&device->sync_mutex);
    return reserved;
}

static void sampler_slot_release(VkDevice device) {
    pthread_mutex_lock(&device->sync_mutex);
    if (device->sampler_count) device->sampler_count--;
    pthread_mutex_unlock(&device->sync_mutex);
}

static vx_vk_image *image_from_handle(VkImage handle, VkDevice device) {
    vx_vk_image *image = (vx_vk_image *)(uintptr_t)handle;
    return image && image->magic == VX_VK_IMAGE_MAGIC && image->externally_alive && image->device == device ? image : NULL;
}

static vx_vk_image_view *image_view_from_handle(VkImageView handle, VkDevice device) {
    vx_vk_image_view *view = (vx_vk_image_view *)(uintptr_t)handle;
    return view && view->magic == VX_VK_IMAGE_VIEW_MAGIC && view->externally_alive && view->device == device ? view : NULL;
}

static vx_vk_render_pass *render_pass_from_handle(VkRenderPass handle, VkDevice device) {
    vx_vk_render_pass *render_pass = (vx_vk_render_pass *)(uintptr_t)handle;
    return render_pass && render_pass->magic == VX_VK_RENDER_PASS_MAGIC && render_pass->externally_alive &&
        render_pass->device == device ? render_pass : NULL;
}

static vx_vk_framebuffer *framebuffer_from_handle(VkFramebuffer handle, VkDevice device) {
    vx_vk_framebuffer *framebuffer = (vx_vk_framebuffer *)(uintptr_t)handle;
    return framebuffer && framebuffer->magic == VX_VK_FRAMEBUFFER_MAGIC && framebuffer->externally_alive &&
        framebuffer->device == device ? framebuffer : NULL;
}

static vx_vk_command_pool *command_pool_from_handle(VkCommandPool handle, VkDevice device) {
    vx_vk_command_pool *pool = (vx_vk_command_pool *)(uintptr_t)handle;
    return pool && pool->magic == VX_VK_COMMAND_POOL_MAGIC && pool->device == device ? pool : NULL;
}

static vx_vk_fence *fence_from_handle(VkFence handle, VkDevice device) {
    vx_vk_fence *fence = (vx_vk_fence *)(uintptr_t)handle;
    return fence && fence->magic == VX_VK_FENCE_MAGIC && fence->device == device ? fence : NULL;
}

static vx_vk_semaphore *semaphore_from_handle(VkSemaphore handle, VkDevice device) {
    vx_vk_semaphore *semaphore = (vx_vk_semaphore *)(uintptr_t)handle;
    return semaphore && semaphore->magic == VX_VK_SEMAPHORE_MAGIC && semaphore->device == device ? semaphore : NULL;
}

static vx_vk_sampler *sampler_from_handle(VkSampler handle, VkDevice device) {
    vx_vk_sampler *sampler = (vx_vk_sampler *)(uintptr_t)handle;
    return sampler && sampler->magic == VX_VK_SAMPLER_MAGIC && sampler->device == device &&
        sampler->externally_alive ? sampler : NULL;
}

static vx_vk_shader_module *shader_module_from_handle(VkShaderModule handle, VkDevice device) {
    vx_vk_shader_module *module = (vx_vk_shader_module *)(uintptr_t)handle;
    return module && module->magic == VX_VK_SHADER_MODULE_MAGIC && module->device == device &&
        module->externally_alive ? module : NULL;
}

static vx_vk_descriptor_layout *descriptor_layout_from_handle(VkDescriptorSetLayout handle, VkDevice device) {
    vx_vk_descriptor_layout *layout = (vx_vk_descriptor_layout *)(uintptr_t)handle;
    return layout && layout->magic == VX_VK_DESCRIPTOR_LAYOUT_MAGIC && layout->device == device &&
        layout->externally_alive ? layout : NULL;
}

static vx_vk_pipeline_layout *pipeline_layout_from_handle(VkPipelineLayout handle, VkDevice device) {
    vx_vk_pipeline_layout *layout = (vx_vk_pipeline_layout *)(uintptr_t)handle;
    return layout && layout->magic == VX_VK_PIPELINE_LAYOUT_MAGIC && layout->device == device &&
        layout->externally_alive ? layout : NULL;
}

static vx_vk_pipeline *pipeline_from_handle(VkPipeline handle, VkDevice device) {
    vx_vk_pipeline *pipeline = (vx_vk_pipeline *)(uintptr_t)handle;
    return pipeline && pipeline->magic == VX_VK_PIPELINE_MAGIC && pipeline->device == device &&
        pipeline->externally_alive ? pipeline : NULL;
}

static vx_vk_descriptor_pool *descriptor_pool_from_handle(VkDescriptorPool handle, VkDevice device) {
    vx_vk_descriptor_pool *pool = (vx_vk_descriptor_pool *)(uintptr_t)handle;
    return pool && pool->magic == VX_VK_DESCRIPTOR_POOL_MAGIC && pool->device == device &&
        pool->externally_alive ? pool : NULL;
}

static vx_vk_descriptor_set *descriptor_set_from_handle(VkDescriptorSet handle, VkDevice device) {
    vx_vk_descriptor_set *set = (vx_vk_descriptor_set *)(uintptr_t)handle;
    return set && set->magic == VX_VK_DESCRIPTOR_SET_MAGIC && set->device == device &&
        set->externally_alive ? set : NULL;
}

static vx_vk_descriptor_set *descriptor_set_from_internal_handle(VkDescriptorSet handle, VkDevice device) {
    vx_vk_descriptor_set *set = (vx_vk_descriptor_set *)(uintptr_t)handle;
    return set && set->magic == VX_VK_DESCRIPTOR_SET_MAGIC && set->device == device ? set : NULL;
}

static void descriptor_layout_retain(vx_vk_descriptor_layout *layout) {
    if (layout) layout->refs++;
}

static void descriptor_layout_release(vx_vk_descriptor_layout *layout) {
    if (!layout || !layout->refs || --layout->refs) return;
    VkAllocationCallbacks allocator = layout->allocator;
    int has_allocator = layout->has_allocator;
    object_free(has_allocator ? &allocator : NULL, layout->bindings);
    layout->magic = 0;
    object_free(has_allocator ? &allocator : NULL, layout);
}

static void pipeline_layout_retain(vx_vk_pipeline_layout *layout) {
    if (layout) layout->refs++;
}

static void pipeline_layout_release(vx_vk_pipeline_layout *layout) {
    if (!layout || !layout->refs || --layout->refs) return;
    for (uint32_t i = 0; i < layout->set_layout_count; i++) descriptor_layout_release(layout->set_layouts[i]);
    VkAllocationCallbacks allocator = layout->allocator;
    int has_allocator = layout->has_allocator;
    object_free(has_allocator ? &allocator : NULL, layout->set_layouts);
    gpu_ir_pipeline_layout_release(layout->gpu_layout);
    layout->magic = 0;
    object_free(has_allocator ? &allocator : NULL, layout);
}

static int pipeline_layout_push_constants_compatible(const vx_vk_pipeline_layout *a,
                                                      const vx_vk_pipeline_layout *b) {
    if (!a || !b) return 0;
    uint32_t count = gpu_ir_pipeline_layout_push_constant_count(a->gpu_layout);
    if (count != gpu_ir_pipeline_layout_push_constant_count(b->gpu_layout)) return 0;
    for (uint32_t i = 0; i < count; i++) {
        gpu_ir_push_constant_range left, right;
        if (gpu_ir_pipeline_layout_push_constant_at(a->gpu_layout, i, &left) != GPU_IR_OK ||
            gpu_ir_pipeline_layout_push_constant_at(b->gpu_layout, i, &right) != GPU_IR_OK ||
            left.offset != right.offset || left.size != right.size || left.stage_mask != right.stage_mask)
            return 0;
    }
    return 1;
}

static int pipeline_layout_covers_push_constants(const vx_vk_pipeline_layout *layout,
                                                  uint32_t offset, uint32_t size,
                                                  uint32_t stage_mask) {
    if (!layout || !size || offset >= VX_VK_MAX_PUSH_CONSTANT_SIZE ||
        size > VX_VK_MAX_PUSH_CONSTANT_SIZE - offset || stage_mask != GPU_IR_STAGE_COMPUTE)
        return 0;
    uint32_t end = offset + size;
    for (uint32_t cursor = offset; cursor < end;) {
        uint32_t covered_until = cursor;
        for (uint32_t i = 0; i < gpu_ir_pipeline_layout_push_constant_count(layout->gpu_layout); i++) {
            gpu_ir_push_constant_range range;
            if (gpu_ir_pipeline_layout_push_constant_at(layout->gpu_layout, i, &range) != GPU_IR_OK)
                return 0;
            uint32_t range_end = range.offset + range.size;
            if (range.offset <= cursor && range_end > cursor &&
                (range.stage_mask & stage_mask) == stage_mask && range_end > covered_until)
                covered_until = range_end;
        }
        if (covered_until == cursor) return 0;
        cursor = covered_until < end ? covered_until : end;
    }
    return 1;
}

static int command_buffer_ensure_push_constant_resource(VkCommandBuffer commandBuffer) {
    if (!commandBuffer) return 0;
    if (commandBuffer->push_constant_resource) return 1;
    uint8_t initial[GPU_IR_METAL_RUNTIME_DATA_SIZE] = { 0 };
    commandBuffer->push_constant_resource = gpu_ir_metal_buffer_create(
        commandBuffer->device->physical_device->owner->metal, sizeof initial, initial);
    return commandBuffer->push_constant_resource != NULL;
}

static void pipeline_retain(vx_vk_pipeline *pipeline) {
    if (pipeline) pipeline->refs++;
}

static void pipeline_release(vx_vk_pipeline *pipeline) {
    if (!pipeline || !pipeline->refs || --pipeline->refs) return;
    for (uint32_t i = 0; i < pipeline->set_layout_count; i++) descriptor_layout_release(pipeline->set_layouts[i]);
    VkAllocationCallbacks allocator = pipeline->allocator;
    int has_allocator = pipeline->has_allocator;
    object_free(has_allocator ? &allocator : NULL, pipeline->set_layouts);
    pipeline_layout_release(pipeline->layout);
    gpu_ir_pipeline_release(pipeline->gpu_pipeline);
    gpu_ir_pipeline_layout_release(pipeline->reflected_layout);
    pipeline->magic = 0;
    object_free(has_allocator ? &allocator : NULL, pipeline);
}

static void descriptor_set_retain(vx_vk_descriptor_set *set) {
    if (set) set->refs++;
}

static void descriptor_set_release(vx_vk_descriptor_set *set) {
    if (!set || !set->refs || --set->refs) return;
    VkAllocationCallbacks *allocator = set->has_allocator ? &set->allocator : NULL;
    uint32_t descriptor_count = set->layout ? set->layout->descriptor_count : 0;
    for (uint32_t i = 0; i < descriptor_count; i++) {
        if (set->values[i].buffer) buffer_release(set->values[i].buffer);
        if (set->values[i].image) image_release(set->values[i].image);
        if (set->values[i].sampler) sampler_release(set->values[i].sampler);
    }
    descriptor_layout_release(set->layout);
    object_free(allocator, set->values);
    set->magic = 0;
    object_free(allocator, set);
}

static VkDeviceMemory memory_to_handle(vx_vk_memory *memory) { return (VkDeviceMemory)(uintptr_t)memory; }
static VkBuffer buffer_to_handle(vx_vk_buffer *buffer) { return (VkBuffer)(uintptr_t)buffer; }
static VkImage image_to_handle(vx_vk_image *image) { return (VkImage)(uintptr_t)image; }
static VkImageView image_view_to_handle(vx_vk_image_view *view) { return (VkImageView)(uintptr_t)view; }
static VkCommandPool command_pool_to_handle(vx_vk_command_pool *pool) { return (VkCommandPool)(uintptr_t)pool; }
static VkFence fence_to_handle(vx_vk_fence *fence) { return (VkFence)(uintptr_t)fence; }
static VkSemaphore semaphore_to_handle(vx_vk_semaphore *semaphore) { return (VkSemaphore)(uintptr_t)semaphore; }
static VkSwapchainKHR swapchain_to_handle(vx_vk_swapchain *swapchain) {
    return (VkSwapchainKHR)(uintptr_t)swapchain;
}
static VkSampler sampler_to_handle(vx_vk_sampler *sampler) { return (VkSampler)(uintptr_t)sampler; }
static VkRenderPass render_pass_to_handle(vx_vk_render_pass *render_pass) {
    return (VkRenderPass)(uintptr_t)render_pass;
}
static VkFramebuffer framebuffer_to_handle(vx_vk_framebuffer *framebuffer) {
    return (VkFramebuffer)(uintptr_t)framebuffer;
}
static VkShaderModule shader_module_to_handle(vx_vk_shader_module *module) { return (VkShaderModule)(uintptr_t)module; }
static VkDescriptorSetLayout descriptor_layout_to_handle(vx_vk_descriptor_layout *layout) {
    return (VkDescriptorSetLayout)(uintptr_t)layout;
}
static VkPipelineLayout pipeline_layout_to_handle(vx_vk_pipeline_layout *layout) {
    return (VkPipelineLayout)(uintptr_t)layout;
}
static VkPipeline pipeline_to_handle(vx_vk_pipeline *pipeline) { return (VkPipeline)(uintptr_t)pipeline; }
static VkDescriptorPool descriptor_pool_to_handle(vx_vk_descriptor_pool *pool) {
    return (VkDescriptorPool)(uintptr_t)pool;
}
static VkDescriptorSet descriptor_set_to_handle(vx_vk_descriptor_set *set) {
    return (VkDescriptorSet)(uintptr_t)set;
}

static VkResult empty_enumeration(uint32_t *p_count, const char *layer_name) {
    if (!p_count) return VK_ERROR_INITIALIZATION_FAILED;
    if (layer_name) return VK_ERROR_LAYER_NOT_PRESENT;
    *p_count = 0;
    return VK_SUCCESS;
}

VkResult WINAPI vkEnumerateInstanceVersion(uint32_t *pApiVersion) {
    if (!pApiVersion) return VK_ERROR_INITIALIZATION_FAILED;
    *pApiVersion = VK_API_VERSION_1_0;
    return VK_SUCCESS;
}

VkResult WINAPI vkEnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pPropertyCount,
                                                        VkExtensionProperties *pProperties) {
    static const struct { const char *name; uint32_t version; } extensions[] = {
        { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION },
        { VK_KHR_WIN32_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_SPEC_VERSION }
    };
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (pLayerName) return VK_ERROR_LAYER_NOT_PRESENT;
    if (!pProperties) {
        *pPropertyCount = sizeof extensions / sizeof extensions[0];
        return VK_SUCCESS;
    }
    uint32_t capacity = *pPropertyCount;
    uint32_t count = capacity < sizeof extensions / sizeof extensions[0] ?
        capacity : (uint32_t)(sizeof extensions / sizeof extensions[0]);
    for (uint32_t i = 0; i < count; i++) {
        memset(&pProperties[i], 0, sizeof pProperties[i]);
        snprintf(pProperties[i].extensionName, sizeof pProperties[i].extensionName, "%s", extensions[i].name);
        pProperties[i].specVersion = extensions[i].version;
    }
    *pPropertyCount = count;
    return count < sizeof extensions / sizeof extensions[0] ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult WINAPI vkEnumerateInstanceLayerProperties(uint32_t *pPropertyCount, VkLayerProperties *pProperties) {
    (void)pProperties;
    return empty_enumeration(pPropertyCount, NULL);
}

VkResult WINAPI vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char *pLayerName,
    uint32_t *pPropertyCount, VkExtensionProperties *pProperties) {
    if (!physical_device_is_valid(physicalDevice)) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (pLayerName) return VK_ERROR_LAYER_NOT_PRESENT;
    const uint32_t supported_count = 2u;
    if (!pProperties) {
        *pPropertyCount = supported_count;
        return VK_SUCCESS;
    }
    static const struct { const char *name; uint32_t version; } extensions[] = {
        { VK_KHR_MAINTENANCE1_EXTENSION_NAME, VK_KHR_MAINTENANCE1_SPEC_VERSION },
        { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_SPEC_VERSION }
    };
    uint32_t capacity = *pPropertyCount;
    uint32_t count = capacity < supported_count ? capacity : supported_count;
    for (uint32_t i = 0; i < count; i++) {
        memset(&pProperties[i], 0, sizeof pProperties[i]);
        snprintf(pProperties[i].extensionName, sizeof pProperties[i].extensionName, "%s", extensions[i].name);
        pProperties[i].specVersion = extensions[i].version;
    }
    *pPropertyCount = count;
    return count < supported_count ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult WINAPI vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
    uint32_t *pPropertyCount, VkLayerProperties *pProperties) {
    (void)pProperties;
    if (!physical_device_is_valid(physicalDevice)) return VK_ERROR_INITIALIZATION_FAILED;
    // This instance accepted no layers, so Vulkan's legacy device-layer list is empty as well.
    return empty_enumeration(pPropertyCount, NULL);
}

static int version_supported(uint32_t version) {
    uint32_t variant = version >> 29;
    uint32_t major = (version >> 22) & 0x7fu;
    uint32_t minor = (version >> 12) & 0x3ffu;
    // Vulkan patch versions are API-compatible; this frontend implements core 1.0 only.
    return variant == 0 && (major == 0 || (major == 1 && minor == 0));
}

static void *instance_allocate(const VkAllocationCallbacks *allocator, size_t size) {
    if (!allocator) return calloc(1, size);
    return allocator->pfnAllocation(allocator->pUserData, size, alignof(struct VkInstance_T),
                                   VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
}

static void instance_free(const VkAllocationCallbacks *allocator, void *memory) {
    if (!memory) return;
    if (allocator) allocator->pfnFree(allocator->pUserData, memory);
    else free(memory);
}

VkResult WINAPI vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                 const VkAllocationCallbacks *pAllocator, VkInstance *pInstance) {
    if (!pInstance) return VK_ERROR_INITIALIZATION_FAILED;
    *pInstance = NULL;
    if (!pCreateInfo || pCreateInfo->sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO ||
        pCreateInfo->pNext || pCreateInfo->flags ||
        (pCreateInfo->enabledLayerCount && !pCreateInfo->ppEnabledLayerNames) ||
        (pCreateInfo->enabledExtensionCount && !pCreateInfo->ppEnabledExtensionNames))
        return VK_ERROR_INITIALIZATION_FAILED;

    const VkApplicationInfo *application = pCreateInfo->pApplicationInfo;
    if (application && (application->sType != VK_STRUCTURE_TYPE_APPLICATION_INFO || application->pNext))
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t requested_version = application ? application->apiVersion : VK_API_VERSION_1_0;
    if (!version_supported(requested_version)) return VK_ERROR_INCOMPATIBLE_DRIVER;

    for (uint32_t i = 0; i < pCreateInfo->enabledLayerCount; i++) {
        if (!pCreateInfo->ppEnabledLayerNames[i]) return VK_ERROR_INITIALIZATION_FAILED;
        return VK_ERROR_LAYER_NOT_PRESENT; // No layer is advertised by the frontend.
    }
    uint8_t surface_enabled = 0, win32_surface_enabled = 0;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        if (!pCreateInfo->ppEnabledExtensionNames[i]) return VK_ERROR_INITIALIZATION_FAILED;
        const char *name = pCreateInfo->ppEnabledExtensionNames[i];
        if (!strcmp(name, VK_KHR_SURFACE_EXTENSION_NAME)) {
            if (surface_enabled) return VK_ERROR_INITIALIZATION_FAILED;
            surface_enabled = 1;
        } else if (!strcmp(name, VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) {
            if (win32_surface_enabled) return VK_ERROR_INITIALIZATION_FAILED;
            win32_surface_enabled = 1;
        } else {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    if (win32_surface_enabled && !surface_enabled) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (pAllocator && (!pAllocator->pfnAllocation || !pAllocator->pfnFree))
        return VK_ERROR_INITIALIZATION_FAILED;

    struct VkInstance_T *instance = instance_allocate(pAllocator, sizeof *instance);
    if (!instance) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(instance, 0, sizeof *instance);
    instance->dispatch = &g_vk_dispatch;
    instance->magic = VX_VK_INSTANCE_MAGIC;
    instance->api_version = requested_version ? requested_version : VK_API_VERSION_1_0;
    instance->surface_enabled = surface_enabled;
    instance->win32_surface_enabled = win32_surface_enabled;
    instance->metal = gpu_ir_metal_device_create();
    if (instance->metal && gpu_ir_metal_has_unified_memory(instance->metal)) {
        instance->physical_device.dispatch = &g_vk_dispatch;
        instance->physical_device.magic = VX_VK_PHYSICAL_DEVICE_MAGIC;
        instance->physical_device.owner = instance;
        instance->physical_device.max_sampler_anisotropy =
            gpu_ir_metal_sampler_max_anisotropy(instance->metal);
        instance->physical_device.storage_image_supported = gpu_ir_metal_storage_texture_supported(
            instance->metal, GPU_IR_FORMAT_RGBA8_UNORM) != 0;
    }
    if (pAllocator) {
        instance->allocator = *pAllocator;
        instance->has_allocator = 1;
    }
    *pInstance = instance;
    return VK_SUCCESS;
}

void WINAPI vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator; // The creation allocator is retained and always used for the matching free.
    if (!instance_is_valid(instance)) return;
    VkAllocationCallbacks allocator = instance->allocator;
    int has_allocator = instance->has_allocator;
    instance->magic = 0;
    gpu_ir_metal_device_release(instance->metal);
    instance_free(has_allocator ? &allocator : NULL, instance);
}

static const struct {
    const char *name;
    PFN_vkVoidFunction function;
} g_vk_global_functions[] = {
    { "vkGetInstanceProcAddr", (PFN_vkVoidFunction)vkGetInstanceProcAddr },
    { "vkEnumerateInstanceVersion", (PFN_vkVoidFunction)vkEnumerateInstanceVersion },
    { "vkEnumerateInstanceExtensionProperties", (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties },
    { "vkEnumerateInstanceLayerProperties", (PFN_vkVoidFunction)vkEnumerateInstanceLayerProperties },
    { "vkCreateInstance", (PFN_vkVoidFunction)vkCreateInstance },
};

static const struct {
    const char *name;
    PFN_vkVoidFunction function;
} g_vk_instance_functions[] = {
    { "vkCreateWin32SurfaceKHR", (PFN_vkVoidFunction)vkCreateWin32SurfaceKHR },
    { "vkDestroySurfaceKHR", (PFN_vkVoidFunction)vkDestroySurfaceKHR },
    { "vkGetPhysicalDeviceSurfaceSupportKHR", (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceSupportKHR },
    { "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceCapabilitiesKHR },
    { "vkGetPhysicalDeviceSurfaceFormatsKHR", (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceFormatsKHR },
    { "vkGetPhysicalDeviceSurfacePresentModesKHR", (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfacePresentModesKHR },
    { "vkGetPhysicalDeviceWin32PresentationSupportKHR",
      (PFN_vkVoidFunction)vkGetPhysicalDeviceWin32PresentationSupportKHR },
    { "vkDestroyInstance", (PFN_vkVoidFunction)vkDestroyInstance },
    { "vkEnumeratePhysicalDevices", (PFN_vkVoidFunction)vkEnumeratePhysicalDevices },
    { "vkEnumerateDeviceExtensionProperties", (PFN_vkVoidFunction)vkEnumerateDeviceExtensionProperties },
    { "vkEnumerateDeviceLayerProperties", (PFN_vkVoidFunction)vkEnumerateDeviceLayerProperties },
    { "vkGetPhysicalDeviceFeatures", (PFN_vkVoidFunction)vkGetPhysicalDeviceFeatures },
    { "vkGetPhysicalDeviceProperties", (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties },
    { "vkGetPhysicalDeviceQueueFamilyProperties", (PFN_vkVoidFunction)vkGetPhysicalDeviceQueueFamilyProperties },
    { "vkGetPhysicalDeviceMemoryProperties", (PFN_vkVoidFunction)vkGetPhysicalDeviceMemoryProperties },
    { "vkGetPhysicalDeviceFormatProperties", (PFN_vkVoidFunction)vkGetPhysicalDeviceFormatProperties },
    { "vkGetPhysicalDeviceImageFormatProperties", (PFN_vkVoidFunction)vkGetPhysicalDeviceImageFormatProperties },
    { "vkCreateDevice", (PFN_vkVoidFunction)vkCreateDevice },
    { "vkGetDeviceProcAddr", (PFN_vkVoidFunction)vkGetDeviceProcAddr },
};

static const struct {
    const char *name;
    PFN_vkVoidFunction function;
} g_vk_device_functions[] = {
    { "vkDestroyDevice", (PFN_vkVoidFunction)vkDestroyDevice },
    { "vkGetDeviceQueue", (PFN_vkVoidFunction)vkGetDeviceQueue },
    { "vkCreateBuffer", (PFN_vkVoidFunction)vkCreateBuffer },
    { "vkDestroyBuffer", (PFN_vkVoidFunction)vkDestroyBuffer },
    { "vkGetBufferMemoryRequirements", (PFN_vkVoidFunction)vkGetBufferMemoryRequirements },
    { "vkAllocateMemory", (PFN_vkVoidFunction)vkAllocateMemory },
    { "vkFreeMemory", (PFN_vkVoidFunction)vkFreeMemory },
    { "vkBindBufferMemory", (PFN_vkVoidFunction)vkBindBufferMemory },
    { "vkCreateImage", (PFN_vkVoidFunction)vkCreateImage },
    { "vkDestroyImage", (PFN_vkVoidFunction)vkDestroyImage },
    { "vkCreateSwapchainKHR", (PFN_vkVoidFunction)vkCreateSwapchainKHR },
    { "vkDestroySwapchainKHR", (PFN_vkVoidFunction)vkDestroySwapchainKHR },
    { "vkGetSwapchainImagesKHR", (PFN_vkVoidFunction)vkGetSwapchainImagesKHR },
    { "vkAcquireNextImageKHR", (PFN_vkVoidFunction)vkAcquireNextImageKHR },
    { "vkCreateImageView", (PFN_vkVoidFunction)vkCreateImageView },
    { "vkDestroyImageView", (PFN_vkVoidFunction)vkDestroyImageView },
    { "vkCreateRenderPass", (PFN_vkVoidFunction)vkCreateRenderPass },
    { "vkDestroyRenderPass", (PFN_vkVoidFunction)vkDestroyRenderPass },
    { "vkCreateFramebuffer", (PFN_vkVoidFunction)vkCreateFramebuffer },
    { "vkDestroyFramebuffer", (PFN_vkVoidFunction)vkDestroyFramebuffer },
    { "vkCreateSampler", (PFN_vkVoidFunction)vkCreateSampler },
    { "vkDestroySampler", (PFN_vkVoidFunction)vkDestroySampler },
    { "vkGetImageMemoryRequirements", (PFN_vkVoidFunction)vkGetImageMemoryRequirements },
    { "vkBindImageMemory", (PFN_vkVoidFunction)vkBindImageMemory },
    { "vkGetImageSubresourceLayout", (PFN_vkVoidFunction)vkGetImageSubresourceLayout },
    { "vkMapMemory", (PFN_vkVoidFunction)vkMapMemory },
    { "vkUnmapMemory", (PFN_vkVoidFunction)vkUnmapMemory },
    { "vkFlushMappedMemoryRanges", (PFN_vkVoidFunction)vkFlushMappedMemoryRanges },
    { "vkInvalidateMappedMemoryRanges", (PFN_vkVoidFunction)vkInvalidateMappedMemoryRanges },
    { "vkCreateShaderModule", (PFN_vkVoidFunction)vkCreateShaderModule },
    { "vkDestroyShaderModule", (PFN_vkVoidFunction)vkDestroyShaderModule },
    { "vkCreateDescriptorSetLayout", (PFN_vkVoidFunction)vkCreateDescriptorSetLayout },
    { "vkDestroyDescriptorSetLayout", (PFN_vkVoidFunction)vkDestroyDescriptorSetLayout },
    { "vkCreatePipelineLayout", (PFN_vkVoidFunction)vkCreatePipelineLayout },
    { "vkDestroyPipelineLayout", (PFN_vkVoidFunction)vkDestroyPipelineLayout },
    { "vkCreateComputePipelines", (PFN_vkVoidFunction)vkCreateComputePipelines },
    { "vkCreateGraphicsPipelines", (PFN_vkVoidFunction)vkCreateGraphicsPipelines },
    { "vkDestroyPipeline", (PFN_vkVoidFunction)vkDestroyPipeline },
    { "vkCreateDescriptorPool", (PFN_vkVoidFunction)vkCreateDescriptorPool },
    { "vkDestroyDescriptorPool", (PFN_vkVoidFunction)vkDestroyDescriptorPool },
    { "vkResetDescriptorPool", (PFN_vkVoidFunction)vkResetDescriptorPool },
    { "vkAllocateDescriptorSets", (PFN_vkVoidFunction)vkAllocateDescriptorSets },
    { "vkFreeDescriptorSets", (PFN_vkVoidFunction)vkFreeDescriptorSets },
    { "vkUpdateDescriptorSets", (PFN_vkVoidFunction)vkUpdateDescriptorSets },
    { "vkCreateCommandPool", (PFN_vkVoidFunction)vkCreateCommandPool },
    { "vkDestroyCommandPool", (PFN_vkVoidFunction)vkDestroyCommandPool },
    { "vkResetCommandPool", (PFN_vkVoidFunction)vkResetCommandPool },
    { "vkTrimCommandPoolKHR", (PFN_vkVoidFunction)vkTrimCommandPoolKHR },
    { "vkAllocateCommandBuffers", (PFN_vkVoidFunction)vkAllocateCommandBuffers },
    { "vkFreeCommandBuffers", (PFN_vkVoidFunction)vkFreeCommandBuffers },
    { "vkBeginCommandBuffer", (PFN_vkVoidFunction)vkBeginCommandBuffer },
    { "vkEndCommandBuffer", (PFN_vkVoidFunction)vkEndCommandBuffer },
    { "vkResetCommandBuffer", (PFN_vkVoidFunction)vkResetCommandBuffer },
    { "vkCmdBeginRenderPass", (PFN_vkVoidFunction)vkCmdBeginRenderPass },
    { "vkCmdEndRenderPass", (PFN_vkVoidFunction)vkCmdEndRenderPass },
    { "vkCmdCopyBuffer", (PFN_vkVoidFunction)vkCmdCopyBuffer },
    { "vkCmdCopyBufferToImage", (PFN_vkVoidFunction)vkCmdCopyBufferToImage },
    { "vkCmdCopyImageToBuffer", (PFN_vkVoidFunction)vkCmdCopyImageToBuffer },
    { "vkCmdCopyImage", (PFN_vkVoidFunction)vkCmdCopyImage },
    { "vkCmdClearColorImage", (PFN_vkVoidFunction)vkCmdClearColorImage },
    { "vkCmdUpdateBuffer", (PFN_vkVoidFunction)vkCmdUpdateBuffer },
    { "vkCmdFillBuffer", (PFN_vkVoidFunction)vkCmdFillBuffer },
    { "vkCmdBindPipeline", (PFN_vkVoidFunction)vkCmdBindPipeline },
    { "vkCmdSetViewport", (PFN_vkVoidFunction)vkCmdSetViewport },
    { "vkCmdSetScissor", (PFN_vkVoidFunction)vkCmdSetScissor },
    { "vkCmdSetBlendConstants", (PFN_vkVoidFunction)vkCmdSetBlendConstants },
    { "vkCmdBindVertexBuffers", (PFN_vkVoidFunction)vkCmdBindVertexBuffers },
    { "vkCmdBindIndexBuffer", (PFN_vkVoidFunction)vkCmdBindIndexBuffer },
    { "vkCmdDraw", (PFN_vkVoidFunction)vkCmdDraw },
    { "vkCmdDrawIndirect", (PFN_vkVoidFunction)vkCmdDrawIndirect },
    { "vkCmdDrawIndexed", (PFN_vkVoidFunction)vkCmdDrawIndexed },
    { "vkCmdDrawIndexedIndirect", (PFN_vkVoidFunction)vkCmdDrawIndexedIndirect },
    { "vkCmdBindDescriptorSets", (PFN_vkVoidFunction)vkCmdBindDescriptorSets },
    { "vkCmdPushConstants", (PFN_vkVoidFunction)vkCmdPushConstants },
    { "vkCmdDispatch", (PFN_vkVoidFunction)vkCmdDispatch },
    { "vkCmdDispatchIndirect", (PFN_vkVoidFunction)vkCmdDispatchIndirect },
    { "vkCmdPipelineBarrier", (PFN_vkVoidFunction)vkCmdPipelineBarrier },
    { "vkQueueSubmit", (PFN_vkVoidFunction)vkQueueSubmit },
    { "vkQueuePresentKHR", (PFN_vkVoidFunction)vkQueuePresentKHR },
    { "vkQueueWaitIdle", (PFN_vkVoidFunction)vkQueueWaitIdle },
    { "vkDeviceWaitIdle", (PFN_vkVoidFunction)vkDeviceWaitIdle },
    { "vkCreateFence", (PFN_vkVoidFunction)vkCreateFence },
    { "vkDestroyFence", (PFN_vkVoidFunction)vkDestroyFence },
    { "vkGetFenceStatus", (PFN_vkVoidFunction)vkGetFenceStatus },
    { "vkWaitForFences", (PFN_vkVoidFunction)vkWaitForFences },
    { "vkResetFences", (PFN_vkVoidFunction)vkResetFences },
    { "vkCreateSemaphore", (PFN_vkVoidFunction)vkCreateSemaphore },
    { "vkDestroySemaphore", (PFN_vkVoidFunction)vkDestroySemaphore },
};

PFN_vkVoidFunction WINAPI vkGetInstanceProcAddr(VkInstance instance, const char *pName) {
    if (!pName || (instance && !instance_is_valid(instance))) return NULL;
    for (size_t i = 0; i < sizeof g_vk_global_functions / sizeof g_vk_global_functions[0]; i++)
        if (!strcmp(g_vk_global_functions[i].name, pName)) return g_vk_global_functions[i].function;
    if (!instance) return NULL;
    for (size_t i = 0; i < sizeof g_vk_instance_functions / sizeof g_vk_instance_functions[0]; i++)
        if (!strcmp(g_vk_instance_functions[i].name, pName)) return g_vk_instance_functions[i].function;
    for (size_t i = 0; i < sizeof g_vk_device_functions / sizeof g_vk_device_functions[0]; i++)
        if (!strcmp(g_vk_device_functions[i].name, pName)) return g_vk_device_functions[i].function;
    return NULL;
}

static const vx_vk_dispatch g_vk_dispatch = {
    .get_instance_proc_addr = vkGetInstanceProcAddr,
    .enumerate_instance_version = vkEnumerateInstanceVersion,
    .enumerate_instance_extension_properties =
        vkEnumerateInstanceExtensionProperties,
    .enumerate_instance_layer_properties =
        vkEnumerateInstanceLayerProperties,
    .enumerate_device_extension_properties = vkEnumerateDeviceExtensionProperties,
    .enumerate_device_layer_properties = vkEnumerateDeviceLayerProperties,
    .create_instance = vkCreateInstance,
    .destroy_instance = vkDestroyInstance,
    .enumerate_physical_devices = vkEnumeratePhysicalDevices,
    .get_physical_device_features = vkGetPhysicalDeviceFeatures,
    .get_physical_device_properties = vkGetPhysicalDeviceProperties,
    .get_physical_device_queue_family_properties = vkGetPhysicalDeviceQueueFamilyProperties,
    .get_physical_device_memory_properties = vkGetPhysicalDeviceMemoryProperties,
    .get_physical_device_format_properties = vkGetPhysicalDeviceFormatProperties,
    .get_physical_device_image_format_properties = vkGetPhysicalDeviceImageFormatProperties,
    .create_device = vkCreateDevice,
    .get_device_proc_addr = vkGetDeviceProcAddr,
};

// vulkan-1.dll exports every command the frontend implements, as the Khronos loader does for core 1.0 and its WSI
// commands: programs linked against vulkan-1.lib import vkCreateDevice, vkCmdDraw and the rest by name instead of
// asking vkGetInstanceProcAddr. The same tables answer both ways, so the two never disagree.
void vx_vulkan_init(void) {
    enum {
        GLOBAL = sizeof g_vk_global_functions / sizeof g_vk_global_functions[0],
        INSTANCE = sizeof g_vk_instance_functions / sizeof g_vk_instance_functions[0],
        DEVICE = sizeof g_vk_device_functions / sizeof g_vk_device_functions[0],
    };
    static vx_export exports[GLOBAL + INSTANCE + DEVICE];
    size_t n = 0;
    for (size_t i = 0; i < GLOBAL; i++)
        exports[n++] = (vx_export){ g_vk_global_functions[i].name, (void *)g_vk_global_functions[i].function };
    for (size_t i = 0; i < INSTANCE; i++)
        exports[n++] = (vx_export){ g_vk_instance_functions[i].name, (void *)g_vk_instance_functions[i].function };
    for (size_t i = 0; i < DEVICE; i++) {
        int seen = 0;   // (vkGetDeviceProcAddr and a few more are in both tables)
        for (size_t j = 0; j < n && !seen; j++) seen = !strcmp(exports[j].name, g_vk_device_functions[i].name);
        if (!seen) exports[n++] = (vx_export){ g_vk_device_functions[i].name, (void *)g_vk_device_functions[i].function };
    }
    vx_register_module("vulkan-1.dll", exports, n);
}

VkResult WINAPI vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *pPhysicalDeviceCount,
                                             VkPhysicalDevice *pPhysicalDevices) {
    if (!instance_is_valid(instance) || !pPhysicalDeviceCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!physical_device_is_valid(&instance->physical_device)) {
        *pPhysicalDeviceCount = 0;
        return VK_SUCCESS;
    }
    if (!pPhysicalDevices) {
        *pPhysicalDeviceCount = 1;
        return VK_SUCCESS;
    }
    if (*pPhysicalDeviceCount == 0) return VK_INCOMPLETE;
    pPhysicalDevices[0] = &instance->physical_device;
    *pPhysicalDeviceCount = 1;
    return VK_SUCCESS;
}

void WINAPI vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures *pFeatures) {
    if (!physical_device_is_valid(physicalDevice) || !pFeatures) return;
    memset(pFeatures, 0, sizeof *pFeatures);
    // Robust buffer access is enforced in the supported SPIR-V uint storage-buffer lowering by
    // checking each descriptor-relative load and store against the bound VkDescriptorBufferInfo range.
    pFeatures->robustBufferAccess = 1;
    // Metal's indexed-draw path supports the full uint32 index range. Primitive restart is not
    // exposed by the current graphics-pipeline subset.
    pFeatures->fullDrawIndexUint32 = 1;
    // Vulkan's indirect argument structures match Metal's native indirect draw layouts.
    pFeatures->multiDrawIndirect = 1;
    pFeatures->drawIndirectFirstInstance = 1;
    // Metal samplers expose anisotropic filtering with a documented maximum of 16 samples.
    pFeatures->samplerAnisotropy = physicalDevice->max_sampler_anisotropy > 1;
    // Fixed-size sampler, sampled-image and combined-image-sampler arrays use dynamically uniform indexing.
    pFeatures->shaderSampledImageArrayDynamicIndexing = 1;
    // Fixed-size storage-buffer descriptor arrays support dynamically uniform 32-bit indexing in the MSL emitter.
    pFeatures->shaderStorageBufferArrayDynamicIndexing = 1;
    // Fixed-size storage-image descriptor arrays use dynamically uniform 32-bit indexing on Metal.
    pFeatures->shaderStorageImageArrayDynamicIndexing = physicalDevice->storage_image_supported;
}

static int vk_format_to_gpu(VkFormat format, uint32_t *gpu_format, uint32_t *bytes_per_texel) {
    if (!gpu_format || !bytes_per_texel) return 0;
    switch (format) {
        case VK_FORMAT_R8_UNORM:
            *gpu_format = GPU_IR_FORMAT_R8_UNORM; *bytes_per_texel = 1; return 1;
        case VK_FORMAT_R8G8_UNORM:
            *gpu_format = GPU_IR_FORMAT_RG8_UNORM; *bytes_per_texel = 2; return 1;
        case VK_FORMAT_R8G8B8A8_UNORM:
            *gpu_format = GPU_IR_FORMAT_RGBA8_UNORM; *bytes_per_texel = 4; return 1;
        case VK_FORMAT_B8G8R8A8_UNORM:
            *gpu_format = GPU_IR_FORMAT_BGRA8_UNORM; *bytes_per_texel = 4; return 1;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            *gpu_format = GPU_IR_FORMAT_R16G16B16A16_FLOAT; *bytes_per_texel = 8; return 1;
        case VK_FORMAT_R32_UINT:
            *gpu_format = GPU_IR_FORMAT_R32_UINT; *bytes_per_texel = 4; return 1;
        case VK_FORMAT_R32_SINT:
            *gpu_format = GPU_IR_FORMAT_R32_SINT; *bytes_per_texel = 4; return 1;
        case VK_FORMAT_R32_SFLOAT:
            *gpu_format = GPU_IR_FORMAT_R32_FLOAT; *bytes_per_texel = 4; return 1;
        default: return 0;
    }
}

static VkFormatFeatureFlags vk_image_format_features(VkPhysicalDevice physicalDevice, VkFormat format,
                                                       VkImageTiling tiling) {
    uint32_t gpu_format = 0, bytes = 0;
    if (!physical_device_is_valid(physicalDevice) ||
        (tiling != VK_IMAGE_TILING_LINEAR && tiling != VK_IMAGE_TILING_OPTIMAL) ||
        !vk_format_to_gpu(format, &gpu_format, &bytes) ||
        !gpu_ir_metal_linear_texture_alignment(physicalDevice->owner->metal, gpu_format)) return 0;
    (void)bytes;
    VkFormatFeatureFlags features = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT_KHR |
                                   VK_FORMAT_FEATURE_TRANSFER_DST_BIT_KHR;
    if (tiling == VK_IMAGE_TILING_OPTIMAL &&
        (format == VK_FORMAT_R8_UNORM || format == VK_FORMAT_R8G8_UNORM ||
         format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM ||
         format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R32_SFLOAT)) {
        features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if (format == VK_FORMAT_R8G8B8A8_UNORM && physicalDevice->storage_image_supported)
            features |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    }
    if (tiling == VK_IMAGE_TILING_OPTIMAL) {
        gpu_ir_clear_color_value_type clear_type = format == VK_FORMAT_R32_UINT ?
            GPU_IR_CLEAR_COLOR_VALUE_UINT : format == VK_FORMAT_R32_SINT ?
            GPU_IR_CLEAR_COLOR_VALUE_SINT : GPU_IR_CLEAR_COLOR_VALUE_FLOAT;
        if (gpu_ir_metal_color_attachment_supported(physicalDevice->owner->metal, gpu_format, clear_type))
            features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    }
    return features;
}

static int vk_image_usage_supported(VkPhysicalDevice physicalDevice, VkFormat format, VkImageTiling tiling,
                                    VkImageUsageFlags usage, int maintenance1_enabled) {
    VkFormatFeatureFlags features = vk_image_format_features(physicalDevice, format, tiling);
    if (!features) return 0;
    if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) && !(features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return 0;
    if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) && !(features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return 0;
    if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
        !(features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return 0;
    VkFormatFeatureFlags ordinary_features = features &
        ~(VK_FORMAT_FEATURE_TRANSFER_SRC_BIT_KHR | VK_FORMAT_FEATURE_TRANSFER_DST_BIT_KHR);
    if (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
        if (maintenance1_enabled) {
            if (!(features & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT_KHR)) return 0;
        } else if (!ordinary_features) return 0;
    }
    if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
        if (maintenance1_enabled) {
            if (!(features & VK_FORMAT_FEATURE_TRANSFER_DST_BIT_KHR)) return 0;
        } else if (!ordinary_features) return 0;
    }
    return 1;
}

static vx_vk_surface *surface_from_handle(VkSurfaceKHR handle) {
    vx_vk_surface *surface = (vx_vk_surface *)(uintptr_t)handle;
    return surface && surface->magic == VX_VK_SURFACE_MAGIC && instance_is_valid(surface->instance) ? surface : NULL;
}

static vx_vk_swapchain *swapchain_from_handle(VkSwapchainKHR handle, VkDevice device) {
    vx_vk_swapchain *swapchain = (vx_vk_swapchain *)(uintptr_t)handle;
    return swapchain && swapchain->magic == VX_VK_SWAPCHAIN_MAGIC && swapchain->externally_alive &&
        swapchain->device == device ? swapchain : NULL;
}

static VkExtent2D surface_extent_from_window(const vx_window *window) {
    uint32_t width = window && window->client_w > 0 ? (uint32_t)window->client_w : 0;
    uint32_t height = window && window->client_h > 0 ? (uint32_t)window->client_h : 0;
    if (width > 4096) width = 4096;
    if (height > 4096) height = 4096;
    return (VkExtent2D){ width, height };
}

VkResult WINAPI vkCreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR *pCreateInfo,
                                         const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface) {
    if (!pSurface) return VK_ERROR_INITIALIZATION_FAILED;
    *pSurface = 0;
    if (!instance_is_valid(instance)) return VK_ERROR_INITIALIZATION_FAILED;
    if (!instance->surface_enabled || !instance->win32_surface_enabled) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (!pCreateInfo || pCreateInfo->sType != VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR ||
        pCreateInfo->pNext || pCreateInfo->flags || !pCreateInfo->hinstance || !pCreateInfo->hwnd ||
        !allocator_is_valid(pAllocator)) return VK_ERROR_INITIALIZATION_FAILED;
    vx_window *window = vx_window_get(pCreateInfo->hwnd);
    if (!window || window->destroyed || !window->native) return VK_ERROR_INITIALIZATION_FAILED;
    vx_vk_surface *surface = object_allocate(pAllocator, sizeof *surface, alignof(vx_vk_surface),
                                              VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!surface) return VK_ERROR_OUT_OF_HOST_MEMORY;
    surface->magic = VX_VK_SURFACE_MAGIC;
    surface->instance = instance;
    surface->hwnd = pCreateInfo->hwnd;
    surface->has_allocator = pAllocator != NULL;
    if (pAllocator) surface->allocator = *pAllocator;
    *pSurface = (VkSurfaceKHR)(uintptr_t)surface;
    return VK_SUCCESS;
}

void WINAPI vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surfaceHandle,
                                 const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    vx_vk_surface *surface = surface_from_handle(surfaceHandle);
    if (!instance_is_valid(instance) || !instance->surface_enabled || !surface || surface->instance != instance ||
        surface->swapchain_count) return;
    VkAllocationCallbacks allocator = surface->allocator;
    int has_allocator = surface->has_allocator;
    surface->magic = 0;
    object_free(has_allocator ? &allocator : NULL, surface);
}

static int surface_is_supported_by_device(VkPhysicalDevice physicalDevice, vx_vk_surface *surface) {
    return physical_device_is_valid(physicalDevice) && surface &&
        surface->instance == physicalDevice->owner && surface->instance->surface_enabled &&
        vx_window_get(surface->hwnd) != NULL;
}

VkResult WINAPI vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
                                                       VkSurfaceKHR surfaceHandle, VkBool32 *pSupported) {
    vx_vk_surface *surface = surface_from_handle(surfaceHandle);
    if (!surface_is_supported_by_device(physicalDevice, surface) || !pSupported)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (queueFamilyIndex != VX_VK_QUEUE_FAMILY) return VK_ERROR_INITIALIZATION_FAILED;
    *pSupported = 1;
    return VK_SUCCESS;
}

VkBool32 WINAPI vkGetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice physicalDevice,
                                                                 uint32_t queueFamilyIndex) {
    return physical_device_is_valid(physicalDevice) && queueFamilyIndex == VX_VK_QUEUE_FAMILY &&
        physicalDevice->owner->win32_surface_enabled;
}

VkResult WINAPI vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surfaceHandle, VkSurfaceCapabilitiesKHR *pSurfaceCapabilities) {
    vx_vk_surface *surface = surface_from_handle(surfaceHandle);
    if (!surface_is_supported_by_device(physicalDevice, surface) || !pSurfaceCapabilities)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!vk_image_usage_supported(physicalDevice, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 1))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    vx_window *window = vx_window_get(surface->hwnd);
    if (!window) return VK_ERROR_SURFACE_LOST_KHR;
    VkExtent2D extent = surface_extent_from_window(window);
    *pSurfaceCapabilities = (VkSurfaceCapabilitiesKHR){
        .minImageCount = 2,
        .maxImageCount = 3,
        .currentExtent = extent,
        .minImageExtent = { 1, 1 },
        .maxImageExtent = { 4096, 4096 },
        .maxImageArrayLayers = 1,
        .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
    };
    return VK_SUCCESS;
}

VkResult WINAPI vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surfaceHandle,
    uint32_t *pSurfaceFormatCount, VkSurfaceFormatKHR *pSurfaceFormats) {
    vx_vk_surface *surface = surface_from_handle(surfaceHandle);
    if (!surface_is_supported_by_device(physicalDevice, surface) || !pSurfaceFormatCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!vk_image_usage_supported(physicalDevice, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 1)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (!pSurfaceFormats) {
        *pSurfaceFormatCount = 1;
        return VK_SUCCESS;
    }
    if (*pSurfaceFormatCount == 0) return VK_INCOMPLETE;
    pSurfaceFormats[0] = (VkSurfaceFormatKHR){ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    *pSurfaceFormatCount = 1;
    return VK_SUCCESS;
}

VkResult WINAPI vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surfaceHandle, uint32_t *pPresentModeCount, VkPresentModeKHR *pPresentModes) {
    vx_vk_surface *surface = surface_from_handle(surfaceHandle);
    if (!surface_is_supported_by_device(physicalDevice, surface) || !pPresentModeCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!pPresentModes) {
        *pPresentModeCount = 1;
        return VK_SUCCESS;
    }
    if (*pPresentModeCount == 0) return VK_INCOMPLETE;
    pPresentModes[0] = VK_PRESENT_MODE_FIFO_KHR;
    *pPresentModeCount = 1;
    return VK_SUCCESS;
}

static void swapchain_release(vx_vk_swapchain *swapchain) {
    if (!swapchain) return;
    VkAllocationCallbacks allocator = swapchain->allocator;
    VkAllocationCallbacks *allocator_ptr = swapchain->has_allocator ? &allocator : NULL;
    if (swapchain->surface) {
        if (swapchain->surface->active_swapchain == swapchain)
            swapchain->surface->active_swapchain = NULL;
        if (swapchain->surface_registered && swapchain->surface->swapchain_count)
            swapchain->surface->swapchain_count--;
    }
    for (uint32_t i = 0; i < swapchain->image_count; i++) {
        vx_vk_image *image = swapchain->images ? swapchain->images[i] : NULL;
        if (!image) continue;
        image->externally_alive = 0;
        image->swapchain = NULL;
        image_release(image);
    }
    object_free(allocator_ptr, swapchain->acquired);
    object_free(allocator_ptr, swapchain->images);
    object_free(allocator_ptr, swapchain->image_handles);
    swapchain->magic = 0;
    swapchain->externally_alive = 0;
    object_free(allocator_ptr, swapchain);
}

VkResult WINAPI vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain) {
    if (!pSwapchain) return VK_ERROR_INITIALIZATION_FAILED;
    *pSwapchain = 0;
    if (!device_is_valid(device)) return VK_ERROR_INITIALIZATION_FAILED;
    if (!device->swapchain_enabled) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (!pCreateInfo || pCreateInfo->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR ||
        pCreateInfo->pNext || pCreateInfo->flags || !allocator_is_valid(pAllocator) ||
        (pCreateInfo->clipped != 0 && pCreateInfo->clipped != 1))
        return VK_ERROR_INITIALIZATION_FAILED;

    vx_vk_surface *surface = surface_from_handle(pCreateInfo->surface);
    if (!surface_is_supported_by_device(device->physical_device, surface)) return VK_ERROR_SURFACE_LOST_KHR;
    vx_vk_swapchain *old_swapchain = NULL;
    if (pCreateInfo->oldSwapchain) {
        old_swapchain = swapchain_from_handle(pCreateInfo->oldSwapchain, device);
        if (!old_swapchain || old_swapchain->surface != surface || old_swapchain->retired ||
            surface->active_swapchain != old_swapchain) return VK_ERROR_INITIALIZATION_FAILED;
        old_swapchain->retired = 1;
    } else if (surface->swapchain_count) {
        return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
    }
    VkSurfaceCapabilitiesKHR capabilities = { 0 };
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->physical_device,
        pCreateInfo->surface, &capabilities);
    if (result != VK_SUCCESS) return result;
    if (pCreateInfo->minImageCount < capabilities.minImageCount ||
        pCreateInfo->minImageCount > capabilities.maxImageCount ||
        pCreateInfo->imageFormat != VK_FORMAT_B8G8R8A8_UNORM ||
        pCreateInfo->imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ||
        pCreateInfo->imageExtent.width != capabilities.currentExtent.width ||
        pCreateInfo->imageExtent.height != capabilities.currentExtent.height ||
        !pCreateInfo->imageExtent.width || !pCreateInfo->imageExtent.height ||
        pCreateInfo->imageArrayLayers != 1 || !pCreateInfo->imageUsage ||
        (pCreateInfo->imageUsage & ~capabilities.supportedUsageFlags) ||
        pCreateInfo->imageSharingMode != VK_SHARING_MODE_EXCLUSIVE ||
        pCreateInfo->queueFamilyIndexCount || pCreateInfo->pQueueFamilyIndices ||
        pCreateInfo->preTransform != capabilities.currentTransform ||
        pCreateInfo->compositeAlpha != VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ||
        pCreateInfo->presentMode != VK_PRESENT_MODE_FIFO_KHR)
        return VK_ERROR_INITIALIZATION_FAILED;

    uint32_t image_count = pCreateInfo->minImageCount;
    uint64_t row_pitch = gpu_ir_metal_linear_texture_row_pitch(device->physical_device->owner->metal,
        GPU_IR_FORMAT_BGRA8_UNORM, pCreateInfo->imageExtent.width);
    uint64_t alignment = gpu_ir_metal_linear_texture_alignment(device->physical_device->owner->metal,
        GPU_IR_FORMAT_BGRA8_UNORM);
    if (!row_pitch || !alignment || row_pitch > UINT64_MAX / pCreateInfo->imageExtent.height)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    uint64_t image_size = row_pitch * pCreateInfo->imageExtent.height;

    vx_vk_swapchain *swapchain = object_allocate(pAllocator, sizeof *swapchain, alignof(vx_vk_swapchain),
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!swapchain) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(swapchain, 0, sizeof *swapchain);
    swapchain->magic = VX_VK_SWAPCHAIN_MAGIC;
    swapchain->device = device;
    swapchain->surface = surface;
    swapchain->externally_alive = 1;
    swapchain->image_count = image_count;
    swapchain->width = pCreateInfo->imageExtent.width;
    swapchain->height = pCreateInfo->imageExtent.height;
    swapchain->format = pCreateInfo->imageFormat;
    if (pAllocator) {
        swapchain->allocator = *pAllocator;
        swapchain->has_allocator = 1;
    }
    VkAllocationCallbacks *allocator = swapchain->has_allocator ? &swapchain->allocator : NULL;
    swapchain->image_handles = object_allocate(allocator, image_count * sizeof *swapchain->image_handles,
        alignof(VkImage), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    swapchain->images = object_allocate(allocator, image_count * sizeof *swapchain->images,
        alignof(vx_vk_image *), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    swapchain->acquired = object_allocate(allocator, image_count * sizeof *swapchain->acquired,
        alignof(uint8_t), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!swapchain->image_handles || !swapchain->images || !swapchain->acquired) {
        swapchain_release(swapchain);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memset(swapchain->image_handles, 0, image_count * sizeof *swapchain->image_handles);
    memset(swapchain->images, 0, image_count * sizeof *swapchain->images);
    memset(swapchain->acquired, 0, image_count * sizeof *swapchain->acquired);

    gpu_ir_texture_desc description = {
        .width = swapchain->width, .height = swapchain->height, .depth = 1,
        .array_layers = 1, .mip_levels = 1, .bytes_per_texel = 4,
        .sample_count = 1, .is_3d = 0, .block_width = 1, .block_height = 1,
        .bytes_per_block = 4, .format = GPU_IR_FORMAT_BGRA8_UNORM
    };
    for (uint32_t i = 0; i < image_count; i++) {
        vx_vk_image *image = object_allocate(allocator, sizeof *image, alignof(vx_vk_image),
            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
        if (!image) {
            swapchain_release(swapchain);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        memset(image, 0, sizeof *image);
        image->magic = VX_VK_IMAGE_MAGIC;
        image->device = device;
        image->refs = 1;
        image->externally_alive = 1;
        image->swapchain_owned = 1;
        image->bound = 1;
        image->format = swapchain->format;
        image->usage = pCreateInfo->imageUsage;
        image->width = swapchain->width;
        image->height = swapchain->height;
        image->array_layers = 1;
        image->bytes_per_texel = 4;
        image->gpu_format = GPU_IR_FORMAT_BGRA8_UNORM;
        image->tiling = VK_IMAGE_TILING_OPTIMAL;
        image->row_pitch = row_pitch;
        image->array_pitch = image_size;
        image->size = image_size;
        image->alignment = alignment;
        image->swapchain = swapchain;
        image->swapchain_index = i;
        if (allocator) {
            image->allocator = *allocator;
            image->has_allocator = 1;
        }
        image->backing_resource = gpu_ir_metal_buffer_create(device->physical_device->owner->metal, image_size, NULL);
        image->resource = image->backing_resource ? gpu_ir_metal_texture_create_from_buffer(
            device->physical_device->owner->metal, image->backing_resource, 0, row_pitch, &description, 1, 0) : NULL;
        if (!image->backing_resource || !image->resource) {
            image_release(image);
            swapchain_release(swapchain);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        swapchain->images[i] = image;
        swapchain->image_handles[i] = image_to_handle(image);
    }

    surface->active_swapchain = swapchain;
    surface->swapchain_count++;
    swapchain->surface_registered = 1;
    *pSwapchain = swapchain_to_handle(swapchain);
    return VK_SUCCESS;
}

void WINAPI vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchainHandle,
    const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    vx_vk_swapchain *swapchain = swapchain_from_handle(swapchainHandle, device);
    if (!swapchain) return;
    pthread_mutex_lock(&device->sync_mutex);
    swapchain->externally_alive = 0;
    pthread_cond_broadcast(&device->sync_condition);
    pthread_mutex_unlock(&device->sync_mutex);
    swapchain_release(swapchain);
}

VkResult WINAPI vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchainHandle,
    uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages) {
    if (!device_is_valid(device)) return VK_ERROR_INITIALIZATION_FAILED;
    if (!device->swapchain_enabled) return VK_ERROR_EXTENSION_NOT_PRESENT;
    vx_vk_swapchain *swapchain = swapchain_from_handle(swapchainHandle, device);
    if (!swapchain || !pSwapchainImageCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pSwapchainImages) {
        *pSwapchainImageCount = swapchain->image_count;
        return VK_SUCCESS;
    }
    uint32_t count = *pSwapchainImageCount < swapchain->image_count ?
        *pSwapchainImageCount : swapchain->image_count;
    memcpy(pSwapchainImages, swapchain->image_handles, (size_t)count * sizeof *pSwapchainImages);
    *pSwapchainImageCount = count;
    return count < swapchain->image_count ? VK_INCOMPLETE : VK_SUCCESS;
}

static void swapchain_acquire_deadline(uint64_t timeout, struct timespec *deadline) {
    clock_gettime(CLOCK_REALTIME, deadline);
    uint64_t seconds = timeout / 1000000000ull;
    uint64_t nanoseconds = timeout % 1000000000ull;
    if (seconds > (uint64_t)INT64_MAX - (uint64_t)deadline->tv_sec) seconds =
        (uint64_t)INT64_MAX - (uint64_t)deadline->tv_sec;
    deadline->tv_sec += (time_t)seconds;
    deadline->tv_nsec += (long)nanoseconds;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_nsec -= 1000000000L;
        if (deadline->tv_sec < INT64_MAX) deadline->tv_sec++;
    }
}

VkResult WINAPI vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchainHandle, uint64_t timeout,
    VkSemaphore semaphoreHandle, VkFence fenceHandle, uint32_t *pImageIndex) {
    if (!device_is_valid(device) || !device->swapchain_enabled) return VK_ERROR_EXTENSION_NOT_PRESENT;
    vx_vk_swapchain *swapchain = swapchain_from_handle(swapchainHandle, device);
    vx_vk_semaphore *semaphore = semaphoreHandle ? semaphore_from_handle(semaphoreHandle, device) : NULL;
    vx_vk_fence *fence = fenceHandle ? fence_from_handle(fenceHandle, device) : NULL;
    if (!swapchain || !pImageIndex || (semaphoreHandle && !semaphore) || (fenceHandle && !fence) ||
        (!semaphoreHandle && !fenceHandle)) return VK_ERROR_INITIALIZATION_FAILED;
    struct timespec deadline = { 0 };
    if (timeout != UINT64_MAX && timeout) swapchain_acquire_deadline(timeout, &deadline);

    pthread_mutex_lock(&device->sync_mutex);
    if ((semaphore && semaphore->signaled) || (fence && fence->signaled)) {
        pthread_mutex_unlock(&device->sync_mutex);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (;;) {
        if (device->lost) {
            pthread_mutex_unlock(&device->sync_mutex);
            return VK_ERROR_DEVICE_LOST;
        }
        if (!swapchain->externally_alive || swapchain->retired) {
            pthread_mutex_unlock(&device->sync_mutex);
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
        vx_window *window = vx_window_get(swapchain->surface->hwnd);
        if (!window || window->destroyed) {
            pthread_mutex_unlock(&device->sync_mutex);
            return VK_ERROR_SURFACE_LOST_KHR;
        }
        VkExtent2D extent = surface_extent_from_window(window);
        if (window->minimized || extent.width != swapchain->width || extent.height != swapchain->height) {
            pthread_mutex_unlock(&device->sync_mutex);
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
        uint32_t available = UINT32_MAX;
        for (uint32_t offset = 0; offset < swapchain->image_count; offset++) {
            uint32_t index = (swapchain->next_image + offset) % swapchain->image_count;
            if (!swapchain->acquired[index]) { available = index; break; }
        }
        if (available != UINT32_MAX) {
            swapchain->acquired[available] = 1;
            swapchain->next_image = (available + 1u) % swapchain->image_count;
            if (semaphore) semaphore->signaled = 1;
            if (fence) fence->signaled = 1;
            *pImageIndex = available;
            pthread_mutex_unlock(&device->sync_mutex);
            return VK_SUCCESS;
        }
        if (!timeout) {
            pthread_mutex_unlock(&device->sync_mutex);
            return VK_NOT_READY;
        }
        int wait_result = timeout == UINT64_MAX ? pthread_cond_wait(&device->sync_condition, &device->sync_mutex) :
            pthread_cond_timedwait(&device->sync_condition, &device->sync_mutex, &deadline);
        if (wait_result == ETIMEDOUT) {
            pthread_mutex_unlock(&device->sync_mutex);
            return VK_TIMEOUT;
        }
        if (wait_result != 0) {
            pthread_mutex_unlock(&device->sync_mutex);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
}

static gpu_ir_clear_color_value_type vk_clear_color_value_type(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R32_UINT: return GPU_IR_CLEAR_COLOR_VALUE_UINT;
        case VK_FORMAT_R32_SINT: return GPU_IR_CLEAR_COLOR_VALUE_SINT;
        default: return GPU_IR_CLEAR_COLOR_VALUE_FLOAT;
    }
}

static int vk_image_layout_to_gpu(VkImageLayout layout, uint32_t *gpu_layout) {
    if (!gpu_layout) return 0;
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED: *gpu_layout = GPU_IR_IMAGE_LAYOUT_UNDEFINED; return 1;
        case VK_IMAGE_LAYOUT_GENERAL: *gpu_layout = GPU_IR_IMAGE_LAYOUT_GENERAL; return 1;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: *gpu_layout = GPU_IR_IMAGE_LAYOUT_SHADER_READ_ONLY; return 1;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: *gpu_layout = GPU_IR_IMAGE_LAYOUT_COLOR_ATTACHMENT; return 1;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: *gpu_layout = GPU_IR_IMAGE_LAYOUT_TRANSFER_SOURCE; return 1;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: *gpu_layout = GPU_IR_IMAGE_LAYOUT_TRANSFER_DESTINATION; return 1;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: *gpu_layout = GPU_IR_IMAGE_LAYOUT_GENERAL; return 1;
        case VK_IMAGE_LAYOUT_PREINITIALIZED: *gpu_layout = GPU_IR_IMAGE_LAYOUT_PREINITIALIZED; return 1;
        default: return 0;
    }
}

static int vk_vertex_format_to_gpu(VkFormat format, uint32_t *gpu_format) {
    if (!gpu_format) return GPU_IR_INVALID;
#define VK_VERTEX_FORMAT(vk_format, gpu_ir_format) \
    case vk_format: *gpu_format = gpu_ir_format; return GPU_IR_OK
    switch (format) {
        VK_VERTEX_FORMAT(VK_FORMAT_R8_UNORM, GPU_IR_VERTEX_FORMAT_UNORM8);
        VK_VERTEX_FORMAT(VK_FORMAT_R8_SNORM, GPU_IR_VERTEX_FORMAT_SNORM8);
        VK_VERTEX_FORMAT(VK_FORMAT_R8_UINT, GPU_IR_VERTEX_FORMAT_UINT8);
        VK_VERTEX_FORMAT(VK_FORMAT_R8_SINT, GPU_IR_VERTEX_FORMAT_SINT8);
        VK_VERTEX_FORMAT(VK_FORMAT_R8G8_UNORM, GPU_IR_VERTEX_FORMAT_UNORM8X2);
        VK_VERTEX_FORMAT(VK_FORMAT_R8G8_SNORM, GPU_IR_VERTEX_FORMAT_SNORM8X2);
        VK_VERTEX_FORMAT(VK_FORMAT_R8G8_UINT, GPU_IR_VERTEX_FORMAT_UINT8X2);
        VK_VERTEX_FORMAT(VK_FORMAT_R8G8_SINT, GPU_IR_VERTEX_FORMAT_SINT8X2);
        VK_VERTEX_FORMAT(VK_FORMAT_R8G8B8_UNORM, GPU_IR_VERTEX_FORMAT_UNORM8X3);
        VK_VERTEX_FORMAT(VK_FORMAT_R8G8B8_SNORM, GPU_IR_VERTEX_FORMAT_SNORM8X3);
        VK_VERTEX_FORMAT(VK_FORMAT_R8G8B8_UINT, GPU_IR_VERTEX_FORMAT_UINT8X3);
        VK_VERTEX_FORMAT(VK_FORMAT_R8G8B8_SINT, GPU_IR_VERTEX_FORMAT_SINT8X3);
        VK_VERTEX_FORMAT(VK_FORMAT_R8G8B8A8_UNORM, GPU_IR_VERTEX_FORMAT_UNORM8X4);
        VK_VERTEX_FORMAT(VK_FORMAT_R8G8B8A8_SNORM, GPU_IR_VERTEX_FORMAT_SNORM8X4);
        VK_VERTEX_FORMAT(VK_FORMAT_R8G8B8A8_UINT, GPU_IR_VERTEX_FORMAT_UINT8X4);
        VK_VERTEX_FORMAT(VK_FORMAT_R8G8B8A8_SINT, GPU_IR_VERTEX_FORMAT_SINT8X4);
        VK_VERTEX_FORMAT(VK_FORMAT_R16_UNORM, GPU_IR_VERTEX_FORMAT_UNORM16);
        VK_VERTEX_FORMAT(VK_FORMAT_R16_SNORM, GPU_IR_VERTEX_FORMAT_SNORM16);
        VK_VERTEX_FORMAT(VK_FORMAT_R16_UINT, GPU_IR_VERTEX_FORMAT_UINT16);
        VK_VERTEX_FORMAT(VK_FORMAT_R16_SINT, GPU_IR_VERTEX_FORMAT_SINT16);
        VK_VERTEX_FORMAT(VK_FORMAT_R16_SFLOAT, GPU_IR_VERTEX_FORMAT_FLOAT16);
        VK_VERTEX_FORMAT(VK_FORMAT_R16G16_UNORM, GPU_IR_VERTEX_FORMAT_UNORM16X2);
        VK_VERTEX_FORMAT(VK_FORMAT_R16G16_SNORM, GPU_IR_VERTEX_FORMAT_SNORM16X2);
        VK_VERTEX_FORMAT(VK_FORMAT_R16G16_UINT, GPU_IR_VERTEX_FORMAT_UINT16X2);
        VK_VERTEX_FORMAT(VK_FORMAT_R16G16_SINT, GPU_IR_VERTEX_FORMAT_SINT16X2);
        VK_VERTEX_FORMAT(VK_FORMAT_R16G16_SFLOAT, GPU_IR_VERTEX_FORMAT_FLOAT16X2);
        VK_VERTEX_FORMAT(VK_FORMAT_R16G16B16_UNORM, GPU_IR_VERTEX_FORMAT_UNORM16X3);
        VK_VERTEX_FORMAT(VK_FORMAT_R16G16B16_SNORM, GPU_IR_VERTEX_FORMAT_SNORM16X3);
        VK_VERTEX_FORMAT(VK_FORMAT_R16G16B16_UINT, GPU_IR_VERTEX_FORMAT_UINT16X3);
        VK_VERTEX_FORMAT(VK_FORMAT_R16G16B16_SINT, GPU_IR_VERTEX_FORMAT_SINT16X3);
        VK_VERTEX_FORMAT(VK_FORMAT_R16G16B16_SFLOAT, GPU_IR_VERTEX_FORMAT_FLOAT16X3);
        VK_VERTEX_FORMAT(VK_FORMAT_R16G16B16A16_UNORM, GPU_IR_VERTEX_FORMAT_UNORM16X4);
        VK_VERTEX_FORMAT(VK_FORMAT_R16G16B16A16_SNORM, GPU_IR_VERTEX_FORMAT_SNORM16X4);
        VK_VERTEX_FORMAT(VK_FORMAT_R16G16B16A16_UINT, GPU_IR_VERTEX_FORMAT_UINT16X4);
        VK_VERTEX_FORMAT(VK_FORMAT_R16G16B16A16_SINT, GPU_IR_VERTEX_FORMAT_SINT16X4);
        VK_VERTEX_FORMAT(VK_FORMAT_R16G16B16A16_SFLOAT, GPU_IR_VERTEX_FORMAT_FLOAT16X4);
        VK_VERTEX_FORMAT(VK_FORMAT_R32_SFLOAT, GPU_IR_VERTEX_FORMAT_FLOAT32);
        VK_VERTEX_FORMAT(VK_FORMAT_R32_SINT, GPU_IR_VERTEX_FORMAT_SINT32);
        VK_VERTEX_FORMAT(VK_FORMAT_R32_UINT, GPU_IR_VERTEX_FORMAT_UINT32);
        VK_VERTEX_FORMAT(VK_FORMAT_R32G32_SFLOAT, GPU_IR_VERTEX_FORMAT_FLOAT32X2);
        VK_VERTEX_FORMAT(VK_FORMAT_R32G32_SINT, GPU_IR_VERTEX_FORMAT_SINT32X2);
        VK_VERTEX_FORMAT(VK_FORMAT_R32G32_UINT, GPU_IR_VERTEX_FORMAT_UINT32X2);
        VK_VERTEX_FORMAT(VK_FORMAT_R32G32B32_SFLOAT, GPU_IR_VERTEX_FORMAT_FLOAT32X3);
        VK_VERTEX_FORMAT(VK_FORMAT_R32G32B32_SINT, GPU_IR_VERTEX_FORMAT_SINT32X3);
        VK_VERTEX_FORMAT(VK_FORMAT_R32G32B32_UINT, GPU_IR_VERTEX_FORMAT_UINT32X3);
        VK_VERTEX_FORMAT(VK_FORMAT_R32G32B32A32_SFLOAT, GPU_IR_VERTEX_FORMAT_FLOAT32X4);
        VK_VERTEX_FORMAT(VK_FORMAT_R32G32B32A32_SINT, GPU_IR_VERTEX_FORMAT_SINT32X4);
        VK_VERTEX_FORMAT(VK_FORMAT_R32G32B32A32_UINT, GPU_IR_VERTEX_FORMAT_UINT32X4);
        default: return GPU_IR_UNSUPPORTED;
    }
#undef VK_VERTEX_FORMAT
}

void WINAPI vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format,
                                                 VkFormatProperties *pFormatProperties) {
    if (!pFormatProperties) return;
    memset(pFormatProperties, 0, sizeof *pFormatProperties);
    if (!physical_device_is_valid(physicalDevice)) return;
    pFormatProperties->linearTilingFeatures =
        vk_image_format_features(physicalDevice, format, VK_IMAGE_TILING_LINEAR);
    pFormatProperties->optimalTilingFeatures =
        vk_image_format_features(physicalDevice, format, VK_IMAGE_TILING_OPTIMAL);
    uint32_t vertex_format = 0;
    if (vk_vertex_format_to_gpu(format, &vertex_format) == GPU_IR_OK)
        pFormatProperties->bufferFeatures = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
}

VkResult WINAPI vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format,
    VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags,
    VkImageFormatProperties *pImageFormatProperties) {
    if (pImageFormatProperties) memset(pImageFormatProperties, 0, sizeof *pImageFormatProperties);
    uint32_t gpu_format = 0, bytes = 0;
    if (!physical_device_is_valid(physicalDevice) || !pImageFormatProperties ||
        type != VK_IMAGE_TYPE_2D ||
        (tiling != VK_IMAGE_TILING_LINEAR && tiling != VK_IMAGE_TILING_OPTIMAL) || flags || !usage ||
        !vk_format_to_gpu(format, &gpu_format, &bytes) ||
        (usage & ~(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) ||
        ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) && tiling != VK_IMAGE_TILING_OPTIMAL) ||
        ((usage & VK_IMAGE_USAGE_STORAGE_BIT) &&
         (tiling != VK_IMAGE_TILING_OPTIMAL || format != VK_FORMAT_R8G8B8A8_UNORM ||
          !physicalDevice->storage_image_supported)) ||
        ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && tiling != VK_IMAGE_TILING_OPTIMAL) ||
        !vk_image_usage_supported(physicalDevice, format, tiling, usage, 1) ||
        !gpu_ir_metal_linear_texture_alignment(physicalDevice->owner->metal, gpu_format))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    pImageFormatProperties->maxExtent = (VkExtent3D){ 4096, 4096, 1 };
    pImageFormatProperties->maxMipLevels = 1;
    // Represent transfer-only Vulkan array layers as separate 2D buffer-backed Metal textures.
    pImageFormatProperties->maxArrayLayers =
        (usage & (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) ? 1u : VX_VK_MAX_IMAGE_LAYERS;
    pImageFormatProperties->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
    pImageFormatProperties->maxResourceSize = gpu_ir_metal_max_buffer_length(physicalDevice->owner->metal);
    return VK_SUCCESS;
}

void WINAPI vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties *pProperties) {
    if (!physical_device_is_valid(physicalDevice) || !pProperties) return;
    memset(pProperties, 0, sizeof *pProperties);
    VkPhysicalDeviceLimits *limits = &pProperties->limits;
    const gpu_ir_metal_device *metal = physicalDevice->owner->metal;
    const char *name = gpu_ir_metal_device_name(metal);
    pProperties->apiVersion = VK_API_VERSION_1_0;
    pProperties->driverVersion = 1;
    pProperties->vendorID = 0;
    pProperties->deviceID = 0;
    pProperties->deviceType = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    snprintf(pProperties->deviceName, sizeof pProperties->deviceName, "VXRuntime Metal %.220s",
             name ? name : "GPU");

    // Storage images are limited to the supported 2D RGBA8 optimal-tiling path.
    limits->maxImageDimension1D = 4096;
    limits->maxImageDimension2D = 4096;
    limits->maxImageDimension3D = 256;
    limits->maxImageDimensionCube = 4096;
    limits->maxImageArrayLayers = VX_VK_MAX_IMAGE_LAYERS;
    // This frontend currently supports the Vulkan 1.0 minimum vertex-fetch range.
    limits->maxVertexInputAttributes = 16;
    limits->maxVertexInputBindings = GPU_IR_MAX_VERTEX_BUFFERS;
    limits->maxVertexInputAttributeOffset = 2047;
    limits->maxVertexInputBindingStride = 2048;
    // Reflect the supported Metal GPU family. Position consumes four vertex-output components; user locations
    // are bounded by Metal's fragment stage-in component limit and the shader frontend's float32 subset.
    uint32_t max_fragment_input_components = gpu_ir_metal_max_fragment_input_components(metal);
    if (!max_fragment_input_components) max_fragment_input_components = 60;
    limits->maxVertexOutputComponents = max_fragment_input_components + 4;
    limits->maxFragmentInputComponents = max_fragment_input_components;
    limits->maxFragmentOutputAttachments = 1;
    limits->maxFragmentCombinedOutputResources = 1;
    limits->maxViewports = 1;
    limits->maxViewportDimensions[0] = 4096;
    limits->maxViewportDimensions[1] = 4096;
    // Vulkan requires the viewport-bound range to cover [-2 * size, 2 * size - 1], where
    // size is the largest supported viewport dimension.
    limits->viewportBoundsRange[0] = -8192.0f;
    limits->viewportBoundsRange[1] = 8191.0f;
    limits->viewportSubPixelBits = 8;
    limits->subPixelPrecisionBits = 8;
    limits->maxFramebufferWidth = 4096;
    limits->maxFramebufferHeight = 4096;
    limits->maxFramebufferLayers = 1;
    limits->framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits->maxColorAttachments = 1;
    limits->maxSampleMaskWords = 1;
    limits->standardSampleLocations = 1;

    uint64_t max_buffer_length = gpu_ir_metal_max_buffer_length(metal);
    limits->maxUniformBufferRange = (uint32_t)(max_buffer_length > UINT32_MAX ? UINT32_MAX : max_buffer_length);
    limits->maxStorageBufferRange = limits->maxUniformBufferRange;
    uint64_t max_threadgroup_memory = gpu_ir_metal_max_threadgroup_memory_length(metal);
    limits->maxComputeSharedMemorySize = (uint32_t)(max_threadgroup_memory > UINT32_MAX ?
        UINT32_MAX : max_threadgroup_memory);
    limits->maxMemoryAllocationCount = 4096;
    limits->maxSamplerAllocationCount = VX_VK_MAX_SAMPLER_ALLOCATION_COUNT;
    limits->maxBoundDescriptorSets = VX_VK_MAX_DESCRIPTOR_SETS;
    limits->maxPerStageDescriptorSamplers = GPU_IR_METAL_MAX_COMPUTE_SAMPLERS;
    limits->maxPerStageDescriptorStorageBuffers = VX_VK_MAX_PER_STAGE_STORAGE_BUFFERS;
    limits->maxPerStageDescriptorSampledImages = GPU_IR_METAL_MAX_COMPUTE_SAMPLED_IMAGES;
    limits->maxPerStageDescriptorStorageImages = physicalDevice->storage_image_supported ? 4 : 0;
    limits->maxPerStageDescriptorUniformBuffers = VX_VK_MAX_PER_STAGE_UNIFORM_BUFFERS;
    // Vulkan's resource total counts buffer and image descriptors, not sampler-only descriptors.
    limits->maxPerStageResources = limits->maxPerStageDescriptorUniformBuffers +
        limits->maxPerStageDescriptorStorageBuffers + limits->maxPerStageDescriptorSampledImages +
        limits->maxPerStageDescriptorStorageImages;
    limits->maxDescriptorSetSamplers = GPU_IR_METAL_MAX_COMPUTE_SAMPLERS;
    limits->maxDescriptorSetStorageBuffers = VX_VK_MAX_PER_STAGE_STORAGE_BUFFERS;
    limits->maxDescriptorSetSampledImages = GPU_IR_METAL_MAX_COMPUTE_SAMPLED_IMAGES;
    limits->maxDescriptorSetStorageImages = limits->maxPerStageDescriptorStorageImages;
    limits->maxDescriptorSetStorageBuffersDynamic = VX_VK_MAX_STORAGE_BUFFER_DYNAMIC_DESCRIPTORS;
    limits->maxDescriptorSetUniformBuffers = VX_VK_MAX_PER_STAGE_UNIFORM_BUFFERS;
    limits->maxDescriptorSetUniformBuffersDynamic = VX_VK_MAX_UNIFORM_BUFFER_DYNAMIC_DESCRIPTORS;
    limits->maxPushConstantsSize = VX_VK_MAX_PUSH_CONSTANT_SIZE;
    limits->maxSamplerLodBias = 16.0f;
    limits->maxSamplerAnisotropy = (float)physicalDevice->max_sampler_anisotropy;
    limits->maxDrawIndexedIndexValue = UINT32_MAX;
    limits->maxDrawIndirectCount = VX_VK_MAX_DRAW_INDIRECT_COUNT;
    limits->maxComputeWorkGroupCount[0] = 65535;
    limits->maxComputeWorkGroupCount[1] = 65535;
    limits->maxComputeWorkGroupCount[2] = 65535;
    uint32_t max_group[3] = { 0 };
    gpu_ir_metal_max_threads_per_threadgroup(metal, max_group);
    for (uint32_t i = 0; i < 3; i++) limits->maxComputeWorkGroupSize[i] = max_group[i];
    // Metal exposes per-axis limits here, not the total that a given compute pipeline accepts.
    // Use Vulkan's conservative baseline instead of multiplying independent maxima.
    limits->maxComputeWorkGroupInvocations = 128;
    // Vulkan 1.0 requires at least 64-byte host-address alignment for mapped memory.
    limits->minMemoryMapAlignment = 64;
    // The only exposed memory type is host-coherent, so byte-granular cache management is sufficient.
    limits->nonCoherentAtomSize = 1;
    limits->minUniformBufferOffsetAlignment = VX_VK_BUFFER_ALIGNMENT;
    limits->minStorageBufferOffsetAlignment = VX_VK_BUFFER_ALIGNMENT;
}

void WINAPI vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
    uint32_t *pQueueFamilyPropertyCount, VkQueueFamilyProperties *pQueueFamilyProperties) {
    if (!physical_device_is_valid(physicalDevice) || !pQueueFamilyPropertyCount) return;
    if (!pQueueFamilyProperties) {
        *pQueueFamilyPropertyCount = 1;
        return;
    }
    if (!*pQueueFamilyPropertyCount) return;
    pQueueFamilyProperties[0] = (VkQueueFamilyProperties){
        .queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
        .queueCount = 1,
        .timestampValidBits = 0,
        .minImageTransferGranularity = { 1, 1, 1 }
    };
    *pQueueFamilyPropertyCount = 1;
}

void WINAPI vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties *pMemoryProperties) {
    if (!physical_device_is_valid(physicalDevice) || !pMemoryProperties) return;
    memset(pMemoryProperties, 0, sizeof *pMemoryProperties);
    uint64_t heap_size = 0;
    size_t heap_size_bytes = sizeof heap_size;
    (void)sysctlbyname("hw.memsize", &heap_size, &heap_size_bytes, NULL, 0);
    if (!heap_size) heap_size = gpu_ir_metal_recommended_working_set_size(physicalDevice->owner->metal);
    if (!heap_size) heap_size = gpu_ir_metal_max_buffer_length(physicalDevice->owner->metal);
    pMemoryProperties->memoryTypeCount = 1;
    uint32_t memory_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t heap_flags = 0;
    if (gpu_ir_metal_has_unified_memory(physicalDevice->owner->metal)) {
        memory_flags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        heap_flags |= VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    }
    pMemoryProperties->memoryTypes[0] = (VkMemoryType){
        .propertyFlags = memory_flags,
        .heapIndex = 0
    };
    pMemoryProperties->memoryHeapCount = 1;
    pMemoryProperties->memoryHeaps[0] = (VkMemoryHeap){
        .size = heap_size,
        .flags = heap_flags
    };
}

static int physical_features_supported(const VkPhysicalDeviceFeatures *features, int storage_image_supported,
                                       uint32_t max_sampler_anisotropy) {
    if (!features) return 1;
    const VkBool32 *values = (const VkBool32 *)features;
    const size_t sampled_image_array_dynamic_indexing =
        offsetof(VkPhysicalDeviceFeatures, shaderSampledImageArrayDynamicIndexing) / sizeof *values;
    const size_t full_draw_index_uint32 =
        offsetof(VkPhysicalDeviceFeatures, fullDrawIndexUint32) / sizeof *values;
    const size_t storage_buffer_array_dynamic_indexing =
        offsetof(VkPhysicalDeviceFeatures, shaderStorageBufferArrayDynamicIndexing) / sizeof *values;
    const size_t storage_image_array_dynamic_indexing =
        offsetof(VkPhysicalDeviceFeatures, shaderStorageImageArrayDynamicIndexing) / sizeof *values;
    const size_t sampler_anisotropy = offsetof(VkPhysicalDeviceFeatures, samplerAnisotropy) / sizeof *values;
    const size_t multi_draw_indirect = offsetof(VkPhysicalDeviceFeatures, multiDrawIndirect) / sizeof *values;
    const size_t draw_indirect_first_instance =
        offsetof(VkPhysicalDeviceFeatures, drawIndirectFirstInstance) / sizeof *values;
    if (values[0] > 1 || values[full_draw_index_uint32] > 1 ||
        values[sampled_image_array_dynamic_indexing] > 1 ||
        values[storage_buffer_array_dynamic_indexing] > 1 ||
        values[storage_image_array_dynamic_indexing] > 1 ||
        values[multi_draw_indirect] > 1 || values[draw_indirect_first_instance] > 1 ||
        values[sampler_anisotropy] > 1 ||
        (values[storage_image_array_dynamic_indexing] && !storage_image_supported) ||
        (values[sampler_anisotropy] && max_sampler_anisotropy <= 1)) return 0;
    for (size_t i = 1; i < sizeof *features / sizeof *values; i++)
        if (i != full_draw_index_uint32 && i != sampled_image_array_dynamic_indexing &&
            i != storage_buffer_array_dynamic_indexing &&
            i != storage_image_array_dynamic_indexing && i != sampler_anisotropy &&
            i != multi_draw_indirect && i != draw_indirect_first_instance && values[i])
            return 0;
    return 1;
}

VkResult WINAPI vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
    if (!pDevice) return VK_ERROR_INITIALIZATION_FAILED;
    *pDevice = NULL;
    if (!physical_device_is_valid(physicalDevice) || !pCreateInfo ||
        pCreateInfo->sType != VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO || pCreateInfo->pNext || pCreateInfo->flags ||
        !pCreateInfo->queueCreateInfoCount || !pCreateInfo->pQueueCreateInfos ||
        (pCreateInfo->enabledLayerCount && !pCreateInfo->ppEnabledLayerNames) ||
        (pCreateInfo->enabledExtensionCount && !pCreateInfo->ppEnabledExtensionNames) ||
        !allocator_is_valid(pAllocator)) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < pCreateInfo->enabledLayerCount; i++) {
        if (!pCreateInfo->ppEnabledLayerNames[i]) return VK_ERROR_INITIALIZATION_FAILED;
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    int maintenance1_enabled = 0, swapchain_enabled = 0;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        if (!pCreateInfo->ppEnabledExtensionNames[i]) return VK_ERROR_INITIALIZATION_FAILED;
        const char *name = pCreateInfo->ppEnabledExtensionNames[i];
        for (uint32_t j = 0; j < i; j++)
            if (!strcmp(pCreateInfo->ppEnabledExtensionNames[j], name))
                return VK_ERROR_INITIALIZATION_FAILED;
        if (!strcmp(name, VK_KHR_MAINTENANCE1_EXTENSION_NAME)) maintenance1_enabled = 1;
        else if (!strcmp(name, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) swapchain_enabled = 1;
        else return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    if (swapchain_enabled && (!physicalDevice->owner->surface_enabled ||
                              !physicalDevice->owner->win32_surface_enabled))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (!physical_features_supported(pCreateInfo->pEnabledFeatures, physicalDevice->storage_image_supported,
                                     physicalDevice->max_sampler_anisotropy))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (pCreateInfo->queueCreateInfoCount != 1) return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkDeviceQueueCreateInfo *queue = &pCreateInfo->pQueueCreateInfos[0];
    if (queue->sType != VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO || queue->pNext || queue->flags ||
        queue->queueFamilyIndex != VX_VK_QUEUE_FAMILY || queue->queueCount != 1 || !queue->pQueuePriorities ||
        !isfinite(queue->pQueuePriorities[0]) || queue->pQueuePriorities[0] < 0.0f ||
        queue->pQueuePriorities[0] > 1.0f) return VK_ERROR_INITIALIZATION_FAILED;

    VkDevice device = object_allocate(pAllocator, sizeof *device, alignof(struct VkDevice_T),
                                      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    if (!device) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (pthread_mutex_init(&device->sync_mutex, NULL) != 0) {
        object_free(pAllocator, device);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pthread_cond_init(&device->sync_condition, NULL) != 0) {
        pthread_mutex_destroy(&device->sync_mutex);
        object_free(pAllocator, device);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    device->dispatch = &g_vk_dispatch;
    device->magic = VX_VK_DEVICE_MAGIC;
    device->physical_device = physicalDevice;
    device->shader_sampled_image_array_dynamic_indexing = pCreateInfo->pEnabledFeatures &&
        pCreateInfo->pEnabledFeatures->shaderSampledImageArrayDynamicIndexing;
    device->shader_storage_buffer_array_dynamic_indexing = pCreateInfo->pEnabledFeatures &&
        pCreateInfo->pEnabledFeatures->shaderStorageBufferArrayDynamicIndexing;
    device->shader_storage_image_array_dynamic_indexing = pCreateInfo->pEnabledFeatures &&
        pCreateInfo->pEnabledFeatures->shaderStorageImageArrayDynamicIndexing;
    device->sampler_anisotropy_enabled = pCreateInfo->pEnabledFeatures &&
        pCreateInfo->pEnabledFeatures->samplerAnisotropy;
    device->multi_draw_indirect_enabled = pCreateInfo->pEnabledFeatures &&
        pCreateInfo->pEnabledFeatures->multiDrawIndirect;
    device->maintenance1_enabled = maintenance1_enabled;
    device->swapchain_enabled = swapchain_enabled;
    if (pAllocator) {
        device->allocator = *pAllocator;
        device->has_allocator = 1;
    }
    device->queue = (struct VkQueue_T){
        .dispatch = &g_vk_dispatch,
        .magic = VX_VK_QUEUE_MAGIC,
        .device = device,
        .family_index = VX_VK_QUEUE_FAMILY
    };
    *pDevice = device;
    return VK_SUCCESS;
}

void WINAPI vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device_is_valid(device)) return;
    VkAllocationCallbacks allocator = device->allocator;
    int has_allocator = device->has_allocator;
    pthread_cond_destroy(&device->sync_condition);
    pthread_mutex_destroy(&device->sync_mutex);
    device->magic = 0;
    object_free(has_allocator ? &allocator : NULL, device);
}

PFN_vkVoidFunction WINAPI vkGetDeviceProcAddr(VkDevice device, const char *pName) {
    if (!device_is_valid(device) || !pName) return NULL;
    if (!strcmp(pName, "vkTrimCommandPoolKHR") && !device->maintenance1_enabled) return NULL;
    if ((!strcmp(pName, "vkCreateSwapchainKHR") || !strcmp(pName, "vkDestroySwapchainKHR") ||
         !strcmp(pName, "vkGetSwapchainImagesKHR") || !strcmp(pName, "vkAcquireNextImageKHR") ||
         !strcmp(pName, "vkQueuePresentKHR")) && !device->swapchain_enabled) return NULL;
    for (size_t i = 0; i < sizeof g_vk_device_functions / sizeof g_vk_device_functions[0]; i++)
        if (!strcmp(g_vk_device_functions[i].name, pName)) return g_vk_device_functions[i].function;
    return NULL;
}

void WINAPI vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue) {
    if (!pQueue) return;
    *pQueue = NULL;
    if (!device_is_valid(device) || queueFamilyIndex != VX_VK_QUEUE_FAMILY || queueIndex != 0) return;
    *pQueue = &device->queue;
}

VkResult WINAPI vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer) {
    if (!pBuffer) return VK_ERROR_INITIALIZATION_FAILED;
    *pBuffer = 0;
    const uint32_t known_usage = (1u << 9) - 1u;
    if (!device_is_valid(device) || !pCreateInfo || pCreateInfo->sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO ||
        pCreateInfo->pNext || pCreateInfo->flags || !pCreateInfo->size || !pCreateInfo->usage ||
        (pCreateInfo->usage & ~known_usage) || pCreateInfo->sharingMode != VK_SHARING_MODE_EXCLUSIVE ||
        pCreateInfo->queueFamilyIndexCount || !allocator_is_valid(pAllocator))
        return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t max_size = gpu_ir_metal_max_buffer_length(device->physical_device->owner->metal);
    if (pCreateInfo->size > max_size || pCreateInfo->size > SIZE_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    vx_vk_buffer *buffer = object_allocate(pAllocator, sizeof *buffer, alignof(vx_vk_buffer),
                                           VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!buffer) return VK_ERROR_OUT_OF_HOST_MEMORY;
    buffer->magic = VX_VK_BUFFER_MAGIC;
    buffer->device = device;
    buffer->refs = 1;
    buffer->externally_alive = 1;
    buffer->usage = pCreateInfo->usage;
    buffer->size = pCreateInfo->size;
    if (pAllocator) {
        buffer->allocator = *pAllocator;
        buffer->has_allocator = 1;
    }
    *pBuffer = buffer_to_handle(buffer);
    return VK_SUCCESS;
}

void WINAPI vkDestroyBuffer(VkDevice device, VkBuffer bufferHandle, const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    vx_vk_buffer *buffer = buffer_from_handle(bufferHandle, device);
    if (!buffer) return;
    buffer->externally_alive = 0;
    for (vx_vk_buffer_use *use = buffer->command_buffer_uses; use; use = use->next)
        if (use->command_buffer && use->command_buffer->state != VX_VK_COMMAND_PENDING)
            use->command_buffer->state = VX_VK_COMMAND_INVALID;
    if (buffer->memory && buffer->memory->bound_buffers) buffer->memory->bound_buffers--;
    buffer->memory = NULL;
    buffer->memory_offset = 0;
    buffer->bound = 0;
    buffer_release(buffer);
}

void WINAPI vkGetBufferMemoryRequirements(VkDevice device, VkBuffer bufferHandle,
                                           VkMemoryRequirements *pMemoryRequirements) {
    if (!pMemoryRequirements) return;
    memset(pMemoryRequirements, 0, sizeof *pMemoryRequirements);
    vx_vk_buffer *buffer = buffer_from_handle(bufferHandle, device);
    if (!buffer) return;
    pMemoryRequirements->size = buffer->size;
    pMemoryRequirements->alignment = VX_VK_BUFFER_ALIGNMENT;
    pMemoryRequirements->memoryTypeBits = 1;
}

VkResult WINAPI vkCreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkImage *pImage) {
    if (!pImage) return VK_ERROR_INITIALIZATION_FAILED;
    *pImage = 0;
    uint32_t gpu_format = 0, bytes_per_texel = 0;
    if (!device_is_valid(device) || !pCreateInfo || pCreateInfo->sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO ||
        pCreateInfo->pNext || pCreateInfo->flags || pCreateInfo->imageType != VK_IMAGE_TYPE_2D ||
        (pCreateInfo->tiling != VK_IMAGE_TILING_LINEAR && pCreateInfo->tiling != VK_IMAGE_TILING_OPTIMAL) ||
        !pCreateInfo->extent.width ||
        !pCreateInfo->extent.height || pCreateInfo->extent.depth != 1 || pCreateInfo->mipLevels != 1 ||
        !pCreateInfo->arrayLayers || pCreateInfo->arrayLayers > VX_VK_MAX_IMAGE_LAYERS ||
        pCreateInfo->samples != VK_SAMPLE_COUNT_1_BIT || !pCreateInfo->usage ||
        (pCreateInfo->usage & ~(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) ||
        pCreateInfo->sharingMode != VK_SHARING_MODE_EXCLUSIVE || pCreateInfo->queueFamilyIndexCount ||
        (pCreateInfo->initialLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
         pCreateInfo->initialLayout != VK_IMAGE_LAYOUT_PREINITIALIZED) || !allocator_is_valid(pAllocator))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (!vk_format_to_gpu(pCreateInfo->format, &gpu_format, &bytes_per_texel) ||
        !gpu_ir_metal_linear_texture_alignment(device->physical_device->owner->metal, gpu_format) ||
        !vk_image_usage_supported(device->physical_device, pCreateInfo->format, pCreateInfo->tiling,
                                  pCreateInfo->usage, device->maintenance1_enabled))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if ((pCreateInfo->usage & VK_IMAGE_USAGE_STORAGE_BIT) &&
        (pCreateInfo->tiling != VK_IMAGE_TILING_OPTIMAL || pCreateInfo->format != VK_FORMAT_R8G8B8A8_UNORM ||
         !device->physical_device->storage_image_supported))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if ((pCreateInfo->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
        pCreateInfo->tiling != VK_IMAGE_TILING_OPTIMAL) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (pCreateInfo->arrayLayers > 1 &&
        (pCreateInfo->usage & (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if ((pCreateInfo->usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
        pCreateInfo->tiling != VK_IMAGE_TILING_OPTIMAL)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (pCreateInfo->extent.width > 4096 || pCreateInfo->extent.height > 4096)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    uint64_t row_pitch = gpu_ir_metal_linear_texture_row_pitch(device->physical_device->owner->metal,
        gpu_format, pCreateInfo->extent.width);
    if (!row_pitch || row_pitch > UINT64_MAX / pCreateInfo->extent.height)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    uint64_t array_pitch = row_pitch * pCreateInfo->extent.height;
    if (array_pitch > UINT64_MAX / pCreateInfo->arrayLayers) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    uint64_t size = array_pitch * pCreateInfo->arrayLayers;
    if (size > gpu_ir_metal_max_buffer_length(device->physical_device->owner->metal))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    vx_vk_image *image = object_allocate(pAllocator, sizeof *image, alignof(vx_vk_image),
                                         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!image) return VK_ERROR_OUT_OF_HOST_MEMORY;
    image->magic = VX_VK_IMAGE_MAGIC;
    image->device = device;
    image->refs = 1;
    image->externally_alive = 1;
    image->format = pCreateInfo->format;
    image->tiling = pCreateInfo->tiling;
    for (uint32_t i = 0; i < pCreateInfo->arrayLayers; i++)
        image->current_layouts[i] = pCreateInfo->initialLayout;
    image->usage = pCreateInfo->usage;
    image->width = pCreateInfo->extent.width;
    image->height = pCreateInfo->extent.height;
    image->array_layers = pCreateInfo->arrayLayers;
    image->bytes_per_texel = bytes_per_texel;
    image->gpu_format = gpu_format;
    image->row_pitch = row_pitch;
    image->array_pitch = array_pitch;
    image->size = size;
    image->alignment = gpu_ir_metal_linear_texture_alignment(device->physical_device->owner->metal, gpu_format);
    if (pAllocator) {
        image->allocator = *pAllocator;
        image->has_allocator = 1;
    }
    *pImage = image_to_handle(image);
    return VK_SUCCESS;
}

void WINAPI vkDestroyImage(VkDevice device, VkImage imageHandle, const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    vx_vk_image *image = image_from_handle(imageHandle, device);
    if (!image || image->swapchain_owned) return;
    image->externally_alive = 0;
    for (vx_vk_image_use *use = image->command_buffer_uses; use; use = use->next)
        if (use->command_buffer && use->command_buffer->state != VX_VK_COMMAND_PENDING)
            use->command_buffer->state = VX_VK_COMMAND_INVALID;
    image_release(image);
}

static int vk_component_swizzle_valid(VkComponentSwizzle swizzle) {
    return swizzle <= VK_COMPONENT_SWIZZLE_A;
}

static int image_view_retain(vx_vk_image_view *view) {
    if (!view || !view->refs || view->refs == UINT32_MAX) return 0;
    view->refs++;
    return 1;
}

static void image_view_release(vx_vk_image_view *view) {
    if (!view || !view->refs || --view->refs) return;
    VkAllocationCallbacks allocator = view->allocator;
    int has_allocator = view->has_allocator;
    image_release(view->image);
    view->magic = 0;
    object_free(has_allocator ? &allocator : NULL, view);
}

static uint32_t vk_component_swizzle_to_gpu(VkComponentSwizzle swizzle) {
    switch (swizzle) {
        case VK_COMPONENT_SWIZZLE_IDENTITY: return GPU_IR_COMPONENT_SWIZZLE_IDENTITY;
        case VK_COMPONENT_SWIZZLE_R: return GPU_IR_COMPONENT_SWIZZLE_R;
        case VK_COMPONENT_SWIZZLE_G: return GPU_IR_COMPONENT_SWIZZLE_G;
        case VK_COMPONENT_SWIZZLE_B: return GPU_IR_COMPONENT_SWIZZLE_B;
        case VK_COMPONENT_SWIZZLE_A: return GPU_IR_COMPONENT_SWIZZLE_A;
        case VK_COMPONENT_SWIZZLE_ZERO: return GPU_IR_COMPONENT_SWIZZLE_ZERO;
        case VK_COMPONENT_SWIZZLE_ONE: return GPU_IR_COMPONENT_SWIZZLE_ONE;
        default: return UINT32_MAX;
    }
}

static uint32_t vk_component_mapping_to_gpu(VkComponentMapping components) {
    const VkComponentSwizzle swizzles[4] = {
        components.r, components.g, components.b, components.a
    };
    uint32_t mapping = 0;
    for (uint32_t component = 0; component < 4; component++) {
        uint32_t selector = vk_component_swizzle_to_gpu(swizzles[component]);
        if (selector > GPU_IR_COMPONENT_SWIZZLE_ONE) return UINT32_MAX;
        mapping |= selector << (component * 3u);
    }
    return mapping;
}

VkResult WINAPI vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkImageView *pView) {
    if (!pView) return VK_ERROR_INITIALIZATION_FAILED;
    *pView = 0;
    if (!device_is_valid(device) || !pCreateInfo ||
        pCreateInfo->sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO || pCreateInfo->pNext ||
        pCreateInfo->flags || !allocator_is_valid(pAllocator))
        return VK_ERROR_INITIALIZATION_FAILED;
    vx_vk_image *image = image_from_handle(pCreateInfo->image, device);
    const VkImageSubresourceRange *range = &pCreateInfo->subresourceRange;
    uint32_t view_layer_count = image && range->layerCount == VK_REMAINING_ARRAY_LAYERS &&
        range->baseArrayLayer < image->array_layers ? image->array_layers - range->baseArrayLayer : range->layerCount;
    if (!image || pCreateInfo->viewType != VK_IMAGE_VIEW_TYPE_2D || pCreateInfo->format != image->format ||
        !vk_component_swizzle_valid(pCreateInfo->components.r) ||
        !vk_component_swizzle_valid(pCreateInfo->components.g) ||
        !vk_component_swizzle_valid(pCreateInfo->components.b) ||
        !vk_component_swizzle_valid(pCreateInfo->components.a) ||
        range->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || range->baseMipLevel != 0 ||
        (range->levelCount != 1 && range->levelCount != UINT32_MAX) || !view_layer_count ||
        view_layer_count != 1 || range->baseArrayLayer >= image->array_layers)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    vx_vk_image_view *view = object_allocate(pAllocator, sizeof *view, alignof(vx_vk_image_view),
                                              VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!view) return VK_ERROR_OUT_OF_HOST_MEMORY;
    view->magic = VX_VK_IMAGE_VIEW_MAGIC;
    view->device = device;
    view->externally_alive = 1;
    view->refs = 1;
    view->image = image;
    view->format = pCreateInfo->format;
    view->view_type = pCreateInfo->viewType;
    view->components = pCreateInfo->components;
    view->subresource_range = *range;
    if (pAllocator) {
        view->allocator = *pAllocator;
        view->has_allocator = 1;
    }
    image_retain(image);
    *pView = image_view_to_handle(view);
    return VK_SUCCESS;
}

void WINAPI vkDestroyImageView(VkDevice device, VkImageView imageView,
    const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    vx_vk_image_view *view = image_view_from_handle(imageView, device);
    if (!view) return;
    view->externally_alive = 0;
    image_view_release(view);
}

static void render_pass_retain(vx_vk_render_pass *render_pass) {
    if (render_pass && render_pass->refs < UINT32_MAX) render_pass->refs++;
}

static void render_pass_release(vx_vk_render_pass *render_pass) {
    if (!render_pass || !render_pass->refs || --render_pass->refs) return;
    VkAllocationCallbacks allocator = render_pass->allocator;
    int has_allocator = render_pass->has_allocator;
    render_pass->magic = 0;
    object_free(has_allocator ? &allocator : NULL, render_pass);
}

static int vk_render_pass_layout_supported(VkImageLayout layout, int allow_undefined) {
    if (allow_undefined && layout == VK_IMAGE_LAYOUT_UNDEFINED) return 1;
    return layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || layout == VK_IMAGE_LAYOUT_GENERAL ||
        layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
        layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL || layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
}

VkResult WINAPI vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass) {
    if (!pRenderPass) return VK_ERROR_INITIALIZATION_FAILED;
    *pRenderPass = 0;
    if (!device_is_valid(device) || !pCreateInfo ||
        pCreateInfo->sType != VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO || pCreateInfo->pNext ||
        pCreateInfo->flags || !allocator_is_valid(pAllocator)) return VK_ERROR_INITIALIZATION_FAILED;
    if (pCreateInfo->attachmentCount != 1 || !pCreateInfo->pAttachments ||
        pCreateInfo->subpassCount != 1 || !pCreateInfo->pSubpasses || pCreateInfo->dependencyCount ||
        pCreateInfo->pDependencies) return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkAttachmentDescription *attachment = &pCreateInfo->pAttachments[0];
    const VkSubpassDescription *subpass = &pCreateInfo->pSubpasses[0];
    if (attachment->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
        attachment->initialLayout == VK_IMAGE_LAYOUT_UNDEFINED) return VK_ERROR_INITIALIZATION_FAILED;
    if (attachment->flags || attachment->samples != VK_SAMPLE_COUNT_1_BIT ||
        attachment->loadOp > VK_ATTACHMENT_LOAD_OP_DONT_CARE ||
        attachment->storeOp > VK_ATTACHMENT_STORE_OP_DONT_CARE ||
        attachment->stencilLoadOp != VK_ATTACHMENT_LOAD_OP_DONT_CARE ||
        attachment->stencilStoreOp != VK_ATTACHMENT_STORE_OP_DONT_CARE ||
        !vk_render_pass_layout_supported(attachment->initialLayout, 1) ||
        !vk_render_pass_layout_supported(attachment->finalLayout, 0) ||
        !(vk_image_format_features(device->physical_device, attachment->format, VK_IMAGE_TILING_OPTIMAL) &
          VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (subpass->flags || subpass->pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS ||
        subpass->inputAttachmentCount || subpass->pInputAttachments || subpass->colorAttachmentCount != 1 ||
        !subpass->pColorAttachments || subpass->pResolveAttachments || subpass->pDepthStencilAttachment ||
        subpass->preserveAttachmentCount || subpass->pPreserveAttachments ||
        subpass->pColorAttachments[0].attachment != 0 ||
        subpass->pColorAttachments[0].layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    vx_vk_render_pass *render_pass = object_allocate(pAllocator, sizeof *render_pass,
        alignof(vx_vk_render_pass), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!render_pass) return VK_ERROR_OUT_OF_HOST_MEMORY;
    render_pass->magic = VX_VK_RENDER_PASS_MAGIC;
    render_pass->device = device;
    render_pass->externally_alive = 1;
    render_pass->refs = 1;
    render_pass->attachment = *attachment;
    if (pAllocator) { render_pass->allocator = *pAllocator; render_pass->has_allocator = 1; }
    *pRenderPass = render_pass_to_handle(render_pass);
    return VK_SUCCESS;
}

void WINAPI vkDestroyRenderPass(VkDevice device, VkRenderPass renderPass,
    const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    vx_vk_render_pass *object = render_pass_from_handle(renderPass, device);
    if (!object) return;
    object->externally_alive = 0;
    render_pass_release(object);
}

VkResult WINAPI vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkFramebuffer *pFramebuffer) {
    if (!pFramebuffer) return VK_ERROR_INITIALIZATION_FAILED;
    *pFramebuffer = 0;
    if (!device_is_valid(device) || !pCreateInfo ||
        pCreateInfo->sType != VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO || pCreateInfo->pNext ||
        pCreateInfo->flags || !pCreateInfo->width || !pCreateInfo->height || pCreateInfo->layers != 1 ||
        pCreateInfo->attachmentCount != 1 || !pCreateInfo->pAttachments || !allocator_is_valid(pAllocator))
        return VK_ERROR_INITIALIZATION_FAILED;
    vx_vk_render_pass *render_pass = render_pass_from_handle(pCreateInfo->renderPass, device);
    vx_vk_image_view *view = image_view_from_handle(pCreateInfo->pAttachments[0], device);
    if (!render_pass || !view || view->format != render_pass->attachment.format || !view->image->bound ||
        !(view->image->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || pCreateInfo->width > view->image->width ||
        pCreateInfo->height > view->image->height || pCreateInfo->width > 4096 || pCreateInfo->height > 4096 ||
        view->subresource_range.baseMipLevel != 0 || view->subresource_range.levelCount != 1)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (render_pass->refs == UINT32_MAX || !image_view_retain(view)) return VK_ERROR_TOO_MANY_OBJECTS;
    render_pass_retain(render_pass);
    vx_vk_framebuffer *framebuffer = object_allocate(pAllocator, sizeof *framebuffer,
        alignof(vx_vk_framebuffer), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!framebuffer) {
        image_view_release(view);
        render_pass_release(render_pass);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    framebuffer->magic = VX_VK_FRAMEBUFFER_MAGIC;
    framebuffer->device = device;
    framebuffer->externally_alive = 1;
    framebuffer->render_pass = render_pass;
    framebuffer->view = view;
    framebuffer->width = pCreateInfo->width;
    framebuffer->height = pCreateInfo->height;
    framebuffer->layer = view->subresource_range.baseArrayLayer;
    if (pAllocator) { framebuffer->allocator = *pAllocator; framebuffer->has_allocator = 1; }
    *pFramebuffer = framebuffer_to_handle(framebuffer);
    return VK_SUCCESS;
}

void WINAPI vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebufferHandle,
    const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    vx_vk_framebuffer *framebuffer = framebuffer_from_handle(framebufferHandle, device);
    if (!framebuffer) return;
    VkAllocationCallbacks allocator = framebuffer->allocator;
    int has_allocator = framebuffer->has_allocator;
    framebuffer->externally_alive = 0;
    image_view_release(framebuffer->view);
    render_pass_release(framebuffer->render_pass);
    framebuffer->magic = 0;
    object_free(has_allocator ? &allocator : NULL, framebuffer);
}

static int vk_sampler_address_mode_valid(VkSamplerAddressMode mode) {
    return mode <= VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
}

static int vk_sampler_border_color_valid(VkBorderColor color) {
    return color <= VK_BORDER_COLOR_INT_OPAQUE_WHITE;
}

VkResult WINAPI vkCreateSampler(VkDevice device, const VkSamplerCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSampler *pSampler) {
    if (!pSampler) return VK_ERROR_INITIALIZATION_FAILED;
    *pSampler = 0;
    if (!device_is_valid(device) || !pCreateInfo ||
        pCreateInfo->sType != VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO || pCreateInfo->pNext ||
        !allocator_is_valid(pAllocator) || pCreateInfo->flags ||
        pCreateInfo->magFilter > VK_FILTER_LINEAR || pCreateInfo->minFilter > VK_FILTER_LINEAR ||
        pCreateInfo->mipmapMode > VK_SAMPLER_MIPMAP_MODE_LINEAR ||
        !vk_sampler_address_mode_valid(pCreateInfo->addressModeU) ||
        !vk_sampler_address_mode_valid(pCreateInfo->addressModeV) ||
        !vk_sampler_address_mode_valid(pCreateInfo->addressModeW) ||
        !isfinite(pCreateInfo->mipLodBias) || !isfinite(pCreateInfo->minLod) ||
        !isfinite(pCreateInfo->maxLod) || pCreateInfo->anisotropyEnable > 1 ||
        pCreateInfo->compareEnable > 1 || pCreateInfo->minLod > pCreateInfo->maxLod ||
        pCreateInfo->compareOp < 0 || pCreateInfo->compareOp > 7 ||
        !vk_sampler_border_color_valid(pCreateInfo->borderColor) || pCreateInfo->compareEnable)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (pCreateInfo->anisotropyEnable && !device->sampler_anisotropy_enabled)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (pCreateInfo->anisotropyEnable && (!isfinite(pCreateInfo->maxAnisotropy) ||
        pCreateInfo->maxAnisotropy < 1.0f ||
        pCreateInfo->maxAnisotropy > (float)device->physical_device->max_sampler_anisotropy ||
        pCreateInfo->unnormalizedCoordinates)) return VK_ERROR_INITIALIZATION_FAILED;
    if (pCreateInfo->unnormalizedCoordinates &&
        (pCreateInfo->minFilter != pCreateInfo->magFilter ||
         pCreateInfo->mipmapMode != VK_SAMPLER_MIPMAP_MODE_NEAREST ||
         pCreateInfo->mipLodBias != 0.0f || pCreateInfo->minLod != 0.0f || pCreateInfo->maxLod != 0.0f ||
         (pCreateInfo->addressModeU != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE &&
          pCreateInfo->addressModeU != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) ||
         (pCreateInfo->addressModeV != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE &&
          pCreateInfo->addressModeV != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER)))
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pCreateInfo->addressModeU == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ||
        pCreateInfo->addressModeV == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ||
        pCreateInfo->addressModeW == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ||
        pCreateInfo->borderColor == VK_BORDER_COLOR_INT_TRANSPARENT_BLACK ||
        pCreateInfo->borderColor == VK_BORDER_COLOR_INT_OPAQUE_BLACK ||
        pCreateInfo->borderColor == VK_BORDER_COLOR_INT_OPAQUE_WHITE)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!sampler_slot_reserve(device)) return VK_ERROR_TOO_MANY_OBJECTS;
    gpu_ir_metal_sampler_desc description = {
        .min_filter = pCreateInfo->minFilter,
        .mag_filter = pCreateInfo->magFilter,
        .mip_filter = pCreateInfo->mipmapMode,
        .address_u = pCreateInfo->addressModeU,
        .address_v = pCreateInfo->addressModeV,
        .address_w = pCreateInfo->addressModeW,
        .border_color = pCreateInfo->borderColor,
        .normalized_coordinates = !pCreateInfo->unnormalizedCoordinates,
        .max_anisotropy = pCreateInfo->anisotropyEnable ? pCreateInfo->maxAnisotropy : 1.0f,
        .lod_bias = pCreateInfo->mipLodBias,
        .min_lod = pCreateInfo->minLod,
        .max_lod = pCreateInfo->maxLod
    };
    void *metal_state = gpu_ir_metal_sampler_create(device->physical_device->owner->metal, &description);
    if (!metal_state) {
        sampler_slot_release(device);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    vx_vk_sampler *sampler = object_allocate(pAllocator, sizeof *sampler, alignof(vx_vk_sampler),
                                             VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!sampler) {
        gpu_ir_metal_sampler_release(metal_state);
        sampler_slot_release(device);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    sampler->magic = VX_VK_SAMPLER_MAGIC;
    sampler->device = device;
    sampler->externally_alive = 1;
    sampler->refs = 1;
    sampler->metal_state = metal_state;
    if (pAllocator) { sampler->allocator = *pAllocator; sampler->has_allocator = 1; }
    *pSampler = sampler_to_handle(sampler);
    return VK_SUCCESS;
}

void WINAPI vkDestroySampler(VkDevice device, VkSampler samplerHandle,
    const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    vx_vk_sampler *sampler = (vx_vk_sampler *)(uintptr_t)samplerHandle;
    if (!device_is_valid(device) || !sampler || sampler->magic != VX_VK_SAMPLER_MAGIC ||
        sampler->device != device || !sampler->externally_alive) return;
    pthread_mutex_lock(&device->sync_mutex);
    int destroy = sampler->externally_alive;
    if (destroy) {
        sampler->externally_alive = 0;
        if (device->sampler_count) device->sampler_count--;
    }
    pthread_mutex_unlock(&device->sync_mutex);
    if (destroy) sampler_release(sampler);
}

void WINAPI vkGetImageMemoryRequirements(VkDevice device, VkImage imageHandle,
                                        VkMemoryRequirements *pMemoryRequirements) {
    if (!pMemoryRequirements) return;
    memset(pMemoryRequirements, 0, sizeof *pMemoryRequirements);
    vx_vk_image *image = image_from_handle(imageHandle, device);
    if (!image) return;
    if (image->swapchain_owned) return;
    pMemoryRequirements->size = image->size;
    pMemoryRequirements->alignment = image->alignment;
    pMemoryRequirements->memoryTypeBits = 1;
}

VkResult WINAPI vkBindImageMemory(VkDevice device, VkImage imageHandle, VkDeviceMemory memoryHandle,
                                  VkDeviceSize memoryOffset) {
    vx_vk_image *image = image_from_handle(imageHandle, device);
    vx_vk_memory *memory = memory_from_handle(memoryHandle, device);
    if (!image || image->swapchain_owned || !memory || image->bound || memoryOffset % image->alignment ||
        memoryOffset > memory->size || image->size > memory->size - memoryOffset)
        return VK_ERROR_INITIALIZATION_FAILED;
    gpu_ir_texture_desc description = {
        .width = image->width, .height = image->height, .depth = 1,
        .array_layers = 1, .mip_levels = 1, .bytes_per_texel = image->bytes_per_texel,
        .sample_count = 1, .is_3d = 0, .block_width = 1, .block_height = 1,
        .bytes_per_block = image->bytes_per_texel, .format = image->gpu_format
    };
    gpu_ir_resource **layer_resources = NULL;
    gpu_ir_resource *resource = NULL;
    if (image->array_layers > 1) {
        size_t allocation_size = (size_t)image->array_layers * sizeof *layer_resources;
        if (allocation_size / sizeof *layer_resources != image->array_layers) return VK_ERROR_OUT_OF_HOST_MEMORY;
        layer_resources = object_allocate(image->has_allocator ? &image->allocator : NULL, allocation_size,
            alignof(gpu_ir_resource *), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
        if (!layer_resources) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < image->array_layers; i++) {
            uint64_t layer_offset = memoryOffset + (uint64_t)i * image->array_pitch;
            layer_resources[i] = gpu_ir_metal_texture_create_from_buffer(
                device->physical_device->owner->metal, memory->resource, layer_offset, image->row_pitch,
                &description, 0, 0);
            if (!layer_resources[i]) {
                for (uint32_t j = 0; j < i; j++) gpu_ir_resource_release(layer_resources[j]);
                object_free(image->has_allocator ? &image->allocator : NULL, layer_resources);
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            }
        }
        resource = layer_resources[0];
    } else {
        resource = gpu_ir_metal_texture_create_from_buffer(
            device->physical_device->owner->metal, memory->resource, memoryOffset, image->row_pitch, &description,
            (image->usage & (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) != 0,
            (image->usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0);
        if (!resource) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    image->memory = memory;
    image->memory_offset = memoryOffset;
    image->resource = resource;
    image->layer_resources = layer_resources;
    image->bound = 1;
    memory->bound_images++;
    return VK_SUCCESS;
}

void WINAPI vkGetImageSubresourceLayout(VkDevice device, VkImage imageHandle,
                                       const VkImageSubresource *pSubresource, VkSubresourceLayout *pLayout) {
    if (!pLayout) return;
    memset(pLayout, 0, sizeof *pLayout);
    vx_vk_image *image = image_from_handle(imageHandle, device);
    if (!image || image->tiling != VK_IMAGE_TILING_LINEAR || !pSubresource ||
        pSubresource->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || pSubresource->mipLevel ||
        pSubresource->arrayLayer >= image->array_layers) return;
    pLayout->offset = image->array_pitch * pSubresource->arrayLayer;
    pLayout->size = image->array_pitch;
    pLayout->rowPitch = image->row_pitch;
    pLayout->arrayPitch = image->array_pitch;
    pLayout->depthPitch = image->array_pitch;
}

VkResult WINAPI vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
    const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory) {
    if (!pMemory) return VK_ERROR_INITIALIZATION_FAILED;
    *pMemory = 0;
    if (!device_is_valid(device) || !pAllocateInfo ||
        pAllocateInfo->sType != VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO || pAllocateInfo->pNext ||
        pAllocateInfo->memoryTypeIndex != 0 || !pAllocateInfo->allocationSize ||
        pAllocateInfo->allocationSize > SIZE_MAX || !allocator_is_valid(pAllocator))
        return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t max_size = gpu_ir_metal_max_buffer_length(device->physical_device->owner->metal);
    if (pAllocateInfo->allocationSize > max_size) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    vx_vk_memory *memory = object_allocate(pAllocator, sizeof *memory, alignof(vx_vk_memory),
                                           VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!memory) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memory->resource = gpu_ir_metal_buffer_create(device->physical_device->owner->metal,
                                                   pAllocateInfo->allocationSize, NULL);
    if (!memory->resource) {
        object_free(pAllocator, memory);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    memory->magic = VX_VK_MEMORY_MAGIC;
    memory->device = device;
    memory->size = pAllocateInfo->allocationSize;
    if (pAllocator) {
        memory->allocator = *pAllocator;
        memory->has_allocator = 1;
    }
    *pMemory = memory_to_handle(memory);
    return VK_SUCCESS;
}

void WINAPI vkFreeMemory(VkDevice device, VkDeviceMemory memoryHandle, const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    vx_vk_memory *memory = memory_from_handle(memoryHandle, device);
    if (!memory || memory->bound_buffers || memory->bound_images) return;
    VkAllocationCallbacks allocator = memory->allocator;
    int has_allocator = memory->has_allocator;
    memory->magic = 0;
    gpu_ir_resource_release(memory->resource);
    object_free(has_allocator ? &allocator : NULL, memory);
}

VkResult WINAPI vkBindBufferMemory(VkDevice device, VkBuffer bufferHandle, VkDeviceMemory memoryHandle,
                                     VkDeviceSize memoryOffset) {
    vx_vk_buffer *buffer = buffer_from_handle(bufferHandle, device);
    vx_vk_memory *memory = memory_from_handle(memoryHandle, device);
    if (!buffer || !memory || buffer->bound || memoryOffset % VX_VK_BUFFER_ALIGNMENT != 0 ||
        memoryOffset > memory->size || buffer->size > memory->size - memoryOffset)
        return VK_ERROR_INITIALIZATION_FAILED;
    buffer->memory = memory;
    buffer->memory_offset = memoryOffset;
    buffer->bound = 1;
    memory->bound_buffers++;
    return VK_SUCCESS;
}

VkResult WINAPI vkMapMemory(VkDevice device, VkDeviceMemory memoryHandle, VkDeviceSize offset,
    VkDeviceSize size, VkFlags flags, void **ppData) {
    if (!ppData) return VK_ERROR_MEMORY_MAP_FAILED;
    *ppData = NULL;
    vx_vk_memory *memory = memory_from_handle(memoryHandle, device);
    if (!memory || flags || memory->mapped || offset > memory->size) return VK_ERROR_MEMORY_MAP_FAILED;
    if (size == UINT64_MAX) size = memory->size - offset;
    if (!size || size > memory->size - offset) return VK_ERROR_MEMORY_MAP_FAILED;
    uint8_t *contents = gpu_ir_metal_buffer_contents(memory->resource);
    if (!contents) return VK_ERROR_MEMORY_MAP_FAILED;
    memory->mapped = 1;
    memory->mapped_offset = offset;
    memory->mapped_size = size;
    *ppData = contents + offset;
    return VK_SUCCESS;
}

void WINAPI vkUnmapMemory(VkDevice device, VkDeviceMemory memoryHandle) {
    vx_vk_memory *memory = memory_from_handle(memoryHandle, device);
    if (memory) {
        memory->mapped = 0;
        memory->mapped_offset = 0;
        memory->mapped_size = 0;
    }
}

static VkResult validate_mapped_memory_ranges(VkDevice device, uint32_t range_count,
                                               const VkMappedMemoryRange *ranges) {
    if (!device_is_valid(device) || !range_count || !ranges) return VK_ERROR_INITIALIZATION_FAILED;
    const VkDeviceSize atom_size = 1;
    for (uint32_t i = 0; i < range_count; i++) {
        const VkMappedMemoryRange *range = &ranges[i];
        if (range->sType != VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE || range->pNext)
            return VK_ERROR_INITIALIZATION_FAILED;
        vx_vk_memory *memory = memory_from_handle(range->memory, device);
        if (!memory || !memory->mapped) return VK_ERROR_MEMORY_MAP_FAILED;
        uint64_t mapped_end = memory->mapped_offset + memory->mapped_size;
        if (range->offset < memory->mapped_offset || range->offset >= mapped_end ||
            range->offset > memory->size || range->offset % atom_size)
            return VK_ERROR_MEMORY_MAP_FAILED;
        uint64_t effective_size;
        if (range->size == VK_WHOLE_SIZE) {
            effective_size = mapped_end - range->offset;
            if (mapped_end % atom_size && mapped_end != memory->size) return VK_ERROR_MEMORY_MAP_FAILED;
        } else {
            if (!range->size || range->size > mapped_end - range->offset)
                return VK_ERROR_MEMORY_MAP_FAILED;
            effective_size = range->size;
            if (effective_size % atom_size && range->offset + effective_size != memory->size)
                return VK_ERROR_MEMORY_MAP_FAILED;
        }
    }
    return VK_SUCCESS;
}

VkResult WINAPI vkFlushMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount,
    const VkMappedMemoryRange *pMemoryRanges) {
    VkResult result = validate_mapped_memory_ranges(device, memoryRangeCount, pMemoryRanges);
    if (result != VK_SUCCESS) return result;
    // All exposed allocations are Metal shared buffers backed by the reported host-coherent memory type.
    atomic_thread_fence(memory_order_release);
    return VK_SUCCESS;
}

VkResult WINAPI vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount,
    const VkMappedMemoryRange *pMemoryRanges) {
    VkResult result = validate_mapped_memory_ranges(device, memoryRangeCount, pMemoryRanges);
    if (result != VK_SUCCESS) return result;
    atomic_thread_fence(memory_order_acquire);
    return VK_SUCCESS;
}

VkResult WINAPI vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule) {
    if (!pShaderModule) return VK_ERROR_INITIALIZATION_FAILED;
    *pShaderModule = 0;
    if (!device_is_valid(device) || !pCreateInfo || pCreateInfo->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO ||
        pCreateInfo->pNext || pCreateInfo->flags || !pCreateInfo->codeSize || (pCreateInfo->codeSize & 3u) ||
        !pCreateInfo->pCode || !allocator_is_valid(pAllocator)) return VK_ERROR_INITIALIZATION_FAILED;
    vx_vk_shader_module *module = object_allocate(pAllocator, sizeof *module, alignof(vx_vk_shader_module),
                                                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!module) return VK_ERROR_OUT_OF_HOST_MEMORY;
    int rc = spirv_read(pCreateInfo->pCode, pCreateInfo->codeSize, &module->module);
    if (rc != SPIRV_OK || !spirv_vulkan_memory_model_supported(&module->module)) {
        object_free(pAllocator, module);
        return rc == SPIRV_OUT_OF_MEMORY ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_INITIALIZATION_FAILED;
    }
    module->magic = VX_VK_SHADER_MODULE_MAGIC;
    module->device = device;
    module->externally_alive = 1;
    if (pAllocator) {
        module->allocator = *pAllocator;
        module->has_allocator = 1;
    }
    *pShaderModule = shader_module_to_handle(module);
    return VK_SUCCESS;
}

void WINAPI vkDestroyShaderModule(VkDevice device, VkShaderModule moduleHandle,
    const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    vx_vk_shader_module *module = shader_module_from_handle(moduleHandle, device);
    if (!module) return;
    VkAllocationCallbacks allocator = module->allocator;
    int has_allocator = module->has_allocator;
    spirv_free(&module->module);
    module->externally_alive = 0;
    module->magic = 0;
    object_free(has_allocator ? &allocator : NULL, module);
}

VkResult WINAPI vkCreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDescriptorSetLayout *pSetLayout) {
    if (!pSetLayout) return VK_ERROR_INITIALIZATION_FAILED;
    *pSetLayout = 0;
    if (!device_is_valid(device) || !pCreateInfo ||
        pCreateInfo->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO || pCreateInfo->pNext ||
        pCreateInfo->flags || (pCreateInfo->bindingCount && !pCreateInfo->pBindings) ||
        pCreateInfo->bindingCount > VX_VK_MAX_DESCRIPTOR_VALUES || !allocator_is_valid(pAllocator))
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t descriptor_count = 0, buffer_count = 0, uniform_buffer_count = 0, storage_buffer_count = 0;
    uint32_t dynamic_uniform_count = 0, dynamic_storage_count = 0, sampled_image_count = 0;
    uint32_t storage_image_count = 0, sampler_count = 0;
    for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
        const VkDescriptorSetLayoutBinding *binding = &pCreateInfo->pBindings[i];
        uint32_t stage_mask = 0;
        if (!vk_descriptor_type_supported(binding->descriptorType) ||
            binding->descriptorCount > VX_VK_MAX_DESCRIPTOR_VALUES - descriptor_count ||
            !vk_shader_stage_mask(binding->stageFlags, &stage_mask) || binding->pImmutableSamplers)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if (binding->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            storage_image_count += binding->descriptorCount;
            if (storage_image_count > 4 || !device->physical_device->storage_image_supported)
                return VK_ERROR_FEATURE_NOT_PRESENT;
        } else if (binding->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                   binding->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            sampled_image_count += binding->descriptorCount;
            if (sampled_image_count > GPU_IR_METAL_MAX_COMPUTE_SAMPLED_IMAGES)
                return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if (vk_buffer_descriptor_type(binding->descriptorType)) {
            buffer_count += binding->descriptorCount;
            if (buffer_count > GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            if (vk_uniform_descriptor_type(binding->descriptorType)) {
                uniform_buffer_count += binding->descriptorCount;
                if (uniform_buffer_count > VX_VK_MAX_PER_STAGE_UNIFORM_BUFFERS)
                    return VK_ERROR_FEATURE_NOT_PRESENT;
            } else {
                storage_buffer_count += binding->descriptorCount;
                if (storage_buffer_count > VX_VK_MAX_PER_STAGE_STORAGE_BUFFERS)
                    return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            if (binding->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
                dynamic_uniform_count += binding->descriptorCount;
                if (dynamic_uniform_count > VX_VK_MAX_UNIFORM_BUFFER_DYNAMIC_DESCRIPTORS)
                    return VK_ERROR_FEATURE_NOT_PRESENT;
            } else if (binding->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
                dynamic_storage_count += binding->descriptorCount;
                if (dynamic_storage_count > VX_VK_MAX_STORAGE_BUFFER_DYNAMIC_DESCRIPTORS)
                    return VK_ERROR_FEATURE_NOT_PRESENT;
            }
        }
        if (vk_sampler_descriptor_type(binding->descriptorType)) {
            sampler_count += binding->descriptorCount;
            if (sampler_count > GPU_IR_METAL_MAX_COMPUTE_SAMPLERS)
                return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        descriptor_count += binding->descriptorCount;
        for (uint32_t j = 0; j < i; j++)
            if (pCreateInfo->pBindings[j].binding == binding->binding) return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (sampled_image_count + storage_image_count > GPU_IR_METAL_MAX_COMPUTE_TEXTURES)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    vx_vk_descriptor_layout *layout = object_allocate(pAllocator, sizeof *layout, alignof(vx_vk_descriptor_layout),
                                                        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!layout) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (pCreateInfo->bindingCount) {
        layout->bindings = object_allocate(pAllocator,
            (size_t)pCreateInfo->bindingCount * sizeof *layout->bindings, alignof(VkDescriptorSetLayoutBinding),
            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
        if (!layout->bindings) { object_free(pAllocator, layout); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        memcpy(layout->bindings, pCreateInfo->pBindings,
               (size_t)pCreateInfo->bindingCount * sizeof *layout->bindings);
        // Vulkan binding numbers, rather than the caller's array order, define the layout order.
        for (uint32_t i = 1; i < pCreateInfo->bindingCount; i++) {
            VkDescriptorSetLayoutBinding value = layout->bindings[i];
            uint32_t j = i;
            while (j && layout->bindings[j - 1].binding > value.binding) {
                layout->bindings[j] = layout->bindings[j - 1];
                j--;
            }
            layout->bindings[j] = value;
        }
    }
    layout->magic = VX_VK_DESCRIPTOR_LAYOUT_MAGIC;
    layout->device = device;
    layout->refs = 1;
    layout->externally_alive = 1;
    layout->binding_count = pCreateInfo->bindingCount;
    layout->descriptor_count = descriptor_count;
    if (pAllocator) { layout->allocator = *pAllocator; layout->has_allocator = 1; }
    *pSetLayout = descriptor_layout_to_handle(layout);
    return VK_SUCCESS;
}

void WINAPI vkDestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout layoutHandle,
    const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    vx_vk_descriptor_layout *layout = descriptor_layout_from_handle(layoutHandle, device);
    if (!layout) return;
    layout->externally_alive = 0;
    descriptor_layout_release(layout);
}

VkResult WINAPI vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkPipelineLayout *pPipelineLayout) {
    if (!pPipelineLayout) return VK_ERROR_INITIALIZATION_FAILED;
    *pPipelineLayout = 0;
    if (!device_is_valid(device) || !pCreateInfo || pCreateInfo->sType != VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO ||
        pCreateInfo->pNext || pCreateInfo->flags || pCreateInfo->setLayoutCount > VX_VK_MAX_DESCRIPTOR_SETS ||
        (pCreateInfo->setLayoutCount && !pCreateInfo->pSetLayouts) ||
        (pCreateInfo->pushConstantRangeCount && !pCreateInfo->pPushConstantRanges) ||
        pCreateInfo->pushConstantRangeCount > VX_VK_MAX_PUSH_CONSTANT_SIZE / 4u ||
        !allocator_is_valid(pAllocator)) return VK_ERROR_INITIALIZATION_FAILED;

    gpu_ir_push_constant_range *push_ranges = pCreateInfo->pushConstantRangeCount ?
        calloc(pCreateInfo->pushConstantRangeCount, sizeof *push_ranges) : NULL;
    if (pCreateInfo->pushConstantRangeCount && !push_ranges) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult range_result = VK_SUCCESS;
    for (uint32_t i = 0; i < pCreateInfo->pushConstantRangeCount; i++) {
        const VkPushConstantRange *range = &pCreateInfo->pPushConstantRanges[i];
        if (!range->stageFlags || (range->offset & 3u) || !range->size || (range->size & 3u) ||
            range->offset >= VX_VK_MAX_PUSH_CONSTANT_SIZE ||
            range->size > VX_VK_MAX_PUSH_CONSTANT_SIZE - range->offset) {
            range_result = VK_ERROR_INITIALIZATION_FAILED;
            break;
        }
        if (range->stageFlags != VK_SHADER_STAGE_COMPUTE_BIT) {
            range_result = VK_ERROR_FEATURE_NOT_PRESENT;
            break;
        }
        push_ranges[i] = (gpu_ir_push_constant_range){
            .offset = range->offset,
            .size = range->size,
            .stage_mask = GPU_IR_STAGE_COMPUTE
        };
        for (uint32_t j = 0; j < i; j++) {
            uint32_t previous_end = push_ranges[j].offset + push_ranges[j].size;
            uint32_t current_end = range->offset + range->size;
            if (range->offset < previous_end && push_ranges[j].offset < current_end) {
                range_result = VK_ERROR_INITIALIZATION_FAILED;
                break;
            }
        }
        if (range_result != VK_SUCCESS) break;
    }
    if (range_result != VK_SUCCESS) { free(push_ranges); return range_result; }

    vx_vk_pipeline_layout *layout = object_allocate(pAllocator, sizeof *layout, alignof(vx_vk_pipeline_layout),
                                                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!layout) { free(push_ranges); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    uint32_t binding_count = 0, gpu_binding_count = 0, descriptor_count = 0, buffer_count = 0;
    uint32_t uniform_buffer_count = 0, storage_buffer_count = 0;
    uint32_t dynamic_uniform_count = 0, dynamic_storage_count = 0;
    uint32_t sampled_image_count = 0, storage_image_count = 0, sampler_count = 0;
    for (uint32_t i = 0; i < pCreateInfo->setLayoutCount; i++) {
        vx_vk_descriptor_layout *set_layout = descriptor_layout_from_handle(pCreateInfo->pSetLayouts[i], device);
        if (!set_layout || binding_count > VX_VK_MAX_DESCRIPTOR_VALUES - set_layout->binding_count ||
            set_layout->descriptor_count > VX_VK_MAX_DESCRIPTOR_VALUES - descriptor_count) {
            object_free(pAllocator, layout);
            free(push_ranges);
            return set_layout ? VK_ERROR_FEATURE_NOT_PRESENT : VK_ERROR_INITIALIZATION_FAILED;
        }
        binding_count += set_layout->binding_count;
        descriptor_count += set_layout->descriptor_count;
        for (uint32_t j = 0; j < set_layout->binding_count; j++) {
            gpu_binding_count += set_layout->bindings[j].descriptorCount != 0;
            if (set_layout->bindings[j].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
                storage_image_count += set_layout->bindings[j].descriptorCount;
            else if (set_layout->bindings[j].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                     set_layout->bindings[j].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
                sampled_image_count += set_layout->bindings[j].descriptorCount;
            if (vk_sampler_descriptor_type(set_layout->bindings[j].descriptorType))
                sampler_count += set_layout->bindings[j].descriptorCount;
            if (vk_buffer_descriptor_type(set_layout->bindings[j].descriptorType)) {
                buffer_count += set_layout->bindings[j].descriptorCount;
                if (vk_uniform_descriptor_type(set_layout->bindings[j].descriptorType))
                    uniform_buffer_count += set_layout->bindings[j].descriptorCount;
                else
                    storage_buffer_count += set_layout->bindings[j].descriptorCount;
                if (set_layout->bindings[j].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
                    dynamic_uniform_count += set_layout->bindings[j].descriptorCount;
                else if (set_layout->bindings[j].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
                    dynamic_storage_count += set_layout->bindings[j].descriptorCount;
            }
        }
        if (buffer_count > GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS ||
            uniform_buffer_count > VX_VK_MAX_PER_STAGE_UNIFORM_BUFFERS ||
            storage_buffer_count > VX_VK_MAX_PER_STAGE_STORAGE_BUFFERS ||
            dynamic_uniform_count > VX_VK_MAX_UNIFORM_BUFFER_DYNAMIC_DESCRIPTORS ||
            dynamic_storage_count > VX_VK_MAX_STORAGE_BUFFER_DYNAMIC_DESCRIPTORS ||
            storage_image_count > GPU_IR_METAL_MAX_COMPUTE_STORAGE_IMAGES ||
            sampled_image_count > GPU_IR_METAL_MAX_COMPUTE_SAMPLED_IMAGES ||
            sampler_count > GPU_IR_METAL_MAX_COMPUTE_SAMPLERS ||
            storage_image_count + sampled_image_count > GPU_IR_METAL_MAX_COMPUTE_TEXTURES) {
            object_free(pAllocator, layout);
            free(push_ranges);
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    int has_push_constants = pCreateInfo->pushConstantRangeCount != 0;
    if (pCreateInfo->setLayoutCount) {
        layout->set_layouts = object_allocate(pAllocator,
            (size_t)pCreateInfo->setLayoutCount * sizeof *layout->set_layouts, alignof(vx_vk_descriptor_layout *),
            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
        if (!layout->set_layouts) {
            object_free(pAllocator, layout);
            free(push_ranges);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    uint32_t common_binding_count = gpu_binding_count + (has_push_constants ? 1u : 0u);
    gpu_ir_descriptor_binding *bindings = common_binding_count ?
        calloc(common_binding_count, sizeof *bindings) : NULL;
    if (common_binding_count && !bindings) {
        object_free(pAllocator, layout->set_layouts);
        object_free(pAllocator, layout);
        free(push_ranges);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint32_t at = 0;
    for (uint32_t set = 0; set < pCreateInfo->setLayoutCount; set++) {
        vx_vk_descriptor_layout *set_layout = descriptor_layout_from_handle(pCreateInfo->pSetLayouts[set], device);
        layout->set_layouts[set] = set_layout;
        descriptor_layout_retain(set_layout);
        for (uint32_t j = 0; j < set_layout->binding_count; j++) {
            if (!set_layout->bindings[j].descriptorCount) continue;
            VkDescriptorType descriptor_type = set_layout->bindings[j].descriptorType;
            uint32_t stage_mask = 0;
            if (!vk_shader_stage_mask(set_layout->bindings[j].stageFlags, &stage_mask)) {
                for (uint32_t k = 0; k <= set; k++)
                    if (layout->set_layouts[k]) descriptor_layout_release(layout->set_layouts[k]);
                object_free(pAllocator, layout->set_layouts);
                object_free(pAllocator, layout);
                free(bindings);
                free(push_ranges);
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            bindings[at++] = (gpu_ir_descriptor_binding){
                .set = set,
                .binding = set_layout->bindings[j].binding,
                .kind = vk_uniform_descriptor_type(descriptor_type) ? GPU_IR_DESCRIPTOR_UNIFORM_BUFFER :
                    descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? GPU_IR_DESCRIPTOR_STORAGE_IMAGE :
                    descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? GPU_IR_DESCRIPTOR_SAMPLED_IMAGE :
                    descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER ? GPU_IR_DESCRIPTOR_SAMPLER :
                    descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ?
                        GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER : GPU_IR_DESCRIPTOR_STORAGE_BUFFER,
                .count = set_layout->bindings[j].descriptorCount,
                .stage_mask = stage_mask,
                .access_mask = vk_uniform_descriptor_type(descriptor_type) ? GPU_IR_ACCESS_UNIFORM_READ :
                    (descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                     descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) ?
                        GPU_IR_ACCESS_SHADER_READ :
                    descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER ? 0 :
                    GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE,
                .binding_namespace = 0
            };
        }
    }
    if (has_push_constants) {
        bindings[at++] = (gpu_ir_descriptor_binding){
            .set = GPU_IR_INTERNAL_SET,
            .binding = GPU_IR_INTERNAL_PUSH_CONSTANT_BINDING,
            .kind = GPU_IR_DESCRIPTOR_UNIFORM_BUFFER,
            .count = 1,
            .stage_mask = GPU_IR_STAGE_COMPUTE,
            .access_mask = GPU_IR_ACCESS_UNIFORM_READ,
            .binding_namespace = GPU_IR_INTERNAL_BINDING_NAMESPACE
        };
    }
    gpu_ir_pipeline_layout_desc gpu_desc = {
        .bindings = bindings,
        .binding_count = at,
        .push_constants = push_ranges,
        .push_constant_count = pCreateInfo->pushConstantRangeCount
    };
    layout->gpu_layout = gpu_ir_pipeline_layout_create(&gpu_desc);
    free(bindings);
    free(push_ranges);
    if (!layout->gpu_layout) {
        for (uint32_t i = 0; i < pCreateInfo->setLayoutCount; i++) descriptor_layout_release(layout->set_layouts[i]);
        object_free(pAllocator, layout->set_layouts);
        object_free(pAllocator, layout);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    layout->magic = VX_VK_PIPELINE_LAYOUT_MAGIC;
    layout->device = device;
    layout->refs = 1;
    layout->externally_alive = 1;
    layout->set_layout_count = pCreateInfo->setLayoutCount;
    if (pAllocator) { layout->allocator = *pAllocator; layout->has_allocator = 1; }
    *pPipelineLayout = pipeline_layout_to_handle(layout);
    return VK_SUCCESS;
}

void WINAPI vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout layoutHandle,
    const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    vx_vk_pipeline_layout *layout = pipeline_layout_from_handle(layoutHandle, device);
    if (!layout) return;
    layout->externally_alive = 0;
    for (vx_vk_pipeline_layout_use *use = layout->command_buffer_uses; use; use = use->next)
        if (use->command_buffer && use->command_buffer->state != VX_VK_COMMAND_PENDING)
            use->command_buffer->state = VX_VK_COMMAND_INVALID;
    pipeline_layout_release(layout);
}

static int descriptor_layout_find(const vx_vk_descriptor_layout *layout, uint32_t binding, uint32_t *index) {
    if (!layout) return 0;
    for (uint32_t i = 0; i < layout->binding_count; i++) {
        if (layout->bindings[i].binding != binding) continue;
        if (index) *index = i;
        return 1;
    }
    return 0;
}

static int descriptor_layout_value_index(const vx_vk_descriptor_layout *layout, uint32_t binding_index,
                                         uint32_t array_element, uint32_t *value_index) {
    if (!layout || !value_index || binding_index >= layout->binding_count) return 0;
    uint32_t index = 0;
    for (uint32_t i = 0; i < binding_index; i++) {
        if (layout->bindings[i].descriptorCount > UINT32_MAX - index) return 0;
        index += layout->bindings[i].descriptorCount;
    }
    if (array_element >= layout->bindings[binding_index].descriptorCount ||
        array_element > UINT32_MAX - index) return 0;
    *value_index = index + array_element;
    return *value_index < layout->descriptor_count;
}

static int descriptor_layout_range_indices(const vx_vk_descriptor_layout *layout, uint32_t binding,
    uint32_t array_element, uint32_t descriptor_count, uint32_t *value_indices) {
    if (!layout || !value_indices || !descriptor_count ||
        descriptor_count > VX_VK_MAX_DESCRIPTOR_VALUES) return 0;
    uint32_t binding_index;
    if (!descriptor_layout_find(layout, binding, &binding_index)) return 0;
    const VkDescriptorSetLayoutBinding *first = &layout->bindings[binding_index];
    if (!first->descriptorCount || array_element > first->descriptorCount) return 0;
    uint32_t current_array_element = array_element;
    for (uint32_t element = 0; element < descriptor_count; element++) {
        while (binding_index < layout->binding_count &&
               current_array_element == layout->bindings[binding_index].descriptorCount) {
            uint32_t previous = binding_index++;
            if (binding_index >= layout->binding_count || layout->bindings[previous].binding == UINT32_MAX ||
                layout->bindings[binding_index].binding != layout->bindings[previous].binding + 1u) return 0;
            current_array_element = 0;
        }
        if (binding_index >= layout->binding_count) return 0;
        const VkDescriptorSetLayoutBinding *current = &layout->bindings[binding_index];
        if (current_array_element >= current->descriptorCount || current->descriptorType != first->descriptorType ||
            current->stageFlags != first->stageFlags ||
            (!!current->pImmutableSamplers != !!first->pImmutableSamplers) ||
            !descriptor_layout_value_index(layout, binding_index, current_array_element,
                &value_indices[element])) return 0;
        current_array_element++;
    }
    return 1;
}

static int descriptor_layouts_compatible(const vx_vk_descriptor_layout *a, const vx_vk_descriptor_layout *b) {
    if (a == b) return 1;
    if (!a || !b || a->binding_count != b->binding_count) return 0;
    for (uint32_t i = 0; i < a->binding_count; i++) {
        const VkDescriptorSetLayoutBinding *left = &a->bindings[i];
        const VkDescriptorSetLayoutBinding *right = &b->bindings[i];
        if (left->binding != right->binding || left->descriptorType != right->descriptorType ||
            left->descriptorCount != right->descriptorCount || left->stageFlags != right->stageFlags)
            return 0;
    }
    return 1;
}

static const spirv_id_record *vk_find_specialization_constant(const spirv_module *module, uint32_t constant_id) {
    if (!module || !module->id_records) return NULL;
    const spirv_id_record *slots = module->id_records;
    for (size_t i = 0; i < module->id_capacity; i++)
        if (slots[i].id && slots[i].kind == SPIRV_ID_CONSTANT && slots[i].is_specialization &&
            slots[i].has_specialization_id && slots[i].specialization_id == constant_id) return &slots[i];
    return NULL;
}

static int vk_shader_has_result_id(const gpu_ir_shader *shader, uint32_t result_id) {
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (instruction && instruction->result_id == result_id) return 1;
    }
    return 0;
}

static int vk_resolve_specialization_value(const spirv_module *module, uint32_t id,
                                            const gpu_ir_shader_constant_override *overrides,
                                            uint32_t override_count, uint32_t depth, uint32_t *value) {
    if (!module || !value || depth >= module->id_bound) return 0;
    for (uint32_t i = 0; i < override_count; i++) {
        if (overrides[i].result_id != id) continue;
        if (!overrides[i].literal_words || overrides[i].literal_word_count != 1) return 0;
        *value = overrides[i].literal_words[0];
        return 1;
    }
    const spirv_id_record *constant = spirv_find_id(module, id);
    const spirv_id_record *type = constant ? spirv_find_id(module, constant->type_id) : NULL;
    if (!constant || constant->kind != SPIRV_ID_CONSTANT || constant->constant_kind != SPIRV_CONSTANT_SCALAR ||
        !type || type->kind != SPIRV_ID_TYPE || type->type_kind != SPIRV_TYPE_INT || type->width != 32 ||
        constant->literal_word_count != 1 || !constant->literal_words) return 0;
    if (!constant->specialization_opcode) {
        *value = (uint32_t)constant->value_bits;
        return 1;
    }
    if (constant->specialization_operand_count != 2 || !constant->specialization_operand_ids) return 0;
    uint32_t operands[2];
    for (uint32_t i = 0; i < 2; i++)
        if (!vk_resolve_specialization_value(module, constant->specialization_operand_ids[i],
                overrides, override_count, depth + 1u, &operands[i])) return 0;
    switch (constant->specialization_opcode) {
    case 128: *value = operands[0] + operands[1]; return 1;
    case 130: *value = operands[0] - operands[1]; return 1;
    case 132: *value = operands[0] * operands[1]; return 1;
    case 134:
        if (!operands[1]) return 0;
        *value = operands[0] / operands[1];
        return 1;
    case 135: {
        if (!operands[1] || (operands[0] == UINT32_C(0x80000000) && operands[1] == UINT32_MAX)) return 0;
        int64_t dividend = (operands[0] & UINT32_C(0x80000000)) ?
            (int64_t)operands[0] - INT64_C(0x100000000) : operands[0];
        int64_t divisor = (operands[1] & UINT32_C(0x80000000)) ?
            (int64_t)operands[1] - INT64_C(0x100000000) : operands[1];
        *value = (uint32_t)(dividend / divisor);
        return 1;
    }
    case 137:
        if (!operands[1]) return 0;
        *value = operands[0] % operands[1];
        return 1;
    case 194:
        if (operands[1] >= 32) return 0;
        *value = operands[0] >> operands[1];
        return 1;
    case 195:
        if (operands[1] >= 32) return 0;
        *value = operands[1] ? operands[0] >> operands[1] : operands[0];
        if (operands[1] && (operands[0] & UINT32_C(0x80000000)))
            *value |= UINT32_MAX << (32u - operands[1]);
        return 1;
    case 196:
        if (operands[1] >= 32) return 0;
        *value = operands[0] << operands[1];
        return 1;
    case 197: *value = operands[0] | operands[1]; return 1;
    case 198: *value = operands[0] ^ operands[1]; return 1;
    case 199: *value = operands[0] & operands[1]; return 1;
    default: return 0;
    }
}

static VkResult vk_read_specialization_overrides(const spirv_module *module, const VkSpecializationInfo *info,
                                                  const gpu_ir_shader *shader,
                                                  gpu_ir_shader_constant_override **out_overrides,
                                                  uint32_t *out_count, uint32_t **out_values) {
    *out_overrides = NULL;
    *out_count = 0;
    *out_values = NULL;
    if (!info) return VK_SUCCESS;
    if ((info->mapEntryCount && !info->pMapEntries) || (info->dataSize && !info->pData))
        return VK_ERROR_INITIALIZATION_FAILED;
    const VkSpecializationMapEntry *entries = info->pMapEntries;
    uint64_t expression_count = 0;
    const spirv_id_record *slots = module->id_records;
    for (size_t i = 0; i < module->id_capacity; i++)
        if (slots[i].id && slots[i].kind == SPIRV_ID_CONSTANT && slots[i].specialization_opcode &&
            vk_shader_has_result_id(shader, slots[i].id)) expression_count++;
    uint64_t capacity64 = (uint64_t)info->mapEntryCount + expression_count;
    if (capacity64 > UINT32_MAX || capacity64 > SIZE_MAX / sizeof(gpu_ir_shader_constant_override) ||
        capacity64 > SIZE_MAX / sizeof(uint32_t)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t capacity = (uint32_t)capacity64;
    gpu_ir_shader_constant_override *overrides = capacity ? calloc(capacity, sizeof *overrides) : NULL;
    uint32_t *values = capacity ? calloc(capacity, sizeof *values) : NULL;
    if (capacity && (!overrides || !values)) {
        free(overrides); free(values);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < info->mapEntryCount; i++) {
        const VkSpecializationMapEntry *entry = &entries[i];
        for (uint32_t j = 0; j < i; j++) if (entries[j].constantID == entry->constantID) {
            free(overrides); free(values);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if ((size_t)entry->offset >= info->dataSize || entry->size > info->dataSize - entry->offset) {
            free(overrides); free(values);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const spirv_id_record *constant = vk_find_specialization_constant(module, entry->constantID);
        // Vulkan permits map entries for IDs not used by the shader; these entries have no effect.
        if (!constant) continue;
        if (!vk_shader_has_result_id(shader, constant->id)) continue;
        const spirv_id_record *type = spirv_find_id(module, constant->type_id);
        size_t expected_size = 0;
        if (!type || type->kind != SPIRV_ID_TYPE) {
            free(overrides); free(values);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (constant->constant_kind == SPIRV_CONSTANT_BOOL && type->type_kind == SPIRV_TYPE_BOOL)
            expected_size = sizeof(uint32_t);
        else if (constant->constant_kind == SPIRV_CONSTANT_SCALAR &&
                 (type->type_kind == SPIRV_TYPE_INT || type->type_kind == SPIRV_TYPE_FLOAT))
            expected_size = (size_t)(((uint64_t)type->width + 7u) / 8u);
        else {
            free(overrides); free(values);
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if (entry->size != expected_size) {
            free(overrides); free(values);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if ((type->type_kind == SPIRV_TYPE_BOOL && constant->constant_kind != SPIRV_CONSTANT_BOOL) ||
            (type->type_kind != SPIRV_TYPE_BOOL &&
             (type->width != 32 || constant->literal_word_count != 1))) {
            free(overrides); free(values);
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        uint32_t value;
        memcpy(&value, (const uint8_t *)info->pData + entry->offset, sizeof value);
        if (type->type_kind == SPIRV_TYPE_BOOL) {
            if (value > 1) {
                free(overrides); free(values);
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
        values[count] = value;
        overrides[count] = (gpu_ir_shader_constant_override){
            .result_id = constant->id, .literal_words = &values[count], .literal_word_count = 1
        };
        count++;
    }
    for (size_t i = 0; i < module->id_capacity; i++) {
        const spirv_id_record *constant = &slots[i];
        if (!constant->id || constant->kind != SPIRV_ID_CONSTANT || !constant->specialization_opcode ||
            !vk_shader_has_result_id(shader, constant->id)) continue;
        uint32_t value;
        if (count >= capacity || !vk_resolve_specialization_value(module, constant->id,
                overrides, count, 0, &value)) {
            free(overrides); free(values);
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        values[count] = value;
        overrides[count] = (gpu_ir_shader_constant_override){
            .result_id = constant->id, .literal_words = &values[count], .literal_word_count = 1
        };
        count++;
    }
    *out_overrides = overrides;
    *out_count = count;
    *out_values = values;
    return VK_SUCCESS;
}

static VkResult vk_specialize_shader(const spirv_module *module, const VkSpecializationInfo *info,
                                     const gpu_ir_shader *shader, const uint32_t threadgroup_size[3],
                                     gpu_ir_shader **specialized_shader) {
    if (!specialized_shader) return VK_ERROR_INITIALIZATION_FAILED;
    *specialized_shader = NULL;
    gpu_ir_shader_constant_override *overrides = NULL;
    uint32_t override_count = 0, *override_values = NULL;
    VkResult result = vk_read_specialization_overrides(module, info, shader, &overrides,
                                                        &override_count, &override_values);
    if (result == VK_SUCCESS) {
        int rc = gpu_ir_shader_specialize(shader, overrides, override_count, threadgroup_size,
                                          specialized_shader);
        if (rc != GPU_IR_OK) result = rc == GPU_IR_OUT_OF_MEMORY ? VK_ERROR_OUT_OF_HOST_MEMORY :
            rc == GPU_IR_INVALID ? VK_ERROR_INITIALIZATION_FAILED : VK_ERROR_FEATURE_NOT_PRESENT;
    }
    free(overrides);
    free(override_values);
    return result;
}

#ifdef VX_VULKAN_TEST_HOOKS
VkResult vx_vk_test_specialize_shader(const spirv_module *module, const VkSpecializationInfo *info,
                                      const gpu_ir_shader *shader, gpu_ir_shader **specialized_shader) {
    return vk_specialize_shader(module, info, shader, NULL, specialized_shader);
}
#endif

static int vk_specialized_threadgroup_size(const spirv_module *module, const spirv_entry_point *entry,
                                           const gpu_ir_shader_constant_override *overrides,
                                           uint32_t override_count, uint32_t size[3]) {
    if (entry->local_size_kind == SPIRV_LOCAL_SIZE_LITERAL) {
        memcpy(size, entry->local_size, sizeof entry->local_size);
        return size[0] && size[1] && size[2];
    }
    if (entry->local_size_kind != SPIRV_LOCAL_SIZE_IDS || !entry->has_local_size_default) return 0;
    for (uint32_t axis = 0; axis < 3; axis++) {
        const spirv_id_record *constant = spirv_find_id(module, entry->local_size[axis]);
        const spirv_id_record *type = constant ? spirv_find_id(module, constant->type_id) : NULL;
        if (!constant || constant->kind != SPIRV_ID_CONSTANT || constant->constant_kind != SPIRV_CONSTANT_SCALAR ||
            !type || type->kind != SPIRV_ID_TYPE || type->type_kind != SPIRV_TYPE_INT || type->width != 32 ||
            type->signedness || entry->local_size_default[axis] > UINT32_MAX) return 0;
        uint32_t value = (uint32_t)entry->local_size_default[axis];
        for (uint32_t i = 0; i < override_count; i++) if (overrides[i].result_id == constant->id) {
            value = overrides[i].literal_words[0];
            break;
        }
        if (!value) return 0;
        size[axis] = value;
    }
    return 1;
}

static const gpu_ir_shader_instruction *vk_shader_instruction_result(const gpu_ir_shader *shader, uint32_t id) {
    if (!shader || !id) return NULL;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (instruction && instruction->result_id == id) return instruction;
    }
    return NULL;
}

static int vk_shader_instruction_id_operand(const gpu_ir_shader *shader,
                                            const gpu_ir_shader_instruction *instruction,
                                            uint32_t operand_index, uint32_t *id) {
    if (!shader || !instruction || !id || operand_index >= instruction->operand_count) return 0;
    const gpu_ir_shader_operand *operand =
        gpu_ir_shader_operand_at(shader, instruction->first_operand + operand_index);
    if (!operand || operand->kind != GPU_IR_SHADER_OPERAND_ID || !operand->value) return 0;
    *id = operand->value;
    return 1;
}

static int vk_shader_value_is_dynamically_uniform(const gpu_ir_shader *shader, uint32_t value_id,
                                                  uint32_t depth);

static int vk_shader_pointer_is_workgroup_uniform(const gpu_ir_shader *shader, uint32_t pointer_id,
                                                   uint32_t depth) {
    if (!shader || !pointer_id || depth > 32) return 0;
    const gpu_ir_shader_instruction *definition = vk_shader_instruction_result(shader, pointer_id);
    if (!definition) return 0;
    if (definition->opcode == GPU_IR_SHADER_OP_VARIABLE)
        return definition->storage_class == GPU_IR_SHADER_STORAGE_INPUT && definition->has_builtin &&
            (definition->builtin == GPU_IR_SHADER_BUILTIN_NUM_WORKGROUPS ||
             definition->builtin == GPU_IR_SHADER_BUILTIN_WORKGROUP_SIZE ||
             definition->builtin == GPU_IR_SHADER_BUILTIN_WORKGROUP_ID);
    if ((definition->opcode != GPU_IR_SHADER_OP_ACCESS_CHAIN &&
         definition->opcode != GPU_IR_SHADER_OP_COPY) || !definition->operand_count) return 0;
    uint32_t base_id = 0;
    if (!vk_shader_instruction_id_operand(shader, definition, 0, &base_id) ||
        !vk_shader_pointer_is_workgroup_uniform(shader, base_id, depth + 1)) return 0;
    for (uint32_t i = 1; i < definition->operand_count; i++) {
        uint32_t index_id = 0;
        if (!vk_shader_instruction_id_operand(shader, definition, i, &index_id) ||
            !vk_shader_value_is_dynamically_uniform(shader, index_id, depth + 1)) return 0;
    }
    return 1;
}

static int vk_shader_value_is_dynamically_uniform(const gpu_ir_shader *shader, uint32_t value_id,
                                                  uint32_t depth) {
    if (!shader || !value_id || depth > 32) return 0;
    const gpu_ir_shader_instruction *definition = vk_shader_instruction_result(shader, value_id);
    if (!definition) return 0;
    if (definition->opcode == GPU_IR_SHADER_OP_CONSTANT) return 1;
    if (definition->opcode == GPU_IR_SHADER_OP_LOAD) {
        uint32_t pointer_id = 0;
        return definition->operand_count == 1 &&
            vk_shader_instruction_id_operand(shader, definition, 0, &pointer_id) &&
            vk_shader_pointer_is_workgroup_uniform(shader, pointer_id, depth + 1);
    }
    if (definition->opcode == GPU_IR_SHADER_OP_COPY || definition->opcode == GPU_IR_SHADER_OP_CONVERT ||
        (definition->opcode == GPU_IR_SHADER_OP_COMPOSITE &&
         definition->operation == GPU_IR_SHADER_COMPOSITE_EXTRACT)) {
        uint32_t source_id = 0;
        return definition->operand_count == 1 &&
            vk_shader_instruction_id_operand(shader, definition, 0, &source_id) &&
            vk_shader_value_is_dynamically_uniform(shader, source_id, depth + 1);
    }
    if (definition->opcode != GPU_IR_SHADER_OP_ARITHMETIC &&
        definition->opcode != GPU_IR_SHADER_OP_COMPARE &&
        definition->opcode != GPU_IR_SHADER_OP_SELECT &&
        !(definition->opcode == GPU_IR_SHADER_OP_COMPOSITE &&
          definition->operation == GPU_IR_SHADER_COMPOSITE_CONSTRUCT)) return 0;
    if (!definition->operand_count || definition->operand_count > 8) return 0;
    for (uint32_t i = 0; i < definition->operand_count; i++) {
        uint32_t operand_id = 0;
        if (!vk_shader_instruction_id_operand(shader, definition, i, &operand_id) ||
            !vk_shader_value_is_dynamically_uniform(shader, operand_id, depth + 1)) return 0;
    }
    return 1;
}

// Return 1 for a supported dynamically uniform index, 0 if the shader uses constants only, and
// -1 when an index is dynamic but the frontend cannot prove Vulkan's uniform-index requirement.
static int vk_shader_uses_dynamic_sampled_descriptor_index(const gpu_ir_shader *shader) {
    int uses_dynamic_index = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *access = gpu_ir_shader_instruction_at(shader, i);
        if (!access || access->opcode != GPU_IR_SHADER_OP_ACCESS_CHAIN || access->operand_count < 2) continue;
        uint32_t base_id = 0;
        if (!vk_shader_instruction_id_operand(shader, access, 0, &base_id)) continue;
        const gpu_ir_shader_instruction *base = vk_shader_instruction_result(shader, base_id);
        if (!base || base->opcode != GPU_IR_SHADER_OP_VARIABLE ||
            base->storage_class != GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT) continue;
        const gpu_ir_shader_type *pointer = gpu_ir_shader_find_type(shader, base->result_type_id);
        const gpu_ir_shader_type *array = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER &&
            pointer->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT ?
            gpu_ir_shader_find_type(shader, pointer->element_type_id) : NULL;
        const gpu_ir_shader_type *element = array && array->kind == GPU_IR_SHADER_TYPE_ARRAY ?
            gpu_ir_shader_find_type(shader, array->element_type_id) : NULL;
        int sampled_descriptor = element &&
            (element->kind == GPU_IR_SHADER_TYPE_SAMPLER ||
             element->kind == GPU_IR_SHADER_TYPE_SAMPLED_IMAGE ||
             (element->kind == GPU_IR_SHADER_TYPE_IMAGE && element->image_sampled == 1));
        if (!array || array->kind != GPU_IR_SHADER_TYPE_ARRAY || !sampled_descriptor) continue;
        uint32_t index_id = 0;
        if (!vk_shader_instruction_id_operand(shader, access, 1, &index_id)) return -1;
        const gpu_ir_shader_instruction *index = vk_shader_instruction_result(shader, index_id);
        if (index && index->opcode == GPU_IR_SHADER_OP_CONSTANT) continue;
        if (!vk_shader_value_is_dynamically_uniform(shader, index_id, 0)) return -1;
        uses_dynamic_index = 1;
    }
    return uses_dynamic_index;
}

static int vk_shader_uses_dynamic_storage_buffer_descriptor_index(const gpu_ir_shader *shader) {
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *access = gpu_ir_shader_instruction_at(shader, i);
        if (!access || access->opcode != GPU_IR_SHADER_OP_ACCESS_CHAIN || access->operand_count < 2) continue;
        const gpu_ir_shader_operand *base_operand =
            gpu_ir_shader_operand_at(shader, access->first_operand);
        if (!base_operand || base_operand->kind != GPU_IR_SHADER_OPERAND_ID) continue;
        const gpu_ir_shader_instruction *base = vk_shader_instruction_result(shader, base_operand->value);
        if (!base || base->opcode != GPU_IR_SHADER_OP_VARIABLE ||
            base->storage_class != GPU_IR_SHADER_STORAGE_BUFFER) continue;
        const gpu_ir_shader_type *pointer = gpu_ir_shader_find_type(shader, base->result_type_id);
        const gpu_ir_shader_type *array = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER &&
            pointer->storage_class == GPU_IR_SHADER_STORAGE_BUFFER ?
            gpu_ir_shader_find_type(shader, pointer->element_type_id) : NULL;
        if (!array || array->kind != GPU_IR_SHADER_TYPE_ARRAY) continue;
        const gpu_ir_shader_operand *index_operand =
            gpu_ir_shader_operand_at(shader, access->first_operand + 1);
        if (!index_operand || index_operand->kind != GPU_IR_SHADER_OPERAND_ID) return 1;
        const gpu_ir_shader_instruction *index = vk_shader_instruction_result(shader, index_operand->value);
        if (!index || index->opcode != GPU_IR_SHADER_OP_CONSTANT) return 1;
    }
    return 0;
}

static int vk_shader_uses_dynamic_storage_image_descriptor_index(const gpu_ir_shader *shader) {
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *access = gpu_ir_shader_instruction_at(shader, i);
        if (!access || access->opcode != GPU_IR_SHADER_OP_ACCESS_CHAIN || access->operand_count < 2) continue;
        const gpu_ir_shader_operand *base_operand =
            gpu_ir_shader_operand_at(shader, access->first_operand);
        if (!base_operand || base_operand->kind != GPU_IR_SHADER_OPERAND_ID) continue;
        const gpu_ir_shader_instruction *base = vk_shader_instruction_result(shader, base_operand->value);
        if (!base || base->opcode != GPU_IR_SHADER_OP_VARIABLE ||
            base->storage_class != GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT) continue;
        const gpu_ir_shader_type *pointer = gpu_ir_shader_find_type(shader, base->result_type_id);
        const gpu_ir_shader_type *array = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER &&
            pointer->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT ?
            gpu_ir_shader_find_type(shader, pointer->element_type_id) : NULL;
        const gpu_ir_shader_type *image = array && array->kind == GPU_IR_SHADER_TYPE_ARRAY ?
            gpu_ir_shader_find_type(shader, array->element_type_id) : NULL;
        if (!array || array->kind != GPU_IR_SHADER_TYPE_ARRAY || !image ||
            image->kind != GPU_IR_SHADER_TYPE_IMAGE || image->image_sampled != 2) continue;
        const gpu_ir_shader_operand *index_operand =
            gpu_ir_shader_operand_at(shader, access->first_operand + 1);
        if (!index_operand || index_operand->kind != GPU_IR_SHADER_OPERAND_ID) return 1;
        const gpu_ir_shader_instruction *index = vk_shader_instruction_result(shader, index_operand->value);
        if (!index || index->opcode != GPU_IR_SHADER_OP_CONSTANT) return 1;
    }
    return 0;
}

static int vk_shader_uses_subgroup_builtin(const gpu_ir_shader *shader) {
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction || instruction->opcode != GPU_IR_SHADER_OP_VARIABLE || !instruction->has_builtin)
            continue;
        if (instruction->builtin == GPU_IR_SHADER_BUILTIN_SUBGROUP_SIZE ||
            instruction->builtin == GPU_IR_SHADER_BUILTIN_SUBGROUP_LOCAL_INVOCATION_ID) return 1;
    }
    return 0;
}

VkResult WINAPI vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkComputePipelineCreateInfo *pCreateInfos, const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines) {
    if (!device_is_valid(device) || pipelineCache || !createInfoCount || !pCreateInfos || !pPipelines ||
        !allocator_is_valid(pAllocator)) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < createInfoCount; i++) pPipelines[i] = 0;
    VkResult result = VK_SUCCESS;
    uint32_t created = 0;
    for (uint32_t i = 0; i < createInfoCount; i++) {
        const VkComputePipelineCreateInfo *info = &pCreateInfos[i];
        vx_vk_shader_module *module = shader_module_from_handle(info->stage.module, device);
        vx_vk_pipeline_layout *layout = pipeline_layout_from_handle(info->layout, device);
        if (!module || !layout || info->sType != VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO || info->pNext ||
            info->flags || info->stage.sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO ||
            info->stage.pNext || info->stage.flags || info->stage.stage != VK_SHADER_STAGE_COMPUTE_BIT ||
            !info->stage.pName || !info->stage.pName[0] ||
            info->basePipelineHandle) {
            result = VK_ERROR_INITIALIZATION_FAILED;
            break;
        }
        const spirv_entry_point *entry = spirv_find_entry_point(&module->module, info->stage.pName,
                                                                 SPIRV_EXECUTION_MODEL_GL_COMPUTE);
        if (!entry) { result = VK_ERROR_FEATURE_NOT_PRESENT; break; }
        gpu_ir_pipeline_layout *reflected_layout = NULL;
        gpu_ir_shader *shader = NULL;
        int rc = spirv_build_gpu_ir_layout(&module->module, entry, &reflected_layout);
        if (rc == GPU_IR_OK) rc = spirv_build_gpu_ir_shader(&module->module, entry, &shader);
        // This frontend exposes Vulkan 1.0 without subgroup extensions or subgroup-property queries.
        if (rc == GPU_IR_OK && vk_shader_uses_subgroup_builtin(shader)) rc = GPU_IR_UNSUPPORTED;
        if (rc != GPU_IR_OK || !reflected_layout || !shader) {
            gpu_ir_pipeline_layout_release(reflected_layout);
            gpu_ir_shader_release(shader);
            result = rc == GPU_IR_OUT_OF_MEMORY ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_FEATURE_NOT_PRESENT;
            break;
        }

        gpu_ir_shader_constant_override *overrides = NULL;
        uint32_t override_count = 0, *override_values = NULL;
        VkResult specialization_result = vk_read_specialization_overrides(&module->module,
            info->stage.pSpecializationInfo, shader, &overrides, &override_count, &override_values);
        uint32_t specialized_size[3] = { 0, 0, 0 };
        if (specialization_result == VK_SUCCESS &&
            !vk_specialized_threadgroup_size(&module->module, entry, overrides, override_count, specialized_size))
            specialization_result = VK_ERROR_FEATURE_NOT_PRESENT;
        gpu_ir_shader *specialized_shader = NULL;
        if (specialization_result == VK_SUCCESS) {
            rc = gpu_ir_shader_specialize(shader, overrides, override_count, specialized_size, &specialized_shader);
            if (rc != GPU_IR_OK) specialization_result = rc == GPU_IR_OUT_OF_MEMORY ?
                VK_ERROR_OUT_OF_HOST_MEMORY : rc == GPU_IR_INVALID ? VK_ERROR_INITIALIZATION_FAILED :
                VK_ERROR_FEATURE_NOT_PRESENT;
        }
        free(overrides);
        free(override_values);
        gpu_ir_shader_release(shader);
        shader = specialized_shader;
        if (specialization_result != VK_SUCCESS || !shader) {
            gpu_ir_pipeline_layout_release(reflected_layout);
            gpu_ir_shader_release(shader);
            result = specialization_result == VK_SUCCESS ? VK_ERROR_FEATURE_NOT_PRESENT : specialization_result;
            break;
        }

        int sampled_indexing = vk_shader_uses_dynamic_sampled_descriptor_index(shader);
        if (sampled_indexing < 0 ||
            (sampled_indexing && !device->shader_sampled_image_array_dynamic_indexing)) {
            gpu_ir_pipeline_layout_release(reflected_layout);
            gpu_ir_shader_release(shader);
            result = VK_ERROR_FEATURE_NOT_PRESENT;
            break;
        }

        if (!device->shader_storage_buffer_array_dynamic_indexing &&
            vk_shader_uses_dynamic_storage_buffer_descriptor_index(shader)) {
            gpu_ir_pipeline_layout_release(reflected_layout);
            gpu_ir_shader_release(shader);
            result = VK_ERROR_FEATURE_NOT_PRESENT;
            break;
        }
        if (!device->shader_storage_image_array_dynamic_indexing &&
            vk_shader_uses_dynamic_storage_image_descriptor_index(shader)) {
            gpu_ir_pipeline_layout_release(reflected_layout);
            gpu_ir_shader_release(shader);
            result = VK_ERROR_FEATURE_NOT_PRESENT;
            break;
        }

        uint32_t local_size[3] = { 0 }, axis_limit[3] = { 0 };
        gpu_ir_shader_threadgroup_size(shader, local_size);
        gpu_ir_metal_max_threads_per_threadgroup(device->physical_device->owner->metal, axis_limit);
        uint64_t local_invocations = (uint64_t)local_size[0] * local_size[1] * local_size[2];
        if (!local_size[0] || !local_size[1] || !local_size[2] ||
            local_size[0] > axis_limit[0] || local_size[1] > axis_limit[1] || local_size[2] > axis_limit[2] ||
            local_invocations > 128) {
            gpu_ir_pipeline_layout_release(reflected_layout);
            gpu_ir_shader_release(shader);
            result = VK_ERROR_FEATURE_NOT_PRESENT;
            break;
        }

        int compatible = 1;
        for (uint32_t j = 0; j < gpu_ir_pipeline_layout_binding_count(reflected_layout); j++) {
            gpu_ir_descriptor_binding reflected;
            if (gpu_ir_pipeline_layout_binding_at(reflected_layout, j, &reflected) != GPU_IR_OK ||
                reflected.set >= layout->set_layout_count) { compatible = 0; break; }
            vx_vk_descriptor_layout *set_layout = layout->set_layouts[reflected.set];
            uint32_t binding_index;
            if (!descriptor_layout_find(set_layout, reflected.binding, &binding_index)) { compatible = 0; break; }
            const VkDescriptorSetLayoutBinding *actual = &set_layout->bindings[binding_index];
            int descriptor_kind_matches =
                (reflected.kind == GPU_IR_DESCRIPTOR_UNIFORM_BUFFER &&
                 vk_uniform_descriptor_type(actual->descriptorType)) ||
                (reflected.kind == GPU_IR_DESCRIPTOR_STORAGE_BUFFER &&
                 vk_storage_descriptor_type(actual->descriptorType)) ||
                (reflected.kind == GPU_IR_DESCRIPTOR_STORAGE_IMAGE &&
                 actual->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) ||
                (reflected.kind == GPU_IR_DESCRIPTOR_SAMPLED_IMAGE &&
                 actual->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) ||
                (reflected.kind == GPU_IR_DESCRIPTOR_SAMPLER &&
                 actual->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) ||
                (reflected.kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER &&
                 actual->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            if (!descriptor_kind_matches ||
                actual->descriptorCount < reflected.count ||
                !(actual->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) ||
                !reflected.count ||
                reflected.count == GPU_IR_DESCRIPTOR_COUNT_UNBOUNDED) {
                compatible = 0;
                break;
            }
        }
        for (uint32_t j = 0; compatible &&
             j < gpu_ir_pipeline_layout_push_constant_count(reflected_layout); j++) {
            gpu_ir_push_constant_range reflected;
            if (gpu_ir_pipeline_layout_push_constant_at(reflected_layout, j, &reflected) != GPU_IR_OK ||
                !pipeline_layout_covers_push_constants(layout, reflected.offset, reflected.size,
                                                       reflected.stage_mask))
                compatible = 0;
        }
        if (!compatible) {
            gpu_ir_pipeline_layout_release(reflected_layout);
            gpu_ir_shader_release(shader);
            result = VK_ERROR_FEATURE_NOT_PRESENT;
            break;
        }

        char error[512] = { 0 };
        gpu_ir_pipeline *gpu_pipeline = gpu_ir_metal_compute_pipeline_create(
            device->physical_device->owner->metal, shader, layout->gpu_layout, error, sizeof error);
        gpu_ir_shader_release(shader);
        if (!gpu_pipeline) {
            if (error[0]) fprintf(stderr, "[vxrt] Vulkan: compute pipeline translation failed: %s\n", error);
            gpu_ir_pipeline_layout_release(reflected_layout);
            result = VK_ERROR_FEATURE_NOT_PRESENT;
            break;
        }
        vx_vk_pipeline *pipeline = object_allocate(pAllocator, sizeof *pipeline, alignof(vx_vk_pipeline),
                                                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
        if (!pipeline) {
            gpu_ir_pipeline_release(gpu_pipeline);
            gpu_ir_pipeline_layout_release(reflected_layout);
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            break;
        }
        if (layout->set_layout_count) {
            pipeline->set_layouts = object_allocate(pAllocator,
                (size_t)layout->set_layout_count * sizeof *pipeline->set_layouts,
                alignof(vx_vk_descriptor_layout *), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
            if (!pipeline->set_layouts) {
                object_free(pAllocator, pipeline);
                gpu_ir_pipeline_release(gpu_pipeline);
                gpu_ir_pipeline_layout_release(reflected_layout);
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
                break;
            }
            memcpy(pipeline->set_layouts, layout->set_layouts,
                   (size_t)layout->set_layout_count * sizeof *pipeline->set_layouts);
            for (uint32_t j = 0; j < layout->set_layout_count; j++) descriptor_layout_retain(pipeline->set_layouts[j]);
        }
        pipeline->magic = VX_VK_PIPELINE_MAGIC;
        pipeline->device = device;
        pipeline->refs = 1;
        pipeline->externally_alive = 1;
        pipeline->set_layout_count = layout->set_layout_count;
        pipeline->bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
        pipeline->layout = layout;
        pipeline_layout_retain(layout);
        pipeline->gpu_pipeline = gpu_pipeline;
        pipeline->reflected_layout = reflected_layout;
        if (pAllocator) { pipeline->allocator = *pAllocator; pipeline->has_allocator = 1; }
        pPipelines[i] = pipeline_to_handle(pipeline);
        created++;
    }
    if (result != VK_SUCCESS) {
        for (uint32_t i = 0; i < created; i++) {
            vx_vk_pipeline *pipeline = pipeline_from_handle(pPipelines[i], device);
            if (pipeline) { pipeline->externally_alive = 0; pipeline_release(pipeline); }
            pPipelines[i] = 0;
        }
    }
    return result;
}

static int vk_graphics_topology(VkPrimitiveTopology topology, uint32_t *gpu_topology) {
    if (!gpu_topology) return 0;
    switch (topology) {
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
            *gpu_topology = GPU_IR_TOPOLOGY_TRIANGLE_LIST;
            return 1;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
            *gpu_topology = GPU_IR_TOPOLOGY_TRIANGLE_STRIP;
            return 1;
        default:
            return 0;
    }
}

static int vk_viewport_is_valid(VkDevice device, const VkViewport *viewport) {
    if (!device || !viewport) return 0;
    VkPhysicalDeviceProperties properties = { 0 };
    vkGetPhysicalDeviceProperties(device->physical_device, &properties);
    return isfinite(viewport->x) && isfinite(viewport->y) &&
        isfinite(viewport->width) && isfinite(viewport->height) &&
        isfinite(viewport->minDepth) && isfinite(viewport->maxDepth) &&
        viewport->width > 0.0f && viewport->height != 0.0f &&
        viewport->width <= (float)properties.limits.maxViewportDimensions[0] &&
        fabsf(viewport->height) <= (float)properties.limits.maxViewportDimensions[1] &&
        (viewport->height > 0.0f || device->maintenance1_enabled) &&
        viewport->x >= properties.limits.viewportBoundsRange[0] &&
        (double)viewport->x + viewport->width <= properties.limits.viewportBoundsRange[1] &&
        viewport->y >= properties.limits.viewportBoundsRange[0] &&
        viewport->y <= properties.limits.viewportBoundsRange[1] &&
        (double)viewport->y + viewport->height >= properties.limits.viewportBoundsRange[0] &&
        (double)viewport->y + viewport->height <= properties.limits.viewportBoundsRange[1] &&
        viewport->minDepth >= 0.0f && viewport->minDepth <= 1.0f &&
        viewport->maxDepth >= 0.0f && viewport->maxDepth <= 1.0f;
}

static int vk_scissor_is_valid(const VkRect2D *scissor) {
    return scissor && scissor->offset.x >= 0 && scissor->offset.y >= 0 &&
        (int64_t)scissor->offset.x + scissor->extent.width <= INT32_MAX &&
        (int64_t)scissor->offset.y + scissor->extent.height <= INT32_MAX;
}

static int vk_build_vertex_input(const VkPipelineVertexInputStateCreateInfo *source,
                                 const VkPhysicalDeviceLimits *limits,
                                 gpu_ir_vertex_input_desc *destination) {
    if (!source || !limits || !destination ||
        (source->vertexBindingDescriptionCount && !source->pVertexBindingDescriptions) ||
        (source->vertexAttributeDescriptionCount && !source->pVertexAttributeDescriptions)) return GPU_IR_INVALID;
    memset(destination, 0, sizeof *destination);
    if (source->vertexBindingDescriptionCount > GPU_IR_MAX_VERTEX_BUFFERS ||
        source->vertexAttributeDescriptionCount > GPU_IR_MAX_VERTEX_ATTRIBUTES ||
        source->vertexBindingDescriptionCount > limits->maxVertexInputBindings ||
        source->vertexAttributeDescriptionCount > limits->maxVertexInputAttributes) return GPU_IR_UNSUPPORTED;
    destination->binding_count = source->vertexBindingDescriptionCount;
    destination->attribute_count = source->vertexAttributeDescriptionCount;
    for (uint32_t i = 0; i < source->vertexBindingDescriptionCount; i++) {
        const VkVertexInputBindingDescription *binding = &source->pVertexBindingDescriptions[i];
        if (binding->binding >= limits->maxVertexInputBindings ||
            binding->stride > limits->maxVertexInputBindingStride) return GPU_IR_UNSUPPORTED;
        uint32_t input_rate;
        if (binding->inputRate == VK_VERTEX_INPUT_RATE_VERTEX) input_rate = GPU_IR_VERTEX_INPUT_RATE_VERTEX;
        else if (binding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE) input_rate = GPU_IR_VERTEX_INPUT_RATE_INSTANCE;
        else return GPU_IR_UNSUPPORTED;
        for (uint32_t previous = 0; previous < i; previous++)
            if (source->pVertexBindingDescriptions[previous].binding == binding->binding) return GPU_IR_INVALID;
        destination->bindings[i] = (gpu_ir_vertex_binding_desc){
            .binding = binding->binding,
            .stride = binding->stride,
            .input_rate = input_rate,
            .step_rate = 1
        };
    }
    for (uint32_t i = 0; i < source->vertexAttributeDescriptionCount; i++) {
        const VkVertexInputAttributeDescription *attribute = &source->pVertexAttributeDescriptions[i];
        uint32_t gpu_format = 0;
        int rc = vk_vertex_format_to_gpu(attribute->format, &gpu_format);
        if (rc != GPU_IR_OK) return rc;
        if (attribute->location >= limits->maxVertexInputAttributes ||
            attribute->binding >= limits->maxVertexInputBindings ||
            attribute->offset > limits->maxVertexInputAttributeOffset) return GPU_IR_UNSUPPORTED;
        int binding_found = 0;
        for (uint32_t binding = 0; binding < destination->binding_count; binding++)
            if (destination->bindings[binding].binding == attribute->binding) binding_found = 1;
        if (!binding_found) return GPU_IR_INVALID;
        for (uint32_t previous = 0; previous < i; previous++)
            if (source->pVertexAttributeDescriptions[previous].location == attribute->location)
                return GPU_IR_INVALID;
        destination->attributes[i] = (gpu_ir_vertex_attribute_desc){
            .location = attribute->location,
            .binding = attribute->binding,
            .format = gpu_format,
            .offset = attribute->offset
        };
    }
    return GPU_IR_OK;
}

static int vk_blend_factor_to_gpu(VkBlendFactor factor, uint32_t *destination) {
    if (!destination) return GPU_IR_INVALID;
    switch (factor) {
    case VK_BLEND_FACTOR_ZERO: *destination = GPU_IR_BLEND_FACTOR_ZERO; break;
    case VK_BLEND_FACTOR_ONE: *destination = GPU_IR_BLEND_FACTOR_ONE; break;
    case VK_BLEND_FACTOR_SRC_COLOR: *destination = GPU_IR_BLEND_FACTOR_SOURCE_COLOR; break;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: *destination = GPU_IR_BLEND_FACTOR_ONE_MINUS_SOURCE_COLOR; break;
    case VK_BLEND_FACTOR_DST_COLOR: *destination = GPU_IR_BLEND_FACTOR_DESTINATION_COLOR; break;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: *destination = GPU_IR_BLEND_FACTOR_ONE_MINUS_DESTINATION_COLOR; break;
    case VK_BLEND_FACTOR_SRC_ALPHA: *destination = GPU_IR_BLEND_FACTOR_SOURCE_ALPHA; break;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: *destination = GPU_IR_BLEND_FACTOR_ONE_MINUS_SOURCE_ALPHA; break;
    case VK_BLEND_FACTOR_DST_ALPHA: *destination = GPU_IR_BLEND_FACTOR_DESTINATION_ALPHA; break;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: *destination = GPU_IR_BLEND_FACTOR_ONE_MINUS_DESTINATION_ALPHA; break;
    case VK_BLEND_FACTOR_CONSTANT_COLOR: *destination = GPU_IR_BLEND_FACTOR_CONSTANT_COLOR; break;
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: *destination = GPU_IR_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; break;
    case VK_BLEND_FACTOR_CONSTANT_ALPHA: *destination = GPU_IR_BLEND_FACTOR_CONSTANT_ALPHA; break;
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: *destination = GPU_IR_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA; break;
    case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE: *destination = GPU_IR_BLEND_FACTOR_SOURCE_ALPHA_SATURATE; break;
    // dualSrcBlend is not advertised by this implementation, so these valid Vulkan factors remain unsupported.
    case VK_BLEND_FACTOR_SRC1_COLOR:
    case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:
    case VK_BLEND_FACTOR_SRC1_ALPHA:
    case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA: return GPU_IR_UNSUPPORTED;
    default: return GPU_IR_UNSUPPORTED;
    }
    return GPU_IR_OK;
}

static int vk_blend_operation_to_gpu(VkBlendOp operation, uint32_t *destination) {
    if (!destination) return GPU_IR_INVALID;
    switch (operation) {
    case VK_BLEND_OP_ADD: *destination = GPU_IR_BLEND_OPERATION_ADD; break;
    case VK_BLEND_OP_SUBTRACT: *destination = GPU_IR_BLEND_OPERATION_SUBTRACT; break;
    case VK_BLEND_OP_REVERSE_SUBTRACT: *destination = GPU_IR_BLEND_OPERATION_REVERSE_SUBTRACT; break;
    case VK_BLEND_OP_MIN: *destination = GPU_IR_BLEND_OPERATION_MINIMUM; break;
    case VK_BLEND_OP_MAX: *destination = GPU_IR_BLEND_OPERATION_MAXIMUM; break;
    default: return GPU_IR_UNSUPPORTED;
    }
    return GPU_IR_OK;
}

static int vk_build_color_blend(const VkPipelineColorBlendStateCreateInfo *source,
                                gpu_ir_color_blend_state *destination) {
    if (!source || !destination || (source->attachmentCount && !source->pAttachments)) return GPU_IR_INVALID;
    memset(destination, 0, sizeof *destination);
    if (source->logicOpEnable > 1) return GPU_IR_INVALID;
    if (source->logicOp < VK_LOGIC_OP_CLEAR || source->logicOp > VK_LOGIC_OP_SET) return GPU_IR_INVALID;
    if (source->logicOpEnable || source->attachmentCount != 1) return GPU_IR_UNSUPPORTED;
    const VkPipelineColorBlendAttachmentState *attachment = &source->pAttachments[0];
    const VkColorComponentFlags all_components = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (attachment->blendEnable > 1 || (attachment->colorWriteMask & ~all_components)) return GPU_IR_INVALID;
    destination->attachment_count = 1;
    memcpy(destination->blend_constants, source->blendConstants, sizeof destination->blend_constants);
    gpu_ir_color_blend_attachment *normalized = &destination->attachments[0];
    normalized->enabled = attachment->blendEnable;
    normalized->write_mask = (attachment->colorWriteMask & VK_COLOR_COMPONENT_R_BIT ? GPU_IR_COLOR_WRITE_RED : 0) |
        (attachment->colorWriteMask & VK_COLOR_COMPONENT_G_BIT ? GPU_IR_COLOR_WRITE_GREEN : 0) |
        (attachment->colorWriteMask & VK_COLOR_COMPONENT_B_BIT ? GPU_IR_COLOR_WRITE_BLUE : 0) |
        (attachment->colorWriteMask & VK_COLOR_COMPONENT_A_BIT ? GPU_IR_COLOR_WRITE_ALPHA : 0);
    int rc = vk_blend_factor_to_gpu(attachment->srcColorBlendFactor, &normalized->source_color_factor);
    if (rc == GPU_IR_OK) rc = vk_blend_factor_to_gpu(attachment->dstColorBlendFactor,
                                                       &normalized->destination_color_factor);
    if (rc == GPU_IR_OK) rc = vk_blend_factor_to_gpu(attachment->srcAlphaBlendFactor,
                                                       &normalized->source_alpha_factor);
    if (rc == GPU_IR_OK) rc = vk_blend_factor_to_gpu(attachment->dstAlphaBlendFactor,
                                                       &normalized->destination_alpha_factor);
    if (rc == GPU_IR_OK) rc = vk_blend_operation_to_gpu(attachment->colorBlendOp, &normalized->color_operation);
    if (rc == GPU_IR_OK) rc = vk_blend_operation_to_gpu(attachment->alphaBlendOp, &normalized->alpha_operation);
    if (rc != GPU_IR_OK) return rc;
    return gpu_ir_color_blend_state_valid(destination) ? GPU_IR_OK : GPU_IR_INVALID;
}

static int vk_blend_factor_uses_constants(uint32_t factor) {
    return factor == GPU_IR_BLEND_FACTOR_CONSTANT_COLOR ||
        factor == GPU_IR_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR ||
        factor == GPU_IR_BLEND_FACTOR_CONSTANT_ALPHA ||
        factor == GPU_IR_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
}

static int vk_color_blend_uses_constants(const gpu_ir_color_blend_state *state) {
    if (!state) return 0;
    for (uint32_t i = 0; i < state->attachment_count; i++) {
        const gpu_ir_color_blend_attachment *attachment = &state->attachments[i];
        if (attachment->enabled &&
            (vk_blend_factor_uses_constants(attachment->source_color_factor) ||
             vk_blend_factor_uses_constants(attachment->destination_color_factor) ||
             vk_blend_factor_uses_constants(attachment->source_alpha_factor) ||
             vk_blend_factor_uses_constants(attachment->destination_alpha_factor))) return 1;
    }
    return 0;
}

static int vk_resolve_draw_blend_constants(uint32_t dynamic_enabled, uint32_t dynamic_set,
                                           const gpu_ir_color_blend_state *blend,
                                           const float dynamic_values[4], uint32_t *has_constants,
                                           float constants[4]) {
    if (dynamic_enabled > 1 || dynamic_set > 1 || !blend || !has_constants || !constants)
        return GPU_IR_INVALID;
    *has_constants = 0;
    memset(constants, 0, 4 * sizeof constants[0]);
    if (!dynamic_enabled) return GPU_IR_OK;
    if (!dynamic_set) return vk_color_blend_uses_constants(blend) ? GPU_IR_INVALID : GPU_IR_OK;
    if (!dynamic_values) return GPU_IR_INVALID;
    *has_constants = 1;
    memcpy(constants, dynamic_values, 4 * sizeof constants[0]);
    return GPU_IR_OK;
}

#ifdef VX_VULKAN_TEST_HOOKS
int vx_vk_test_build_color_blend(const VkPipelineColorBlendStateCreateInfo *source,
                                 gpu_ir_color_blend_state *destination) {
    return vk_build_color_blend(source, destination);
}
int vx_vk_test_color_blend_uses_constants(const gpu_ir_color_blend_state *state) {
    return vk_color_blend_uses_constants(state);
}
int vx_vk_test_resolve_draw_blend_constants(uint32_t dynamic_enabled, uint32_t dynamic_set,
    const gpu_ir_color_blend_state *blend, const float dynamic_values[4], uint32_t *has_constants,
    float constants[4]) {
    return vk_resolve_draw_blend_constants(dynamic_enabled, dynamic_set, blend, dynamic_values,
                                           has_constants, constants);
}
#endif

static int vk_validate_vertex_shader_inputs(const gpu_ir_shader *shader,
                                            const gpu_ir_vertex_input_desc *vertex_input) {
    if (!shader || !vertex_input || gpu_ir_shader_stage(shader) != GPU_IR_STAGE_VERTEX) return GPU_IR_INVALID;
    const uint32_t unsupported_qualifiers = GPU_IR_SHADER_DECORATION_NO_PERSPECTIVE |
        GPU_IR_SHADER_DECORATION_FLAT | GPU_IR_SHADER_DECORATION_CENTROID |
        GPU_IR_SHADER_DECORATION_SAMPLE | GPU_IR_SHADER_DECORATION_INVARIANT;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *variable = gpu_ir_shader_instruction_at(shader, i);
        if (!variable || variable->opcode != GPU_IR_SHADER_OP_VARIABLE || variable->function_index != UINT32_MAX ||
            !variable->entry_point_interface || variable->storage_class != GPU_IR_SHADER_STORAGE_INPUT ||
            variable->has_builtin) continue;
        if (!variable->has_location || variable->location >= 31 ||
            (variable->has_component && variable->component != 0) ||
            (variable->has_index && variable->index != 0) ||
            (variable->decoration_flags & unsupported_qualifiers)) return GPU_IR_UNSUPPORTED;
        const gpu_ir_shader_type *pointer = gpu_ir_shader_find_type(shader, variable->result_type_id);
        const gpu_ir_shader_type *type = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
            gpu_ir_shader_find_type(shader, pointer->element_type_id) : NULL;
        uint32_t components = 0, input_numeric_class = 0;
        if (type && type->kind == GPU_IR_SHADER_TYPE_FLOAT && type->bit_width == 32) {
            components = 1;
            input_numeric_class = GPU_IR_VERTEX_NUMERIC_FLOAT;
        } else if (type && type->kind == GPU_IR_SHADER_TYPE_INT && type->bit_width == 32 &&
                   type->signedness <= 1) {
            components = 1;
            input_numeric_class = type->signedness ? GPU_IR_VERTEX_NUMERIC_SINT : GPU_IR_VERTEX_NUMERIC_UINT;
        } else if (type && type->kind == GPU_IR_SHADER_TYPE_VECTOR && type->element_count >= 2 &&
                   type->element_count <= 4) {
            const gpu_ir_shader_type *element = gpu_ir_shader_find_type(shader, type->element_type_id);
            if (element && element->kind == GPU_IR_SHADER_TYPE_FLOAT && element->bit_width == 32) {
                components = type->element_count;
                input_numeric_class = GPU_IR_VERTEX_NUMERIC_FLOAT;
            } else if (element && element->kind == GPU_IR_SHADER_TYPE_INT && element->bit_width == 32 &&
                       element->signedness <= 1) {
                components = type->element_count;
                input_numeric_class = element->signedness ? GPU_IR_VERTEX_NUMERIC_SINT : GPU_IR_VERTEX_NUMERIC_UINT;
            }
        }
        if (!components || !input_numeric_class) return GPU_IR_UNSUPPORTED;
        int found = 0;
        for (uint32_t j = 0; j < vertex_input->attribute_count; j++) {
            const gpu_ir_vertex_attribute_desc *attribute = &vertex_input->attributes[j];
            if (attribute->location != variable->location) continue;
            uint32_t attribute_components = 0, attribute_numeric_class = 0;
            if (!gpu_ir_vertex_format_info(attribute->format, &attribute_components, NULL,
                                           &attribute_numeric_class)) return GPU_IR_UNSUPPORTED;
            if (attribute_numeric_class != input_numeric_class) return GPU_IR_UNSUPPORTED;
            /* Vulkan and Metal both define component expansion/truncation for vertex inputs. */
            found = 1;
            break;
        }
        if (!found) return GPU_IR_UNSUPPORTED;
    }
    return GPU_IR_OK;
}

VkResult WINAPI vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo *pCreateInfos, const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines) {
    if (!device_is_valid(device) || pipelineCache || !createInfoCount || !pCreateInfos || !pPipelines ||
        !allocator_is_valid(pAllocator)) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < createInfoCount; i++) pPipelines[i] = 0;
    VkResult result = VK_SUCCESS;
    uint32_t created = 0;
    for (uint32_t i = 0; i < createInfoCount; i++) {
        const VkGraphicsPipelineCreateInfo *info = &pCreateInfos[i];
        vx_vk_pipeline_layout *layout = pipeline_layout_from_handle(info->layout, device);
        vx_vk_render_pass *render_pass = render_pass_from_handle(info->renderPass, device);
        VkPhysicalDeviceProperties physical_properties = { 0 };
        vkGetPhysicalDeviceProperties(device->physical_device, &physical_properties);
        if (!layout || !render_pass || info->sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO ||
            info->pNext || info->flags || info->stageCount != 2 || !info->pStages || !info->pVertexInputState ||
            !info->pInputAssemblyState || info->pTessellationState || !info->pViewportState ||
            !info->pRasterizationState || !info->pMultisampleState || !info->pColorBlendState || info->subpass != 0) {
            result = info->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO && info->stageCount > 2 ?
                VK_ERROR_FEATURE_NOT_PRESENT : VK_ERROR_INITIALIZATION_FAILED;
            break;
        }

        const VkPipelineVertexInputStateCreateInfo *vertex_input = info->pVertexInputState;
        const VkPipelineInputAssemblyStateCreateInfo *input_assembly = info->pInputAssemblyState;
        const VkPipelineViewportStateCreateInfo *viewport_state = info->pViewportState;
        const VkPipelineRasterizationStateCreateInfo *rasterization = info->pRasterizationState;
        const VkPipelineMultisampleStateCreateInfo *multisample = info->pMultisampleState;
        const VkPipelineColorBlendStateCreateInfo *color_blend = info->pColorBlendState;
        if (vertex_input->sType != VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO || vertex_input->pNext ||
            vertex_input->flags || input_assembly->sType != VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO ||
            input_assembly->pNext || input_assembly->flags ||
            viewport_state->sType != VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO || viewport_state->pNext ||
            viewport_state->flags || rasterization->sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO ||
            rasterization->pNext || rasterization->flags ||
            multisample->sType != VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO || multisample->pNext ||
            multisample->flags || color_blend->sType != VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO ||
            color_blend->pNext || color_blend->flags ||
            (info->pDepthStencilState &&
             (info->pDepthStencilState->sType != VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO ||
              info->pDepthStencilState->pNext || info->pDepthStencilState->flags)) ||
            (info->pDynamicState &&
             (info->pDynamicState->sType != VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO ||
              info->pDynamicState->pNext || info->pDynamicState->flags))) {
            result = VK_ERROR_INITIALIZATION_FAILED;
            break;
        }

        gpu_ir_vertex_input_desc common_vertex_input = { 0 };
        int vertex_input_rc = vk_build_vertex_input(vertex_input, &physical_properties.limits,
                                                     &common_vertex_input);
        if (vertex_input_rc != GPU_IR_OK) {
            result = vertex_input_rc == GPU_IR_INVALID ? VK_ERROR_INITIALIZATION_FAILED : VK_ERROR_FEATURE_NOT_PRESENT;
            break;
        }

        uint32_t topology = 0;
        gpu_ir_color_blend_state common_color_blend = { 0 };
        int color_blend_rc = vk_build_color_blend(color_blend, &common_color_blend);
        if (color_blend_rc != GPU_IR_OK) {
            result = color_blend_rc == GPU_IR_INVALID ? VK_ERROR_INITIALIZATION_FAILED : VK_ERROR_FEATURE_NOT_PRESENT;
            break;
        }
        const VkViewport *viewport = viewport_state->pViewports;
        const VkRect2D *scissor = viewport_state->pScissors;
        int dynamic_viewport = 0, dynamic_scissor = 0, dynamic_blend_constants = 0;
        if (info->pDynamicState) {
            const VkPipelineDynamicStateCreateInfo *dynamic = info->pDynamicState;
            if (dynamic->dynamicStateCount && !dynamic->pDynamicStates) {
                result = VK_ERROR_INITIALIZATION_FAILED;
                break;
            }
            for (uint32_t state_index = 0; state_index < dynamic->dynamicStateCount; state_index++) {
                VkDynamicState state = dynamic->pDynamicStates[state_index];
                if (state == VK_DYNAMIC_STATE_VIEWPORT) {
                    if (dynamic_viewport) { result = VK_ERROR_INITIALIZATION_FAILED; break; }
                    dynamic_viewport = 1;
                } else if (state == VK_DYNAMIC_STATE_SCISSOR) {
                    if (dynamic_scissor) { result = VK_ERROR_INITIALIZATION_FAILED; break; }
                    dynamic_scissor = 1;
                } else if (state == VK_DYNAMIC_STATE_BLEND_CONSTANTS) {
                    if (dynamic_blend_constants) { result = VK_ERROR_INITIALIZATION_FAILED; break; }
                    dynamic_blend_constants = 1;
                } else {
                    result = VK_ERROR_FEATURE_NOT_PRESENT;
                    break;
                }
            }
            if (result != VK_SUCCESS) break;
        }
        int viewport_valid = dynamic_viewport || vk_viewport_is_valid(device, viewport);
        int scissor_valid = dynamic_scissor || vk_scissor_is_valid(scissor);
        if (!vk_graphics_topology(input_assembly->topology, &topology) || input_assembly->primitiveRestartEnable ||
            viewport_state->viewportCount != 1 || !viewport_valid || viewport_state->scissorCount != 1 ||
            !scissor_valid ||
            rasterization->depthClampEnable || rasterization->rasterizerDiscardEnable ||
            rasterization->polygonMode != VK_POLYGON_MODE_FILL || rasterization->cullMode != VK_CULL_MODE_NONE ||
            (rasterization->frontFace != VK_FRONT_FACE_COUNTER_CLOCKWISE &&
             rasterization->frontFace != VK_FRONT_FACE_CLOCKWISE) || rasterization->depthBiasEnable ||
            rasterization->lineWidth != 1.0f || multisample->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT ||
            multisample->sampleShadingEnable || multisample->alphaToCoverageEnable || multisample->alphaToOneEnable ||
            (multisample->pSampleMask && !(*multisample->pSampleMask & 1u)) ||
            (info->pDepthStencilState && (info->pDepthStencilState->depthTestEnable ||
             info->pDepthStencilState->depthWriteEnable || info->pDepthStencilState->depthBoundsTestEnable ||
             info->pDepthStencilState->stencilTestEnable))) {
            result = VK_ERROR_FEATURE_NOT_PRESENT;
            break;
        }

        uint32_t gpu_format = 0, bytes_per_texel = 0;
        if ((render_pass->attachment.format != VK_FORMAT_R8G8B8A8_UNORM &&
             render_pass->attachment.format != VK_FORMAT_B8G8R8A8_UNORM) ||
            render_pass->attachment.samples != VK_SAMPLE_COUNT_1_BIT ||
            !vk_format_to_gpu(render_pass->attachment.format, &gpu_format, &bytes_per_texel) ||
            gpu_ir_pipeline_layout_push_constant_count(layout->gpu_layout)) {
            result = VK_ERROR_FEATURE_NOT_PRESENT;
            break;
        }
        (void)bytes_per_texel;

        const VkPipelineShaderStageCreateInfo *vertex_stage = NULL, *fragment_stage = NULL;
        for (uint32_t stage_index = 0; stage_index < info->stageCount; stage_index++) {
            const VkPipelineShaderStageCreateInfo *stage = &info->pStages[stage_index];
            if (stage->sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO || stage->pNext || stage->flags ||
                !stage->pName || !stage->pName[0]) {
                result = VK_ERROR_INITIALIZATION_FAILED;
                break;
            }
            if (stage->stage == VK_SHADER_STAGE_VERTEX_BIT && !vertex_stage) vertex_stage = stage;
            else if (stage->stage == VK_SHADER_STAGE_FRAGMENT_BIT && !fragment_stage) fragment_stage = stage;
            else { result = VK_ERROR_FEATURE_NOT_PRESENT; break; }
        }
        if (result != VK_SUCCESS) break;
        if (!vertex_stage || !fragment_stage) { result = VK_ERROR_FEATURE_NOT_PRESENT; break; }

        vx_vk_shader_module *vertex_module = shader_module_from_handle(vertex_stage->module, device);
        vx_vk_shader_module *fragment_module = shader_module_from_handle(fragment_stage->module, device);
        const spirv_entry_point *vertex_entry = vertex_module ? spirv_find_entry_point(
            &vertex_module->module, vertex_stage->pName, SPIRV_EXECUTION_MODEL_VERTEX) : NULL;
        const spirv_entry_point *fragment_entry = fragment_module ? spirv_find_entry_point(
            &fragment_module->module, fragment_stage->pName, SPIRV_EXECUTION_MODEL_FRAGMENT) : NULL;
        if (!vertex_entry || !fragment_entry) { result = VK_ERROR_FEATURE_NOT_PRESENT; break; }

        gpu_ir_shader *vertex_shader = NULL, *fragment_shader = NULL;
        gpu_ir_pipeline_layout *vertex_layout = NULL, *fragment_layout = NULL;
        char *vertex_msl = NULL, *fragment_msl = NULL;
        size_t vertex_msl_size = 0, fragment_msl_size = 0;
        int rc = spirv_build_gpu_ir_shader(&vertex_module->module, vertex_entry, &vertex_shader);
        if (rc == GPU_IR_OK) rc = spirv_build_gpu_ir_shader(&fragment_module->module, fragment_entry, &fragment_shader);
        if (rc == GPU_IR_OK && (vk_shader_uses_subgroup_builtin(vertex_shader) ||
                                vk_shader_uses_subgroup_builtin(fragment_shader))) rc = GPU_IR_UNSUPPORTED;
        if (rc == GPU_IR_OK) rc = spirv_build_gpu_ir_layout(&vertex_module->module, vertex_entry, &vertex_layout);
        if (rc == GPU_IR_OK) rc = spirv_build_gpu_ir_layout(&fragment_module->module, fragment_entry, &fragment_layout);
        if (rc == GPU_IR_OK) {
            gpu_ir_shader *specialized_shader = NULL;
            result = vk_specialize_shader(&vertex_module->module, vertex_stage->pSpecializationInfo,
                                           vertex_shader, NULL, &specialized_shader);
            if (result == VK_SUCCESS) {
                gpu_ir_shader_release(vertex_shader);
                vertex_shader = specialized_shader;
            }
        }
        if (rc == GPU_IR_OK && result == VK_SUCCESS) {
            gpu_ir_shader *specialized_shader = NULL;
            result = vk_specialize_shader(&fragment_module->module, fragment_stage->pSpecializationInfo,
                                           fragment_shader, NULL, &specialized_shader);
            if (result == VK_SUCCESS) {
                gpu_ir_shader_release(fragment_shader);
                fragment_shader = specialized_shader;
            }
        }
        if (rc == GPU_IR_OK && result == VK_SUCCESS) {
            int sampled_indexing = vk_shader_uses_dynamic_sampled_descriptor_index(fragment_shader);
            if (sampled_indexing < 0 ||
                (sampled_indexing && !device->shader_sampled_image_array_dynamic_indexing))
                rc = GPU_IR_UNSUPPORTED;
        }
        if (rc == GPU_IR_OK && result != VK_SUCCESS) {
            gpu_ir_shader_release(vertex_shader);
            gpu_ir_shader_release(fragment_shader);
            gpu_ir_pipeline_layout_release(vertex_layout);
            gpu_ir_pipeline_layout_release(fragment_layout);
            free(vertex_msl);
            free(fragment_msl);
            break;
        }
        if (rc == GPU_IR_OK && (gpu_ir_pipeline_layout_binding_count(vertex_layout) ||
            gpu_ir_pipeline_layout_push_constant_count(vertex_layout) ||
            gpu_ir_pipeline_layout_push_constant_count(fragment_layout))) rc = GPU_IR_UNSUPPORTED;
        for (uint32_t binding_index = 0; rc == GPU_IR_OK &&
             binding_index < gpu_ir_pipeline_layout_binding_count(fragment_layout); binding_index++) {
            gpu_ir_descriptor_binding reflected;
            if (gpu_ir_pipeline_layout_binding_at(fragment_layout, binding_index, &reflected) != GPU_IR_OK ||
                (reflected.kind != GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER &&
                 reflected.kind != GPU_IR_DESCRIPTOR_SAMPLED_IMAGE &&
                 reflected.kind != GPU_IR_DESCRIPTOR_SAMPLER) ||
                reflected.set >= layout->set_layout_count || !reflected.count ||
                reflected.count == GPU_IR_DESCRIPTOR_COUNT_UNBOUNDED) {
                rc = GPU_IR_UNSUPPORTED;
                break;
            }
            vx_vk_descriptor_layout *set_layout = layout->set_layouts[reflected.set];
            uint32_t descriptor_binding_index = 0;
            if (!descriptor_layout_find(set_layout, reflected.binding, &descriptor_binding_index)) {
                rc = GPU_IR_UNSUPPORTED;
                break;
            }
            const VkDescriptorSetLayoutBinding *actual = &set_layout->bindings[descriptor_binding_index];
            VkShaderStageFlags reflected_stages = vk_stage_flags_for_gpu(reflected.stage_mask);
            VkDescriptorType expected_type = reflected.kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER ?
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : reflected.kind == GPU_IR_DESCRIPTOR_SAMPLED_IMAGE ?
                VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
            if (actual->descriptorType != expected_type ||
                actual->descriptorCount < reflected.count ||
                (actual->stageFlags & reflected_stages) != reflected_stages)
                rc = GPU_IR_UNSUPPORTED;
        }
        if (rc == GPU_IR_OK) rc = vk_validate_vertex_shader_inputs(vertex_shader, &common_vertex_input);
        if (rc == GPU_IR_OK) {
            rc = gpu_ir_msl_validate_graphics_link(vertex_shader, fragment_shader,
                physical_properties.limits.maxVertexOutputComponents,
                physical_properties.limits.maxFragmentInputComponents);
        }
        if (rc == GPU_IR_OK) rc = gpu_ir_msl_emit_vertex_with_layout(vertex_shader, layout->gpu_layout,
                                                                      &vertex_msl, &vertex_msl_size);
        if (rc == GPU_IR_OK) rc = gpu_ir_msl_emit_fragment_with_layout(fragment_shader, layout->gpu_layout,
                                                                        &fragment_msl, &fragment_msl_size);
        if (rc != GPU_IR_OK || !vertex_msl || !fragment_msl || !vertex_msl_size || !fragment_msl_size) {
            gpu_ir_shader_release(vertex_shader);
            gpu_ir_shader_release(fragment_shader);
            gpu_ir_pipeline_layout_release(vertex_layout);
            gpu_ir_pipeline_layout_release(fragment_layout);
            free(vertex_msl);
            free(fragment_msl);
            if (result == VK_SUCCESS)
                result = rc == GPU_IR_OUT_OF_MEMORY ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_FEATURE_NOT_PRESENT;
            break;
        }

        char error[512] = { 0 };
        gpu_ir_pipeline *gpu_pipeline = gpu_ir_metal_graphics_pipeline_create_with_blend(
            device->physical_device->owner->metal, vertex_msl, fragment_msl, gpu_format,
            &common_vertex_input, &common_color_blend, layout->gpu_layout, error, sizeof error);
        gpu_ir_shader_release(vertex_shader);
        gpu_ir_shader_release(fragment_shader);
        gpu_ir_pipeline_layout_release(vertex_layout);
        free(vertex_msl);
        free(fragment_msl);
        if (!gpu_pipeline) {
            if (error[0]) fprintf(stderr, "[vxrt] Vulkan: graphics pipeline translation failed: %s\n", error);
            gpu_ir_pipeline_layout_release(fragment_layout);
            result = VK_ERROR_FEATURE_NOT_PRESENT;
            break;
        }
        vx_vk_pipeline *pipeline = object_allocate(pAllocator, sizeof *pipeline, alignof(vx_vk_pipeline),
                                                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
        if (!pipeline) {
            gpu_ir_pipeline_release(gpu_pipeline);
            gpu_ir_pipeline_layout_release(fragment_layout);
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            break;
        }
        pipeline->magic = VX_VK_PIPELINE_MAGIC;
        pipeline->device = device;
        pipeline->refs = 1;
        pipeline->externally_alive = 1;
        pipeline->bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
        pipeline->topology = topology;
        pipeline->layout = layout;
        pipeline_layout_retain(layout);
        pipeline->gpu_pipeline = gpu_pipeline;
        pipeline->reflected_layout = fragment_layout;
        pipeline->color_format = render_pass->attachment.format;
        pipeline->dynamic_viewport = (uint8_t)dynamic_viewport;
        pipeline->dynamic_scissor = (uint8_t)dynamic_scissor;
        pipeline->dynamic_blend_constants = (uint8_t)dynamic_blend_constants;
        if (!dynamic_viewport) pipeline->viewport = *viewport;
        if (!dynamic_scissor) pipeline->scissor = *scissor;
        if (pAllocator) { pipeline->allocator = *pAllocator; pipeline->has_allocator = 1; }
        pPipelines[i] = pipeline_to_handle(pipeline);
        created++;
    }
    if (result != VK_SUCCESS) {
        for (uint32_t i = 0; i < created; i++) {
            vx_vk_pipeline *pipeline = pipeline_from_handle(pPipelines[i], device);
            if (pipeline) { pipeline->externally_alive = 0; pipeline_release(pipeline); }
            pPipelines[i] = 0;
        }
    }
    return result;
}

void WINAPI vkDestroyPipeline(VkDevice device, VkPipeline pipelineHandle, const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    vx_vk_pipeline *pipeline = pipeline_from_handle(pipelineHandle, device);
    if (!pipeline) return;
    pipeline->externally_alive = 0;
    for (vx_vk_pipeline_use *use = pipeline->command_buffer_uses; use; use = use->next)
        if (use->command_buffer && use->command_buffer->state != VX_VK_COMMAND_PENDING)
            use->command_buffer->state = VX_VK_COMMAND_INVALID;
    pipeline_release(pipeline);
}

static int descriptor_pool_reserve(vx_vk_descriptor_pool *pool, uint32_t need) {
    if (need <= pool->set_capacity) return 1;
    uint32_t capacity = pool->set_capacity ? pool->set_capacity : 8;
    while (capacity < need) {
        uint32_t next = capacity <= UINT32_MAX / 2 ? capacity * 2 : UINT32_MAX;
        if (next <= capacity) return 0;
        capacity = next;
    }
    size_t bytes = (size_t)capacity * sizeof *pool->sets;
    if (capacity && bytes / sizeof *pool->sets != capacity) return 0;
    VkDescriptorSet *replacement = object_allocate(pool->has_allocator ? &pool->allocator : NULL, bytes,
        alignof(VkDescriptorSet), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!replacement) return 0;
    if (pool->allocated_sets) memcpy(replacement, pool->sets,
        (size_t)pool->allocated_sets * sizeof *replacement);
    object_free(pool->has_allocator ? &pool->allocator : NULL, pool->sets);
    pool->sets = replacement;
    pool->set_capacity = capacity;
    return 1;
}

VkResult WINAPI vkCreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDescriptorPool *pDescriptorPool) {
    if (!pDescriptorPool) return VK_ERROR_INITIALIZATION_FAILED;
    *pDescriptorPool = 0;
    if (!device_is_valid(device) || !pCreateInfo || pCreateInfo->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO ||
        pCreateInfo->pNext || (pCreateInfo->flags & ~VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT) ||
        !pCreateInfo->maxSets || (pCreateInfo->poolSizeCount && !pCreateInfo->pPoolSizes) ||
        !allocator_is_valid(pAllocator)) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t uniform_capacity = 0, dynamic_uniform_capacity = 0;
    uint64_t storage_capacity = 0, dynamic_storage_capacity = 0;
    uint64_t sampled_image_capacity = 0, storage_image_capacity = 0;
    uint64_t sampler_capacity = 0, combined_image_sampler_capacity = 0;
    for (uint32_t i = 0; i < pCreateInfo->poolSizeCount; i++) {
        const VkDescriptorPoolSize *size = &pCreateInfo->pPoolSizes[i];
        if (!vk_descriptor_type_supported(size->type)) return VK_ERROR_FEATURE_NOT_PRESENT;
        for (uint32_t j = 0; j < i; j++)
            if (pCreateInfo->pPoolSizes[j].type == size->type) return VK_ERROR_INITIALIZATION_FAILED;
        uint64_t *capacity = size->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? &uniform_capacity :
            size->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ? &dynamic_uniform_capacity :
            size->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ? &storage_capacity :
            size->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC ? &dynamic_storage_capacity :
            size->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? &sampled_image_capacity :
            size->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? &storage_image_capacity :
            size->type == VK_DESCRIPTOR_TYPE_SAMPLER ? &sampler_capacity :
            &combined_image_sampler_capacity;
        *capacity += size->descriptorCount;
        if (*capacity > UINT32_MAX) return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    vx_vk_descriptor_pool *pool = object_allocate(pAllocator, sizeof *pool, alignof(vx_vk_descriptor_pool),
                                                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pool->magic = VX_VK_DESCRIPTOR_POOL_MAGIC;
    pool->device = device;
    pool->externally_alive = 1;
    pool->allow_free_sets = (pCreateInfo->flags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT) != 0;
    pool->max_sets = pCreateInfo->maxSets;
    pool->uniform_capacity = (uint32_t)uniform_capacity;
    pool->dynamic_uniform_capacity = (uint32_t)dynamic_uniform_capacity;
    pool->storage_capacity = (uint32_t)storage_capacity;
    pool->dynamic_storage_capacity = (uint32_t)dynamic_storage_capacity;
    pool->sampled_image_capacity = (uint32_t)sampled_image_capacity;
    pool->storage_image_capacity = (uint32_t)storage_image_capacity;
    pool->sampler_capacity = (uint32_t)sampler_capacity;
    pool->combined_image_sampler_capacity = (uint32_t)combined_image_sampler_capacity;
    if (pAllocator) { pool->allocator = *pAllocator; pool->has_allocator = 1; }
    *pDescriptorPool = descriptor_pool_to_handle(pool);
    return VK_SUCCESS;
}

static void descriptor_pool_destroy_sets(vx_vk_descriptor_pool *pool) {
    uint32_t set_count = pool->allocated_sets;
    for (uint32_t i = 0; i < set_count; i++) {
        vx_vk_descriptor_set *set = descriptor_set_from_handle(pool->sets[i], pool->device);
        if (!set) continue;
        set->externally_alive = 0;
        set->pool = NULL;
        set->generation++;
        descriptor_set_release(set);
    }
    if (set_count) memset(pool->sets, 0, (size_t)set_count * sizeof *pool->sets);
    pool->allocated_sets = 0;
    pool->uniform_used = 0;
    pool->dynamic_uniform_used = 0;
    pool->storage_used = 0;
    pool->dynamic_storage_used = 0;
    pool->sampled_image_used = 0;
    pool->storage_image_used = 0;
    pool->sampler_used = 0;
    pool->combined_image_sampler_used = 0;
}

void WINAPI vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool poolHandle,
    const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    vx_vk_descriptor_pool *pool = descriptor_pool_from_handle(poolHandle, device);
    if (!pool) return;
    descriptor_pool_destroy_sets(pool);
    VkAllocationCallbacks allocator = pool->allocator;
    int has_allocator = pool->has_allocator;
    object_free(has_allocator ? &allocator : NULL, pool->sets);
    pool->externally_alive = 0;
    pool->magic = 0;
    object_free(has_allocator ? &allocator : NULL, pool);
}

VkResult WINAPI vkResetDescriptorPool(VkDevice device, VkDescriptorPool poolHandle, VkFlags flags) {
    vx_vk_descriptor_pool *pool = descriptor_pool_from_handle(poolHandle, device);
    if (!pool || flags) return VK_ERROR_INITIALIZATION_FAILED;
    descriptor_pool_destroy_sets(pool);
    return VK_SUCCESS;
}

VkResult WINAPI vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *pAllocateInfo,
    VkDescriptorSet *pDescriptorSets) {
    if (!pDescriptorSets) return VK_ERROR_INITIALIZATION_FAILED;
    if (pAllocateInfo && pAllocateInfo->descriptorSetCount)
        memset(pDescriptorSets, 0, (size_t)pAllocateInfo->descriptorSetCount * sizeof *pDescriptorSets);
    vx_vk_descriptor_pool *pool = pAllocateInfo ? descriptor_pool_from_handle(pAllocateInfo->descriptorPool, device) : NULL;
    if (!device_is_valid(device) || !pool || !pAllocateInfo ||
        pAllocateInfo->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO || pAllocateInfo->pNext ||
        !pAllocateInfo->descriptorSetCount || !pAllocateInfo->pSetLayouts)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pAllocateInfo->descriptorSetCount > pool->max_sets - pool->allocated_sets)
        return device->maintenance1_enabled ? VK_ERROR_OUT_OF_POOL_MEMORY_KHR : VK_ERROR_OUT_OF_DEVICE_MEMORY;
    uint64_t required_uniform = 0, required_dynamic_uniform = 0;
    uint64_t required_storage = 0, required_dynamic_storage = 0;
    uint64_t required_sampled_images = 0, required_storage_images = 0;
    uint64_t required_samplers = 0, required_combined_image_samplers = 0;
    for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
        vx_vk_descriptor_layout *layout = descriptor_layout_from_handle(pAllocateInfo->pSetLayouts[i], device);
        if (!layout) return VK_ERROR_INITIALIZATION_FAILED;
        for (uint32_t j = 0; j < layout->binding_count; j++) {
            const VkDescriptorSetLayoutBinding *binding = &layout->bindings[j];
            if (binding->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
                required_uniform += binding->descriptorCount;
            else if (binding->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
                required_dynamic_uniform += binding->descriptorCount;
            else if (binding->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
                required_storage += binding->descriptorCount;
            else if (binding->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
                required_dynamic_storage += binding->descriptorCount;
            else if (binding->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
                required_storage_images += binding->descriptorCount;
            else if (binding->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
                required_sampled_images += binding->descriptorCount;
            else if (binding->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER)
                required_samplers += binding->descriptorCount;
            else if (binding->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
                required_combined_image_samplers += binding->descriptorCount;
        }
    }
    if (required_uniform > pool->uniform_capacity - pool->uniform_used ||
        required_dynamic_uniform > pool->dynamic_uniform_capacity - pool->dynamic_uniform_used ||
        required_storage > pool->storage_capacity - pool->storage_used ||
        required_dynamic_storage > pool->dynamic_storage_capacity - pool->dynamic_storage_used ||
        required_sampled_images > pool->sampled_image_capacity - pool->sampled_image_used ||
        required_storage_images > pool->storage_image_capacity - pool->storage_image_used ||
        required_samplers > pool->sampler_capacity - pool->sampler_used ||
        required_combined_image_samplers > pool->combined_image_sampler_capacity -
            pool->combined_image_sampler_used)
        return device->maintenance1_enabled ? VK_ERROR_OUT_OF_POOL_MEMORY_KHR : VK_ERROR_OUT_OF_DEVICE_MEMORY;
    uint32_t final_count = pool->allocated_sets + pAllocateInfo->descriptorSetCount;
    if (!descriptor_pool_reserve(pool, final_count)) return VK_ERROR_OUT_OF_HOST_MEMORY;

    VkAllocationCallbacks *allocator = pool->has_allocator ? &pool->allocator : NULL;
    VkResult result = VK_SUCCESS;
    uint32_t allocated = 0;
    for (; allocated < pAllocateInfo->descriptorSetCount; allocated++) {
        vx_vk_descriptor_layout *layout = descriptor_layout_from_handle(pAllocateInfo->pSetLayouts[allocated], device);
        vx_vk_descriptor_set *set = object_allocate(allocator, sizeof *set, alignof(vx_vk_descriptor_set),
                                                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
        if (!set) { result = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
        if (layout->descriptor_count) {
            set->values = object_allocate(allocator, (size_t)layout->descriptor_count * sizeof *set->values,
                alignof(vx_vk_descriptor_value), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
            if (!set->values) { object_free(allocator, set); result = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
        }
        set->magic = VX_VK_DESCRIPTOR_SET_MAGIC;
        set->device = device;
        set->pool = pool;
        set->layout = layout;
        set->externally_alive = 1;
        if (pool->has_allocator) {
            set->allocator = pool->allocator;
            set->has_allocator = 1;
        }
        set->refs = 1;
        set->generation = 1;
        for (uint32_t j = 0; j < layout->binding_count; j++) {
            if (layout->bindings[j].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
                set->uniform_descriptor_count += layout->bindings[j].descriptorCount;
            else if (layout->bindings[j].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
                set->dynamic_uniform_descriptor_count += layout->bindings[j].descriptorCount;
            else if (layout->bindings[j].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
                set->dynamic_storage_descriptor_count += layout->bindings[j].descriptorCount;
                set->storage_descriptor_count += layout->bindings[j].descriptorCount;
            } else if (layout->bindings[j].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
                set->storage_descriptor_count += layout->bindings[j].descriptorCount;
            else if (layout->bindings[j].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
                set->storage_image_descriptor_count += layout->bindings[j].descriptorCount;
            else if (layout->bindings[j].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
                set->sampled_image_descriptor_count += layout->bindings[j].descriptorCount;
            else if (layout->bindings[j].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER)
                set->sampler_descriptor_count += layout->bindings[j].descriptorCount;
            else if (layout->bindings[j].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
                set->combined_image_sampler_descriptor_count += layout->bindings[j].descriptorCount;
        }
        descriptor_layout_retain(layout);
        pDescriptorSets[allocated] = descriptor_set_to_handle(set);
        pool->sets[pool->allocated_sets + allocated] = pDescriptorSets[allocated];
    }
    if (result != VK_SUCCESS) {
        for (uint32_t i = 0; i < allocated; i++) {
            vx_vk_descriptor_set *set = descriptor_set_from_handle(pDescriptorSets[i], device);
            if (set) descriptor_set_release(set);
            pDescriptorSets[i] = 0;
        }
        return result;
    }
    pool->allocated_sets = final_count;
    pool->uniform_used += (uint32_t)required_uniform;
    pool->dynamic_uniform_used += (uint32_t)required_dynamic_uniform;
    pool->storage_used += (uint32_t)required_storage;
    pool->dynamic_storage_used += (uint32_t)required_dynamic_storage;
    pool->sampled_image_used += (uint32_t)required_sampled_images;
    pool->storage_image_used += (uint32_t)required_storage_images;
    pool->sampler_used += (uint32_t)required_samplers;
    pool->combined_image_sampler_used += (uint32_t)required_combined_image_samplers;
    return VK_SUCCESS;
}

VkResult WINAPI vkFreeDescriptorSets(VkDevice device, VkDescriptorPool poolHandle, uint32_t descriptorSetCount,
    const VkDescriptorSet *pDescriptorSets) {
    vx_vk_descriptor_pool *pool = descriptor_pool_from_handle(poolHandle, device);
    if (!pool || !pool->allow_free_sets || !descriptorSetCount || !pDescriptorSets)
        return VK_ERROR_INITIALIZATION_FAILED;
    vx_vk_descriptor_set **sets = calloc(descriptorSetCount, sizeof *sets);
    if (!sets) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < descriptorSetCount; i++) {
        sets[i] = descriptor_set_from_handle(pDescriptorSets[i], device);
        if (!sets[i] || sets[i]->pool != pool) {
            free(sets);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        for (uint32_t j = 0; j < i; j++) if (sets[j] == sets[i]) { free(sets); return VK_ERROR_INITIALIZATION_FAILED; }
    }
    for (uint32_t i = 0; i < descriptorSetCount; i++) {
        for (uint32_t j = 0; j < pool->allocated_sets; j++) {
            if (pool->sets[j] != pDescriptorSets[i]) continue;
            pool->sets[j] = pool->sets[--pool->allocated_sets];
            pool->uniform_used -= sets[i]->uniform_descriptor_count;
            pool->dynamic_uniform_used -= sets[i]->dynamic_uniform_descriptor_count;
            pool->storage_used -= sets[i]->storage_descriptor_count - sets[i]->dynamic_storage_descriptor_count;
            pool->dynamic_storage_used -= sets[i]->dynamic_storage_descriptor_count;
            pool->sampled_image_used -= sets[i]->sampled_image_descriptor_count;
            pool->storage_image_used -= sets[i]->storage_image_descriptor_count;
            pool->sampler_used -= sets[i]->sampler_descriptor_count;
            pool->combined_image_sampler_used -= sets[i]->combined_image_sampler_descriptor_count;
            sets[i]->externally_alive = 0;
            sets[i]->pool = NULL;
            sets[i]->generation++;
            descriptor_set_release(sets[i]);
            break;
        }
    }
    free(sets);
    return VK_SUCCESS;
}

void WINAPI vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet *pDescriptorWrites, uint32_t descriptorCopyCount,
    const VkCopyDescriptorSet *pDescriptorCopies) {
    if (!device_is_valid(device) || (descriptorWriteCount && !pDescriptorWrites) ||
        (descriptorCopyCount && !pDescriptorCopies)) return;
    uint64_t max_buffer_length = gpu_ir_metal_max_buffer_length(device->physical_device->owner->metal);
    uint64_t max_range = max_buffer_length > UINT32_MAX ? UINT32_MAX : max_buffer_length;
    for (uint32_t i = 0; i < descriptorWriteCount; i++) {
        const VkWriteDescriptorSet *write = &pDescriptorWrites[i];
        vx_vk_descriptor_set *set = descriptor_set_from_handle(write->dstSet, device);
        uint32_t binding_index;
        if (set && write->sType == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET && !write->pNext &&
            vk_descriptor_image_info_type(write->descriptorType)) {
            if (!write->descriptorCount || write->descriptorCount > VX_VK_MAX_DESCRIPTOR_VALUES ||
                !write->pImageInfo || write->pBufferInfo || write->pTexelBufferView ||
                !descriptor_layout_find(set->layout, write->dstBinding, &binding_index)) continue;
            const VkDescriptorSetLayoutBinding *first_binding = &set->layout->bindings[binding_index];
            if (!first_binding->descriptorCount || write->dstArrayElement > first_binding->descriptorCount) continue;
            vx_vk_descriptor_value values[VX_VK_MAX_DESCRIPTOR_VALUES];
            uint32_t value_indices[VX_VK_MAX_DESCRIPTOR_VALUES];
            uint32_t current_binding_index = binding_index;
            uint32_t current_array_element = write->dstArrayElement;
            int valid_write = 1;
            for (uint32_t element = 0; element < write->descriptorCount; element++) {
                while (current_binding_index < set->layout->binding_count &&
                       current_array_element == set->layout->bindings[current_binding_index].descriptorCount) {
                    uint32_t previous = current_binding_index++;
                    if (current_binding_index >= set->layout->binding_count ||
                        set->layout->bindings[previous].binding == UINT32_MAX ||
                        set->layout->bindings[current_binding_index].binding !=
                            set->layout->bindings[previous].binding + 1u ||
                        set->layout->bindings[current_binding_index].descriptorType != write->descriptorType ||
                        set->layout->bindings[current_binding_index].stageFlags != first_binding->stageFlags) {
                        valid_write = 0;
                        break;
                    }
                    current_array_element = 0;
                }
                if (!valid_write || current_binding_index >= set->layout->binding_count ||
                    current_array_element >= set->layout->bindings[current_binding_index].descriptorCount ||
                    set->layout->bindings[current_binding_index].descriptorType != write->descriptorType ||
                    set->layout->bindings[current_binding_index].stageFlags != first_binding->stageFlags ||
                    !descriptor_layout_value_index(set->layout, current_binding_index, current_array_element,
                                                   &value_indices[element])) {
                    valid_write = 0;
                    break;
                }
                const VkDescriptorImageInfo *info = &write->pImageInfo[element];
                if (write->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) {
                    vx_vk_sampler *sampler = sampler_from_handle(info->sampler, device);
                    if (!sampler) { valid_write = 0; break; }
                    values[element] = (vx_vk_descriptor_value){ .sampler = sampler, .written = 1 };
                    current_array_element++;
                    continue;
                }
                vx_vk_image_view *view = image_view_from_handle(info->imageView, device);
                vx_vk_image *image = view ? view->image : NULL;
                int sampled_image = write->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                    write->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                int combined_image_sampler =
                    write->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                vx_vk_sampler *sampler = combined_image_sampler ? sampler_from_handle(info->sampler, device) : NULL;
                int valid_image_layout = sampled_image ?
                    (info->imageLayout == VK_IMAGE_LAYOUT_GENERAL ||
                     info->imageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) :
                    info->imageLayout == VK_IMAGE_LAYOUT_GENERAL;
                if (!view || !image || !image->externally_alive ||
                    !(image->usage & (sampled_image ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_STORAGE_BIT)) ||
                    (combined_image_sampler && !sampler) || !valid_image_layout ||
                    vk_component_mapping_to_gpu(view->components) == UINT32_MAX) {
                    valid_write = 0;
                    break;
                }
                values[element] = (vx_vk_descriptor_value){
                    .image = image,
                    .sampler = sampler,
                    .image_layout = info->imageLayout,
                    .component_mapping = vk_component_mapping_to_gpu(view->components),
                    .written = 1
                };
                current_array_element++;
            }
            if (!valid_write) continue;
            for (uint32_t element = 0; element < write->descriptorCount; element++) {
                vx_vk_descriptor_value *destination = &set->values[value_indices[element]];
                vx_vk_buffer *old_buffer = destination->buffer;
                vx_vk_image *old_image = destination->image;
                vx_vk_sampler *old_sampler = destination->sampler;
                image_retain(values[element].image);
                sampler_retain(values[element].sampler);
                *destination = values[element];
                if (old_buffer) buffer_release(old_buffer);
                if (old_image) image_release(old_image);
                if (old_sampler) sampler_release(old_sampler);
            }
            set->generation++;
            continue;
        }
        if (!set || write->sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET || write->pNext ||
            !vk_buffer_descriptor_type(write->descriptorType) || !write->descriptorCount ||
            write->descriptorCount > VX_VK_MAX_DESCRIPTOR_VALUES ||
            write->pImageInfo || !write->pBufferInfo || write->pTexelBufferView ||
            !descriptor_layout_find(set->layout, write->dstBinding, &binding_index)) continue;
        const VkDescriptorSetLayoutBinding *first_binding = &set->layout->bindings[binding_index];
        if (!first_binding->descriptorCount || write->dstArrayElement > first_binding->descriptorCount) continue;
        vx_vk_descriptor_value values[VX_VK_MAX_DESCRIPTOR_VALUES];
        uint32_t value_indices[VX_VK_MAX_DESCRIPTOR_VALUES];
        uint32_t current_binding_index = binding_index;
        uint32_t current_array_element = write->dstArrayElement;
        int valid_write = 1;
        for (uint32_t element = 0; element < write->descriptorCount; element++) {
            while (current_binding_index < set->layout->binding_count &&
                   current_array_element == set->layout->bindings[current_binding_index].descriptorCount) {
                uint32_t previous = current_binding_index++;
                if (current_binding_index >= set->layout->binding_count ||
                    set->layout->bindings[previous].binding == UINT32_MAX ||
                    set->layout->bindings[current_binding_index].binding !=
                        set->layout->bindings[previous].binding + 1u ||
                    set->layout->bindings[current_binding_index].descriptorType != first_binding->descriptorType ||
                    set->layout->bindings[current_binding_index].stageFlags != first_binding->stageFlags ||
                    set->layout->bindings[current_binding_index].pImmutableSamplers !=
                        first_binding->pImmutableSamplers) {
                    valid_write = 0;
                    break;
                }
                current_array_element = 0;
            }
            if (!valid_write || current_binding_index >= set->layout->binding_count ||
                current_array_element >= set->layout->bindings[current_binding_index].descriptorCount ||
                set->layout->bindings[current_binding_index].descriptorType != write->descriptorType ||
                set->layout->bindings[current_binding_index].stageFlags != first_binding->stageFlags ||
                set->layout->bindings[current_binding_index].pImmutableSamplers != first_binding->pImmutableSamplers) {
                valid_write = 0;
                break;
            }
            const VkDescriptorBufferInfo *info = &write->pBufferInfo[element];
            vx_vk_buffer *buffer = buffer_from_handle(info->buffer, device);
            uint64_t range = info->range == UINT64_MAX && buffer && info->offset <= buffer->size ?
                buffer->size - info->offset : info->range;
            uint32_t required_usage = vk_uniform_descriptor_type(write->descriptorType) ?
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            if (!buffer || !buffer->bound || !(buffer->usage & required_usage) ||
                info->offset % VX_VK_BUFFER_ALIGNMENT || info->offset >= buffer->size || !range ||
                range > buffer->size - info->offset || range > max_range ||
                !descriptor_layout_value_index(set->layout, current_binding_index,
                    current_array_element, &value_indices[element])) {
                valid_write = 0;
                break;
            }
            values[element] = (vx_vk_descriptor_value){
                .buffer = buffer,
                .offset = info->offset,
                .range = range,
                .written = 1,
                .whole_size = info->range == VK_WHOLE_SIZE
            };
            current_array_element++;
        }
        if (!valid_write) continue;
        for (uint32_t element = 0; element < write->descriptorCount; element++) {
            vx_vk_descriptor_value *destination = &set->values[value_indices[element]];
            vx_vk_buffer *old_buffer = destination->buffer;
            vx_vk_image *old_image = destination->image;
            vx_vk_sampler *old_sampler = destination->sampler;
            buffer_retain(values[element].buffer);
            *destination = values[element];
            if (old_buffer) buffer_release(old_buffer);
            if (old_image) image_release(old_image);
            if (old_sampler) sampler_release(old_sampler);
        }
        set->generation++;
    }

    // Vulkan performs every write above before processing copies, and applies copies in array order.
    for (uint32_t i = 0; i < descriptorCopyCount; i++) {
        const VkCopyDescriptorSet *copy = &pDescriptorCopies[i];
        vx_vk_descriptor_set *src = descriptor_set_from_handle(copy->srcSet, device);
        vx_vk_descriptor_set *dst = descriptor_set_from_handle(copy->dstSet, device);
        if (!src || !dst || copy->sType != VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET || copy->pNext ||
            !copy->descriptorCount || copy->descriptorCount > VX_VK_MAX_DESCRIPTOR_VALUES)
            continue;
        uint32_t src_binding_index, dst_binding_index;
        if (!descriptor_layout_find(src->layout, copy->srcBinding, &src_binding_index) ||
            !descriptor_layout_find(dst->layout, copy->dstBinding, &dst_binding_index)) continue;
        const VkDescriptorSetLayoutBinding *src_first = &src->layout->bindings[src_binding_index];
        const VkDescriptorSetLayoutBinding *dst_first = &dst->layout->bindings[dst_binding_index];
        if (!src_first->descriptorCount || !dst_first->descriptorCount ||
            !vk_descriptor_type_supported(src_first->descriptorType) ||
            dst_first->descriptorType != src_first->descriptorType) continue;
        uint32_t src_indices[VX_VK_MAX_DESCRIPTOR_VALUES];
        uint32_t dst_indices[VX_VK_MAX_DESCRIPTOR_VALUES];
        if (!descriptor_layout_range_indices(src->layout, copy->srcBinding, copy->srcArrayElement,
                copy->descriptorCount, src_indices) ||
            !descriptor_layout_range_indices(dst->layout, copy->dstBinding, copy->dstArrayElement,
                copy->descriptorCount, dst_indices)) continue;
        int overlap = 0;
        if (src == dst) {
            for (uint32_t source = 0; source < copy->descriptorCount && !overlap; source++)
                for (uint32_t destination = 0; destination < copy->descriptorCount; destination++)
                    if (src_indices[source] == dst_indices[destination]) { overlap = 1; break; }
        }
        if (overlap) continue;
        vx_vk_descriptor_value values[VX_VK_MAX_DESCRIPTOR_VALUES];
        for (uint32_t element = 0; element < copy->descriptorCount; element++) {
            values[element] = src->values[src_indices[element]];
            if (values[element].buffer && !values[element].buffer->externally_alive)
                values[element] = (vx_vk_descriptor_value){ 0 };
            if (values[element].image && !values[element].image->externally_alive)
                values[element] = (vx_vk_descriptor_value){ 0 };
            if (values[element].sampler && !values[element].sampler->externally_alive)
                values[element] = (vx_vk_descriptor_value){ 0 };
        }
        for (uint32_t element = 0; element < copy->descriptorCount; element++) {
            vx_vk_descriptor_value *destination = &dst->values[dst_indices[element]];
            vx_vk_buffer *old_buffer = destination->buffer;
            vx_vk_image *old_image = destination->image;
            vx_vk_sampler *old_sampler = destination->sampler;
            if (values[element].buffer) buffer_retain(values[element].buffer);
            if (values[element].image) image_retain(values[element].image);
            if (values[element].sampler) sampler_retain(values[element].sampler);
            *destination = values[element];
            if (old_buffer) buffer_release(old_buffer);
            if (old_image) image_release(old_image);
            if (old_sampler) sampler_release(old_sampler);
        }
        dst->generation++;
    }
}

static int command_buffer_track_descriptor_set(VkCommandBuffer commandBuffer, vx_vk_descriptor_set *set) {
    for (uint32_t i = 0; i < commandBuffer->used_descriptor_set_count; i++)
        if (commandBuffer->used_descriptor_sets[i].set == set) return 1;
    if (commandBuffer->used_descriptor_set_count == commandBuffer->used_descriptor_set_capacity) {
        uint32_t capacity = commandBuffer->used_descriptor_set_capacity ?
            commandBuffer->used_descriptor_set_capacity * 2u : 4u;
        size_t bytes = (size_t)capacity * sizeof *commandBuffer->used_descriptor_sets;
        if (capacity < commandBuffer->used_descriptor_set_capacity ||
            bytes / sizeof *commandBuffer->used_descriptor_sets != capacity) return 0;
        VkAllocationCallbacks *allocator = commandBuffer->pool->has_allocator ?
            &commandBuffer->pool->allocator : NULL;
        vx_vk_descriptor_set_version *replacement = object_allocate(allocator, bytes,
            alignof(vx_vk_descriptor_set_version), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        if (!replacement) return 0;
        if (commandBuffer->used_descriptor_set_count)
            memcpy(replacement, commandBuffer->used_descriptor_sets,
                (size_t)commandBuffer->used_descriptor_set_count * sizeof *replacement);
        object_free(allocator, commandBuffer->used_descriptor_sets);
        commandBuffer->used_descriptor_sets = replacement;
        commandBuffer->used_descriptor_set_capacity = capacity;
    }
    descriptor_set_retain(set);
    commandBuffer->used_descriptor_sets[commandBuffer->used_descriptor_set_count++] =
        (vx_vk_descriptor_set_version){ .set = set, .generation = set->generation };
    return 1;
}

static int command_buffer_descriptor_sets_current(const VkCommandBuffer commandBuffer) {
    for (uint32_t i = 0; i < commandBuffer->used_descriptor_set_count; i++) {
        const vx_vk_descriptor_set_version *version = &commandBuffer->used_descriptor_sets[i];
        if (!version->set || version->set->magic != VX_VK_DESCRIPTOR_SET_MAGIC ||
            !version->set->externally_alive || version->set->generation != version->generation) return 0;
    }
    return 1;
}

static void command_buffer_clear_descriptor_sets(VkCommandBuffer commandBuffer) {
    for (uint32_t i = 0; i < commandBuffer->used_descriptor_set_count; i++)
        descriptor_set_release(commandBuffer->used_descriptor_sets[i].set);
    commandBuffer->used_descriptor_set_count = 0;
    VkAllocationCallbacks *allocator = commandBuffer->pool && commandBuffer->pool->has_allocator ?
        &commandBuffer->pool->allocator : NULL;
    object_free(allocator, commandBuffer->used_descriptor_sets);
    commandBuffer->used_descriptor_sets = NULL;
    commandBuffer->used_descriptor_set_capacity = 0;
}

static int command_buffer_track_buffer(VkCommandBuffer commandBuffer, vx_vk_buffer *buffer) {
    if (!commandBuffer || !buffer || buffer->magic != VX_VK_BUFFER_MAGIC ||
        !buffer->externally_alive || buffer->device != commandBuffer->device) return 0;
    for (uint32_t i = 0; i < commandBuffer->used_buffer_count; i++)
        if (commandBuffer->used_buffers[i].buffer == buffer) return 1;
    if (buffer->refs == UINT32_MAX || commandBuffer->used_buffer_count == UINT32_MAX) return 0;

    VkAllocationCallbacks *allocator = commandBuffer->pool && commandBuffer->pool->has_allocator ?
        &commandBuffer->pool->allocator : NULL;
    vx_vk_buffer_use *use = object_allocate(allocator, sizeof *use, alignof(vx_vk_buffer_use),
                                             VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (!use) return 0;
    if (commandBuffer->used_buffer_count == commandBuffer->used_buffer_capacity) {
        uint32_t capacity = commandBuffer->used_buffer_capacity ? commandBuffer->used_buffer_capacity * 2u : 4u;
        size_t bytes = (size_t)capacity * sizeof *commandBuffer->used_buffers;
        if (capacity < commandBuffer->used_buffer_capacity ||
            bytes / sizeof *commandBuffer->used_buffers != capacity) {
            object_free(allocator, use);
            return 0;
        }
        vx_vk_command_buffer_buffer_use *replacement = object_allocate(allocator, bytes,
            alignof(vx_vk_command_buffer_buffer_use), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        if (!replacement) {
            object_free(allocator, use);
            return 0;
        }
        if (commandBuffer->used_buffer_count)
            memcpy(replacement, commandBuffer->used_buffers,
                (size_t)commandBuffer->used_buffer_count * sizeof *replacement);
        object_free(allocator, commandBuffer->used_buffers);
        commandBuffer->used_buffers = replacement;
        commandBuffer->used_buffer_capacity = capacity;
    }

    use->command_buffer = commandBuffer;
    use->next = buffer->command_buffer_uses;
    buffer->command_buffer_uses = use;
    buffer_retain(buffer);
    commandBuffer->used_buffers[commandBuffer->used_buffer_count++] =
        (vx_vk_command_buffer_buffer_use){ .buffer = buffer, .use = use };
    return 1;
}

static void command_buffer_clear_buffers(VkCommandBuffer commandBuffer) {
    VkAllocationCallbacks *allocator = commandBuffer->pool && commandBuffer->pool->has_allocator ?
        &commandBuffer->pool->allocator : NULL;
    for (uint32_t i = 0; i < commandBuffer->used_buffer_count; i++) {
        vx_vk_command_buffer_buffer_use *tracked = &commandBuffer->used_buffers[i];
        vx_vk_buffer_use **link = &tracked->buffer->command_buffer_uses;
        while (*link && *link != tracked->use) link = &(*link)->next;
        if (*link) *link = tracked->use->next;
        object_free(allocator, tracked->use);
        buffer_release(tracked->buffer);
    }
    object_free(allocator, commandBuffer->used_buffers);
    commandBuffer->used_buffers = NULL;
    commandBuffer->used_buffer_count = 0;
    commandBuffer->used_buffer_capacity = 0;
}

static int command_buffer_track_image(VkCommandBuffer commandBuffer, vx_vk_image *image) {
    if (!commandBuffer || !image || image->magic != VX_VK_IMAGE_MAGIC ||
        !image->externally_alive || image->device != commandBuffer->device) return 0;
    for (uint32_t i = 0; i < commandBuffer->used_image_count; i++)
        if (commandBuffer->used_images[i].image == image) return 1;
    if (commandBuffer->used_image_count == UINT32_MAX) return 0;

    VkAllocationCallbacks *allocator = commandBuffer->pool && commandBuffer->pool->has_allocator ?
        &commandBuffer->pool->allocator : NULL;
    vx_vk_image_use *use = object_allocate(allocator, sizeof *use, alignof(vx_vk_image_use),
                                            VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (!use) return 0;
    if (commandBuffer->used_image_count == commandBuffer->used_image_capacity) {
        uint32_t capacity = commandBuffer->used_image_capacity ? commandBuffer->used_image_capacity * 2u : 4u;
        size_t bytes = (size_t)capacity * sizeof *commandBuffer->used_images;
        if (capacity < commandBuffer->used_image_capacity ||
            bytes / sizeof *commandBuffer->used_images != capacity) {
            object_free(allocator, use);
            return 0;
        }
        vx_vk_command_buffer_image_use *replacement = object_allocate(allocator, bytes,
            alignof(vx_vk_command_buffer_image_use), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        if (!replacement) {
            object_free(allocator, use);
            return 0;
        }
        if (commandBuffer->used_image_count)
            memcpy(replacement, commandBuffer->used_images,
                (size_t)commandBuffer->used_image_count * sizeof *replacement);
        object_free(allocator, commandBuffer->used_images);
        commandBuffer->used_images = replacement;
        commandBuffer->used_image_capacity = capacity;
    }

    use->command_buffer = commandBuffer;
    use->next = image->command_buffer_uses;
    image->command_buffer_uses = use;
    commandBuffer->used_images[commandBuffer->used_image_count++] =
        (vx_vk_command_buffer_image_use){ .image = image, .use = use };
    return 1;
}

static void command_buffer_clear_images(VkCommandBuffer commandBuffer) {
    VkAllocationCallbacks *allocator = commandBuffer->pool && commandBuffer->pool->has_allocator ?
        &commandBuffer->pool->allocator : NULL;
    for (uint32_t i = 0; i < commandBuffer->used_image_count; i++) {
        vx_vk_command_buffer_image_use *tracked = &commandBuffer->used_images[i];
        vx_vk_image_use **link = &tracked->image->command_buffer_uses;
        while (*link && *link != tracked->use) link = &(*link)->next;
        if (*link) *link = tracked->use->next;
        object_free(allocator, tracked->use);
    }
    object_free(allocator, commandBuffer->used_images);
    commandBuffer->used_images = NULL;
    commandBuffer->used_image_count = 0;
    commandBuffer->used_image_capacity = 0;
}

static int command_buffer_track_pipeline(VkCommandBuffer commandBuffer, vx_vk_pipeline *pipeline) {
    if (!commandBuffer || !pipeline || pipeline->magic != VX_VK_PIPELINE_MAGIC ||
        !pipeline->externally_alive || pipeline->device != commandBuffer->device || pipeline->refs == UINT32_MAX)
        return 0;
    for (uint32_t i = 0; i < commandBuffer->used_pipeline_count; i++)
        if (commandBuffer->used_pipelines[i].pipeline == pipeline) return 1;
    if (commandBuffer->used_pipeline_count == UINT32_MAX) return 0;

    VkAllocationCallbacks *allocator = commandBuffer->pool && commandBuffer->pool->has_allocator ?
        &commandBuffer->pool->allocator : NULL;
    vx_vk_pipeline_use *use = object_allocate(allocator, sizeof *use, alignof(vx_vk_pipeline_use),
                                               VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (!use) return 0;
    if (commandBuffer->used_pipeline_count == commandBuffer->used_pipeline_capacity) {
        uint32_t capacity = commandBuffer->used_pipeline_capacity ? commandBuffer->used_pipeline_capacity * 2u : 4u;
        size_t bytes = (size_t)capacity * sizeof *commandBuffer->used_pipelines;
        if (capacity < commandBuffer->used_pipeline_capacity ||
            bytes / sizeof *commandBuffer->used_pipelines != capacity) {
            object_free(allocator, use);
            return 0;
        }
        vx_vk_command_buffer_pipeline_use *replacement = object_allocate(allocator, bytes,
            alignof(vx_vk_command_buffer_pipeline_use), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        if (!replacement) {
            object_free(allocator, use);
            return 0;
        }
        if (commandBuffer->used_pipeline_count)
            memcpy(replacement, commandBuffer->used_pipelines,
                (size_t)commandBuffer->used_pipeline_count * sizeof *replacement);
        object_free(allocator, commandBuffer->used_pipelines);
        commandBuffer->used_pipelines = replacement;
        commandBuffer->used_pipeline_capacity = capacity;
    }

    use->command_buffer = commandBuffer;
    use->next = pipeline->command_buffer_uses;
    pipeline->command_buffer_uses = use;
    pipeline_retain(pipeline);
    commandBuffer->used_pipelines[commandBuffer->used_pipeline_count++] =
        (vx_vk_command_buffer_pipeline_use){ .pipeline = pipeline, .use = use };
    return 1;
}

static void command_buffer_clear_pipelines(VkCommandBuffer commandBuffer) {
    VkAllocationCallbacks *allocator = commandBuffer->pool && commandBuffer->pool->has_allocator ?
        &commandBuffer->pool->allocator : NULL;
    for (uint32_t i = 0; i < commandBuffer->used_pipeline_count; i++) {
        vx_vk_command_buffer_pipeline_use *tracked = &commandBuffer->used_pipelines[i];
        vx_vk_pipeline_use **link = &tracked->pipeline->command_buffer_uses;
        while (*link && *link != tracked->use) link = &(*link)->next;
        if (*link) *link = tracked->use->next;
        object_free(allocator, tracked->use);
        pipeline_release(tracked->pipeline);
    }
    object_free(allocator, commandBuffer->used_pipelines);
    commandBuffer->used_pipelines = NULL;
    commandBuffer->used_pipeline_count = 0;
    commandBuffer->used_pipeline_capacity = 0;
}

static int command_buffer_track_pipeline_layout(VkCommandBuffer commandBuffer, vx_vk_pipeline_layout *layout) {
    if (!commandBuffer || !layout || layout->magic != VX_VK_PIPELINE_LAYOUT_MAGIC ||
        !layout->externally_alive || layout->device != commandBuffer->device || layout->refs == UINT32_MAX)
        return 0;
    for (uint32_t i = 0; i < commandBuffer->used_pipeline_layout_count; i++)
        if (commandBuffer->used_pipeline_layouts[i].layout == layout) return 1;
    if (commandBuffer->used_pipeline_layout_count == UINT32_MAX) return 0;

    VkAllocationCallbacks *allocator = commandBuffer->pool && commandBuffer->pool->has_allocator ?
        &commandBuffer->pool->allocator : NULL;
    vx_vk_pipeline_layout_use *use = object_allocate(allocator, sizeof *use,
        alignof(vx_vk_pipeline_layout_use), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (!use) return 0;
    if (commandBuffer->used_pipeline_layout_count == commandBuffer->used_pipeline_layout_capacity) {
        uint32_t capacity = commandBuffer->used_pipeline_layout_capacity ?
            commandBuffer->used_pipeline_layout_capacity * 2u : 4u;
        size_t bytes = (size_t)capacity * sizeof *commandBuffer->used_pipeline_layouts;
        if (capacity < commandBuffer->used_pipeline_layout_capacity ||
            bytes / sizeof *commandBuffer->used_pipeline_layouts != capacity) {
            object_free(allocator, use);
            return 0;
        }
        vx_vk_command_buffer_pipeline_layout_use *replacement = object_allocate(allocator, bytes,
            alignof(vx_vk_command_buffer_pipeline_layout_use), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        if (!replacement) {
            object_free(allocator, use);
            return 0;
        }
        if (commandBuffer->used_pipeline_layout_count)
            memcpy(replacement, commandBuffer->used_pipeline_layouts,
                (size_t)commandBuffer->used_pipeline_layout_count * sizeof *replacement);
        object_free(allocator, commandBuffer->used_pipeline_layouts);
        commandBuffer->used_pipeline_layouts = replacement;
        commandBuffer->used_pipeline_layout_capacity = capacity;
    }

    use->command_buffer = commandBuffer;
    use->next = layout->command_buffer_uses;
    layout->command_buffer_uses = use;
    pipeline_layout_retain(layout);
    commandBuffer->used_pipeline_layouts[commandBuffer->used_pipeline_layout_count++] =
        (vx_vk_command_buffer_pipeline_layout_use){ .layout = layout, .use = use };
    return 1;
}

static void command_buffer_clear_pipeline_layouts(VkCommandBuffer commandBuffer) {
    VkAllocationCallbacks *allocator = commandBuffer->pool && commandBuffer->pool->has_allocator ?
        &commandBuffer->pool->allocator : NULL;
    for (uint32_t i = 0; i < commandBuffer->used_pipeline_layout_count; i++) {
        vx_vk_command_buffer_pipeline_layout_use *tracked = &commandBuffer->used_pipeline_layouts[i];
        vx_vk_pipeline_layout_use **link = &tracked->layout->command_buffer_uses;
        while (*link && *link != tracked->use) link = &(*link)->next;
        if (*link) *link = tracked->use->next;
        object_free(allocator, tracked->use);
        pipeline_layout_release(tracked->layout);
    }
    object_free(allocator, commandBuffer->used_pipeline_layouts);
    commandBuffer->used_pipeline_layouts = NULL;
    commandBuffer->used_pipeline_layout_count = 0;
    commandBuffer->used_pipeline_layout_capacity = 0;
}

static void command_buffer_clear_image_layout_ops(VkCommandBuffer commandBuffer) {
    for (uint32_t i = 0; i < commandBuffer->image_layout_op_count; i++)
        image_release(commandBuffer->image_layout_ops[i].image);
    commandBuffer->image_layout_op_count = 0;
    VkAllocationCallbacks *allocator = commandBuffer->pool && commandBuffer->pool->has_allocator ?
        &commandBuffer->pool->allocator : NULL;
    object_free(allocator, commandBuffer->image_layout_ops);
    commandBuffer->image_layout_ops = NULL;
    commandBuffer->image_layout_op_capacity = 0;
}

static void command_buffer_destroy(VkCommandBuffer commandBuffer) {
    if (!commandBuffer || commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC) return;
    vx_vk_command_pool *pool = commandBuffer->pool;
    if (commandBuffer->compute_pipeline) pipeline_release(commandBuffer->compute_pipeline);
    if (commandBuffer->graphics_pipeline) pipeline_release(commandBuffer->graphics_pipeline);
    if (commandBuffer->bound_layout) pipeline_layout_release(commandBuffer->bound_layout);
    for (uint32_t i = 0; i < VX_VK_MAX_DESCRIPTOR_SETS; i++) {
        vx_vk_descriptor_set *set = descriptor_set_from_internal_handle(commandBuffer->bound_sets[i],
                                                                          commandBuffer->device);
        if (set) descriptor_set_release(set);
    }
    command_buffer_clear_descriptor_sets(commandBuffer);
    command_buffer_clear_buffers(commandBuffer);
    command_buffer_clear_images(commandBuffer);
    command_buffer_clear_pipelines(commandBuffer);
    command_buffer_clear_pipeline_layouts(commandBuffer);
    command_buffer_clear_image_layout_ops(commandBuffer);
    gpu_ir_stream_destroy(&commandBuffer->stream);
    if (commandBuffer->push_constant_layout)
        pipeline_layout_release(commandBuffer->push_constant_layout);
    gpu_ir_resource_release(commandBuffer->push_constant_resource);
    commandBuffer->magic = 0;
    object_free(pool && pool->has_allocator ? &pool->allocator : NULL, commandBuffer);
}

static int command_pool_add(vx_vk_command_pool *pool, VkCommandBuffer commandBuffer) {
    if (pool->buffer_count == pool->buffer_capacity) {
        uint32_t capacity = pool->buffer_capacity ? pool->buffer_capacity * 2u : 8u;
        size_t allocation_size = (size_t)capacity * sizeof *pool->buffers;
        if (capacity < pool->buffer_capacity ||
            (capacity && allocation_size / sizeof *pool->buffers != capacity)) return 0;
        VkCommandBuffer *replacement = object_allocate(pool->has_allocator ? &pool->allocator : NULL,
            allocation_size, alignof(VkCommandBuffer), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        if (!replacement) return 0;
        if (pool->buffer_count) memcpy(replacement, pool->buffers,
                                       (size_t)pool->buffer_count * sizeof *replacement);
        object_free(pool->has_allocator ? &pool->allocator : NULL, pool->buffers);
        pool->buffers = replacement;
        pool->buffer_capacity = capacity;
    }
    pool->buffers[pool->buffer_count++] = commandBuffer;
    return 1;
}

static void command_pool_remove(vx_vk_command_pool *pool, VkCommandBuffer commandBuffer) {
    if (!pool) return;
    for (uint32_t i = 0; i < pool->buffer_count; i++) {
        if (pool->buffers[i] != commandBuffer) continue;
        pool->buffers[i] = pool->buffers[--pool->buffer_count];
        return;
    }
}

static void command_buffer_reset(VkCommandBuffer commandBuffer) {
    gpu_ir_stream_clear(&commandBuffer->stream);
    commandBuffer->render_pass_active = 0;
    commandBuffer->render_pass_image = NULL;
    commandBuffer->render_pass_layer = 0;
    commandBuffer->render_pass_width = 0;
    commandBuffer->render_pass_height = 0;
    commandBuffer->render_pass_attachment_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    commandBuffer->render_pass_final_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (commandBuffer->compute_pipeline) pipeline_release(commandBuffer->compute_pipeline);
    commandBuffer->compute_pipeline = NULL;
    if (commandBuffer->graphics_pipeline) pipeline_release(commandBuffer->graphics_pipeline);
    commandBuffer->graphics_pipeline = NULL;
    commandBuffer->index_buffer = NULL;
    commandBuffer->index_buffer_offset = 0;
    commandBuffer->index_type = 0;
    memset(commandBuffer->vertex_buffers, 0, sizeof commandBuffer->vertex_buffers);
    memset(commandBuffer->vertex_buffer_offsets, 0, sizeof commandBuffer->vertex_buffer_offsets);
    memset(&commandBuffer->dynamic_viewport, 0, sizeof commandBuffer->dynamic_viewport);
    memset(&commandBuffer->dynamic_scissor, 0, sizeof commandBuffer->dynamic_scissor);
    memset(commandBuffer->dynamic_blend_constants, 0, sizeof commandBuffer->dynamic_blend_constants);
    commandBuffer->dynamic_viewport_set = 0;
    commandBuffer->dynamic_scissor_set = 0;
    commandBuffer->dynamic_blend_constants_set = 0;
    if (commandBuffer->bound_layout) pipeline_layout_release(commandBuffer->bound_layout);
    commandBuffer->bound_layout = NULL;
    if (commandBuffer->push_constant_layout) pipeline_layout_release(commandBuffer->push_constant_layout);
    commandBuffer->push_constant_layout = NULL;
    gpu_ir_resource_release(commandBuffer->push_constant_resource);
    commandBuffer->push_constant_resource = NULL;
    for (uint32_t i = 0; i < VX_VK_MAX_DESCRIPTOR_SETS; i++) {
        vx_vk_descriptor_set *set = descriptor_set_from_internal_handle(commandBuffer->bound_sets[i],
                                                                          commandBuffer->device);
        if (set) descriptor_set_release(set);
        commandBuffer->bound_sets[i] = 0;
    }
    memset(commandBuffer->bound_dynamic_offsets, 0, sizeof commandBuffer->bound_dynamic_offsets);
    command_buffer_clear_descriptor_sets(commandBuffer);
    command_buffer_clear_buffers(commandBuffer);
    command_buffer_clear_images(commandBuffer);
    command_buffer_clear_pipelines(commandBuffer);
    command_buffer_clear_pipeline_layouts(commandBuffer);
    command_buffer_clear_image_layout_ops(commandBuffer);
    commandBuffer->state = VX_VK_COMMAND_INITIAL;
    commandBuffer->usage_flags = 0;
    commandBuffer->record_error = GPU_IR_OK;
}

static int command_buffer_record_image_layout_op(VkCommandBuffer commandBuffer, vx_vk_image *image,
    int transition, VkImageLayout old_layout, VkImageLayout new_layout,
    uint32_t base_array_layer, uint32_t array_layer_count) {
    if (!commandBuffer || !image || image->refs == UINT32_MAX ||
        base_array_layer >= image->array_layers || !array_layer_count ||
        array_layer_count > image->array_layers - base_array_layer ||
        commandBuffer->image_layout_op_count == UINT32_MAX) return GPU_IR_RANGE;
    if (commandBuffer->image_layout_op_count == commandBuffer->image_layout_op_capacity) {
        uint32_t capacity = commandBuffer->image_layout_op_capacity ?
            commandBuffer->image_layout_op_capacity * 2u : 8u;
        size_t allocation_size = (size_t)capacity * sizeof *commandBuffer->image_layout_ops;
        if (capacity < commandBuffer->image_layout_op_capacity ||
            (capacity && allocation_size / sizeof *commandBuffer->image_layout_ops != capacity))
            return GPU_IR_RANGE;
        VkAllocationCallbacks *allocator = commandBuffer->pool && commandBuffer->pool->has_allocator ?
            &commandBuffer->pool->allocator : NULL;
        vx_vk_image_layout_op *replacement = object_allocate(allocator, allocation_size,
            alignof(vx_vk_image_layout_op), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        if (!replacement) return GPU_IR_OUT_OF_MEMORY;
        if (commandBuffer->image_layout_op_count)
            memcpy(replacement, commandBuffer->image_layout_ops,
                (size_t)commandBuffer->image_layout_op_count * sizeof *replacement);
        object_free(allocator, commandBuffer->image_layout_ops);
        commandBuffer->image_layout_ops = replacement;
        commandBuffer->image_layout_op_capacity = capacity;
    }
    if (!command_buffer_track_image(commandBuffer, image)) return GPU_IR_OUT_OF_MEMORY;
    image_retain(image);
    commandBuffer->image_layout_ops[commandBuffer->image_layout_op_count++] = (vx_vk_image_layout_op){
        .image = image,
        .old_layout = old_layout,
        .new_layout = new_layout,
        .base_array_layer = base_array_layer,
        .array_layer_count = array_layer_count,
        .transition = transition != 0
    };
    return GPU_IR_OK;
}

VkResult WINAPI vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkCommandPool *pCommandPool) {
    if (!pCommandPool) return VK_ERROR_INITIALIZATION_FAILED;
    *pCommandPool = 0;
    const uint32_t allowed_flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (!device_is_valid(device) || !pCreateInfo || pCreateInfo->sType != VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO ||
        pCreateInfo->pNext || (pCreateInfo->flags & ~allowed_flags) ||
        pCreateInfo->queueFamilyIndex != VX_VK_QUEUE_FAMILY || !allocator_is_valid(pAllocator))
        return VK_ERROR_INITIALIZATION_FAILED;
    vx_vk_command_pool *pool = object_allocate(pAllocator, sizeof *pool, alignof(vx_vk_command_pool),
                                                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pool->magic = VX_VK_COMMAND_POOL_MAGIC;
    pool->device = device;
    pool->flags = pCreateInfo->flags;
    pool->family_index = pCreateInfo->queueFamilyIndex;
    if (pAllocator) {
        pool->allocator = *pAllocator;
        pool->has_allocator = 1;
    }
    *pCommandPool = command_pool_to_handle(pool);
    return VK_SUCCESS;
}

void WINAPI vkDestroyCommandPool(VkDevice device, VkCommandPool poolHandle,
    const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    vx_vk_command_pool *pool = command_pool_from_handle(poolHandle, device);
    if (!pool) return;
    while (pool->buffer_count) {
        VkCommandBuffer commandBuffer = pool->buffers[--pool->buffer_count];
        command_buffer_destroy(commandBuffer);
    }
    VkAllocationCallbacks allocator = pool->allocator;
    int has_allocator = pool->has_allocator;
    object_free(has_allocator ? &allocator : NULL, pool->buffers);
    pool->magic = 0;
    object_free(has_allocator ? &allocator : NULL, pool);
}

VkResult WINAPI vkResetCommandPool(VkDevice device, VkCommandPool poolHandle, VkFlags flags) {
    vx_vk_command_pool *pool = command_pool_from_handle(poolHandle, device);
    if (!pool || flags) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < pool->buffer_count; i++) {
        VkCommandBuffer commandBuffer = pool->buffers[i];
        if (!commandBuffer || commandBuffer->state == VX_VK_COMMAND_PENDING) return VK_ERROR_DEVICE_LOST;
    }
    for (uint32_t i = 0; i < pool->buffer_count; i++) command_buffer_reset(pool->buffers[i]);
    return VK_SUCCESS;
}

void WINAPI vkTrimCommandPoolKHR(VkDevice device, VkCommandPool poolHandle,
                                  VkCommandPoolTrimFlagsKHR flags) {
    vx_vk_command_pool *pool = command_pool_from_handle(poolHandle, device);
    if (!pool || !device->maintenance1_enabled || flags) return;
    for (uint32_t i = 0; i < pool->buffer_count; i++) {
        VkCommandBuffer commandBuffer = pool->buffers[i];
        if (commandBuffer && commandBuffer->state == VX_VK_COMMAND_INITIAL &&
            gpu_ir_stream_count(&commandBuffer->stream) == 0) {
            gpu_ir_stream_destroy(&commandBuffer->stream);
            gpu_ir_stream_init(&commandBuffer->stream);
        }
    }
    if (!pool->buffer_count) {
        object_free(pool->has_allocator ? &pool->allocator : NULL, pool->buffers);
        pool->buffers = NULL;
        pool->buffer_capacity = 0;
    } else if (pool->buffer_capacity > pool->buffer_count) {
        size_t bytes = (size_t)pool->buffer_count * sizeof *pool->buffers;
        VkCommandBuffer *replacement = object_allocate(pool->has_allocator ? &pool->allocator : NULL, bytes,
            alignof(VkCommandBuffer), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        if (replacement) {
            memcpy(replacement, pool->buffers, bytes);
            object_free(pool->has_allocator ? &pool->allocator : NULL, pool->buffers);
            pool->buffers = replacement;
            pool->buffer_capacity = pool->buffer_count;
        }
    }
}

VkResult WINAPI vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *pAllocateInfo,
    VkCommandBuffer *pCommandBuffers) {
    if (!device_is_valid(device) || !pAllocateInfo ||
        pAllocateInfo->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO || pAllocateInfo->pNext ||
        !pAllocateInfo->commandBufferCount || !pCommandBuffers ||
        pAllocateInfo->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY)
        return VK_ERROR_INITIALIZATION_FAILED;
    vx_vk_command_pool *pool = command_pool_from_handle(pAllocateInfo->commandPool, device);
    if (!pool) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) pCommandBuffers[i] = NULL;
    for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) {
        VkCommandBuffer commandBuffer = object_allocate(pool->has_allocator ? &pool->allocator : NULL,
            sizeof *commandBuffer, alignof(struct VkCommandBuffer_T), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        if (!commandBuffer) goto fail;
        commandBuffer->dispatch = &g_vk_dispatch;
        commandBuffer->magic = VX_VK_COMMAND_BUFFER_MAGIC;
        commandBuffer->device = device;
        commandBuffer->pool = pool;
        commandBuffer->state = VX_VK_COMMAND_INITIAL;
        commandBuffer->record_error = GPU_IR_OK;
        gpu_ir_stream_init(&commandBuffer->stream);
        if (!command_pool_add(pool, commandBuffer)) {
            command_buffer_destroy(commandBuffer);
            goto fail;
        }
        pCommandBuffers[i] = commandBuffer;
    }
    return VK_SUCCESS;

fail:
    for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) {
        if (!pCommandBuffers[i]) continue;
        command_pool_remove(pool, pCommandBuffers[i]);
        command_buffer_destroy(pCommandBuffers[i]);
        pCommandBuffers[i] = NULL;
    }
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

void WINAPI vkFreeCommandBuffers(VkDevice device, VkCommandPool poolHandle, uint32_t commandBufferCount,
    const VkCommandBuffer *pCommandBuffers) {
    vx_vk_command_pool *pool = command_pool_from_handle(poolHandle, device);
    if (!pool || (commandBufferCount && !pCommandBuffers)) return;
    for (uint32_t i = 0; i < commandBufferCount; i++) {
        VkCommandBuffer commandBuffer = pCommandBuffers[i];
        if (!commandBuffer || commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->device != device ||
            commandBuffer->pool != pool || commandBuffer->state == VX_VK_COMMAND_PENDING) continue;
        command_pool_remove(pool, commandBuffer);
        command_buffer_destroy(commandBuffer);
    }
}

VkResult WINAPI vkBeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || !device_is_valid(commandBuffer->device) ||
        !pBeginInfo || pBeginInfo->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO || pBeginInfo->pNext ||
        pBeginInfo->pInheritanceInfo ||
        (pBeginInfo->flags & ~(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                               VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT |
                               VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT)) ||
        (pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT))
        return VK_ERROR_INITIALIZATION_FAILED;
    if (commandBuffer->state == VX_VK_COMMAND_PENDING || commandBuffer->state == VX_VK_COMMAND_RECORDING ||
        (commandBuffer->state != VX_VK_COMMAND_INITIAL &&
         !(commandBuffer->pool->flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT)))
        return VK_ERROR_INITIALIZATION_FAILED;
    command_buffer_reset(commandBuffer);
    commandBuffer->state = VX_VK_COMMAND_RECORDING;
    commandBuffer->usage_flags = pBeginInfo->flags;
    return VK_SUCCESS;
}

VkResult WINAPI vkEndCommandBuffer(VkCommandBuffer commandBuffer) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (commandBuffer->record_error != GPU_IR_OK || commandBuffer->render_pass_active) {
        commandBuffer->state = VX_VK_COMMAND_INVALID;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!command_buffer_descriptor_sets_current(commandBuffer)) {
        commandBuffer->state = VX_VK_COMMAND_INVALID;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    commandBuffer->state = VX_VK_COMMAND_EXECUTABLE;
    return VK_SUCCESS;
}

void WINAPI vkCmdBeginRenderPass(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo *pRenderPassBegin,
    VkSubpassContents contents) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING ||
        commandBuffer->record_error != GPU_IR_OK) return;
    vx_vk_render_pass *render_pass = pRenderPassBegin ?
        render_pass_from_handle(pRenderPassBegin->renderPass, commandBuffer->device) : NULL;
    vx_vk_framebuffer *framebuffer = pRenderPassBegin ?
        framebuffer_from_handle(pRenderPassBegin->framebuffer, commandBuffer->device) : NULL;
    if (!pRenderPassBegin || pRenderPassBegin->sType != VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO ||
        pRenderPassBegin->pNext || contents != VK_SUBPASS_CONTENTS_INLINE || !render_pass || !framebuffer ||
        commandBuffer->render_pass_active || framebuffer->render_pass != render_pass ||
        !framebuffer->view || !framebuffer->view->image->bound || !framebuffer->view->image->resource ||
        pRenderPassBegin->renderArea.offset.x < 0 || pRenderPassBegin->renderArea.offset.y < 0 ||
        !pRenderPassBegin->renderArea.extent.width || !pRenderPassBegin->renderArea.extent.height ||
        pRenderPassBegin->clearValueCount > 1 ||
        (pRenderPassBegin->clearValueCount && !pRenderPassBegin->pClearValues) ||
        (uint64_t)(uint32_t)pRenderPassBegin->renderArea.offset.x + pRenderPassBegin->renderArea.extent.width >
            framebuffer->width ||
        (uint64_t)(uint32_t)pRenderPassBegin->renderArea.offset.y + pRenderPassBegin->renderArea.extent.height >
            framebuffer->height ||
        (render_pass->attachment.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && !pRenderPassBegin->clearValueCount)) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    vx_vk_image *image = framebuffer->view->image;
    uint32_t layer = framebuffer->layer;
    if (layer >= image->array_layers ||
        (image->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0 ||
        ((render_pass->attachment.initialLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
          render_pass->attachment.finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) && !image->swapchain_owned) ||
        command_buffer_record_image_layout_op(commandBuffer, image, 1,
            render_pass->attachment.initialLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, layer, 1) != GPU_IR_OK) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    gpu_ir_render_pass_desc pass = {
        .mip_level = 0,
        .array_layer = 0,
        .render_area = {
            pRenderPassBegin->renderArea.offset.x,
            pRenderPassBegin->renderArea.offset.y,
            pRenderPassBegin->renderArea.offset.x + (int32_t)pRenderPassBegin->renderArea.extent.width,
            pRenderPassBegin->renderArea.offset.y + (int32_t)pRenderPassBegin->renderArea.extent.height
        },
        .load_action = render_pass->attachment.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ?
            GPU_IR_RENDER_PASS_CLEAR : render_pass->attachment.loadOp == VK_ATTACHMENT_LOAD_OP_DONT_CARE ?
            GPU_IR_RENDER_PASS_DONT_CARE : GPU_IR_RENDER_PASS_LOAD,
        .store_action = render_pass->attachment.storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE ?
            GPU_IR_RENDER_PASS_STORE_DONT_CARE : GPU_IR_RENDER_PASS_STORE,
        .clear_value_type = vk_clear_color_value_type(image->format)
    };
    if (render_pass->attachment.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
        const VkClearColorValue *color = &pRenderPassBegin->pClearValues[0].color;
        if (pass.clear_value_type == GPU_IR_CLEAR_COLOR_VALUE_UINT)
            memcpy(pass.clear_value.color_uint, color->uint32, sizeof pass.clear_value.color_uint);
        else if (pass.clear_value_type == GPU_IR_CLEAR_COLOR_VALUE_SINT)
            memcpy(pass.clear_value.color_sint, color->int32, sizeof pass.clear_value.color_sint);
        else memcpy(pass.clear_value.color, color->float32, sizeof pass.clear_value.color);
    }
    int pass_result = gpu_ir_record_begin_render_pass(&commandBuffer->stream,
        image_layer_resource(image, layer), &pass, NULL);
    if (pass_result != GPU_IR_OK) {
        commandBuffer->record_error = pass_result;
        return;
    }
    commandBuffer->render_pass_active = 1;
    commandBuffer->render_pass_image = image;
    commandBuffer->render_pass_layer = layer;
    commandBuffer->render_pass_width = framebuffer->width;
    commandBuffer->render_pass_height = framebuffer->height;
    commandBuffer->render_pass_attachment_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    commandBuffer->render_pass_final_layout = render_pass->attachment.finalLayout;
}

void WINAPI vkCmdEndRenderPass(VkCommandBuffer commandBuffer) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING ||
        commandBuffer->record_error != GPU_IR_OK) return;
    if (!commandBuffer->render_pass_active || !commandBuffer->render_pass_image) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    int rc = gpu_ir_record_end_render_pass(&commandBuffer->stream, NULL);
    if (rc != GPU_IR_OK) {
        commandBuffer->record_error = rc;
        commandBuffer->render_pass_active = 0;
        return;
    }
    rc = command_buffer_record_image_layout_op(commandBuffer, commandBuffer->render_pass_image, 1,
        commandBuffer->render_pass_attachment_layout, commandBuffer->render_pass_final_layout,
        commandBuffer->render_pass_layer, 1);
    commandBuffer->render_pass_active = 0;
    commandBuffer->render_pass_image = NULL;
    commandBuffer->render_pass_width = 0;
    commandBuffer->render_pass_height = 0;
    if (rc != GPU_IR_OK) commandBuffer->record_error = rc;
}

VkResult WINAPI vkResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || flags ||
        !(commandBuffer->pool->flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) ||
        commandBuffer->state == VX_VK_COMMAND_PENDING)
        return VK_ERROR_INITIALIZATION_FAILED;
    command_buffer_reset(commandBuffer);
    return VK_SUCCESS;
}

static int vk_buffer_ranges_overlap(uint64_t a_offset, uint64_t a_size, uint64_t b_offset, uint64_t b_size) {
    return a_offset <= b_offset ? b_offset - a_offset < a_size : a_offset - b_offset < b_size;
}

void WINAPI vkCmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer srcBufferHandle, VkBuffer dstBufferHandle,
    uint32_t regionCount, const VkBufferCopy *pRegions) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING) return;
    VkDevice device = commandBuffer->device;
    vx_vk_buffer *src = buffer_from_handle(srcBufferHandle, device);
    vx_vk_buffer *dst = buffer_from_handle(dstBufferHandle, device);
    if (!src || !dst || !src->bound || !dst->bound || !regionCount || !pRegions ||
        !(src->usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) || !(dst->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    if (!command_buffer_track_buffer(commandBuffer, src) || !command_buffer_track_buffer(commandBuffer, dst)) {
        commandBuffer->record_error = GPU_IR_OUT_OF_MEMORY;
        return;
    }
    for (uint32_t i = 0; i < regionCount; i++) {
        const VkBufferCopy *region = &pRegions[i];
        if (!region->size || (region->srcOffset | region->dstOffset | region->size) & 3u ||
            region->srcOffset > src->size || region->size > src->size - region->srcOffset ||
            region->dstOffset > dst->size || region->size > dst->size - region->dstOffset ||
            src->memory_offset > UINT64_MAX - region->srcOffset ||
            dst->memory_offset > UINT64_MAX - region->dstOffset) {
            commandBuffer->record_error = GPU_IR_RANGE;
            return;
        }
        uint64_t src_offset = src->memory_offset + region->srcOffset;
        uint64_t dst_offset = dst->memory_offset + region->dstOffset;
        if (src->memory->resource == dst->memory->resource &&
            vk_buffer_ranges_overlap(src_offset, region->size, dst_offset, region->size)) {
            commandBuffer->record_error = GPU_IR_INVALID;
            return;
        }
        for (uint32_t previous_index = 0; previous_index < i; previous_index++) {
            const VkBufferCopy *previous = &pRegions[previous_index];
            uint64_t previous_src_offset = src->memory_offset + previous->srcOffset;
            uint64_t previous_dst_offset = dst->memory_offset + previous->dstOffset;
            if (src->memory->resource == dst->memory->resource &&
                (vk_buffer_ranges_overlap(src_offset, region->size, previous_dst_offset, previous->size) ||
                 vk_buffer_ranges_overlap(dst_offset, region->size, previous_src_offset, previous->size) ||
                 vk_buffer_ranges_overlap(dst_offset, region->size, previous_dst_offset, previous->size))) {
                commandBuffer->record_error = GPU_IR_INVALID;
                return;
            }
        }
        int rc = gpu_ir_record_copy_buffer(&commandBuffer->stream, src->memory->resource, src_offset,
            dst->memory->resource, dst_offset, region->size, NULL);
        if (rc != GPU_IR_OK) {
            commandBuffer->record_error = rc;
            return;
        }
    }
}

static int vk_image_copy_regions_overlap(const vx_vk_buffer *buffer, const vx_vk_image *image,
    const VkBufferImageCopy *region, uint64_t buffer_row_pitch, uint64_t buffer_image_pitch) {
    if (!buffer || !image || !region || buffer->memory != image->memory) return 0;
    uint64_t bytes = (uint64_t)region->imageExtent.width * image->bytes_per_texel;
    uint32_t layer_count = region->imageSubresource.layerCount;
    uint64_t buffer_first = buffer->memory_offset + region->bufferOffset;
    uint64_t image_first = image->memory_offset +
        (uint64_t)region->imageSubresource.baseArrayLayer * image->array_pitch +
        (uint64_t)(uint32_t)region->imageOffset.y * image->row_pitch +
        (uint64_t)(uint32_t)region->imageOffset.x * image->bytes_per_texel;
    uint64_t buffer_last = buffer_first + (uint64_t)(layer_count - 1u) * buffer_image_pitch +
        (uint64_t)(region->imageExtent.height - 1u) * buffer_row_pitch + bytes;
    uint64_t image_last = image_first + (uint64_t)(layer_count - 1u) * image->array_pitch +
        (uint64_t)(region->imageExtent.height - 1u) * image->row_pitch + bytes;
    if (buffer_first >= image_last || image_first >= buffer_last) return 0;
    for (uint32_t buffer_layer = 0; buffer_layer < layer_count; buffer_layer++) {
        for (uint32_t image_layer = 0; image_layer < layer_count; image_layer++) {
            uint64_t buffer_row = buffer_first + (uint64_t)buffer_layer * buffer_image_pitch;
            uint64_t image_row = image_first + (uint64_t)image_layer * image->array_pitch;
            uint32_t buffer_y = 0, image_y = 0;
            while (buffer_y < region->imageExtent.height && image_y < region->imageExtent.height) {
                uint64_t buffer_end = buffer_row + bytes;
                uint64_t image_end = image_row + bytes;
                if (buffer_row < image_end && image_row < buffer_end) return 1;
                if (buffer_end <= image_row) {
                    buffer_row += buffer_row_pitch;
                    buffer_y++;
                } else {
                    image_row += image->row_pitch;
                    image_y++;
                }
            }
        }
    }
    return 0;
}

static int vk_record_buffer_image_copy(VkCommandBuffer commandBuffer, VkBuffer bufferHandle, VkImage imageHandle,
    VkImageLayout imageLayout, uint32_t regionCount, const VkBufferImageCopy *regions, int buffer_to_image) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING)
        return GPU_IR_INVALID;
    vx_vk_buffer *buffer = buffer_from_handle(bufferHandle, commandBuffer->device);
    vx_vk_image *image = image_from_handle(imageHandle, commandBuffer->device);
    VkImageLayout expected_layout = buffer_to_image ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL :
                                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (!buffer || !buffer->bound || !buffer->memory || !image || !image->bound || !image->resource ||
        !regionCount || !regions || !(buffer->usage & (buffer_to_image ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT :
                                                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT)) ||
        !(image->usage & (buffer_to_image ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) ||
        (imageLayout != expected_layout && imageLayout != VK_IMAGE_LAYOUT_GENERAL)) return GPU_IR_INVALID;
    if (!command_buffer_track_buffer(commandBuffer, buffer)) return GPU_IR_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < regionCount; i++) {
        const VkBufferImageCopy *region = &regions[i];
        if (region->bufferOffset & 3u || region->imageSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
            region->imageSubresource.mipLevel || !region->imageSubresource.layerCount ||
            region->imageSubresource.baseArrayLayer >= image->array_layers ||
            region->imageSubresource.layerCount > image->array_layers - region->imageSubresource.baseArrayLayer ||
            region->imageOffset.x < 0 || region->imageOffset.y < 0 ||
            region->imageOffset.z || !region->imageExtent.width || !region->imageExtent.height ||
            region->imageExtent.depth != 1 ||
            (region->bufferRowLength && region->bufferRowLength < region->imageExtent.width) ||
            (region->bufferImageHeight && region->bufferImageHeight < region->imageExtent.height))
            return GPU_IR_INVALID;
        uint32_t row_length = region->bufferRowLength ? region->bufferRowLength : region->imageExtent.width;
        uint32_t image_height = region->bufferImageHeight ? region->bufferImageHeight : region->imageExtent.height;
        uint64_t row_pitch = (uint64_t)row_length * image->bytes_per_texel;
        if (image_height && row_pitch > UINT64_MAX / image_height) return GPU_IR_RANGE;
        uint64_t image_pitch = row_pitch * image_height;
        uint64_t copied_rows = region->imageExtent.height - 1u;
        if (copied_rows && row_pitch > (UINT64_MAX -
            (uint64_t)region->imageExtent.width * image->bytes_per_texel) / copied_rows) return GPU_IR_RANGE;
        uint64_t footprint = copied_rows * row_pitch + (uint64_t)region->imageExtent.width * image->bytes_per_texel;
        uint64_t layer_count = region->imageSubresource.layerCount;
        uint64_t preceding_layers = layer_count - 1u;
        if (preceding_layers && image_pitch > (UINT64_MAX - footprint) / preceding_layers)
            return GPU_IR_RANGE;
        uint64_t total_footprint = preceding_layers * image_pitch + footprint;
        if (region->imageOffset.x > (int32_t)image->width || region->imageOffset.y > (int32_t)image->height ||
            region->imageExtent.width > image->width - (uint32_t)region->imageOffset.x ||
            region->imageExtent.height > image->height - (uint32_t)region->imageOffset.y ||
            region->bufferOffset > buffer->size || total_footprint > buffer->size - region->bufferOffset ||
            row_pitch > SIZE_MAX || image_pitch > SIZE_MAX ||
            buffer->memory_offset > UINT64_MAX - region->bufferOffset) return GPU_IR_RANGE;
        // Vulkan permits disjoint buffer and image bindings to share an allocation. Reject only copies whose
        // actual row ranges overlap, since those bytes have undefined results for a blit.
        if (vk_image_copy_regions_overlap(buffer, image, region, row_pitch, image_pitch)) return GPU_IR_INVALID;
        for (uint32_t layer = 0; layer < region->imageSubresource.layerCount; layer++) {
            gpu_ir_texture_location buffer_location = {
                .buffer_offset = buffer->memory_offset + region->bufferOffset + (uint64_t)layer * image_pitch,
                .row_pitch = row_pitch,
                .image_pitch = image_pitch,
                .buffer_width = row_length,
                .buffer_height = image_height,
                .buffer_depth = 1
            };
            gpu_ir_texture_location image_location = {
                .mip_level = 0,
                .array_slice = image->layer_resources ? 0 : region->imageSubresource.baseArrayLayer + layer,
                .origin_x = (uint32_t)region->imageOffset.x,
                .origin_y = (uint32_t)region->imageOffset.y
            };
            gpu_ir_resource *layer_resource = image_layer_resource(image,
                region->imageSubresource.baseArrayLayer + layer);
            if (!layer_resource) return GPU_IR_INVALID;
            int rc = buffer_to_image ? gpu_ir_record_copy_texture(&commandBuffer->stream, buffer->memory->resource,
                &buffer_location, layer_resource, &image_location, region->imageExtent.width,
                region->imageExtent.height, 1, NULL) : gpu_ir_record_copy_texture(&commandBuffer->stream,
                layer_resource, &image_location, buffer->memory->resource, &buffer_location,
                region->imageExtent.width, region->imageExtent.height, 1, NULL);
            if (rc != GPU_IR_OK) return rc;
        }
        int layout_rc = command_buffer_record_image_layout_op(commandBuffer, image, 0,
            imageLayout, imageLayout, region->imageSubresource.baseArrayLayer,
            region->imageSubresource.layerCount);
        if (layout_rc != GPU_IR_OK) return layout_rc;
    }
    return GPU_IR_OK;
}

void WINAPI vkCmdCopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage,
    VkImageLayout dstImageLayout, uint32_t regionCount, const VkBufferImageCopy *pRegions) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING) return;
    int rc = vk_record_buffer_image_copy(commandBuffer, srcBuffer, dstImage, dstImageLayout,
                                         regionCount, pRegions, 1);
    if (rc != GPU_IR_OK) commandBuffer->record_error = rc;
}

void WINAPI vkCmdCopyImageToBuffer(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout,
    VkBuffer dstBuffer, uint32_t regionCount, const VkBufferImageCopy *pRegions) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING) return;
    int rc = vk_record_buffer_image_copy(commandBuffer, dstBuffer, srcImage, srcImageLayout,
                                         regionCount, pRegions, 0);
    if (rc != GPU_IR_OK) commandBuffer->record_error = rc;
}

static int vk_image_copy_rectangles_overlap(const vx_vk_image *a, uint32_t a_layer,
    const VkOffset3D *a_offset, const VkExtent3D *a_extent, const vx_vk_image *b, uint32_t b_layer,
    const VkOffset3D *b_offset, const VkExtent3D *b_extent) {
    if (!a || !b || !a_offset || !a_extent || !b_offset || !b_extent || a->memory != b->memory) return 0;
    if (a_layer >= a->array_layers || b_layer >= b->array_layers) return -1;
    uint64_t row_bytes_a = (uint64_t)a_extent->width * a->bytes_per_texel;
    uint64_t row_bytes_b = (uint64_t)b_extent->width * b->bytes_per_texel;
    uint64_t row_a = a->memory_offset + (uint64_t)a_layer * a->array_pitch +
        (uint64_t)(uint32_t)a_offset->y * a->row_pitch + (uint64_t)(uint32_t)a_offset->x * a->bytes_per_texel;
    uint64_t row_b = b->memory_offset + (uint64_t)b_layer * b->array_pitch +
        (uint64_t)(uint32_t)b_offset->y * b->row_pitch + (uint64_t)(uint32_t)b_offset->x * b->bytes_per_texel;
    uint32_t y_a = 0, y_b = 0;
    while (y_a < a_extent->height && y_b < b_extent->height) {
        if (row_a > UINT64_MAX - row_bytes_a || row_b > UINT64_MAX - row_bytes_b) return -1;
        uint64_t end_a = row_a + row_bytes_a;
        uint64_t end_b = row_b + row_bytes_b;
        if (row_a < end_b && row_b < end_a) return 1;
        if (end_a <= row_b) {
            if (row_a > UINT64_MAX - a->row_pitch) return -1;
            row_a += a->row_pitch;
            y_a++;
        } else {
            if (row_b > UINT64_MAX - b->row_pitch) return -1;
            row_b += b->row_pitch;
            y_b++;
        }
    }
    return 0;
}

static int vk_record_image_copy(VkCommandBuffer commandBuffer, VkImage srcImageHandle,
    VkImageLayout srcImageLayout, VkImage dstImageHandle, VkImageLayout dstImageLayout,
    uint32_t regionCount, const VkImageCopy *regions) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING)
        return GPU_IR_INVALID;
    vx_vk_image *src = image_from_handle(srcImageHandle, commandBuffer->device);
    vx_vk_image *dst = image_from_handle(dstImageHandle, commandBuffer->device);
    if (!src || !src->bound || !src->memory || !src->resource || !dst || !dst->bound || !dst->memory ||
        !dst->resource || !regionCount || !regions || !(src->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
        !(dst->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        (srcImageLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && srcImageLayout != VK_IMAGE_LAYOUT_GENERAL) ||
        (dstImageLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && dstImageLayout != VK_IMAGE_LAYOUT_GENERAL))
        return GPU_IR_INVALID;
    if (src == dst && (srcImageLayout != VK_IMAGE_LAYOUT_GENERAL || dstImageLayout != VK_IMAGE_LAYOUT_GENERAL))
        return GPU_IR_INVALID;
    if (src->format != dst->format || src->bytes_per_texel != dst->bytes_per_texel)
        return GPU_IR_UNSUPPORTED;

    for (uint32_t i = 0; i < regionCount; i++) {
        const VkImageCopy *region = &regions[i];
        if (region->srcSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
            region->dstSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
            region->srcSubresource.mipLevel || region->dstSubresource.mipLevel ||
            !region->srcSubresource.layerCount ||
            region->srcSubresource.layerCount != region->dstSubresource.layerCount ||
            region->srcSubresource.baseArrayLayer >= src->array_layers ||
            region->dstSubresource.baseArrayLayer >= dst->array_layers ||
            region->srcSubresource.layerCount > src->array_layers - region->srcSubresource.baseArrayLayer ||
            region->dstSubresource.layerCount > dst->array_layers - region->dstSubresource.baseArrayLayer ||
            region->srcOffset.x < 0 || region->srcOffset.y < 0 || region->srcOffset.z ||
            region->dstOffset.x < 0 || region->dstOffset.y < 0 || region->dstOffset.z ||
            !region->extent.width || !region->extent.height || region->extent.depth != 1)
            return GPU_IR_INVALID;
        if ((uint32_t)region->srcOffset.x > src->width || (uint32_t)region->srcOffset.y > src->height ||
            region->extent.width > src->width - (uint32_t)region->srcOffset.x ||
            region->extent.height > src->height - (uint32_t)region->srcOffset.y ||
            (uint32_t)region->dstOffset.x > dst->width || (uint32_t)region->dstOffset.y > dst->height ||
            region->extent.width > dst->width - (uint32_t)region->dstOffset.x ||
            region->extent.height > dst->height - (uint32_t)region->dstOffset.y)
            return GPU_IR_RANGE;
    }

    for (uint32_t i = 0; i < regionCount; i++) {
        const VkImageCopy *region = &regions[i];
        if (src->memory == dst->memory) {
            for (uint32_t j = 0; j < regionCount; j++) {
                const VkImageCopy *other = &regions[j];
                for (uint32_t src_layer = 0; src_layer < region->srcSubresource.layerCount; src_layer++) {
                    for (uint32_t dst_layer = 0; dst_layer < other->dstSubresource.layerCount; dst_layer++) {
                        int overlap = vk_image_copy_rectangles_overlap(src,
                            region->srcSubresource.baseArrayLayer + src_layer, &region->srcOffset, &region->extent,
                            dst, other->dstSubresource.baseArrayLayer + dst_layer,
                            &other->dstOffset, &other->extent);
                        if (overlap != 0) return overlap < 0 ? GPU_IR_RANGE : GPU_IR_INVALID;
                    }
                }
            }
        }
        for (uint32_t j = 0; j < i; j++) {
            const VkImageCopy *other = &regions[j];
            for (uint32_t dst_layer = 0; dst_layer < region->dstSubresource.layerCount; dst_layer++) {
                for (uint32_t other_layer = 0; other_layer < other->dstSubresource.layerCount; other_layer++) {
                    int overlap = vk_image_copy_rectangles_overlap(dst,
                        region->dstSubresource.baseArrayLayer + dst_layer, &region->dstOffset, &region->extent,
                        dst, other->dstSubresource.baseArrayLayer + other_layer,
                        &other->dstOffset, &other->extent);
                    if (overlap != 0) return overlap < 0 ? GPU_IR_RANGE : GPU_IR_INVALID;
                }
            }
        }

        for (uint32_t layer = 0; layer < region->srcSubresource.layerCount; layer++) {
            uint32_t src_layer = region->srcSubresource.baseArrayLayer + layer;
            uint32_t dst_layer = region->dstSubresource.baseArrayLayer + layer;
            gpu_ir_resource *src_resource = image_layer_resource(src, src_layer);
            gpu_ir_resource *dst_resource = image_layer_resource(dst, dst_layer);
            if (!src_resource || !dst_resource) return GPU_IR_INVALID;
            gpu_ir_texture_location src_location = {
                .mip_level = 0, .array_slice = src->layer_resources ? 0 : src_layer,
                .origin_x = (uint32_t)region->srcOffset.x, .origin_y = (uint32_t)region->srcOffset.y
            };
            gpu_ir_texture_location dst_location = {
                .mip_level = 0, .array_slice = dst->layer_resources ? 0 : dst_layer,
                .origin_x = (uint32_t)region->dstOffset.x, .origin_y = (uint32_t)region->dstOffset.y
            };
            int rc = gpu_ir_record_copy_texture(&commandBuffer->stream, src_resource, &src_location,
                dst_resource, &dst_location, region->extent.width, region->extent.height, 1, NULL);
            if (rc != GPU_IR_OK) return rc;
        }
        int layout_rc = command_buffer_record_image_layout_op(commandBuffer, src, 0,
            srcImageLayout, srcImageLayout, region->srcSubresource.baseArrayLayer,
            region->srcSubresource.layerCount);
        if (layout_rc != GPU_IR_OK) return layout_rc;
        layout_rc = command_buffer_record_image_layout_op(commandBuffer, dst, 0,
            dstImageLayout, dstImageLayout, region->dstSubresource.baseArrayLayer,
            region->dstSubresource.layerCount);
        if (layout_rc != GPU_IR_OK) return layout_rc;
    }
    return GPU_IR_OK;
}

void WINAPI vkCmdCopyImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout,
    VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageCopy *pRegions) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING) return;
    int rc = vk_record_image_copy(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout,
                                  regionCount, pRegions);
    if (rc != GPU_IR_OK) commandBuffer->record_error = rc;
}

void WINAPI vkCmdClearColorImage(VkCommandBuffer commandBuffer, VkImage imageHandle, VkImageLayout imageLayout,
    const VkClearColorValue *pColor, uint32_t rangeCount, const VkImageSubresourceRange *pRanges) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING)
        return;
    vx_vk_image *image = image_from_handle(imageHandle, commandBuffer->device);
    if (!image || !image->bound || !image->resource || !pColor || !rangeCount || !pRanges ||
        !(image->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        (imageLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && imageLayout != VK_IMAGE_LAYOUT_GENERAL)) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    for (uint32_t i = 0; i < rangeCount; i++) {
        const VkImageSubresourceRange *range = &pRanges[i];
        uint32_t layer_count = range->layerCount == VK_REMAINING_ARRAY_LAYERS &&
            range->baseArrayLayer <= image->array_layers ? image->array_layers - range->baseArrayLayer :
            range->layerCount;
        if (range->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || range->baseMipLevel != 0 ||
            (range->levelCount != 1 && range->levelCount != VK_REMAINING_MIP_LEVELS) ||
            range->baseArrayLayer >= image->array_layers || !layer_count ||
            layer_count > image->array_layers - range->baseArrayLayer) {
            commandBuffer->record_error = GPU_IR_INVALID;
            return;
        }
        gpu_ir_clear_color_value_type value_type = vk_clear_color_value_type(image->format);
        const void *clear_value = value_type == GPU_IR_CLEAR_COLOR_VALUE_UINT ?
            (const void *)pColor->uint32 : value_type == GPU_IR_CLEAR_COLOR_VALUE_SINT ?
            (const void *)pColor->int32 : (const void *)pColor->float32;
        for (uint32_t layer = 0; layer < layer_count; layer++) {
            uint32_t image_layer = range->baseArrayLayer + layer;
            gpu_ir_resource *layer_resource = image_layer_resource(image, image_layer);
            if (!layer_resource) {
                commandBuffer->record_error = GPU_IR_INVALID;
                return;
            }
            uint32_t resource_layer = image->layer_resources ? 0 : image_layer;
            int rc = gpu_ir_record_clear_color_layers_typed(&commandBuffer->stream, layer_resource, value_type,
                clear_value, resource_layer, 1, NULL, 0, NULL);
            if (rc != GPU_IR_OK) {
                commandBuffer->record_error = rc;
                return;
            }
        }
        int layout_rc = command_buffer_record_image_layout_op(commandBuffer, image, 0,
            imageLayout, imageLayout, range->baseArrayLayer, layer_count);
        if (layout_rc != GPU_IR_OK) {
            commandBuffer->record_error = layout_rc;
            return;
        }
    }
}

void WINAPI vkCmdUpdateBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBufferHandle, VkDeviceSize dstOffset,
                              VkDeviceSize dataSize, const void *pData) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING) return;
    vx_vk_buffer *dst = buffer_from_handle(dstBufferHandle, commandBuffer->device);
    if (!dst || !dst->bound || !dst->memory || !(dst->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
        !pData || !dataSize || dataSize > 65536 || (dstOffset & 3u) || (dataSize & 3u) ||
        dstOffset >= dst->size || dataSize > dst->size - dstOffset ||
        dst->memory_offset > UINT64_MAX - dstOffset) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    if (!command_buffer_track_buffer(commandBuffer, dst)) {
        commandBuffer->record_error = GPU_IR_OUT_OF_MEMORY;
        return;
    }
    int rc = gpu_ir_record_update_buffer(&commandBuffer->stream, dst->memory->resource,
        dst->memory_offset + dstOffset, pData, dataSize, NULL);
    if (rc != GPU_IR_OK) commandBuffer->record_error = rc;
}

void WINAPI vkCmdFillBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBufferHandle, VkDeviceSize dstOffset,
                            VkDeviceSize size, uint32_t data) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING) return;
    vx_vk_buffer *dst = buffer_from_handle(dstBufferHandle, commandBuffer->device);
    if (!dst || !dst->bound || !dst->memory || !(dst->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
        dstOffset >= dst->size || (dstOffset & 3u) || dst->memory_offset > UINT64_MAX - dstOffset) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    uint64_t fill_size = size;
    uint64_t remaining = dst->size - dstOffset;
    if (size == VK_WHOLE_SIZE) {
        fill_size = remaining & ~(uint64_t)3u;
    } else if (!size || (size & 3u) || size > remaining) {
        commandBuffer->record_error = GPU_IR_RANGE;
        return;
    }
    if (!fill_size) return;
    if (!command_buffer_track_buffer(commandBuffer, dst)) {
        commandBuffer->record_error = GPU_IR_OUT_OF_MEMORY;
        return;
    }
    int rc = gpu_ir_record_fill_buffer(&commandBuffer->stream, dst->memory->resource,
        dst->memory_offset + dstOffset, fill_size, data, NULL);
    if (rc != GPU_IR_OK) commandBuffer->record_error = rc;
}

static int vk_stage_scope(VkPipelineStageFlags stages, uint64_t *scope) {
    const VkPipelineStageFlags known = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT |
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (!scope || !stages || (stages & ~known)) return 0;
    uint64_t result = 0;
#define VK_MAP_STAGE(vk_bit, ir_bit) do { if (stages & (vk_bit)) result |= (ir_bit); } while (0)
    VK_MAP_STAGE(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, GPU_IR_SYNC_STAGE_TOP_OF_PIPE);
    VK_MAP_STAGE(VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, GPU_IR_SYNC_STAGE_DRAW_INDIRECT);
    VK_MAP_STAGE(VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, GPU_IR_SYNC_STAGE_VERTEX_INPUT);
    VK_MAP_STAGE(VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, GPU_IR_SYNC_STAGE_VERTEX_SHADER);
    VK_MAP_STAGE(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, GPU_IR_SYNC_STAGE_FRAGMENT_SHADER);
    VK_MAP_STAGE(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, GPU_IR_SYNC_STAGE_EARLY_FRAGMENT_TESTS);
    VK_MAP_STAGE(VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, GPU_IR_SYNC_STAGE_LATE_FRAGMENT_TESTS);
    VK_MAP_STAGE(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, GPU_IR_SYNC_STAGE_COLOR_ATTACHMENT_OUTPUT);
    VK_MAP_STAGE(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, GPU_IR_SYNC_STAGE_COMPUTE_SHADER);
    VK_MAP_STAGE(VK_PIPELINE_STAGE_TRANSFER_BIT, GPU_IR_SYNC_STAGE_TRANSFER);
    VK_MAP_STAGE(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, GPU_IR_SYNC_STAGE_BOTTOM_OF_PIPE);
    VK_MAP_STAGE(VK_PIPELINE_STAGE_HOST_BIT, GPU_IR_SYNC_STAGE_HOST);
    if (stages & VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT) result |= GPU_IR_SYNC_STAGE_ALL_GRAPHICS;
    if (stages & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) result |= GPU_IR_SYNC_STAGE_ALL_COMMANDS;
#undef VK_MAP_STAGE
    *scope = result;
    return 1;
}

static int vk_access_mask(VkAccessFlags access, uint32_t *mask) {
    const VkAccessFlags supported = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
        VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
        VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    if (!mask || (access & ~supported)) return 0;
    uint32_t result = 0;
    if (access & VK_ACCESS_INDIRECT_COMMAND_READ_BIT) result |= GPU_IR_ACCESS_INDIRECT_READ;
    if (access & VK_ACCESS_INDEX_READ_BIT) result |= GPU_IR_ACCESS_INDEX_READ;
    if (access & VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT) result |= GPU_IR_ACCESS_VERTEX_READ;
    if (access & VK_ACCESS_UNIFORM_READ_BIT) result |= GPU_IR_ACCESS_UNIFORM_READ;
    if (access & VK_ACCESS_INPUT_ATTACHMENT_READ_BIT) result |= GPU_IR_ACCESS_INPUT_ATTACHMENT_READ;
    if (access & VK_ACCESS_SHADER_READ_BIT) result |= GPU_IR_ACCESS_SHADER_READ;
    if (access & VK_ACCESS_SHADER_WRITE_BIT) result |= GPU_IR_ACCESS_SHADER_WRITE;
    if (access & VK_ACCESS_COLOR_ATTACHMENT_READ_BIT) result |= GPU_IR_ACCESS_COLOR_READ;
    if (access & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) result |= GPU_IR_ACCESS_COLOR_WRITE;
    if (access & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT) result |= GPU_IR_ACCESS_DEPTH_READ;
    if (access & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) result |= GPU_IR_ACCESS_DEPTH_WRITE;
    if (access & VK_ACCESS_TRANSFER_READ_BIT) result |= GPU_IR_ACCESS_COPY_READ;
    if (access & VK_ACCESS_TRANSFER_WRITE_BIT) result |= GPU_IR_ACCESS_COPY_WRITE;
    if (access & VK_ACCESS_HOST_READ_BIT) result |= GPU_IR_ACCESS_HOST_READ;
    if (access & VK_ACCESS_HOST_WRITE_BIT) result |= GPU_IR_ACCESS_HOST_WRITE;
    if (access & VK_ACCESS_MEMORY_READ_BIT) result |= GPU_IR_ACCESS_MEMORY_READ;
    if (access & VK_ACCESS_MEMORY_WRITE_BIT) result |= GPU_IR_ACCESS_MEMORY_WRITE;
    *mask = result;
    return 1;
}

static int vk_access_scope_supported(VkAccessFlags access, VkPipelineStageFlags stages) {
    const VkAccessFlags supported = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
        VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT |
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    if (access & ~supported) return 0;
    if (access & (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT))
        access &= ~(VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    if (!access) return 1;

    const VkPipelineStageFlags all = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkPipelineStageFlags graphics = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    if ((access & VK_ACCESS_INDIRECT_COMMAND_READ_BIT) &&
        !(stages & (VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | graphics | all))) return 0;
    if ((access & VK_ACCESS_INDEX_READ_BIT) &&
        !(stages & (VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | graphics | all))) return 0;
    if ((access & VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT) &&
        !(stages & (VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | graphics | all))) return 0;
    const VkPipelineStageFlags shader_stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if ((access & (VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)) &&
        !(stages & (shader_stages | graphics | all))) return 0;
    if ((access & VK_ACCESS_INPUT_ATTACHMENT_READ_BIT) &&
        !(stages & (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | graphics | all))) return 0;
    if ((access & VK_ACCESS_COLOR_ATTACHMENT_READ_BIT) &&
        !(stages & (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                    graphics | all))) return 0;
    if ((access & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) &&
        !(stages & (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | graphics | all))) return 0;
    if ((access & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT) &&
        !(stages & (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | graphics | all))) return 0;
    if ((access & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) &&
        !(stages & (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                    graphics | all))) return 0;
    if ((access & (VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT)) &&
        !(stages & (VK_PIPELINE_STAGE_TRANSFER_BIT | all))) return 0;
    if ((access & (VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT)) &&
        !(stages & VK_PIPELINE_STAGE_HOST_BIT)) return 0;
    return 1;
}

void WINAPI vkCmdPipelineBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags, uint32_t memoryBarrierCount,
    const VkMemoryBarrier *pMemoryBarriers, uint32_t bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier *pBufferMemoryBarriers, uint32_t imageMemoryBarrierCount,
    const VkImageMemoryBarrier *pImageMemoryBarriers) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING) return;
    uint64_t source_scope = 0, destination_scope = 0;
    if (!vk_stage_scope(srcStageMask, &source_scope) || !vk_stage_scope(dstStageMask, &destination_scope) ||
        (dependencyFlags & ~VK_DEPENDENCY_BY_REGION_BIT) ||
        (memoryBarrierCount && !pMemoryBarriers) || (bufferMemoryBarrierCount && !pBufferMemoryBarriers) ||
        (imageMemoryBarrierCount && !pImageMemoryBarriers)) {
        commandBuffer->record_error = GPU_IR_UNSUPPORTED;
        return;
    }
    uint32_t dependency = (dependencyFlags & VK_DEPENDENCY_BY_REGION_BIT) ?
        GPU_IR_BARRIER_DEPENDENCY_BY_REGION : 0;
    for (uint32_t i = 0; i < memoryBarrierCount; i++) {
        const VkMemoryBarrier *source = &pMemoryBarriers[i];
        uint32_t before = 0, after = 0;
        if (source->sType != VK_STRUCTURE_TYPE_MEMORY_BARRIER || source->pNext ||
            !vk_access_mask(source->srcAccessMask, &before) || !vk_access_mask(source->dstAccessMask, &after) ||
            !vk_access_scope_supported(source->srcAccessMask, srcStageMask) ||
            !vk_access_scope_supported(source->dstAccessMask, dstStageMask)) {
            commandBuffer->record_error = GPU_IR_UNSUPPORTED;
            return;
        }
        gpu_ir_barrier_desc barrier = {
            .type = GPU_IR_BARRIER_MEMORY_DEPENDENCY,
            .before_access = before,
            .after_access = after,
            .source_stage_scope = source_scope,
            .destination_stage_scope = destination_scope,
            .dependency_flags = dependency,
            .source_queue_family = GPU_IR_QUEUE_FAMILY_IGNORED,
            .destination_queue_family = GPU_IR_QUEUE_FAMILY_IGNORED
        };
        int rc = gpu_ir_record_barrier(&commandBuffer->stream, &barrier, NULL);
        if (rc != GPU_IR_OK) { commandBuffer->record_error = rc; return; }
    }
    for (uint32_t i = 0; i < bufferMemoryBarrierCount; i++) {
        const VkBufferMemoryBarrier *source = &pBufferMemoryBarriers[i];
        vx_vk_buffer *buffer = buffer_from_handle(source->buffer, commandBuffer->device);
        uint32_t before = 0, after = 0;
        if (source->sType != VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER || source->pNext ||
            !buffer || !buffer->bound || !vk_access_mask(source->srcAccessMask, &before) ||
            !vk_access_mask(source->dstAccessMask, &after) ||
            !vk_access_scope_supported(source->srcAccessMask, srcStageMask) ||
            !vk_access_scope_supported(source->dstAccessMask, dstStageMask) || source->offset > buffer->size) {
            commandBuffer->record_error = GPU_IR_UNSUPPORTED;
            return;
        }
        if ((source->srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED &&
             source->srcQueueFamilyIndex != VX_VK_QUEUE_FAMILY) ||
            (source->dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED &&
             source->dstQueueFamilyIndex != VX_VK_QUEUE_FAMILY) ||
            ((source->srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED) !=
             (source->dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED))) {
            commandBuffer->record_error = GPU_IR_UNSUPPORTED;
            return;
        }
        uint64_t range_size = source->size == UINT64_MAX ? buffer->size - source->offset : source->size;
        if (!range_size || range_size > buffer->size - source->offset ||
            buffer->memory_offset > UINT64_MAX - source->offset) {
            commandBuffer->record_error = GPU_IR_RANGE;
            return;
        }
        if (!command_buffer_track_buffer(commandBuffer, buffer)) {
            commandBuffer->record_error = GPU_IR_OUT_OF_MEMORY;
            return;
        }
        uint32_t source_family = source->srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED ?
            GPU_IR_QUEUE_FAMILY_IGNORED : VX_VK_QUEUE_FAMILY;
        uint32_t destination_family = source->dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED ?
            GPU_IR_QUEUE_FAMILY_IGNORED : VX_VK_QUEUE_FAMILY;
        gpu_ir_barrier_desc barrier = {
            .type = GPU_IR_BARRIER_BUFFER_DEPENDENCY,
            .before_access = before,
            .after_access = after,
            .resource = buffer->memory->resource,
            .source_stage_scope = source_scope,
            .destination_stage_scope = destination_scope,
            .dependency_flags = dependency,
            .source_queue_family = source_family,
            .destination_queue_family = destination_family,
            .buffer_offset = buffer->memory_offset + source->offset,
            .buffer_size = range_size
        };
        int rc = gpu_ir_record_barrier(&commandBuffer->stream, &barrier, NULL);
        if (rc != GPU_IR_OK) { commandBuffer->record_error = rc; return; }
    }
    for (uint32_t i = 0; i < imageMemoryBarrierCount; i++) {
        const VkImageMemoryBarrier *source = &pImageMemoryBarriers[i];
        vx_vk_image *image = image_from_handle(source->image, commandBuffer->device);
        uint32_t before = 0, after = 0, old_layout = 0, new_layout = 0;
        uint32_t mip_count = source->subresourceRange.levelCount;
        uint32_t layer_count = source->subresourceRange.layerCount;
        if (mip_count == UINT32_MAX) mip_count = 1;
        if (layer_count == UINT32_MAX && image)
            layer_count = source->subresourceRange.baseArrayLayer <= image->array_layers ?
                image->array_layers - source->subresourceRange.baseArrayLayer : 0;
        if (source->sType != VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER || source->pNext ||
            !image || !image->bound || !image->resource ||
            !vk_access_mask(source->srcAccessMask, &before) || !vk_access_mask(source->dstAccessMask, &after) ||
            !vk_access_scope_supported(source->srcAccessMask, srcStageMask) ||
            !vk_access_scope_supported(source->dstAccessMask, dstStageMask) ||
            !vk_image_layout_to_gpu(source->oldLayout, &old_layout) ||
            !vk_image_layout_to_gpu(source->newLayout, &new_layout) ||
            ((source->oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
              source->newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) && !image->swapchain_owned) ||
            source->newLayout == VK_IMAGE_LAYOUT_UNDEFINED || source->newLayout == VK_IMAGE_LAYOUT_PREINITIALIZED ||
            source->subresourceRange.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
            source->subresourceRange.baseMipLevel || mip_count != 1 ||
            source->subresourceRange.baseArrayLayer >= image->array_layers || !layer_count ||
            layer_count > image->array_layers - source->subresourceRange.baseArrayLayer) {
            commandBuffer->record_error = GPU_IR_UNSUPPORTED;
            return;
        }
        if ((source->srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED &&
             source->srcQueueFamilyIndex != VX_VK_QUEUE_FAMILY) ||
            (source->dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED &&
             source->dstQueueFamilyIndex != VX_VK_QUEUE_FAMILY) ||
            ((source->srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED) !=
             (source->dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED))) {
            commandBuffer->record_error = GPU_IR_UNSUPPORTED;
            return;
        }
        if (((source->oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
              source->newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) &&
             !(image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) ||
            ((source->oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
              source->newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) &&
             !(image->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) ||
            ((source->oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
              source->newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) &&
             !(image->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) ||
            ((source->oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
              source->newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) &&
             !(image->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))) {
            commandBuffer->record_error = GPU_IR_INVALID;
            return;
        }
        uint32_t source_family = source->srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED ?
            GPU_IR_QUEUE_FAMILY_IGNORED : VX_VK_QUEUE_FAMILY;
        uint32_t destination_family = source->dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED ?
            GPU_IR_QUEUE_FAMILY_IGNORED : VX_VK_QUEUE_FAMILY;
        for (uint32_t offset = 0; offset < layer_count; offset++) {
            uint32_t layer = source->subresourceRange.baseArrayLayer + offset;
            gpu_ir_resource *layer_resource = image_layer_resource(image, layer);
            if (!layer_resource) { commandBuffer->record_error = GPU_IR_INVALID; return; }
            gpu_ir_barrier_desc barrier = {
            .type = GPU_IR_BARRIER_IMAGE_DEPENDENCY,
            .before_access = before,
            .after_access = after,
            .resource = layer_resource,
            .source_stage_scope = source_scope,
            .destination_stage_scope = destination_scope,
            .dependency_flags = dependency,
            .old_layout = old_layout,
            .new_layout = new_layout,
            .source_queue_family = source_family,
            .destination_queue_family = destination_family,
            .image_aspects = GPU_IR_IMAGE_ASPECT_COLOR,
            .base_mip_level = 0,
            .mip_level_count = 1,
            .base_array_layer = image->layer_resources ? 0 : layer,
            .array_layer_count = 1
            };
            int rc = gpu_ir_record_barrier(&commandBuffer->stream, &barrier, NULL);
            if (rc != GPU_IR_OK) {
                commandBuffer->record_error = rc; return;
            }
        }
        int rc = command_buffer_record_image_layout_op(commandBuffer, image, 1,
            source->oldLayout, source->newLayout, source->subresourceRange.baseArrayLayer, layer_count);
        if (rc != GPU_IR_OK) { commandBuffer->record_error = rc; return; }
    }
    // Even with no memory barriers, vkCmdPipelineBarrier creates an execution
    // dependency. Preserve the stage scopes in the common stream with empty
    // access scopes; Vulkan defines no memory accesses in either scope here.
    if (!memoryBarrierCount && !bufferMemoryBarrierCount && !imageMemoryBarrierCount) {
        gpu_ir_barrier_desc barrier = {
            .type = GPU_IR_BARRIER_MEMORY_DEPENDENCY,
            .source_stage_scope = source_scope,
            .destination_stage_scope = destination_scope,
            .dependency_flags = dependency,
            .source_queue_family = GPU_IR_QUEUE_FAMILY_IGNORED,
            .destination_queue_family = GPU_IR_QUEUE_FAMILY_IGNORED
        };
        int rc = gpu_ir_record_barrier(&commandBuffer->stream, &barrier, NULL);
        if (rc != GPU_IR_OK) commandBuffer->record_error = rc;
    }
}

void WINAPI vkCmdBindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
    VkPipeline pipelineHandle) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING) return;
    if (pipelineBindPoint != VK_PIPELINE_BIND_POINT_COMPUTE &&
        pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS) {
        commandBuffer->record_error = GPU_IR_UNSUPPORTED;
        return;
    }
    vx_vk_pipeline *pipeline = pipeline_from_handle(pipelineHandle, commandBuffer->device);
    if (!pipeline || pipeline->bind_point != pipelineBindPoint) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    if (!command_buffer_track_pipeline(commandBuffer, pipeline)) {
        commandBuffer->record_error = GPU_IR_OUT_OF_MEMORY;
        return;
    }
    pipeline_retain(pipeline);
    if (pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
        if (commandBuffer->compute_pipeline) pipeline_release(commandBuffer->compute_pipeline);
        commandBuffer->compute_pipeline = pipeline;
    } else {
        if (commandBuffer->graphics_pipeline) pipeline_release(commandBuffer->graphics_pipeline);
        commandBuffer->graphics_pipeline = pipeline;
    }
}

void WINAPI vkCmdSetViewport(VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount,
    const VkViewport *pViewports) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING ||
        commandBuffer->record_error != GPU_IR_OK) return;
    if (firstViewport != 0 || viewportCount != 1 || !pViewports ||
        !vk_viewport_is_valid(commandBuffer->device, pViewports)) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    commandBuffer->dynamic_viewport = pViewports[0];
    commandBuffer->dynamic_viewport_set = 1;
}

void WINAPI vkCmdSetScissor(VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount,
    const VkRect2D *pScissors) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING ||
        commandBuffer->record_error != GPU_IR_OK) return;
    if (firstScissor != 0 || scissorCount != 1 || !pScissors || !vk_scissor_is_valid(pScissors)) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    commandBuffer->dynamic_scissor = pScissors[0];
    commandBuffer->dynamic_scissor_set = 1;
}

void WINAPI vkCmdSetBlendConstants(VkCommandBuffer commandBuffer, const float blendConstants[4]) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING ||
        commandBuffer->record_error != GPU_IR_OK) return;
    if (!blendConstants) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    memcpy(commandBuffer->dynamic_blend_constants, blendConstants,
           sizeof commandBuffer->dynamic_blend_constants);
    commandBuffer->dynamic_blend_constants_set = 1;
}

static int vk_capture_draw_blend_constants(VkCommandBuffer commandBuffer, vx_vk_pipeline *pipeline,
                                            uint32_t *has_constants, float constants[4]) {
    if (!commandBuffer || !pipeline || !has_constants || !constants) return GPU_IR_INVALID;
    const gpu_ir_pipeline_info *info = gpu_ir_pipeline_description(pipeline->gpu_pipeline);
    if (!info) return GPU_IR_INVALID;
    return vk_resolve_draw_blend_constants(pipeline->dynamic_blend_constants,
        commandBuffer->dynamic_blend_constants_set, &info->color_blend,
        commandBuffer->dynamic_blend_constants, has_constants, constants);
}

void WINAPI vkCmdBindVertexBuffers(VkCommandBuffer commandBuffer, uint32_t firstBinding,
    uint32_t bindingCount, const VkBuffer *pBuffers, const VkDeviceSize *pOffsets) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING ||
        commandBuffer->record_error != GPU_IR_OK) return;
    if ((uint64_t)firstBinding + bindingCount > GPU_IR_MAX_VERTEX_BUFFERS ||
        (bindingCount && (!pBuffers || !pOffsets))) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    vx_vk_buffer *buffers[GPU_IR_MAX_VERTEX_BUFFERS] = { 0 };
    for (uint32_t i = 0; i < bindingCount; i++) {
        vx_vk_buffer *buffer = buffer_from_handle(pBuffers[i], commandBuffer->device);
        uint64_t offset = pOffsets[i];
        if (!buffer || !(buffer->usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) || !buffer->bound ||
            !buffer->memory || !buffer->memory->resource || offset >= buffer->size ||
            buffer->memory_offset > UINT64_MAX - offset ||
            buffer->memory_offset + offset >= gpu_ir_buffer_size(buffer->memory->resource)) {
            commandBuffer->record_error = GPU_IR_INVALID;
            return;
        }
        buffers[i] = buffer;
    }
    for (uint32_t i = 0; i < bindingCount; i++) {
        if (!command_buffer_track_buffer(commandBuffer, buffers[i])) {
            commandBuffer->record_error = GPU_IR_OUT_OF_MEMORY;
            return;
        }
    }
    for (uint32_t i = 0; i < bindingCount; i++) {
        commandBuffer->vertex_buffers[firstBinding + i] = buffers[i];
        commandBuffer->vertex_buffer_offsets[firstBinding + i] = pOffsets[i];
    }
}

static int vk_capture_vertex_buffers(VkCommandBuffer commandBuffer, vx_vk_pipeline *pipeline,
                                     gpu_ir_vertex_buffer_binding *captured, uint32_t *captured_count) {
    if (!commandBuffer || !pipeline || !captured || !captured_count || !pipeline->gpu_pipeline)
        return GPU_IR_INVALID;
    const gpu_ir_pipeline_info *description = gpu_ir_pipeline_description(pipeline->gpu_pipeline);
    if (!description || description->vertex_input.binding_count > GPU_IR_MAX_VERTEX_BUFFERS)
        return GPU_IR_INVALID;
    *captured_count = 0;
    for (uint32_t i = 0; i < description->vertex_input.binding_count; i++) {
        uint32_t binding = description->vertex_input.bindings[i].binding;
        if (binding >= GPU_IR_MAX_VERTEX_BUFFERS) return GPU_IR_INVALID;
        vx_vk_buffer *buffer = commandBuffer->vertex_buffers[binding];
        if (!buffer) continue;
        if (!buffer->externally_alive || !buffer->bound || !buffer->memory || !buffer->memory->resource ||
            buffer->memory_offset > UINT64_MAX - commandBuffer->vertex_buffer_offsets[binding])
            return GPU_IR_INVALID;
        captured[*captured_count] = (gpu_ir_vertex_buffer_binding){
            .binding = binding,
            .resource = buffer->memory->resource,
            .offset = buffer->memory_offset + commandBuffer->vertex_buffer_offsets[binding],
            .size = buffer->size - commandBuffer->vertex_buffer_offsets[binding]
        };
        (*captured_count)++;
    }
    return GPU_IR_OK;
}

static int vk_cmd_record_graphics_descriptors(VkCommandBuffer commandBuffer, vx_vk_pipeline *pipeline);

void WINAPI vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount,
    uint32_t firstVertex, uint32_t firstInstance) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING ||
        commandBuffer->record_error != GPU_IR_OK) return;
    vx_vk_pipeline *pipeline = commandBuffer->graphics_pipeline;
    if (!commandBuffer->render_pass_active || !commandBuffer->render_pass_image || !pipeline ||
        pipeline->bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS || !pipeline->gpu_pipeline ||
        pipeline->color_format != commandBuffer->render_pass_image->format ||
        (vertexCount && firstVertex > UINT32_MAX - (vertexCount - 1u)) ||
        (instanceCount && firstInstance > UINT32_MAX - (instanceCount - 1u))) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    const VkViewport *viewport = pipeline->dynamic_viewport ?
        (commandBuffer->dynamic_viewport_set ? &commandBuffer->dynamic_viewport : NULL) : &pipeline->viewport;
    const VkRect2D *scissor = pipeline->dynamic_scissor ?
        (commandBuffer->dynamic_scissor_set ? &commandBuffer->dynamic_scissor : NULL) : &pipeline->scissor;
    if (!viewport || !scissor) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    uint32_t has_blend_constants = 0;
    float blend_constants[4] = { 0 };
    int blend_rc = vk_capture_draw_blend_constants(commandBuffer, pipeline, &has_blend_constants, blend_constants);
    if (blend_rc != GPU_IR_OK) {
        commandBuffer->record_error = blend_rc;
        return;
    }
    gpu_ir_vertex_buffer_binding vertex_buffers[GPU_IR_MAX_VERTEX_BUFFERS] = { 0 };
    uint32_t vertex_buffer_count = 0;
    int vertex_rc = vk_capture_vertex_buffers(commandBuffer, pipeline, vertex_buffers, &vertex_buffer_count);
    if (vertex_rc != GPU_IR_OK) {
        commandBuffer->record_error = vertex_rc;
        return;
    }
    gpu_ir_draw_info draw = {
        .topology = pipeline->topology,
        .vertex_count = vertexCount,
        .instance_count = instanceCount,
        .first_vertex = firstVertex,
        .first_instance = firstInstance,
        .vertex_buffers_captured = 1,
        .vertex_buffer_count = vertex_buffer_count,
        .has_viewport = 1,
        .has_scissor = 1,
        .has_blend_constants = has_blend_constants,
        .viewport = { viewport->x, viewport->y, viewport->width,
                      viewport->height, viewport->minDepth, viewport->maxDepth },
        .scissor = { scissor->offset.x, scissor->offset.y,
                     scissor->extent.width, scissor->extent.height }
    };
    if (has_blend_constants) memcpy(draw.blend_constants, blend_constants, sizeof draw.blend_constants);
    if (vertex_buffer_count) memcpy(draw.vertex_buffers, vertex_buffers,
        (size_t)vertex_buffer_count * sizeof vertex_buffers[0]);
    int rc = vk_cmd_record_graphics_descriptors(commandBuffer, pipeline);
    if (rc == GPU_IR_OK) rc = gpu_ir_record_draw(&commandBuffer->stream, pipeline->gpu_pipeline, &draw, NULL);
    if (rc != GPU_IR_OK) commandBuffer->record_error = rc;
}

void WINAPI vkCmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer bufferHandle, VkDeviceSize offset,
                                 VkIndexType indexType) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING ||
        commandBuffer->record_error != GPU_IR_OK) return;
    vx_vk_buffer *buffer = buffer_from_handle(bufferHandle, commandBuffer->device);
    uint64_t index_size = indexType == VK_INDEX_TYPE_UINT16 ? 2u :
        indexType == VK_INDEX_TYPE_UINT32 ? 4u : 0u;
    if (!buffer || !index_size || !(buffer->usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) ||
        !buffer->bound || !buffer->memory || !buffer->memory->resource || offset >= buffer->size ||
        offset > UINT64_MAX - buffer->memory_offset ||
        ((buffer->memory_offset + offset) % index_size)) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    if (!command_buffer_track_buffer(commandBuffer, buffer)) {
        commandBuffer->record_error = GPU_IR_OUT_OF_MEMORY;
        return;
    }
    commandBuffer->index_buffer = buffer;
    commandBuffer->index_buffer_offset = offset;
    commandBuffer->index_type = indexType;
}

void WINAPI vkCmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount,
    uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING ||
        commandBuffer->record_error != GPU_IR_OK) return;
    vx_vk_pipeline *pipeline = commandBuffer->graphics_pipeline;
    vx_vk_buffer *buffer = commandBuffer->index_buffer;
    if (!commandBuffer->render_pass_active || !commandBuffer->render_pass_image || !pipeline ||
        pipeline->bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS || !pipeline->gpu_pipeline ||
        pipeline->color_format != commandBuffer->render_pass_image->format ||
        !buffer || !buffer->externally_alive || !buffer->bound || !buffer->memory ||
        !buffer->memory->resource || buffer->device != commandBuffer->device ||
        !(buffer->usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) ||
        (instanceCount && firstInstance > UINT32_MAX - (instanceCount - 1u))) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    uint64_t index_size = commandBuffer->index_type == VK_INDEX_TYPE_UINT16 ? 2u :
        commandBuffer->index_type == VK_INDEX_TYPE_UINT32 ? 4u : 0u;
    uint64_t first_byte, byte_count, end_byte;
    if (!index_size) {
        commandBuffer->record_error = GPU_IR_RANGE;
        return;
    }
    first_byte = (uint64_t)firstIndex * index_size;
    byte_count = (uint64_t)indexCount * index_size;
    if (commandBuffer->index_buffer_offset > UINT64_MAX - first_byte) {
        commandBuffer->record_error = GPU_IR_RANGE;
        return;
    }
    first_byte += commandBuffer->index_buffer_offset;
    if (byte_count > UINT64_MAX - first_byte) {
        commandBuffer->record_error = GPU_IR_RANGE;
        return;
    }
    end_byte = first_byte + byte_count;
    if (end_byte > buffer->size) {
        commandBuffer->record_error = GPU_IR_RANGE;
        return;
    }
    const VkViewport *viewport = pipeline->dynamic_viewport ?
        (commandBuffer->dynamic_viewport_set ? &commandBuffer->dynamic_viewport : NULL) : &pipeline->viewport;
    const VkRect2D *scissor = pipeline->dynamic_scissor ?
        (commandBuffer->dynamic_scissor_set ? &commandBuffer->dynamic_scissor : NULL) : &pipeline->scissor;
    if (!viewport || !scissor) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    uint32_t has_blend_constants = 0;
    float blend_constants[4] = { 0 };
    int blend_rc = vk_capture_draw_blend_constants(commandBuffer, pipeline, &has_blend_constants, blend_constants);
    if (blend_rc != GPU_IR_OK) {
        commandBuffer->record_error = blend_rc;
        return;
    }
    gpu_ir_vertex_buffer_binding vertex_buffers[GPU_IR_MAX_VERTEX_BUFFERS] = { 0 };
    uint32_t vertex_buffer_count = 0;
    int vertex_rc = vk_capture_vertex_buffers(commandBuffer, pipeline, vertex_buffers, &vertex_buffer_count);
    if (vertex_rc != GPU_IR_OK) {
        commandBuffer->record_error = vertex_rc;
        return;
    }
    gpu_ir_draw_info draw = {
        .topology = pipeline->topology,
        .indexed = 1,
        .index_count = indexCount,
        .instance_count = instanceCount,
        .first_index = firstIndex,
        .first_instance = firstInstance,
        .vertex_offset = vertexOffset,
        .index_type = commandBuffer->index_type == VK_INDEX_TYPE_UINT16 ?
            GPU_IR_INDEX_TYPE_UINT16 : GPU_IR_INDEX_TYPE_UINT32,
        .index_buffer_offset = buffer->memory_offset + commandBuffer->index_buffer_offset,
        .index_buffer_size = buffer->size - commandBuffer->index_buffer_offset,
        .index_buffer = buffer->memory->resource,
        .vertex_buffers_captured = 1,
        .vertex_buffer_count = vertex_buffer_count,
        .has_viewport = 1,
        .has_scissor = 1,
        .has_blend_constants = has_blend_constants,
        .viewport = { viewport->x, viewport->y, viewport->width,
                      viewport->height, viewport->minDepth, viewport->maxDepth },
        .scissor = { scissor->offset.x, scissor->offset.y,
                     scissor->extent.width, scissor->extent.height }
    };
    if (has_blend_constants) memcpy(draw.blend_constants, blend_constants, sizeof draw.blend_constants);
    if (vertex_buffer_count) memcpy(draw.vertex_buffers, vertex_buffers,
        (size_t)vertex_buffer_count * sizeof vertex_buffers[0]);
    int rc = vk_cmd_record_graphics_descriptors(commandBuffer, pipeline);
    if (rc == GPU_IR_OK) rc = gpu_ir_record_draw(&commandBuffer->stream, pipeline->gpu_pipeline, &draw, NULL);
    if (rc != GPU_IR_OK) commandBuffer->record_error = rc;
}

static void vk_cmd_draw_indirect(VkCommandBuffer commandBuffer, VkBuffer bufferHandle, VkDeviceSize offset,
                                 uint32_t drawCount, uint32_t stride, int indexed) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING ||
        commandBuffer->record_error != GPU_IR_OK) return;
    vx_vk_buffer *buffer = buffer_from_handle(bufferHandle, commandBuffer->device);
    uint32_t argument_size = indexed ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand);
    if (!commandBuffer->render_pass_active || !commandBuffer->render_pass_image || !buffer ||
        !buffer->externally_alive || !buffer->bound || !buffer->memory || !buffer->memory->resource ||
        !(buffer->usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) || (offset & 3u) ||
        drawCount > VX_VK_MAX_DRAW_INDIRECT_COUNT ||
        (drawCount > 1 && (!commandBuffer->device->multi_draw_indirect_enabled ||
                           stride < argument_size || (stride & 3u))) ||
        buffer->memory_offset > UINT64_MAX - offset) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    // Vulkan ignores stride when drawCount is zero or one. Normalize it for the common IR's
    // fixed-record range validation and Metal's per-record offset calculation.
    uint32_t effective_stride = drawCount > 1 ? stride : argument_size;
    if (drawCount) {
        uint64_t last_offset = (uint64_t)(drawCount - 1u) * effective_stride;
        if (offset > UINT64_MAX - last_offset || offset + last_offset > UINT64_MAX - argument_size ||
            offset + last_offset + argument_size > buffer->size) {
            commandBuffer->record_error = GPU_IR_RANGE;
            return;
        }
    }
    if (!drawCount) return;

    vx_vk_pipeline *pipeline = commandBuffer->graphics_pipeline;
    if (!pipeline || pipeline->bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS || !pipeline->gpu_pipeline ||
        pipeline->color_format != commandBuffer->render_pass_image->format) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    vx_vk_buffer *index_buffer = NULL;
    uint32_t gpu_index_type = GPU_IR_INDEX_TYPE_NONE;
    uint64_t index_buffer_offset = 0;
    if (indexed) {
        index_buffer = commandBuffer->index_buffer;
        uint64_t index_size = commandBuffer->index_type == VK_INDEX_TYPE_UINT16 ? 2u :
            commandBuffer->index_type == VK_INDEX_TYPE_UINT32 ? 4u : 0u;
        if (!index_buffer || !index_buffer->externally_alive || !index_buffer->bound || !index_buffer->memory ||
            !index_buffer->memory->resource || index_buffer->device != commandBuffer->device ||
            !(index_buffer->usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) || !index_size ||
            index_buffer->memory_offset > UINT64_MAX - commandBuffer->index_buffer_offset) {
            commandBuffer->record_error = GPU_IR_INVALID;
            return;
        }
        gpu_index_type = commandBuffer->index_type == VK_INDEX_TYPE_UINT16 ?
            GPU_IR_INDEX_TYPE_UINT16 : GPU_IR_INDEX_TYPE_UINT32;
        index_buffer_offset = index_buffer->memory_offset + commandBuffer->index_buffer_offset;
    }
    const VkViewport *viewport = pipeline->dynamic_viewport ?
        (commandBuffer->dynamic_viewport_set ? &commandBuffer->dynamic_viewport : NULL) : &pipeline->viewport;
    const VkRect2D *scissor = pipeline->dynamic_scissor ?
        (commandBuffer->dynamic_scissor_set ? &commandBuffer->dynamic_scissor : NULL) : &pipeline->scissor;
    if (!viewport || !scissor) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    uint32_t has_blend_constants = 0;
    float blend_constants[4] = { 0 };
    int blend_rc = vk_capture_draw_blend_constants(commandBuffer, pipeline, &has_blend_constants, blend_constants);
    if (blend_rc != GPU_IR_OK) {
        commandBuffer->record_error = blend_rc;
        return;
    }
    gpu_ir_vertex_buffer_binding vertex_buffers[GPU_IR_MAX_VERTEX_BUFFERS] = { 0 };
    uint32_t vertex_buffer_count = 0;
    int vertex_rc = vk_capture_vertex_buffers(commandBuffer, pipeline, vertex_buffers, &vertex_buffer_count);
    if (vertex_rc != GPU_IR_OK) {
        commandBuffer->record_error = vertex_rc;
        return;
    }
    if (!command_buffer_track_buffer(commandBuffer, buffer) ||
        (index_buffer && !command_buffer_track_buffer(commandBuffer, index_buffer))) {
        commandBuffer->record_error = GPU_IR_OUT_OF_MEMORY;
        return;
    }
    gpu_ir_indirect_draw_info draw = {
        .topology = pipeline->topology,
        .indexed = indexed != 0,
        .draw_count = drawCount,
        .stride = effective_stride,
        .argument_offset = buffer->memory_offset + offset,
        .index_buffer = index_buffer ? index_buffer->memory->resource : NULL,
        .index_type = gpu_index_type,
        .has_viewport = 1,
        .has_scissor = 1,
        .has_blend_constants = has_blend_constants,
        .index_buffer_offset = index_buffer_offset,
        .index_buffer_size = indexed ? index_buffer->size - commandBuffer->index_buffer_offset : 0,
        .vertex_buffers_captured = 1,
        .vertex_buffer_count = vertex_buffer_count,
        .viewport = { viewport->x, viewport->y, viewport->width,
                      viewport->height, viewport->minDepth, viewport->maxDepth },
        .scissor = { scissor->offset.x, scissor->offset.y,
                     scissor->extent.width, scissor->extent.height }
    };
    if (has_blend_constants) memcpy(draw.blend_constants, blend_constants, sizeof draw.blend_constants);
    if (vertex_buffer_count) memcpy(draw.vertex_buffers, vertex_buffers,
        (size_t)vertex_buffer_count * sizeof vertex_buffers[0]);
    int rc = vk_cmd_record_graphics_descriptors(commandBuffer, pipeline);
    if (rc == GPU_IR_OK) rc = gpu_ir_record_draw_indirect(&commandBuffer->stream, pipeline->gpu_pipeline,
                                                           buffer->memory->resource, &draw, NULL);
    if (rc != GPU_IR_OK) commandBuffer->record_error = rc;
}

void WINAPI vkCmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
                              uint32_t drawCount, uint32_t stride) {
    vk_cmd_draw_indirect(commandBuffer, buffer, offset, drawCount, stride, 0);
}

void WINAPI vkCmdDrawIndexedIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
                                     uint32_t drawCount, uint32_t stride) {
    vk_cmd_draw_indirect(commandBuffer, buffer, offset, drawCount, stride, 1);
}

void WINAPI vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layoutHandle, uint32_t firstSet, uint32_t descriptorSetCount,
    const VkDescriptorSet *pDescriptorSets, uint32_t dynamicOffsetCount, const uint32_t *pDynamicOffsets) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING) return;
    vx_vk_pipeline_layout *layout = pipeline_layout_from_handle(layoutHandle, commandBuffer->device);
    if ((pipelineBindPoint != VK_PIPELINE_BIND_POINT_COMPUTE &&
         pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS) || !layout ||
        (descriptorSetCount && !pDescriptorSets) ||
        (dynamicOffsetCount && !pDynamicOffsets) ||
        (uint64_t)firstSet + descriptorSetCount > layout->set_layout_count ||
        (uint64_t)firstSet + descriptorSetCount > VX_VK_MAX_DESCRIPTOR_SETS) {
        commandBuffer->record_error = dynamicOffsetCount ? GPU_IR_INVALID : GPU_IR_UNSUPPORTED;
        return;
    }
    vx_vk_descriptor_set *sets[VX_VK_MAX_DESCRIPTOR_SETS] = { 0 };
    for (uint32_t i = 0; i < descriptorSetCount; i++) {
        sets[i] = descriptor_set_from_handle(pDescriptorSets[i], commandBuffer->device);
        if (!sets[i] || !descriptor_layouts_compatible(sets[i]->layout, layout->set_layouts[firstSet + i])) {
            commandBuffer->record_error = GPU_IR_INVALID;
            return;
        }
    }
    uint32_t dynamic_offsets[VX_VK_MAX_DESCRIPTOR_SETS][VX_VK_MAX_DESCRIPTOR_VALUES] = { 0 };
    uint32_t dynamic_offset_index = 0;
    for (uint32_t i = 0; i < descriptorSetCount; i++) {
        vx_vk_descriptor_set *set = sets[i];
        for (uint32_t binding_index = 0; binding_index < set->layout->binding_count; binding_index++) {
            const VkDescriptorSetLayoutBinding *binding = &set->layout->bindings[binding_index];
            if (!vk_dynamic_buffer_descriptor_type(binding->descriptorType)) continue;
            for (uint32_t element = 0; element < binding->descriptorCount; element++) {
                if (dynamic_offset_index >= dynamicOffsetCount) {
                    commandBuffer->record_error = GPU_IR_INVALID;
                    return;
                }
                uint32_t offset = pDynamicOffsets[dynamic_offset_index++];
                uint32_t value_index;
                if (offset % VX_VK_BUFFER_ALIGNMENT ||
                    !descriptor_layout_value_index(set->layout, binding_index, element, &value_index)) {
                    commandBuffer->record_error = GPU_IR_INVALID;
                    return;
                }
                vx_vk_descriptor_value *descriptor = &set->values[value_index];
                if (descriptor->written) {
                    vx_vk_buffer *buffer = descriptor->buffer;
                    if (!buffer || buffer->magic != VX_VK_BUFFER_MAGIC || !buffer->externally_alive ||
                        buffer->device != commandBuffer->device) buffer = NULL;
                    if (!buffer || !buffer->bound || offset > UINT64_MAX - descriptor->offset ||
                        (descriptor->whole_size && offset) ||
                        descriptor->offset + offset > buffer->size ||
                        descriptor->range > buffer->size - (descriptor->offset + offset)) {
                        commandBuffer->record_error = GPU_IR_RANGE;
                        return;
                    }
                }
                dynamic_offsets[i][value_index] = offset;
            }
        }
    }
    if (dynamic_offset_index != dynamicOffsetCount) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    for (uint32_t i = 0; i < descriptorSetCount; i++) {
        if (!command_buffer_track_descriptor_set(commandBuffer, sets[i])) {
            commandBuffer->record_error = GPU_IR_OUT_OF_MEMORY;
            return;
        }
    }
    if (!command_buffer_track_pipeline_layout(commandBuffer, layout)) {
        commandBuffer->record_error = GPU_IR_OUT_OF_MEMORY;
        return;
    }
    pipeline_layout_retain(layout);
    if (commandBuffer->bound_layout) pipeline_layout_release(commandBuffer->bound_layout);
    commandBuffer->bound_layout = layout;
    for (uint32_t i = 0; i < descriptorSetCount; i++) {
        uint32_t set_index = firstSet + i;
        descriptor_set_retain(sets[i]);
        vx_vk_descriptor_set *old = descriptor_set_from_internal_handle(commandBuffer->bound_sets[set_index],
                                                                          commandBuffer->device);
        if (old) descriptor_set_release(old);
        commandBuffer->bound_sets[set_index] = pDescriptorSets[i];
        memcpy(commandBuffer->bound_dynamic_offsets[set_index], dynamic_offsets[i],
            sizeof commandBuffer->bound_dynamic_offsets[set_index]);
    }
}

void WINAPI vkCmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout layoutHandle,
    VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size, const void *pValues) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING) return;
    vx_vk_pipeline_layout *layout = pipeline_layout_from_handle(layoutHandle, commandBuffer->device);
    if (!layout || stageFlags != VK_SHADER_STAGE_COMPUTE_BIT || (offset & 3u) || !size || (size & 3u) ||
        !pValues || !pipeline_layout_covers_push_constants(layout, offset, size, GPU_IR_STAGE_COMPUTE) ||
        (commandBuffer->push_constant_layout &&
         !pipeline_layout_push_constants_compatible(commandBuffer->push_constant_layout, layout)) ||
        (commandBuffer->compute_pipeline &&
         !pipeline_layout_push_constants_compatible(commandBuffer->compute_pipeline->layout, layout))) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    if (!command_buffer_ensure_push_constant_resource(commandBuffer)) {
        commandBuffer->record_error = GPU_IR_OUT_OF_MEMORY;
        return;
    }
    int rc = gpu_ir_record_update_buffer(&commandBuffer->stream, commandBuffer->push_constant_resource,
                                        offset, pValues, size, NULL);
    if (rc != GPU_IR_OK) {
        commandBuffer->record_error = rc;
        return;
    }
    if (!command_buffer_track_pipeline_layout(commandBuffer, layout)) {
        commandBuffer->record_error = GPU_IR_OUT_OF_MEMORY;
        return;
    }
    if (!commandBuffer->push_constant_layout) {
        pipeline_layout_retain(layout);
        commandBuffer->push_constant_layout = layout;
    }
}

static int vk_descriptor_binding_value(VkCommandBuffer commandBuffer, uint32_t set_index,
    vx_vk_descriptor_set *set, uint32_t binding, uint32_t array_element, uint32_t stage_mask,
    gpu_ir_descriptor_value *out,
    vx_vk_buffer **buffer_out, vx_vk_image **image_out, VkImageLayout *image_layout_out) {
    uint32_t binding_index, value_index;
    if (!commandBuffer || set_index >= VX_VK_MAX_DESCRIPTOR_SETS || !set || !out ||
        !descriptor_layout_find(set->layout, binding, &binding_index) ||
        !descriptor_layout_value_index(set->layout, binding_index, array_element, &value_index) ||
        !set->values[value_index].written)
        return 0;
    vx_vk_descriptor_value *descriptor = &set->values[value_index];
    VkDescriptorType descriptor_type = set->layout->bindings[binding_index].descriptorType;
    if (descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER) {
        vx_vk_sampler *sampler = descriptor->sampler;
        if (!sampler || sampler->magic != VX_VK_SAMPLER_MAGIC || sampler->device != set->device ||
            !sampler->metal_state || commandBuffer->bound_dynamic_offsets[set_index][value_index]) return 0;
        *out = (gpu_ir_descriptor_value){
            .set = 0,
            .binding_namespace = 0,
            .binding = binding,
            .array_element = array_element,
            .kind = GPU_IR_DESCRIPTOR_SAMPLER,
            .stage_mask = stage_mask,
            .access_mask = 0,
            .backend_object = sampler->metal_state
        };
        if (buffer_out) *buffer_out = NULL;
        if (image_out) *image_out = NULL;
        if (image_layout_out) *image_layout_out = VK_IMAGE_LAYOUT_UNDEFINED;
        return 1;
    }
    if (vk_image_descriptor_type(descriptor_type)) {
        vx_vk_image *image = descriptor->image;
        vx_vk_sampler *sampler = descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ?
            descriptor->sampler : NULL;
        if (!image || image->magic != VX_VK_IMAGE_MAGIC || !image->externally_alive ||
            image->device != set->device || !image->bound || !image->resource ||
            (descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
             (!sampler || sampler->magic != VX_VK_SAMPLER_MAGIC || sampler->device != set->device ||
              !sampler->metal_state)) ||
            commandBuffer->bound_dynamic_offsets[set_index][value_index]) return 0;
        *out = (gpu_ir_descriptor_value){
            .set = 0,
            .binding_namespace = 0,
            .binding = binding,
            .array_element = array_element,
            .kind = descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? GPU_IR_DESCRIPTOR_SAMPLED_IMAGE :
                descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ?
                    GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER : GPU_IR_DESCRIPTOR_STORAGE_IMAGE,
            .stage_mask = stage_mask,
            .access_mask = descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ?
                GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE : GPU_IR_ACCESS_SHADER_READ,
            .resource = image->resource,
            // Vulkan component mappings affect the vector returned by sampled-image
            // instructions. Storage-image views require identity mappings.
            .component_mapping = descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? 0 :
                descriptor->component_mapping,
            .backend_object = sampler ? sampler->metal_state : NULL
        };
        if (buffer_out) *buffer_out = NULL;
        if (image_out) *image_out = image;
        if (image_layout_out) *image_layout_out = descriptor->image_layout;
        return 1;
    }
    uint32_t dynamic_offset = commandBuffer->bound_dynamic_offsets[set_index][value_index];
    vx_vk_buffer *buffer = descriptor->buffer;
    if (!buffer || buffer->magic != VX_VK_BUFFER_MAGIC || !buffer->externally_alive ||
        buffer->device != set->device) buffer = NULL;
    if (!buffer || !buffer->bound || !buffer->memory || dynamic_offset > UINT64_MAX - descriptor->offset ||
        buffer->memory_offset > UINT64_MAX - (descriptor->offset + dynamic_offset)) return 0;
    int uniform = vk_uniform_descriptor_type(descriptor_type);
    *out = (gpu_ir_descriptor_value){
        .set = 0,
        .binding_namespace = 0,
        .binding = binding,
        .array_element = array_element,
        .kind = uniform ? GPU_IR_DESCRIPTOR_UNIFORM_BUFFER : GPU_IR_DESCRIPTOR_STORAGE_BUFFER,
        .stage_mask = stage_mask,
        .access_mask = uniform ? GPU_IR_ACCESS_UNIFORM_READ :
            GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE,
        .resource = buffer->memory->resource,
        .offset = buffer->memory_offset + descriptor->offset + dynamic_offset,
        .range = descriptor->range
    };
    if (buffer_out) *buffer_out = buffer;
    if (image_out) *image_out = NULL;
    if (image_layout_out) *image_layout_out = VK_IMAGE_LAYOUT_UNDEFINED;
    return 1;
}

static int vk_cmd_record_graphics_descriptors(VkCommandBuffer commandBuffer, vx_vk_pipeline *pipeline) {
    if (!commandBuffer || !pipeline || pipeline->bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS ||
        !pipeline->layout || !pipeline->reflected_layout) return GPU_IR_INVALID;
    uint32_t binding_count = gpu_ir_pipeline_layout_binding_count(pipeline->reflected_layout);
    if (!binding_count) return GPU_IR_OK;
    gpu_ir_descriptor_value values[VX_VK_MAX_DESCRIPTOR_VALUES];
    uint32_t value_count = 0;
    for (uint32_t i = 0; i < binding_count; i++) {
        gpu_ir_descriptor_binding binding;
        if (gpu_ir_pipeline_layout_binding_at(pipeline->reflected_layout, i, &binding) != GPU_IR_OK ||
            (binding.kind != GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER &&
             binding.kind != GPU_IR_DESCRIPTOR_SAMPLED_IMAGE &&
             binding.kind != GPU_IR_DESCRIPTOR_SAMPLER) ||
            binding.set >= pipeline->layout->set_layout_count || !binding.count ||
            binding.count == GPU_IR_DESCRIPTOR_COUNT_UNBOUNDED ||
            binding.count > VX_VK_MAX_DESCRIPTOR_VALUES - value_count)
            return GPU_IR_UNSUPPORTED;
        vx_vk_descriptor_set *set = descriptor_set_from_handle(commandBuffer->bound_sets[binding.set],
                                                                 commandBuffer->device);
        if (!set || !descriptor_layouts_compatible(set->layout, pipeline->layout->set_layouts[binding.set]))
            return GPU_IR_INVALID;
        for (uint32_t element = 0; element < binding.count; element++) {
            gpu_ir_descriptor_value value;
            vx_vk_image *image = NULL;
            VkImageLayout image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (!vk_descriptor_binding_value(commandBuffer, binding.set, set, binding.binding, element,
                    binding.stage_mask, &value, NULL, &image, &image_layout) || value.kind != binding.kind ||
                ((binding.kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER ||
                  binding.kind == GPU_IR_DESCRIPTOR_SAMPLED_IMAGE) && !image) ||
                (binding.kind == GPU_IR_DESCRIPTOR_SAMPLER && (image || !value.backend_object)) ||
                (binding.kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER && !value.backend_object))
                return GPU_IR_INVALID;
            if (image) {
                int rc = command_buffer_record_image_layout_op(commandBuffer, image, 0,
                    image_layout, image_layout, 0, 1);
                if (rc != GPU_IR_OK) return rc;
                if (!command_buffer_track_image(commandBuffer, image)) return GPU_IR_OUT_OF_MEMORY;
            }
            value.set = binding.set;
            values[value_count++] = value;
        }
    }
    for (uint32_t i = 0; i < value_count; i++) {
        if (values[i].backend_object) {
            gpu_ir_metal_sampler_retain(values[i].backend_object);
            values[i].backend_release = gpu_ir_metal_sampler_release;
        }
    }
    gpu_ir_descriptor_state_desc state_desc = { .values = values, .value_count = value_count };
    gpu_ir_descriptor_state *state = gpu_ir_descriptor_state_create(pipeline->layout->gpu_layout, &state_desc);
    if (!state) {
        for (uint32_t i = 0; i < value_count; i++)
            if (values[i].backend_release) values[i].backend_release(values[i].backend_object);
        return GPU_IR_INVALID;
    }
    int rc = gpu_ir_record_bind_descriptor_state(&commandBuffer->stream, state, NULL);
    gpu_ir_descriptor_state_release(state);
    return rc;
}

static void vk_cmd_record_compute(VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY,
    uint32_t groupCountZ, vx_vk_buffer *indirect_buffer, uint64_t indirect_offset) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING) return;
    vx_vk_pipeline *pipeline = commandBuffer->compute_pipeline;
    if (!pipeline || !pipeline->gpu_pipeline ||
        (!indirect_buffer && (groupCountX > 65535 || groupCountY > 65535 || groupCountZ > 65535))) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    if (indirect_buffer && !command_buffer_track_buffer(commandBuffer, indirect_buffer)) {
        commandBuffer->record_error = GPU_IR_OUT_OF_MEMORY;
        return;
    }
    if (commandBuffer->push_constant_layout &&
        !pipeline_layout_push_constants_compatible(commandBuffer->push_constant_layout, pipeline->layout)) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }

    for (uint32_t i = 0; i < gpu_ir_pipeline_layout_binding_count(pipeline->reflected_layout); i++) {
        gpu_ir_descriptor_binding binding;
        if (gpu_ir_pipeline_layout_binding_at(pipeline->reflected_layout, i, &binding) != GPU_IR_OK ||
            binding.set >= pipeline->set_layout_count) {
            commandBuffer->record_error = GPU_IR_INVALID;
            return;
        }
        vx_vk_descriptor_set *set = descriptor_set_from_handle(commandBuffer->bound_sets[binding.set],
                                                                 commandBuffer->device);
        if (!set || !descriptor_layouts_compatible(set->layout, pipeline->set_layouts[binding.set])) {
            commandBuffer->record_error = GPU_IR_INVALID;
            return;
        }
        for (uint32_t array_element = 0; array_element < binding.count; array_element++) {
            gpu_ir_descriptor_value unused;
            vx_vk_buffer *buffer = NULL;
            vx_vk_image *image = NULL;
            VkImageLayout image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (!vk_descriptor_binding_value(commandBuffer, binding.set, set,
                    binding.binding, array_element, binding.stage_mask, &unused, &buffer, &image, &image_layout)) {
                commandBuffer->record_error = GPU_IR_INVALID;
                return;
            }
            if (image) {
                int layout_rc = command_buffer_record_image_layout_op(commandBuffer, image, 0,
                    image_layout, image_layout, 0, 1);
                if (layout_rc != GPU_IR_OK) {
                    commandBuffer->record_error = layout_rc;
                    return;
                }
            }
            if (buffer && !command_buffer_track_buffer(commandBuffer, buffer)) {
                commandBuffer->record_error = GPU_IR_OUT_OF_MEMORY;
                return;
            }
        }
    }

    gpu_ir_descriptor_value values[VX_VK_MAX_DESCRIPTOR_VALUES + 1u];
    uint64_t buffer_ranges[GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS] = { 0 };
    uint32_t value_count = 0;
    uint32_t metal_slot = 0;
    int has_runtime_data = 0;
    const gpu_ir_pipeline_layout *gpu_layout = gpu_ir_pipeline_layout_of(pipeline->gpu_pipeline);
    for (uint32_t i = 0; i < gpu_ir_pipeline_layout_binding_count(gpu_layout); i++) {
        gpu_ir_descriptor_binding binding;
        if (gpu_ir_pipeline_layout_binding_at(gpu_layout, i, &binding) != GPU_IR_OK) {
            commandBuffer->record_error = GPU_IR_INVALID;
            return;
        }
        if (!(binding.stage_mask & GPU_IR_STAGE_COMPUTE)) continue;
        if (binding.set == GPU_IR_INTERNAL_SET &&
            binding.binding_namespace == GPU_IR_INTERNAL_BINDING_NAMESPACE &&
            binding.binding == GPU_IR_INTERNAL_PUSH_CONSTANT_BINDING) {
            if (binding.count != 1 || value_count >= VX_VK_MAX_DESCRIPTOR_VALUES + 1u) {
                commandBuffer->record_error = GPU_IR_INVALID;
                return;
            }
            if (!command_buffer_ensure_push_constant_resource(commandBuffer)) {
                commandBuffer->record_error = GPU_IR_OUT_OF_MEMORY;
                return;
            }
            values[value_count++] = (gpu_ir_descriptor_value){
                .set = GPU_IR_INTERNAL_SET,
                .binding_namespace = GPU_IR_INTERNAL_BINDING_NAMESPACE,
                .binding = GPU_IR_INTERNAL_PUSH_CONSTANT_BINDING,
                .array_element = 0,
                .kind = GPU_IR_DESCRIPTOR_UNIFORM_BUFFER,
                .stage_mask = GPU_IR_STAGE_COMPUTE,
                .access_mask = GPU_IR_ACCESS_UNIFORM_READ,
                .resource = commandBuffer->push_constant_resource,
                .range = GPU_IR_METAL_RUNTIME_DATA_SIZE
            };
            has_runtime_data = 1;
            continue;
        }
        if (binding.set >= VX_VK_MAX_DESCRIPTOR_SETS) {
            commandBuffer->record_error = GPU_IR_INVALID;
            return;
        }
        vx_vk_descriptor_set *set = descriptor_set_from_handle(commandBuffer->bound_sets[binding.set],
                                                                 commandBuffer->device);
        for (uint32_t array_element = 0; array_element < binding.count; array_element++) {
            gpu_ir_descriptor_value value;
            vx_vk_image *image = NULL;
            if (set && vk_descriptor_binding_value(commandBuffer, binding.set, set,
                    binding.binding, array_element, binding.stage_mask, &value, NULL, &image, NULL)) {
                if (value_count >= VX_VK_MAX_DESCRIPTOR_VALUES + 1u) {
                    commandBuffer->record_error = GPU_IR_INVALID;
                    return;
                }
                value.set = binding.set;
                values[value_count++] = value;
                if (binding.kind == GPU_IR_DESCRIPTOR_STORAGE_IMAGE ||
                    binding.kind == GPU_IR_DESCRIPTOR_SAMPLED_IMAGE ||
                    binding.kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER) {
                    if (!image) {
                        commandBuffer->record_error = GPU_IR_INVALID;
                        return;
                    }
                } else if (binding.kind == GPU_IR_DESCRIPTOR_SAMPLER) {
                    if (image || !value.backend_object) {
                        commandBuffer->record_error = GPU_IR_INVALID;
                        return;
                    }
                } else {
                    if (metal_slot >= GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS ||
                        !gpu_ir_resource_is_buffer(value.resource)) {
                        commandBuffer->record_error = GPU_IR_INVALID;
                        return;
                    }
                    uint64_t value_range = value.range;
                    uint64_t resource_size = gpu_ir_buffer_size(value.resource);
                    if (!value_range && value.offset < resource_size) value_range = resource_size - value.offset;
                    buffer_ranges[metal_slot++] = value_range;
                }
            } else if (binding.kind == GPU_IR_DESCRIPTOR_STORAGE_IMAGE ||
                       binding.kind == GPU_IR_DESCRIPTOR_SAMPLED_IMAGE ||
                       binding.kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER ||
                       binding.kind == GPU_IR_DESCRIPTOR_SAMPLER) {
                commandBuffer->record_error = GPU_IR_INVALID;
                return;
            } else {
                if (metal_slot >= GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS) {
                    commandBuffer->record_error = GPU_IR_INVALID;
                    return;
                }
                metal_slot++;
            }
        }
    }
    int rc = GPU_IR_OK;
    if (has_runtime_data) {
        rc = gpu_ir_record_update_buffer(&commandBuffer->stream, commandBuffer->push_constant_resource,
            GPU_IR_METAL_MAX_PUSH_CONSTANT_SIZE, buffer_ranges, sizeof buffer_ranges, NULL);
        if (rc != GPU_IR_OK) { commandBuffer->record_error = rc; return; }
    }
    for (uint32_t i = 0; i < value_count; i++) {
        if (values[i].kind == GPU_IR_DESCRIPTOR_SAMPLER ||
            values[i].kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER) {
            if (!values[i].backend_object) {
                commandBuffer->record_error = GPU_IR_INVALID;
                return;
            }
            gpu_ir_metal_sampler_retain(values[i].backend_object);
            values[i].backend_release = gpu_ir_metal_sampler_release;
        }
    }
    gpu_ir_descriptor_state_desc state_desc = { .values = values, .value_count = value_count };
    gpu_ir_descriptor_state *state = gpu_ir_descriptor_state_create((gpu_ir_pipeline_layout *)gpu_layout, &state_desc);
    if (!state) {
        for (uint32_t i = 0; i < value_count; i++)
            if (values[i].backend_release) values[i].backend_release(values[i].backend_object);
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    rc = gpu_ir_record_bind_descriptor_state(&commandBuffer->stream, state, NULL);
    if (rc == GPU_IR_OK) {
        if (indirect_buffer) {
            rc = gpu_ir_record_dispatch_indirect(&commandBuffer->stream, pipeline->gpu_pipeline,
                indirect_buffer->memory->resource, indirect_buffer->memory_offset + indirect_offset, NULL);
        } else {
            rc = gpu_ir_record_dispatch(&commandBuffer->stream, pipeline->gpu_pipeline,
                groupCountX, groupCountY, groupCountZ, NULL);
        }
    }
    gpu_ir_descriptor_state_release(state);
    if (rc != GPU_IR_OK) commandBuffer->record_error = rc;
}

void WINAPI vkCmdDispatch(VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY,
    uint32_t groupCountZ) {
    vk_cmd_record_compute(commandBuffer, groupCountX, groupCountY, groupCountZ, NULL, 0);
}

void WINAPI vkCmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer bufferHandle, VkDeviceSize offset) {
    if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
        commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->state != VX_VK_COMMAND_RECORDING) return;
    vx_vk_buffer *buffer = buffer_from_handle(bufferHandle, commandBuffer->device);
    if (!buffer || !buffer->bound || !buffer->memory || !(buffer->usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) ||
        (offset & 3u) || offset > buffer->size || 12u > buffer->size - offset ||
        buffer->memory_offset > UINT64_MAX - offset) {
        commandBuffer->record_error = GPU_IR_INVALID;
        return;
    }
    vk_cmd_record_compute(commandBuffer, 0, 0, 0, buffer, offset);
}

VkResult WINAPI vkCreateFence(VkDevice device, const VkFenceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkFence *pFence) {
    if (!pFence) return VK_ERROR_INITIALIZATION_FAILED;
    *pFence = 0;
    if (!device_is_valid(device) || !pCreateInfo || pCreateInfo->sType != VK_STRUCTURE_TYPE_FENCE_CREATE_INFO ||
        pCreateInfo->pNext || (pCreateInfo->flags & ~VK_FENCE_CREATE_SIGNALED_BIT) || !allocator_is_valid(pAllocator))
        return VK_ERROR_INITIALIZATION_FAILED;
    vx_vk_fence *fence = object_allocate(pAllocator, sizeof *fence, alignof(vx_vk_fence),
                                         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!fence) return VK_ERROR_OUT_OF_HOST_MEMORY;
    fence->magic = VX_VK_FENCE_MAGIC;
    fence->device = device;
    fence->signaled = (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0;
    if (pAllocator) {
        fence->allocator = *pAllocator;
        fence->has_allocator = 1;
    }
    *pFence = fence_to_handle(fence);
    return VK_SUCCESS;
}

void WINAPI vkDestroyFence(VkDevice device, VkFence fenceHandle, const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    vx_vk_fence *fence = fence_from_handle(fenceHandle, device);
    if (!fence) return;
    VkAllocationCallbacks allocator = fence->allocator;
    int has_allocator = fence->has_allocator;
    fence->magic = 0;
    object_free(has_allocator ? &allocator : NULL, fence);
}

VkResult WINAPI vkGetFenceStatus(VkDevice device, VkFence fenceHandle) {
    vx_vk_fence *fence = fence_from_handle(fenceHandle, device);
    if (!fence) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&device->sync_mutex);
    VkResult result = device->lost ? VK_ERROR_DEVICE_LOST : fence->signaled ? VK_SUCCESS : VK_NOT_READY;
    pthread_mutex_unlock(&device->sync_mutex);
    return result;
}

static int fence_wait_condition_met(VkDevice device, uint32_t fenceCount, const VkFence *pFences,
                                    VkBool32 waitAll) {
    uint32_t signaled = 0;
    for (uint32_t i = 0; i < fenceCount; i++) signaled += fence_from_handle(pFences[i], device)->signaled != 0;
    return waitAll ? signaled == fenceCount : signaled != 0;
}

static int fence_wait_remaining(const struct timespec *deadline, struct timespec *remaining) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    remaining->tv_sec = deadline->tv_sec - now.tv_sec;
    remaining->tv_nsec = deadline->tv_nsec - now.tv_nsec;
    if (remaining->tv_nsec < 0) { remaining->tv_sec--; remaining->tv_nsec += 1000000000L; }
    return remaining->tv_sec > 0 || (remaining->tv_sec == 0 && remaining->tv_nsec > 0);
}

VkResult WINAPI vkWaitForFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences,
    VkBool32 waitAll, uint64_t timeout) {
    if (!device_is_valid(device) || !fenceCount || !pFences || waitAll > 1) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < fenceCount; i++) {
        if (!fence_from_handle(pFences[i], device)) return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct timespec deadline = { 0, 0 };
    if (timeout != UINT64_MAX) {
        if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return VK_ERROR_UNKNOWN;
        deadline.tv_sec += (time_t)(timeout / 1000000000ull);
        deadline.tv_nsec += (long)(timeout % 1000000000ull);
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    }
    pthread_mutex_lock(&device->sync_mutex);
    VkResult result;
    for (;;) {
        if (device->lost) { result = VK_ERROR_DEVICE_LOST; break; }
        if (fence_wait_condition_met(device, fenceCount, pFences, waitAll)) { result = VK_SUCCESS; break; }
        if (!timeout) { result = VK_TIMEOUT; break; }
        int wait_result;
        if (timeout == UINT64_MAX) {
            wait_result = pthread_cond_wait(&device->sync_condition, &device->sync_mutex);
        } else {
            struct timespec remaining;
            int has_time = fence_wait_remaining(&deadline, &remaining);
            if (has_time < 0) { result = VK_ERROR_UNKNOWN; break; }
            if (!has_time) { result = VK_TIMEOUT; break; }
            wait_result = pthread_cond_timedwait_relative_np(&device->sync_condition, &device->sync_mutex, &remaining);
        }
        if (wait_result && wait_result != ETIMEDOUT) { result = VK_ERROR_UNKNOWN; break; }
        if (wait_result == ETIMEDOUT && !fence_wait_condition_met(device, fenceCount, pFences, waitAll)) {
            result = VK_TIMEOUT;
            break;
        }
    }
    pthread_mutex_unlock(&device->sync_mutex);
    return result;
}

VkResult WINAPI vkResetFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences) {
    if (!device_is_valid(device) || !fenceCount || !pFences) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < fenceCount; i++) if (!fence_from_handle(pFences[i], device))
        return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&device->sync_mutex);
    if (device->lost) {
        pthread_mutex_unlock(&device->sync_mutex);
        return VK_ERROR_DEVICE_LOST;
    }
    for (uint32_t i = 0; i < fenceCount; i++) fence_from_handle(pFences[i], device)->signaled = 0;
    pthread_mutex_unlock(&device->sync_mutex);
    return VK_SUCCESS;
}

VkResult WINAPI vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSemaphore *pSemaphore) {
    if (!pSemaphore) return VK_ERROR_INITIALIZATION_FAILED;
    *pSemaphore = 0;
    if (!device_is_valid(device) || !pCreateInfo || pCreateInfo->sType != VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO ||
        pCreateInfo->pNext || pCreateInfo->flags || !allocator_is_valid(pAllocator))
        return VK_ERROR_INITIALIZATION_FAILED;
    vx_vk_semaphore *semaphore = object_allocate(pAllocator, sizeof *semaphore, alignof(vx_vk_semaphore),
                                                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!semaphore) return VK_ERROR_OUT_OF_HOST_MEMORY;
    semaphore->magic = VX_VK_SEMAPHORE_MAGIC;
    semaphore->device = device;
    if (pAllocator) {
        semaphore->allocator = *pAllocator;
        semaphore->has_allocator = 1;
    }
    *pSemaphore = semaphore_to_handle(semaphore);
    return VK_SUCCESS;
}

void WINAPI vkDestroySemaphore(VkDevice device, VkSemaphore semaphoreHandle,
    const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    vx_vk_semaphore *semaphore = semaphore_from_handle(semaphoreHandle, device);
    if (!semaphore) return;
    VkAllocationCallbacks allocator = semaphore->allocator;
    int has_allocator = semaphore->has_allocator;
    semaphore->magic = 0;
    object_free(has_allocator ? &allocator : NULL, semaphore);
}

typedef struct {
    vx_vk_semaphore *semaphore;
    uint8_t signaled;
} vx_vk_semaphore_state;

static vx_vk_semaphore_state *semaphore_state_get(vx_vk_semaphore_state *states, uint32_t *count,
                                                   vx_vk_semaphore *semaphore) {
    for (uint32_t i = 0; i < *count; i++) if (states[i].semaphore == semaphore) return &states[i];
    vx_vk_semaphore_state *state = &states[(*count)++];
    state->semaphore = semaphore;
    state->signaled = semaphore->signaled;
    return state;
}

static int queue_wait_stage_supported(VkPipelineStageFlags stage) {
    const VkPipelineStageFlags supported = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT |
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    return stage && !(stage & ~supported);
}

static int command_buffer_seen(VkCommandBuffer *buffers, uint32_t count, VkCommandBuffer commandBuffer) {
    for (uint32_t i = 0; i < count; i++) if (buffers[i] == commandBuffer) return 1;
    return 0;
}

static VkResult image_layout_transaction_apply(vx_vk_image_layout_state **states, uint32_t *state_count,
    uint32_t *state_capacity, const vx_vk_image_layout_op *op) {
    if (!states || !state_count || !state_capacity || !op || !op->image ||
        op->image->magic != VX_VK_IMAGE_MAGIC || !op->image->externally_alive ||
        ((op->old_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
          op->new_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) && !op->image->swapchain_owned) ||
        op->base_array_layer >= op->image->array_layers || !op->array_layer_count ||
        op->array_layer_count > op->image->array_layers - op->base_array_layer)
        return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t offset = 0; offset < op->array_layer_count; offset++) {
        uint32_t layer = op->base_array_layer + offset;
        uint32_t index = 0;
        while (index < *state_count &&
            ((*states)[index].image != op->image || (*states)[index].array_layer != layer)) index++;
        if (index == *state_count) {
            if (*state_count == UINT32_MAX) return VK_ERROR_OUT_OF_HOST_MEMORY;
            if (*state_count == *state_capacity) {
                uint32_t capacity = *state_capacity ? *state_capacity * 2u : 8u;
                if (capacity < *state_capacity || (size_t)capacity > SIZE_MAX / sizeof **states)
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                vx_vk_image_layout_state *replacement = realloc(*states, (size_t)capacity * sizeof **states);
                if (!replacement) return VK_ERROR_OUT_OF_HOST_MEMORY;
                *states = replacement;
                *state_capacity = capacity;
            }
            (*states)[index] = (vx_vk_image_layout_state){ .image = op->image, .array_layer = layer,
                .layout = op->image->current_layouts[layer] };
            (*state_count)++;
        }
        vx_vk_image_layout_state *state = &(*states)[index];
        if (op->transition) {
            if (op->new_layout == VK_IMAGE_LAYOUT_UNDEFINED ||
                op->new_layout == VK_IMAGE_LAYOUT_PREINITIALIZED ||
                (op->old_layout != VK_IMAGE_LAYOUT_UNDEFINED && state->layout != op->old_layout))
                return VK_ERROR_INITIALIZATION_FAILED;
            state->layout = op->new_layout;
        } else if (state->layout != op->old_layout) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    return VK_SUCCESS;
}

VkResult WINAPI vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fenceHandle) {
    if (!queue_is_valid(queue) || (submitCount && !pSubmits)) return VK_ERROR_INITIALIZATION_FAILED;
    VkDevice device = queue->device;
    vx_vk_fence *fence = fenceHandle ? fence_from_handle(fenceHandle, device) : NULL;
    if (fenceHandle && !fence) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&device->sync_mutex);
    int device_lost = device->lost;
    int fence_signaled = fence && fence->signaled;
    pthread_mutex_unlock(&device->sync_mutex);
    if (device_lost) return VK_ERROR_DEVICE_LOST;
    if (fence_signaled) return VK_ERROR_INITIALIZATION_FAILED;

    size_t semaphore_capacity = 0, command_capacity = 0;
    for (uint32_t i = 0; i < submitCount; i++) {
        if (pSubmits[i].waitSemaphoreCount > SIZE_MAX - semaphore_capacity ||
            pSubmits[i].signalSemaphoreCount > SIZE_MAX - semaphore_capacity - pSubmits[i].waitSemaphoreCount ||
            pSubmits[i].commandBufferCount > SIZE_MAX - command_capacity)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        semaphore_capacity += pSubmits[i].waitSemaphoreCount + pSubmits[i].signalSemaphoreCount;
        command_capacity += pSubmits[i].commandBufferCount;
    }
    if (semaphore_capacity > UINT32_MAX || command_capacity > UINT32_MAX ||
        semaphore_capacity > SIZE_MAX / sizeof(vx_vk_semaphore_state) ||
        command_capacity > SIZE_MAX / sizeof(VkCommandBuffer)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    vx_vk_semaphore_state *semaphore_states = semaphore_capacity ?
        calloc(semaphore_capacity, sizeof *semaphore_states) : NULL;
    VkCommandBuffer *seen_commands = command_capacity ? calloc(command_capacity, sizeof *seen_commands) : NULL;
    if ((semaphore_capacity && !semaphore_states) || (command_capacity && !seen_commands)) {
        free(semaphore_states);
        free(seen_commands);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint32_t semaphore_state_count = 0, seen_command_count = 0;
    uint32_t image_layout_state_count = 0, image_layout_state_capacity = 0;
    vx_vk_image_layout_state *image_layout_states = NULL;
    VkResult preflight = VK_SUCCESS;
    // Acquire, submit, present, and host fence waits share these binary-sync states.
    pthread_mutex_lock(&device->sync_mutex);
    if (device->lost || (fence && fence->signaled)) {
        VkResult result = device->lost ? VK_ERROR_DEVICE_LOST : VK_ERROR_INITIALIZATION_FAILED;
        pthread_mutex_unlock(&device->sync_mutex);
        free(semaphore_states);
        free(seen_commands);
        return result;
    }
    for (uint32_t i = 0; i < submitCount && preflight == VK_SUCCESS; i++) {
        const VkSubmitInfo *submit = &pSubmits[i];
        if (submit->sType != VK_STRUCTURE_TYPE_SUBMIT_INFO || submit->pNext ||
            (submit->waitSemaphoreCount && (!submit->pWaitSemaphores || !submit->pWaitDstStageMask)) ||
            (submit->commandBufferCount && !submit->pCommandBuffers) ||
            (submit->signalSemaphoreCount && !submit->pSignalSemaphores)) {
            preflight = VK_ERROR_INITIALIZATION_FAILED;
            break;
        }
        for (uint32_t j = 0; j < submit->waitSemaphoreCount; j++) {
            vx_vk_semaphore *semaphore = semaphore_from_handle(submit->pWaitSemaphores[j], device);
            if (!semaphore || !queue_wait_stage_supported(submit->pWaitDstStageMask[j])) {
                preflight = VK_ERROR_INITIALIZATION_FAILED;
                break;
            }
            vx_vk_semaphore_state *state = semaphore_state_get(semaphore_states, &semaphore_state_count, semaphore);
            if (!state->signaled) { preflight = VK_ERROR_INITIALIZATION_FAILED; break; }
            state->signaled = 0;
        }
        for (uint32_t j = 0; j < submit->commandBufferCount && preflight == VK_SUCCESS; j++) {
            VkCommandBuffer commandBuffer = submit->pCommandBuffers[j];
            if (commandBuffer && commandBuffer->dispatch == &g_vk_dispatch &&
                commandBuffer->magic == VX_VK_COMMAND_BUFFER_MAGIC &&
                commandBuffer->device == device && commandBuffer->state == VX_VK_COMMAND_EXECUTABLE &&
                !command_buffer_descriptor_sets_current(commandBuffer)) {
                commandBuffer->state = VX_VK_COMMAND_INVALID;
                preflight = VK_ERROR_INITIALIZATION_FAILED;
                break;
            }
            if (!commandBuffer || commandBuffer->dispatch != &g_vk_dispatch ||
                commandBuffer->magic != VX_VK_COMMAND_BUFFER_MAGIC || commandBuffer->device != device ||
                commandBuffer->state != VX_VK_COMMAND_EXECUTABLE || commandBuffer->pool->family_index != queue->family_index) {
                preflight = VK_ERROR_INITIALIZATION_FAILED;
                break;
            }
            if (command_buffer_seen(seen_commands, seen_command_count, commandBuffer) &&
                (!(commandBuffer->usage_flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) ||
                 (commandBuffer->usage_flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT))) {
                preflight = VK_ERROR_INITIALIZATION_FAILED;
                break;
            }
            if (!command_buffer_seen(seen_commands, seen_command_count, commandBuffer))
                seen_commands[seen_command_count++] = commandBuffer;
            for (uint32_t k = 0; k < commandBuffer->image_layout_op_count; k++) {
                preflight = image_layout_transaction_apply(&image_layout_states, &image_layout_state_count,
                    &image_layout_state_capacity, &commandBuffer->image_layout_ops[k]);
                if (preflight != VK_SUCCESS) break;
            }
        }
        for (uint32_t j = 0; j < submit->signalSemaphoreCount && preflight == VK_SUCCESS; j++) {
            vx_vk_semaphore *semaphore = semaphore_from_handle(submit->pSignalSemaphores[j], device);
            if (!semaphore) { preflight = VK_ERROR_INITIALIZATION_FAILED; break; }
            vx_vk_semaphore_state *state = semaphore_state_get(semaphore_states, &semaphore_state_count, semaphore);
            if (state->signaled) { preflight = VK_ERROR_INITIALIZATION_FAILED; break; }
            state->signaled = 1;
        }
    }
    if (preflight != VK_SUCCESS) {
        pthread_mutex_unlock(&device->sync_mutex);
        free(image_layout_states);
        free(semaphore_states);
        free(seen_commands);
        return preflight;
    }

    for (uint32_t i = 0; i < submitCount; i++) {
        const VkSubmitInfo *submit = &pSubmits[i];
        for (uint32_t j = 0; j < submit->waitSemaphoreCount; j++)
            semaphore_from_handle(submit->pWaitSemaphores[j], device)->signaled = 0;
        for (uint32_t j = 0; j < submit->commandBufferCount; j++) {
            VkCommandBuffer commandBuffer = submit->pCommandBuffers[j];
            commandBuffer->state = VX_VK_COMMAND_PENDING;
            int rc = gpu_ir_metal_execute_stream(device->physical_device->owner->metal, &commandBuffer->stream);
            if (rc != GPU_IR_OK) {
                device->lost = 1;
                pthread_cond_broadcast(&device->sync_condition);
                pthread_mutex_unlock(&device->sync_mutex);
                free(image_layout_states);
                free(semaphore_states);
                free(seen_commands);
                return VK_ERROR_DEVICE_LOST;
            }
            commandBuffer->state = (commandBuffer->usage_flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) ?
                VX_VK_COMMAND_INVALID : VX_VK_COMMAND_EXECUTABLE;
        }
        for (uint32_t j = 0; j < submit->signalSemaphoreCount; j++)
            semaphore_from_handle(submit->pSignalSemaphores[j], device)->signaled = 1;
    }
    for (uint32_t i = 0; i < image_layout_state_count; i++)
        image_layout_states[i].image->current_layouts[image_layout_states[i].array_layer] =
            image_layout_states[i].layout;
    free(image_layout_states);
    free(semaphore_states);
    free(seen_commands);
    if (fence) {
        fence->signaled = 1;
        pthread_cond_broadcast(&device->sync_condition);
    }
    pthread_mutex_unlock(&device->sync_mutex);
    return VK_SUCCESS;
}

static uint32_t vk_present_result_priority(VkResult result) {
    switch (result) {
    case VK_ERROR_DEVICE_LOST: return 5;
    case VK_ERROR_SURFACE_LOST_KHR: return 4;
    case VK_ERROR_OUT_OF_DATE_KHR: return 3;
    case VK_SUBOPTIMAL_KHR: return 2;
    case VK_SUCCESS: return 1;
    default: return 0;
    }
}

static VkResult vk_present_result_combine(VkResult current, VkResult next) {
    return vk_present_result_priority(next) > vk_present_result_priority(current) ? next : current;
}

#ifdef VX_VULKAN_TEST_HOOKS
VkResult vx_vk_test_select_present_result(const VkResult *results, uint32_t count) {
    if (count && !results) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult overall = VK_SUCCESS;
    for (uint32_t i = 0; i < count; i++) overall = vk_present_result_combine(overall, results[i]);
    return overall;
}
#endif

VkResult WINAPI vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) {
    if (!queue_is_valid(queue)) return VK_ERROR_INITIALIZATION_FAILED;
    VkDevice device = queue->device;
    if (!device->swapchain_enabled) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (!pPresentInfo || pPresentInfo->sType != VK_STRUCTURE_TYPE_PRESENT_INFO_KHR || pPresentInfo->pNext ||
        !pPresentInfo->swapchainCount || !pPresentInfo->pSwapchains || !pPresentInfo->pImageIndices ||
        (pPresentInfo->waitSemaphoreCount && !pPresentInfo->pWaitSemaphores))
        return VK_ERROR_INITIALIZATION_FAILED;

    for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; i++) {
        if (!semaphore_from_handle(pPresentInfo->pWaitSemaphores[i], device))
            return VK_ERROR_INITIALIZATION_FAILED;
        for (uint32_t j = 0; j < i; j++)
            if (pPresentInfo->pWaitSemaphores[j] == pPresentInfo->pWaitSemaphores[i])
                return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        vx_vk_swapchain *swapchain = swapchain_from_handle(pPresentInfo->pSwapchains[i], device);
        if (!swapchain || pPresentInfo->pImageIndices[i] >= swapchain->image_count ||
            !swapchain->surface || swapchain->surface->instance != device->physical_device->owner)
            return VK_ERROR_INITIALIZATION_FAILED;
        for (uint32_t j = 0; j < i; j++)
            if (pPresentInfo->pSwapchains[j] == pPresentInfo->pSwapchains[i])
                return VK_ERROR_INITIALIZATION_FAILED;
    }

    pthread_mutex_lock(&device->sync_mutex);
    if (device->lost) {
        pthread_mutex_unlock(&device->sync_mutex);
        return VK_ERROR_DEVICE_LOST;
    }
    for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; i++) {
        vx_vk_semaphore *semaphore = semaphore_from_handle(pPresentInfo->pWaitSemaphores[i], device);
        if (!semaphore || !semaphore->signaled) {
            pthread_mutex_unlock(&device->sync_mutex);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        vx_vk_swapchain *swapchain = swapchain_from_handle(pPresentInfo->pSwapchains[i], device);
        uint32_t image_index = pPresentInfo->pImageIndices[i];
        vx_vk_image *image = swapchain ? swapchain->images[image_index] : NULL;
        if (!swapchain || !image || !swapchain->acquired[image_index] || image->swapchain != swapchain ||
            image->swapchain_index != image_index ||
            image->current_layouts[0] != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
            pthread_mutex_unlock(&device->sync_mutex);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; i++)
        semaphore_from_handle(pPresentInfo->pWaitSemaphores[i], device)->signaled = 0;
    pthread_mutex_unlock(&device->sync_mutex);

    VkResult overall = VK_SUCCESS;
    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        vx_vk_swapchain *swapchain = swapchain_from_handle(pPresentInfo->pSwapchains[i], device);
        uint32_t image_index = pPresentInfo->pImageIndices[i];
        vx_vk_surface *surface = swapchain->surface;
        vx_window *window = vx_window_get(surface->hwnd);
        VkResult present_result = VK_SUCCESS;
        if (!window || window->destroyed) {
            present_result = VK_ERROR_SURFACE_LOST_KHR;
        } else if (window->minimized || surface_extent_from_window(window).width != swapchain->width ||
                   surface_extent_from_window(window).height != swapchain->height) {
            present_result = VK_ERROR_OUT_OF_DATE_KHR;
        } else {
            void *layer = vx_cocoa_metal_layer_bgra(window,
                gpu_ir_metal_native_device(device->physical_device->owner->metal));
            if (!layer) present_result = VK_ERROR_SURFACE_LOST_KHR;
            else {
                int rc = gpu_ir_metal_present_texture(device->physical_device->owner->metal,
                    swapchain->images[image_index]->resource, layer, 1);
                if (rc == GPU_IR_RANGE) present_result = VK_ERROR_OUT_OF_DATE_KHR;
                else if (rc != GPU_IR_OK) present_result = VK_ERROR_DEVICE_LOST;
            }
        }
        pthread_mutex_lock(&device->sync_mutex);
        swapchain->acquired[image_index] = 0;
        pthread_cond_broadcast(&device->sync_condition);
        pthread_mutex_unlock(&device->sync_mutex);
        if (pPresentInfo->pResults) pPresentInfo->pResults[i] = present_result;
        overall = vk_present_result_combine(overall, present_result);
        if (present_result == VK_ERROR_DEVICE_LOST) {
            pthread_mutex_lock(&device->sync_mutex);
            device->lost = 1;
            pthread_cond_broadcast(&device->sync_condition);
            pthread_mutex_unlock(&device->sync_mutex);
        }
    }
    return overall;
}

VkResult WINAPI vkQueueWaitIdle(VkQueue queue) {
    if (!queue_is_valid(queue)) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&queue->device->sync_mutex);
    VkResult result = queue->device->lost ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
    pthread_mutex_unlock(&queue->device->sync_mutex);
    return result;
}

VkResult WINAPI vkDeviceWaitIdle(VkDevice device) {
    if (!device_is_valid(device)) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&device->sync_mutex);
    VkResult result = device->lost ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
    pthread_mutex_unlock(&device->sync_mutex);
    return result;
}
