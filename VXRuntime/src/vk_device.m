// Vulkan instance, physical device, device, memory, buffers, images, views and samplers on Metal.
#include "vk_impl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#import <IOSurface/IOSurface.h>

int vk_trace(void) {
    static int on = -1;
    if (on < 0) on = getenv("VXRT_TRACE_VULKAN") && atoi(getenv("VXRT_TRACE_VULKAN"));
    return on;
}
#define TRACE(...) do { if (vk_trace()) vx_log("vulkan: " __VA_ARGS__); } while (0)

const void *vk_find_struct(const void *chain, VkStructureType type) {
    for (const VkBaseInStructure *s = chain; s; s = s->pNext) if (s->sType == type) return s;
    return NULL;
}
void *vk_find_struct_mut(void *chain, VkStructureType type) {
    for (VkBaseOutStructure *s = chain; s; s = s->pNext) if (s->sType == type) return s;
    return NULL;
}

uint32_t vk_api_version(void) { return VK_MAKE_API_VERSION(0, 1, 3, 280); }

#define VK_WIN32_IMAGE_HANDLES (VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT | VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT | \
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT | VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT)

// ---- instance ----------------------------------------------------------------------------------------------------
typedef struct { const char *name; uint32_t version, bit; } vk_ext;
static const vk_ext g_instance_exts[] = {
    { VK_KHR_SURFACE_EXTENSION_NAME, 25, VK_INST_EXT_SURFACE },
    { VK_KHR_WIN32_SURFACE_EXTENSION_NAME, 6, VK_INST_EXT_WIN32_SURFACE },
    { VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, 2, VK_INST_EXT_PROPS2 },
    { VK_EXT_DEBUG_UTILS_EXTENSION_NAME, 2, VK_INST_EXT_DEBUG_UTILS },
    { VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME, 4, VK_INST_EXT_COLORSPACE },
    { VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, 1, VK_INST_EXT_SURFACE_CAPS2 },
    { VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME, 1, 0 },
    { VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME, 1, 0 },
    { VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME, 1, 0 },
    { VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME, 1, 0 },
};
enum {
    VK_DEV_EXT_SWAPCHAIN = 1u << 0, VK_DEV_EXT_HDR = 1u << 1, VK_DEV_EXT_BUDGET = 1u << 2
};
static const vk_ext g_device_exts[] = {
    { VK_KHR_SWAPCHAIN_EXTENSION_NAME, 70, VK_DEV_EXT_SWAPCHAIN },
    { VK_KHR_MAINTENANCE_1_EXTENSION_NAME, 2, 0 },
    { VK_KHR_MAINTENANCE_2_EXTENSION_NAME, 1, 0 },
    { VK_KHR_MAINTENANCE_3_EXTENSION_NAME, 1, 0 },
    { VK_KHR_MAINTENANCE_4_EXTENSION_NAME, 2, 0 },
    { VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, 3, 0 },
    { VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, 1, 0 },
    { VK_KHR_BIND_MEMORY_2_EXTENSION_NAME, 1, 0 },
    { VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, 2, 0 },
    { VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, 1, 0 },
    { VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, 1, 0 },
    { VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, 2, 0 },
    { VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME, 1, 0 },
    { VK_KHR_16BIT_STORAGE_EXTENSION_NAME, 1, 0 },
    { VK_KHR_8BIT_STORAGE_EXTENSION_NAME, 1, 0 },
    { VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME, 1, 0 },
    { VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME, 1, 0 },
    { VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, 1, 0 },
    { VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME, 1, 0 },
    { VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME, 1, 0 },
    { VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME, 1, 0 },
    { VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME, 3, 0 },
    { VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME, 1, 0 },
    { VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME, 1, 0 },
    { VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME, 1, 0 },
    { VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME, 1, 0 },
    { VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME, 1, 0 },
    { VK_KHR_SHADER_SUBGROUP_EXTENDED_TYPES_EXTENSION_NAME, 1, 0 },
    { VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME, 2, 0 },
    { VK_EXT_SHADER_SUBGROUP_BALLOT_EXTENSION_NAME, 1, 0 },
    { VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, 1, VK_DEV_EXT_BUDGET },
    { VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME, 1, 0 },
    { VK_EXT_HDR_METADATA_EXTENSION_NAME, 2, VK_DEV_EXT_HDR },
    { VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME, 1, 0 },
    { VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, 1, 0 },
    { VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME, 1, 0 },
    { VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME, 1, 0 },
    { VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, 2, 0 },
    { VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME, 1, 0 },
    { VK_KHR_SHADER_TERMINATE_INVOCATION_EXTENSION_NAME, 1, 0 },
    { VK_EXT_PRIVATE_DATA_EXTENSION_NAME, 1, 0 },
    { VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME, 1, 0 },
    { VK_KHR_SPIRV_1_4_EXTENSION_NAME, 1, 0 },
    { VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME, 4, 0 },
    { VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, 1, 0 },
    { VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, 1, 0 },
    { VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME, 1, 0 },
    { VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, 1, 0 },
    { VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME, 1, 0 },
    { VK_EXT_SEPARATE_STENCIL_USAGE_EXTENSION_NAME, 1, 0 },
    { VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME, 1, 0 },
    { VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME, 2, 0 },
    { VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME, 1, 0 },
    { VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, 12, 0 },
    { VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME, 1, 0 },
    { VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME, 1, 0 },
    { VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, 1, 0 },
    { VK_EXT_DEBUG_MARKER_EXTENSION_NAME, 4, 0 },
    { VK_AMD_BUFFER_MARKER_EXTENSION_NAME, 1, 0 },
};

static VkResult enumerate_exts(const vk_ext *list, uint32_t n, uint32_t *count, VkExtensionProperties *props) {
    if (!props) { *count = n; return VK_SUCCESS; }
    uint32_t k = *count < n ? *count : n;
    for (uint32_t i = 0; i < k; i++) {
        memset(&props[i], 0, sizeof props[i]);
        snprintf(props[i].extensionName, sizeof props[i].extensionName, "%s", list[i].name);
        props[i].specVersion = list[i].version;
    }
    *count = k;
    return k < n ? VK_INCOMPLETE : VK_SUCCESS;
}

VKFN VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *v) { *v = vk_api_version(); return VK_SUCCESS; }
VKFN VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count, VkExtensionProperties *props) {
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    return enumerate_exts(g_instance_exts, sizeof g_instance_exts / sizeof g_instance_exts[0], count, props);
}
VKFN VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *props) {
    (void)props;
    *count = 0;
    return VK_SUCCESS;
}

VKFN VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *alloc, VkInstance *out) {
    (void)alloc;
    *out = NULL;
    if (info->enabledLayerCount) {
        for (uint32_t i = 0; i < info->enabledLayerCount; i++) TRACE("layer %s requested: not present", info->ppEnabledLayerNames[i]);
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    uint32_t exts = 0;
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
        const char *name = info->ppEnabledExtensionNames[i];
        int found = 0;
        for (size_t k = 0; k < sizeof g_instance_exts / sizeof g_instance_exts[0]; k++)
            if (!strcmp(name, g_instance_exts[k].name)) { exts |= g_instance_exts[k].bit; found = 1; }
        if (!found) { TRACE("instance extension %s: not present", name); return VK_ERROR_EXTENSION_NOT_PRESENT; }
    }
    id<MTLDevice> mtl = MTLCreateSystemDefaultDevice();
    if (!mtl) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkInstance_T *inst = calloc(1, sizeof *inst);
    if (!inst) return VK_ERROR_OUT_OF_HOST_MEMORY;
    inst->api_version = info->pApplicationInfo && info->pApplicationInfo->apiVersion ? info->pApplicationInfo->apiVersion :
                        VK_API_VERSION_1_0;
    inst->extensions = exts;
    inst->mtl = mtl;
    inst->phys.instance = inst;
    inst->phys.mtl = mtl;
    pthread_mutex_init(&inst->lock, NULL);
    if (info->pApplicationInfo)
        TRACE("instance for %s %u / %s %u, API %u.%u", info->pApplicationInfo->pApplicationName ? info->pApplicationInfo->pApplicationName : "?",
              info->pApplicationInfo->applicationVersion, info->pApplicationInfo->pEngineName ? info->pApplicationInfo->pEngineName : "?",
              info->pApplicationInfo->engineVersion, VK_API_VERSION_MAJOR(inst->api_version), VK_API_VERSION_MINOR(inst->api_version));
    *out = inst;
    return VK_SUCCESS;
}

VKFN void VKAPI_CALL vkDestroyInstance(VkInstance inst, const VkAllocationCallbacks *alloc) {
    (void)alloc;
    if (!inst) return;
    for (vk_messenger *m = inst->messengers, *n; m; m = n) { n = m->next; free(m); }
    inst->mtl = nil;
    pthread_mutex_destroy(&inst->lock);
    free(inst);
}

VKFN VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance inst, uint32_t *count, VkPhysicalDevice *devs) {
    if (!devs) { *count = 1; return VK_SUCCESS; }
    if (!*count) return VK_INCOMPLETE;
    devs[0] = &inst->phys;
    *count = 1;
    return VK_SUCCESS;
}
VKFN VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroups(VkInstance inst, uint32_t *count, VkPhysicalDeviceGroupProperties *props) {
    if (!props) { *count = 1; return VK_SUCCESS; }
    if (!*count) return VK_INCOMPLETE;
    props[0].physicalDeviceCount = 1;
    props[0].physicalDevices[0] = &inst->phys;
    props[0].subsetAllocation = VK_FALSE;
    *count = 1;
    return VK_SUCCESS;
}

// Debug messengers: kept, and told nothing (there is no validation here).
VKFN VkResult VKAPI_CALL vkCreateDebugUtilsMessengerEXT(VkInstance inst, const VkDebugUtilsMessengerCreateInfoEXT *info,
                                                       const VkAllocationCallbacks *alloc, VkDebugUtilsMessengerEXT *out) {
    (void)alloc;
    vk_messenger *m = calloc(1, sizeof *m);
    if (!m) return VK_ERROR_OUT_OF_HOST_MEMORY;
    m->callback = info->pfnUserCallback; m->user = info->pUserData;
    m->severities = info->messageSeverity; m->types = info->messageType;
    pthread_mutex_lock(&inst->lock);
    m->next = inst->messengers;
    inst->messengers = m;
    pthread_mutex_unlock(&inst->lock);
    *out = (VkDebugUtilsMessengerEXT)m;
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkDestroyDebugUtilsMessengerEXT(VkInstance inst, VkDebugUtilsMessengerEXT messenger, const VkAllocationCallbacks *alloc) {
    (void)alloc;
    if (!messenger) return;
    pthread_mutex_lock(&inst->lock);
    for (vk_messenger **p = &inst->messengers; *p; p = &(*p)->next)
        if (*p == (vk_messenger *)messenger) { vk_messenger *m = *p; *p = m->next; free(m); break; }
    pthread_mutex_unlock(&inst->lock);
}
VKFN void VKAPI_CALL vkSubmitDebugUtilsMessageEXT(VkInstance inst, VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                                 VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT *data) {
    (void)inst; (void)sev; (void)types; (void)data;
}
VKFN VkResult VKAPI_CALL vkSetDebugUtilsObjectNameEXT(VkDevice dev, const VkDebugUtilsObjectNameInfoEXT *info) { (void)dev; (void)info; return VK_SUCCESS; }
VKFN VkResult VKAPI_CALL vkDebugMarkerSetObjectNameEXT(VkDevice dev, const VkDebugMarkerObjectNameInfoEXT *info) { (void)dev; (void)info; return VK_SUCCESS; }
VKFN VkResult VKAPI_CALL vkDebugMarkerSetObjectTagEXT(VkDevice dev, const VkDebugMarkerObjectTagInfoEXT *info) { (void)dev; (void)info; return VK_SUCCESS; }
VKFN VkResult VKAPI_CALL vkSetDebugUtilsObjectTagEXT(VkDevice dev, const VkDebugUtilsObjectTagInfoEXT *info) { (void)dev; (void)info; return VK_SUCCESS; }
VKFN void VKAPI_CALL vkQueueBeginDebugUtilsLabelEXT(VkQueue q, const VkDebugUtilsLabelEXT *l) { (void)q; (void)l; }
VKFN void VKAPI_CALL vkQueueEndDebugUtilsLabelEXT(VkQueue q) { (void)q; }
VKFN void VKAPI_CALL vkQueueInsertDebugUtilsLabelEXT(VkQueue q, const VkDebugUtilsLabelEXT *l) { (void)q; (void)l; }

// ---- physical device ---------------------------------------------------------------------------------------------
static void fill_features(VkPhysicalDevice phys, VkPhysicalDeviceFeatures *f) {
    memset(f, 0, sizeof *f);
    f->fullDrawIndexUint32 = 1; f->imageCubeArray = 1; f->independentBlend = 1; f->sampleRateShading = 1;
    f->dualSrcBlend = 1; f->multiDrawIndirect = 1; f->drawIndirectFirstInstance = 1; f->depthClamp = 1;
    f->depthBiasClamp = 1; f->fillModeNonSolid = 1; f->largePoints = 1; f->alphaToOne = 1; f->multiViewport = 1;
    f->samplerAnisotropy = 1; f->textureCompressionBC = phys->mtl.supportsBCTextureCompression;
    f->occlusionQueryPrecise = 1; f->vertexPipelineStoresAndAtomics = 1; f->fragmentStoresAndAtomics = 1;
    f->shaderImageGatherExtended = 1; f->shaderStorageImageExtendedFormats = 1;
    f->shaderStorageImageReadWithoutFormat = 1; f->shaderStorageImageWriteWithoutFormat = 1;
    f->shaderUniformBufferArrayDynamicIndexing = 1; f->shaderSampledImageArrayDynamicIndexing = 1;
    f->shaderStorageBufferArrayDynamicIndexing = 1; f->shaderStorageImageArrayDynamicIndexing = 1;
    f->shaderClipDistance = 1; f->shaderInt16 = 1; f->shaderInt64 = 1; f->shaderResourceMinLod = 1;
    f->inheritedQueries = 1;
    f->geometryShader = 1;   // (as kernels: vk_command.m)
    f->tessellationShader = 1;   // (the control shader as a kernel, Metal's tessellator: vk_command.m)
}
static void fill_features11(VkPhysicalDeviceVulkan11Features *f) {
    f->storageBuffer16BitAccess = 1; f->uniformAndStorageBuffer16BitAccess = 1; f->storagePushConstant16 = 1;
    f->shaderDrawParameters = 1;
}
static void fill_features12(VkPhysicalDeviceVulkan12Features *f) {
    f->samplerMirrorClampToEdge = 1; f->drawIndirectCount = 1; f->storageBuffer8BitAccess = 1;
    f->uniformAndStorageBuffer8BitAccess = 1; f->storagePushConstant8 = 1; f->shaderFloat16 = 1; f->shaderInt8 = 1;
    f->descriptorIndexing = 1; f->shaderUniformTexelBufferArrayDynamicIndexing = 1;
    f->shaderStorageTexelBufferArrayDynamicIndexing = 1; f->shaderUniformBufferArrayNonUniformIndexing = 1;
    f->shaderSampledImageArrayNonUniformIndexing = 1; f->shaderStorageBufferArrayNonUniformIndexing = 1;
    f->shaderStorageImageArrayNonUniformIndexing = 1; f->shaderUniformTexelBufferArrayNonUniformIndexing = 1;
    f->shaderStorageTexelBufferArrayNonUniformIndexing = 1; f->descriptorBindingUniformBufferUpdateAfterBind = 1;
    f->descriptorBindingSampledImageUpdateAfterBind = 1; f->descriptorBindingStorageImageUpdateAfterBind = 1;
    f->descriptorBindingStorageBufferUpdateAfterBind = 1; f->descriptorBindingUniformTexelBufferUpdateAfterBind = 1;
    f->descriptorBindingStorageTexelBufferUpdateAfterBind = 1; f->descriptorBindingUpdateUnusedWhilePending = 1;
    f->descriptorBindingPartiallyBound = 1; f->descriptorBindingVariableDescriptorCount = 1;
    f->runtimeDescriptorArray = 1; f->scalarBlockLayout = 1; f->imagelessFramebuffer = 1;
    f->uniformBufferStandardLayout = 1; f->shaderSubgroupExtendedTypes = 1; f->separateDepthStencilLayouts = 1;
    f->hostQueryReset = 1; f->timelineSemaphore = 1; f->shaderOutputViewportIndex = 1; f->shaderOutputLayer = 1;
    f->subgroupBroadcastDynamicId = 1;
}
static void fill_features13(VkPhysicalDeviceVulkan13Features *f) {
    f->pipelineCreationCacheControl = 1; f->privateData = 1; f->shaderDemoteToHelperInvocation = 1;
    f->shaderTerminateInvocation = 1; f->subgroupSizeControl = 1; f->computeFullSubgroups = 1; f->synchronization2 = 1;
    f->dynamicRendering = 1; f->maintenance4 = 1;
}

VKFN void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice phys, VkPhysicalDeviceFeatures *f) { fill_features(phys, f); }

VKFN void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice phys, VkPhysicalDeviceFeatures2 *f2) {
    fill_features(phys, &f2->features);
    VkPhysicalDeviceVulkan11Features v11 = { 0 };
    VkPhysicalDeviceVulkan12Features v12 = { 0 };
    VkPhysicalDeviceVulkan13Features v13 = { 0 };
    fill_features11(&v11); fill_features12(&v12); fill_features13(&v13);
    for (VkBaseOutStructure *s = f2->pNext; s; s = s->pNext) {
        void *next = s->pNext;
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: { VkPhysicalDeviceVulkan11Features *p = (void *)s; *p = v11; p->sType = s->sType; p->pNext = next; break; }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: { VkPhysicalDeviceVulkan12Features *p = (void *)s; *p = v12; p->sType = s->sType; p->pNext = next; break; }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: { VkPhysicalDeviceVulkan13Features *p = (void *)s; *p = v13; p->sType = s->sType; p->pNext = next; break; }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES: {
            VkPhysicalDevice16BitStorageFeatures *p = (void *)s;
            p->storageBuffer16BitAccess = 1; p->uniformAndStorageBuffer16BitAccess = 1; p->storagePushConstant16 = 1; p->storageInputOutput16 = 0;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES: {
            VkPhysicalDevice8BitStorageFeatures *p = (void *)s;
            p->storageBuffer8BitAccess = 1; p->uniformAndStorageBuffer8BitAccess = 1; p->storagePushConstant8 = 1;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES: {
            VkPhysicalDeviceShaderFloat16Int8Features *p = (void *)s; p->shaderFloat16 = 1; p->shaderInt8 = 1; break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
            ((VkPhysicalDeviceShaderDrawParametersFeatures *)s)->shaderDrawParameters = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
            ((VkPhysicalDeviceTimelineSemaphoreFeatures *)s)->timelineSemaphore = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES:
            ((VkPhysicalDeviceSynchronization2Features *)s)->synchronization2 = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES:
            ((VkPhysicalDeviceDynamicRenderingFeatures *)s)->dynamicRendering = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES: {
            VkPhysicalDeviceDescriptorIndexingFeatures *p = (void *)s;
            p->shaderInputAttachmentArrayDynamicIndexing = v12.shaderInputAttachmentArrayDynamicIndexing;
            p->shaderUniformTexelBufferArrayDynamicIndexing = v12.shaderUniformTexelBufferArrayDynamicIndexing;
            p->shaderStorageTexelBufferArrayDynamicIndexing = v12.shaderStorageTexelBufferArrayDynamicIndexing;
            p->shaderUniformBufferArrayNonUniformIndexing = v12.shaderUniformBufferArrayNonUniformIndexing;
            p->shaderSampledImageArrayNonUniformIndexing = v12.shaderSampledImageArrayNonUniformIndexing;
            p->shaderStorageBufferArrayNonUniformIndexing = v12.shaderStorageBufferArrayNonUniformIndexing;
            p->shaderStorageImageArrayNonUniformIndexing = v12.shaderStorageImageArrayNonUniformIndexing;
            p->shaderInputAttachmentArrayNonUniformIndexing = v12.shaderInputAttachmentArrayNonUniformIndexing;
            p->shaderUniformTexelBufferArrayNonUniformIndexing = v12.shaderUniformTexelBufferArrayNonUniformIndexing;
            p->shaderStorageTexelBufferArrayNonUniformIndexing = v12.shaderStorageTexelBufferArrayNonUniformIndexing;
            p->descriptorBindingUniformBufferUpdateAfterBind = v12.descriptorBindingUniformBufferUpdateAfterBind;
            p->descriptorBindingSampledImageUpdateAfterBind = v12.descriptorBindingSampledImageUpdateAfterBind;
            p->descriptorBindingStorageImageUpdateAfterBind = v12.descriptorBindingStorageImageUpdateAfterBind;
            p->descriptorBindingStorageBufferUpdateAfterBind = v12.descriptorBindingStorageBufferUpdateAfterBind;
            p->descriptorBindingUniformTexelBufferUpdateAfterBind = v12.descriptorBindingUniformTexelBufferUpdateAfterBind;
            p->descriptorBindingStorageTexelBufferUpdateAfterBind = v12.descriptorBindingStorageTexelBufferUpdateAfterBind;
            p->descriptorBindingUpdateUnusedWhilePending = v12.descriptorBindingUpdateUnusedWhilePending;
            p->descriptorBindingPartiallyBound = v12.descriptorBindingPartiallyBound;
            p->descriptorBindingVariableDescriptorCount = v12.descriptorBindingVariableDescriptorCount;
            p->runtimeDescriptorArray = v12.runtimeDescriptorArray;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:
            ((VkPhysicalDeviceScalarBlockLayoutFeatures *)s)->scalarBlockLayout = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES:
            ((VkPhysicalDeviceImagelessFramebufferFeatures *)s)->imagelessFramebuffer = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES:
            ((VkPhysicalDeviceUniformBufferStandardLayoutFeatures *)s)->uniformBufferStandardLayout = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES:
            ((VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures *)s)->shaderSubgroupExtendedTypes = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES:
            ((VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures *)s)->separateDepthStencilLayouts = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:
            ((VkPhysicalDeviceHostQueryResetFeatures *)s)->hostQueryReset = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES: {
            VkPhysicalDeviceSubgroupSizeControlFeatures *p = (void *)s; p->subgroupSizeControl = 1; p->computeFullSubgroups = 1; break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES:
            ((VkPhysicalDeviceMaintenance4Features *)s)->maintenance4 = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES:
            ((VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures *)s)->shaderDemoteToHelperInvocation = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES:
            ((VkPhysicalDeviceShaderTerminateInvocationFeatures *)s)->shaderTerminateInvocation = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES:
            ((VkPhysicalDevicePrivateDataFeatures *)s)->privateData = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES:
            ((VkPhysicalDevicePipelineCreationCacheControlFeatures *)s)->pipelineCreationCacheControl = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT:
            ((VkPhysicalDeviceExtendedDynamicStateFeaturesEXT *)s)->extendedDynamicState = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT: {
            VkPhysicalDeviceExtendedDynamicState2FeaturesEXT *p = (void *)s; p->extendedDynamicState2 = 1; break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT:
            ((VkPhysicalDeviceMemoryPriorityFeaturesEXT *)s)->memoryPriority = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT:
            ((VkPhysicalDeviceDepthClipEnableFeaturesEXT *)s)->depthClipEnable = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT: {
            VkPhysicalDeviceCustomBorderColorFeaturesEXT *p = (void *)s; p->customBorderColors = 1; p->customBorderColorWithoutFormat = 1; break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT:
            ((VkPhysicalDeviceIndexTypeUint8FeaturesEXT *)s)->indexTypeUint8 = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT: {
            VkPhysicalDeviceRobustness2FeaturesEXT *p = (void *)s; p->nullDescriptor = 1; break;
        }
        default: break;
        }
    }
}

static void fill_limits(VkPhysicalDevice phys, VkPhysicalDeviceLimits *l) {
    memset(l, 0, sizeof *l);
    l->maxImageDimension1D = 16384; l->maxImageDimension2D = 16384; l->maxImageDimension3D = 2048;
    l->maxImageDimensionCube = 16384; l->maxImageArrayLayers = 2048; l->maxTexelBufferElements = 64u << 20;
    l->maxUniformBufferRange = 65536; l->maxStorageBufferRange = UINT32_MAX; l->maxPushConstantsSize = 256;
    l->maxMemoryAllocationCount = 1u << 20; l->maxSamplerAllocationCount = 1024;
    l->bufferImageGranularity = 1; l->maxBoundDescriptorSets = VKMSL_MAX_SETS;
    l->maxPerStageDescriptorSamplers = 1024; l->maxPerStageDescriptorUniformBuffers = 500000;
    l->maxPerStageDescriptorStorageBuffers = 500000; l->maxPerStageDescriptorSampledImages = 500000;
    l->maxPerStageDescriptorStorageImages = 500000; l->maxPerStageDescriptorInputAttachments = 8;
    l->maxPerStageResources = 1000000; l->maxDescriptorSetSamplers = 1024; l->maxDescriptorSetUniformBuffers = 500000;
    l->maxDescriptorSetUniformBuffersDynamic = 64; l->maxDescriptorSetStorageBuffers = 500000;
    l->maxDescriptorSetStorageBuffersDynamic = 64; l->maxDescriptorSetSampledImages = 500000;
    l->maxDescriptorSetStorageImages = 500000; l->maxDescriptorSetInputAttachments = 8;
    l->maxVertexInputAttributes = 31; l->maxVertexInputBindings = VKMSL_MAX_VERTEX_BUFFERS;
    l->maxVertexInputAttributeOffset = 2047; l->maxVertexInputBindingStride = 2048;
    l->maxTessellationGenerationLevel = 64; l->maxTessellationPatchSize = 32;
    l->maxTessellationControlPerVertexInputComponents = 124; l->maxTessellationControlPerVertexOutputComponents = 124;
    l->maxTessellationControlPerPatchOutputComponents = 120; l->maxTessellationControlTotalOutputComponents = 4096;
    l->maxTessellationEvaluationInputComponents = 124; l->maxTessellationEvaluationOutputComponents = 124;
    l->maxVertexOutputComponents = 124; l->maxFragmentInputComponents = 124; l->maxFragmentOutputAttachments = 8;
    l->maxFragmentDualSrcAttachments = 1; l->maxFragmentCombinedOutputResources = 1000000;
    l->maxComputeSharedMemorySize = (uint32_t)phys->mtl.maxThreadgroupMemoryLength;
    l->maxComputeWorkGroupCount[0] = l->maxComputeWorkGroupCount[1] = l->maxComputeWorkGroupCount[2] = 65535;
    l->maxComputeWorkGroupInvocations = 1024;
    l->maxComputeWorkGroupSize[0] = 1024; l->maxComputeWorkGroupSize[1] = 1024; l->maxComputeWorkGroupSize[2] = 1024;
    l->subPixelPrecisionBits = 8; l->subTexelPrecisionBits = 8; l->mipmapPrecisionBits = 8;
    l->maxDrawIndexedIndexValue = UINT32_MAX; l->maxDrawIndirectCount = UINT32_MAX;
    l->maxSamplerLodBias = 16; l->maxSamplerAnisotropy = 16; l->maxViewports = 16;
    l->maxViewportDimensions[0] = l->maxViewportDimensions[1] = 16384;
    l->viewportBoundsRange[0] = -32768; l->viewportBoundsRange[1] = 32767; l->viewportSubPixelBits = 8;
    l->minMemoryMapAlignment = 64; l->minTexelBufferOffsetAlignment = 64; l->minUniformBufferOffsetAlignment = 64;
    l->minStorageBufferOffsetAlignment = 64; l->minTexelOffset = -8; l->maxTexelOffset = 7;
    l->minTexelGatherOffset = -8; l->maxTexelGatherOffset = 7; l->minInterpolationOffset = -0.5f;
    l->maxInterpolationOffset = 0.4375f; l->subPixelInterpolationOffsetBits = 4;
    l->maxFramebufferWidth = 16384; l->maxFramebufferHeight = 16384; l->maxFramebufferLayers = 2048;
    VkSampleCountFlags samples = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT |
                                 ([phys->mtl supportsTextureSampleCount:8] ? VK_SAMPLE_COUNT_8_BIT : 0);
    l->framebufferColorSampleCounts = samples; l->framebufferDepthSampleCounts = samples;
    l->framebufferStencilSampleCounts = samples; l->framebufferNoAttachmentsSampleCounts = samples;
    l->maxColorAttachments = 8; l->sampledImageColorSampleCounts = samples; l->sampledImageIntegerSampleCounts = samples;
    l->sampledImageDepthSampleCounts = samples; l->sampledImageStencilSampleCounts = samples;
    l->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT; l->maxSampleMaskWords = 1;
    l->timestampComputeAndGraphics = 1; l->timestampPeriod = 1.0f; l->maxClipDistances = 8; l->maxCullDistances = 0;
    l->maxCombinedClipAndCullDistances = 8; l->discreteQueuePriorities = 2;
    l->pointSizeRange[0] = 1; l->pointSizeRange[1] = 511; l->lineWidthRange[0] = 1; l->lineWidthRange[1] = 1;
    l->pointSizeGranularity = 1; l->lineWidthGranularity = 1; l->strictLines = 0; l->standardSampleLocations = 1;
    l->optimalBufferCopyOffsetAlignment = 16; l->optimalBufferCopyRowPitchAlignment = 16; l->nonCoherentAtomSize = 64;
}

static void device_uuid(VkPhysicalDevice phys, uint8_t uuid[VK_UUID_SIZE]) {
    memset(uuid, 0, VK_UUID_SIZE);
    uint64_t id = phys->mtl.registryID;
    memcpy(uuid, "VXRT", 4);
    memcpy(uuid + 8, &id, 8);
}

VKFN void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice phys, VkPhysicalDeviceProperties *p) {
    memset(p, 0, sizeof *p);
    p->apiVersion = vk_api_version();
    p->driverVersion = VK_MAKE_VERSION(0, 3, 0);
    p->vendorID = 0x106b;   // Apple
    p->deviceID = 0x1000 | (uint32_t)(phys->mtl.registryID & 0xfff);
    p->deviceType = phys->mtl.hasUnifiedMemory ? VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU : VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    snprintf(p->deviceName, sizeof p->deviceName, "%s", phys->mtl.name.UTF8String);
    device_uuid(phys, p->pipelineCacheUUID);
    fill_limits(phys, &p->limits);
}

VKFN void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice phys, VkPhysicalDeviceProperties2 *p2) {
    vkGetPhysicalDeviceProperties(phys, &p2->properties);
    for (VkBaseOutStructure *s = p2->pNext; s; s = s->pNext) {
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES: {
            VkPhysicalDeviceVulkan11Properties *p = (void *)s;
            device_uuid(phys, p->deviceUUID);
            memcpy(p->driverUUID, "VXRuntime-Metal", 15);
            p->subgroupSize = 32;
            p->subgroupSupportedStages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
            p->subgroupSupportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT |
                VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;
            p->subgroupQuadOperationsInAllStages = 0;
            p->pointClippingBehavior = VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES;
            p->maxMultiviewViewCount = 1; p->maxMultiviewInstanceIndex = 0;
            p->maxPerSetDescriptors = 1000000;
            p->maxMemoryAllocationSize = phys->mtl.maxBufferLength;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES: {
            VkPhysicalDeviceVulkan12Properties *p = (void *)s;
            p->driverID = VK_DRIVER_ID_MOLTENVK;   // (the closest listed: a Vulkan driver over Metal)
            snprintf(p->driverName, sizeof p->driverName, "VXRuntime Vulkan");
            snprintf(p->driverInfo, sizeof p->driverInfo, "VeRantyx VXRuntime, Metal");
            p->conformanceVersion = (VkConformanceVersion){ 1, 3, 0, 0 };
            p->denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
            p->roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
            p->shaderSignedZeroInfNanPreserveFloat16 = 1; p->shaderSignedZeroInfNanPreserveFloat32 = 1;
            p->shaderRoundingModeRTEFloat16 = 1; p->shaderRoundingModeRTEFloat32 = 1;
            p->maxUpdateAfterBindDescriptorsInAllPools = 1000000;
            p->shaderSampledImageArrayNonUniformIndexingNative = 1;
            p->shaderStorageBufferArrayNonUniformIndexingNative = 1;
            p->shaderStorageImageArrayNonUniformIndexingNative = 1;
            p->shaderUniformBufferArrayNonUniformIndexingNative = 1;
            p->maxPerStageDescriptorUpdateAfterBindSamplers = 1024;
            p->maxPerStageDescriptorUpdateAfterBindUniformBuffers = 500000;
            p->maxPerStageDescriptorUpdateAfterBindStorageBuffers = 500000;
            p->maxPerStageDescriptorUpdateAfterBindSampledImages = 500000;
            p->maxPerStageDescriptorUpdateAfterBindStorageImages = 500000;
            p->maxPerStageDescriptorUpdateAfterBindInputAttachments = 8;
            p->maxPerStageUpdateAfterBindResources = 1000000;
            p->maxDescriptorSetUpdateAfterBindSamplers = 1024;
            p->maxDescriptorSetUpdateAfterBindUniformBuffers = 500000;
            p->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = 64;
            p->maxDescriptorSetUpdateAfterBindStorageBuffers = 500000;
            p->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = 64;
            p->maxDescriptorSetUpdateAfterBindSampledImages = 500000;
            p->maxDescriptorSetUpdateAfterBindStorageImages = 500000;
            p->maxDescriptorSetUpdateAfterBindInputAttachments = 8;
            p->supportedDepthResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT | VK_RESOLVE_MODE_MIN_BIT | VK_RESOLVE_MODE_MAX_BIT;
            p->supportedStencilResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            p->independentResolveNone = 1; p->independentResolve = 1;
            p->maxTimelineSemaphoreValueDifference = UINT64_MAX;
            p->framebufferIntegerColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES: {
            VkPhysicalDeviceVulkan13Properties *p = (void *)s;
            p->minSubgroupSize = 32; p->maxSubgroupSize = 32; p->maxComputeWorkgroupSubgroups = 32;
            p->requiredSubgroupSizeStages = VK_SHADER_STAGE_COMPUTE_BIT;
            p->maxBufferSize = phys->mtl.maxBufferLength;
            p->storageTexelBufferOffsetAlignmentBytes = 64; p->uniformTexelBufferOffsetAlignmentBytes = 64;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES: {
            VkPhysicalDeviceDriverProperties *p = (void *)s;
            p->driverID = VK_DRIVER_ID_MOLTENVK;
            snprintf(p->driverName, sizeof p->driverName, "VXRuntime Vulkan");
            snprintf(p->driverInfo, sizeof p->driverInfo, "VeRantyx VXRuntime, Metal");
            p->conformanceVersion = (VkConformanceVersion){ 1, 3, 0, 0 };
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES: {
            VkPhysicalDeviceIDProperties *p = (void *)s;
            device_uuid(phys, p->deviceUUID);
            memcpy(p->driverUUID, "VXRuntime-Metal", 15);
            // (the LUID DXGI gives the adapter: SteamVR's client finds the Vulkan device of the headset's adapter by it)
            uint32_t luid[2] = { 0x5658, 0 };
            memcpy(p->deviceLUID, luid, sizeof luid);
            p->deviceNodeMask = 1;
            p->deviceLUIDValid = 1;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES: {
            VkPhysicalDeviceSubgroupProperties *p = (void *)s;
            p->subgroupSize = 32;
            p->supportedStages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
            p->supportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT |
                VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;
            p->quadOperationsInAllStages = 0;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES: {
            VkPhysicalDeviceSubgroupSizeControlProperties *p = (void *)s;
            p->minSubgroupSize = 32; p->maxSubgroupSize = 32; p->maxComputeWorkgroupSubgroups = 32;
            p->requiredSubgroupSizeStages = VK_SHADER_STAGE_COMPUTE_BIT;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES: {
            VkPhysicalDeviceMaintenance3Properties *p = (void *)s;
            p->maxPerSetDescriptors = 1000000; p->maxMemoryAllocationSize = phys->mtl.maxBufferLength;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES:
            ((VkPhysicalDeviceMaintenance4Properties *)s)->maxBufferSize = phys->mtl.maxBufferLength; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES: {
            VkPhysicalDeviceDescriptorIndexingProperties *p = (void *)s;
            p->maxUpdateAfterBindDescriptorsInAllPools = 1000000;
            p->shaderSampledImageArrayNonUniformIndexingNative = 1;
            p->shaderStorageBufferArrayNonUniformIndexingNative = 1;
            p->shaderStorageImageArrayNonUniformIndexingNative = 1;
            p->shaderUniformBufferArrayNonUniformIndexingNative = 1;
            p->maxPerStageDescriptorUpdateAfterBindSamplers = 1024;
            p->maxPerStageDescriptorUpdateAfterBindUniformBuffers = 500000;
            p->maxPerStageDescriptorUpdateAfterBindStorageBuffers = 500000;
            p->maxPerStageDescriptorUpdateAfterBindSampledImages = 500000;
            p->maxPerStageDescriptorUpdateAfterBindStorageImages = 500000;
            p->maxPerStageDescriptorUpdateAfterBindInputAttachments = 8;
            p->maxPerStageUpdateAfterBindResources = 1000000;
            p->maxDescriptorSetUpdateAfterBindSamplers = 1024;
            p->maxDescriptorSetUpdateAfterBindUniformBuffers = 500000;
            p->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = 64;
            p->maxDescriptorSetUpdateAfterBindStorageBuffers = 500000;
            p->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = 64;
            p->maxDescriptorSetUpdateAfterBindSampledImages = 500000;
            p->maxDescriptorSetUpdateAfterBindStorageImages = 500000;
            p->maxDescriptorSetUpdateAfterBindInputAttachments = 8;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES:
            ((VkPhysicalDeviceTimelineSemaphoreProperties *)s)->maxTimelineSemaphoreValueDifference = UINT64_MAX; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES: {
            VkPhysicalDeviceDepthStencilResolveProperties *p = (void *)s;
            p->supportedDepthResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT | VK_RESOLVE_MODE_MIN_BIT | VK_RESOLVE_MODE_MAX_BIT;
            p->supportedStencilResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            p->independentResolveNone = 1; p->independentResolve = 1;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES:
            ((VkPhysicalDevicePushDescriptorProperties *)s)->maxPushDescriptors = 32; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES: {
            VkPhysicalDeviceFloatControlsProperties *p = (void *)s;
            p->denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
            p->roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
            p->shaderSignedZeroInfNanPreserveFloat32 = 1; p->shaderRoundingModeRTEFloat32 = 1;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT:
            ((VkPhysicalDeviceCustomBorderColorPropertiesEXT *)s)->maxCustomBorderColorSamplers = 1024; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT: {
            VkPhysicalDeviceRobustness2PropertiesEXT *p = (void *)s;
            p->robustStorageBufferAccessSizeAlignment = 4; p->robustUniformBufferAccessSizeAlignment = 4; break;
        }
        default: break;
        }
    }
}

VKFN void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice phys, uint32_t *count, VkQueueFamilyProperties *props) {
    (void)phys;
    if (!props) { *count = 1; return; }
    if (!*count) return;
    props[0] = (VkQueueFamilyProperties){ VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
                                          VK_MAX_QUEUES, 64, { 1, 1, 1 } };
    *count = 1;
}
VKFN void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice phys, uint32_t *count, VkQueueFamilyProperties2 *props) {
    if (!props) { *count = 1; return; }
    if (!*count) return;
    uint32_t one = 1;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &one, &props[0].queueFamilyProperties);
    *count = 1;
}

static uint64_t heap_size(VkPhysicalDevice phys) {
    uint64_t size = phys->mtl.recommendedMaxWorkingSetSize;
    return size ? size : 8ull << 30;
}
VKFN void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice phys, VkPhysicalDeviceMemoryProperties *p) {
    memset(p, 0, sizeof *p);
    p->memoryHeapCount = 1;
    p->memoryHeaps[0] = (VkMemoryHeap){ heap_size(phys), VK_MEMORY_HEAP_DEVICE_LOCAL_BIT };
    p->memoryTypeCount = VK_MEMTYPE_COUNT;
    p->memoryTypes[VK_MEMTYPE_DEVICE] = (VkMemoryType){ VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0 };
    p->memoryTypes[VK_MEMTYPE_HOST] = (VkMemoryType){ VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0 };
    p->memoryTypes[VK_MEMTYPE_HOST_CACHED] = (VkMemoryType){ VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0 };
}
VKFN void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice phys, VkPhysicalDeviceMemoryProperties2 *p) {
    vkGetPhysicalDeviceMemoryProperties(phys, &p->memoryProperties);
    VkPhysicalDeviceMemoryBudgetPropertiesEXT *b = vk_find_struct_mut(p->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT);
    if (b) {
        memset(b->heapBudget, 0, sizeof b->heapBudget);
        memset(b->heapUsage, 0, sizeof b->heapUsage);
        b->heapBudget[0] = heap_size(phys);
        b->heapUsage[0] = phys->mtl.currentAllocatedSize;
    }
}

VKFN void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice phys, VkFormat format, VkFormatProperties *p) {
    const vk_format_info *f = vk_format(format);
    p->optimalTilingFeatures = vk_format_features(phys->mtl, f, 1, 0);
    p->linearTilingFeatures = vk_format_features(phys->mtl, f, 0, 0);
    p->bufferFeatures = vk_format_features(phys->mtl, f, 0, 1);
}
VKFN void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice phys, VkFormat format, VkFormatProperties2 *p) {
    vkGetPhysicalDeviceFormatProperties(phys, format, &p->formatProperties);
    VkFormatProperties3 *p3 = vk_find_struct_mut(p->pNext, VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3);
    if (p3) {
        p3->optimalTilingFeatures = p->formatProperties.optimalTilingFeatures;
        p3->linearTilingFeatures = p->formatProperties.linearTilingFeatures;
        p3->bufferFeatures = p->formatProperties.bufferFeatures;
        if (p3->optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
            p3->optimalTilingFeatures |= VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT | VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
        if (p3->optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            p3->optimalTilingFeatures |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT;
    }
}

VKFN VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice phys, VkFormat format, VkImageType type,
    VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *p) {
    memset(p, 0, sizeof *p);
    const vk_format_info *f = vk_format(format);
    VkFormatFeatureFlags feats = vk_format_features(phys->mtl, f, tiling == VK_IMAGE_TILING_OPTIMAL, 0);
    if (!feats) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) && !(feats & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) && !(feats & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && !(feats & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) && !(feats & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (tiling == VK_IMAGE_TILING_LINEAR && type != VK_IMAGE_TYPE_2D) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (f->compressed && type == VK_IMAGE_TYPE_1D) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    (void)flags;
    uint32_t max = type == VK_IMAGE_TYPE_3D ? 2048 : 16384;
    p->maxExtent = (VkExtent3D){ max, type == VK_IMAGE_TYPE_1D ? 1 : max, type == VK_IMAGE_TYPE_3D ? 2048 : 1 };
    p->maxMipLevels = tiling == VK_IMAGE_TILING_LINEAR ? 1 : 15;
    p->maxArrayLayers = tiling == VK_IMAGE_TILING_LINEAR || type == VK_IMAGE_TYPE_3D ? 1 : 2048;
    p->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
    if (tiling == VK_IMAGE_TILING_OPTIMAL && type == VK_IMAGE_TYPE_2D && !f->compressed &&
        (feats & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)))
        p->sampleCounts |= VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT | ([phys->mtl supportsTextureSampleCount:8] ? VK_SAMPLE_COUNT_8_BIT : 0);
    p->maxResourceSize = phys->mtl.maxBufferLength;
    return VK_SUCCESS;
}
VKFN VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice phys, const VkPhysicalDeviceImageFormatInfo2 *info,
                                                                  VkImageFormatProperties2 *p) {
    VkResult r = vkGetPhysicalDeviceImageFormatProperties(phys, info->format, info->type, info->tiling, info->usage, info->flags,
                                                           &p->imageFormatProperties);
    VkExternalImageFormatProperties *ext = vk_find_struct_mut(p->pNext, VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
    if (ext) {
        memset(&ext->externalMemoryProperties, 0, sizeof ext->externalMemoryProperties);
        // Win32 handles of 2D single-sample images, one level: shared through IOSurfaces (Direct3D 11's shared textures).
        const VkPhysicalDeviceExternalImageFormatInfo *x = vk_find_struct(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
        if (r == VK_SUCCESS && x && (x->handleType & VK_WIN32_IMAGE_HANDLES) && info->type == VK_IMAGE_TYPE_2D &&
            info->tiling == VK_IMAGE_TILING_OPTIMAL) {
            ext->externalMemoryProperties.externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT |
                VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
            ext->externalMemoryProperties.exportFromImportedHandleTypes = VK_WIN32_IMAGE_HANDLES;
            ext->externalMemoryProperties.compatibleHandleTypes = VK_WIN32_IMAGE_HANDLES;
            p->imageFormatProperties.maxMipLevels = 1;
            p->imageFormatProperties.maxArrayLayers = 1;
            p->imageFormatProperties.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
        } else if (x && x->handleType) r = VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    return r;
}
VKFN void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice phys, VkFormat format, VkImageType type,
    VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageTiling tiling, uint32_t *count, VkSparseImageFormatProperties *p) {
    (void)phys; (void)format; (void)type; (void)samples; (void)usage; (void)tiling; (void)p;
    *count = 0;
}
VKFN void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice phys, const VkPhysicalDeviceSparseImageFormatInfo2 *info,
    uint32_t *count, VkSparseImageFormatProperties2 *p) {
    (void)phys; (void)info; (void)p;
    *count = 0;
}
VKFN VkResult VKAPI_CALL vkGetPhysicalDeviceFragmentShadingRatesKHR(VkPhysicalDevice phys, uint32_t *count, VkPhysicalDeviceFragmentShadingRateKHR *r) {
    (void)phys; (void)r;
    *count = 0;
    return VK_SUCCESS;
}
VKFN VkResult VKAPI_CALL vkGetPhysicalDeviceToolProperties(VkPhysicalDevice phys, uint32_t *count, VkPhysicalDeviceToolProperties *p) {
    (void)phys; (void)p;
    *count = 0;
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkGetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice phys, const VkPhysicalDeviceExternalBufferInfo *info,
                                                                VkExternalBufferProperties *p) {
    (void)phys; (void)info;
    memset(&p->externalMemoryProperties, 0, sizeof p->externalMemoryProperties);
}
VKFN void VKAPI_CALL vkGetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice phys, const VkPhysicalDeviceExternalSemaphoreInfo *info,
                                                                   VkExternalSemaphoreProperties *p) {
    (void)phys; (void)info;
    p->exportFromImportedHandleTypes = 0; p->compatibleHandleTypes = 0; p->externalSemaphoreFeatures = 0;
}
VKFN void VKAPI_CALL vkGetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice phys, const VkPhysicalDeviceExternalFenceInfo *info,
                                                               VkExternalFenceProperties *p) {
    (void)phys; (void)info;
    p->exportFromImportedHandleTypes = 0; p->compatibleHandleTypes = 0; p->externalFenceFeatures = 0;
}

VKFN VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice phys, const char *layer, uint32_t *count, VkExtensionProperties *props) {
    (void)phys;
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    return enumerate_exts(g_device_exts, sizeof g_device_exts / sizeof g_device_exts[0], count, props);
}
VKFN VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice phys, uint32_t *count, VkLayerProperties *props) {
    (void)phys; (void)props;
    *count = 0;
    return VK_SUCCESS;
}

// ---- device ------------------------------------------------------------------------------------------------------
void vk_device_add_heap(VkDevice dev, id<MTLHeap> heap) {
    vk_heap_list *l = &dev->heaps;
    pthread_mutex_lock(&l->lock);
    if (l->count == l->cap) {
        uint32_t cap = l->cap ? l->cap * 2 : 256;
        __strong id<MTLHeap> *n = (__strong id<MTLHeap> *)calloc(cap, sizeof *n);
        for (uint32_t i = 0; i < l->count; i++) { n[i] = l->heaps[i]; l->heaps[i] = nil; }
        free(l->heaps);
        l->heaps = n;
        l->cap = cap;
    }
    l->heaps[l->count++] = heap;
    l->generation++;
    if (@available(macOS 15.0, *)) if (l->residency) { [(id<MTLResidencySet>)l->residency addAllocation:heap]; l->residency_dirty = 1; }
    pthread_mutex_unlock(&l->lock);
}
void vk_residency_commit(VkDevice dev) {
    vk_heap_list *l = &dev->heaps;
    if (!l->residency || !__atomic_load_n(&l->residency_dirty, __ATOMIC_RELAXED)) return;
    pthread_mutex_lock(&l->lock);
    if (@available(macOS 15.0, *)) if (l->residency_dirty) { [(id<MTLResidencySet>)l->residency commit]; l->residency_dirty = 0; }
    pthread_mutex_unlock(&l->lock);
}
int vk_residency_on(VkDevice dev) { return dev->heaps.residency != nil && vk_option("RESIDENCY_SET", 1); }
void vk_device_remove_heap(VkDevice dev, id<MTLHeap> heap) {
    vk_heap_list *l = &dev->heaps;
    pthread_mutex_lock(&l->lock);
    for (uint32_t i = 0; i < l->count; i++)
        if (l->heaps[i] == heap) { l->heaps[i] = l->heaps[l->count - 1]; l->heaps[--l->count] = nil; break; }
    l->generation++;
    if (@available(macOS 15.0, *)) if (l->residency) { [(id<MTLResidencySet>)l->residency removeAllocation:heap]; l->residency_dirty = 1; }
    pthread_mutex_unlock(&l->lock);
}

VKFN VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *info, const VkAllocationCallbacks *alloc, VkDevice *out) {
    (void)alloc;
    *out = NULL;
    uint32_t exts = 0;
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
        const char *name = info->ppEnabledExtensionNames[i];
        int found = 0;
        for (size_t k = 0; k < sizeof g_device_exts / sizeof g_device_exts[0]; k++)
            if (!strcmp(name, g_device_exts[k].name)) { exts |= g_device_exts[k].bit; found = 1; }
        if (!found) { TRACE("device extension %s: not present", name); return VK_ERROR_EXTENSION_NOT_PRESENT; }
        TRACE("device extension %s", name);
    }
    struct VkDevice_T *dev = calloc(1, sizeof *dev);
    if (!dev) return VK_ERROR_OUT_OF_HOST_MEMORY;
    dev->phys = phys;
    dev->mtl = phys->mtl;
    dev->extensions = exts;
    if (info->pEnabledFeatures) dev->features = *info->pEnabledFeatures;
    const VkPhysicalDeviceFeatures2 *f2 = vk_find_struct(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    if (f2) dev->features = f2->features;
    pthread_mutex_init(&dev->heaps.lock, NULL);
    pthread_mutex_init(&dev->internal_lock, NULL);
    if (@available(macOS 15.0, *)) {
        MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new];
        rd.label = @"vxrt vulkan heaps";
        NSError *err = nil;
        dev->heaps.residency = [dev->mtl newResidencySetWithDescriptor:rd error:&err];
        if (!dev->heaps.residency) TRACE("no residency set: %s", err.localizedDescription.UTF8String);
    }
    uint32_t queues = 0;
    for (uint32_t i = 0; i < info->queueCreateInfoCount; i++)
        if (info->pQueueCreateInfos[i].queueFamilyIndex == 0) queues += info->pQueueCreateInfos[i].queueCount;
    if (queues > VK_MAX_QUEUES) queues = VK_MAX_QUEUES;
    if (!queues) queues = 1;
    for (uint32_t i = 0; i < queues; i++) {
        struct VkQueue_T *q = &dev->queues[i];
        q->device = dev;
        q->family = 0;
        q->index = i;
        q->mtl = [dev->mtl newCommandQueueWithMaxCommandBufferCount:256];
        q->done = [dev->mtl newSharedEvent];
        if (@available(macOS 15.0, *)) if (dev->heaps.residency) [q->mtl addResidencySet:(id<MTLResidencySet>)dev->heaps.residency];
        pthread_mutex_init(&q->lock, NULL);
    }
    dev->queue_count = queues;
    MTLResourceOptions zo = MTLResourceStorageModeShared;
    dev->zero_buffer = [dev->mtl newBufferWithLength:65536 options:zo];
    vk_internal_init(dev);
    TRACE("device on %s: %u queues", dev->mtl.name.UTF8String, queues);
    *out = dev;
    return VK_SUCCESS;
}

VKFN void VKAPI_CALL vkDestroyDevice(VkDevice dev, const VkAllocationCallbacks *alloc) {
    (void)alloc;
    if (!dev) return;
    for (uint32_t i = 0; i < dev->queue_count; i++) {
        struct VkQueue_T *q = &dev->queues[i];
        // (the queue's work finishes first)
        if (q->submitted) [q->done waitUntilSignaledValue:q->submitted timeoutMS:5000];
        q->mtl = nil; q->done = nil; q->last_fence = nil;
        pthread_mutex_destroy(&q->lock);
    }
    for (uint32_t i = 0; i < dev->heaps.count; i++) dev->heaps.heaps[i] = nil;
    free(dev->heaps.heaps);
    dev->internal = nil; dev->internal_pipelines = nil; dev->internal_linear = nil; dev->internal_nearest = nil;
    dev->zero_buffer = nil;
    pthread_mutex_destroy(&dev->heaps.lock);
    pthread_mutex_destroy(&dev->internal_lock);
    free(dev);
}

VKFN void VKAPI_CALL vkGetDeviceQueue(VkDevice dev, uint32_t family, uint32_t index, VkQueue *q) {
    (void)family;
    *q = index < dev->queue_count ? &dev->queues[index] : &dev->queues[0];
}
VKFN void VKAPI_CALL vkGetDeviceQueue2(VkDevice dev, const VkDeviceQueueInfo2 *info, VkQueue *q) {
    vkGetDeviceQueue(dev, info->queueFamilyIndex, info->queueIndex, q);
}

// ---- memory ------------------------------------------------------------------------------------------------------
// ---- external memory (VK_KHR_external_memory_win32) ---------------------------------------------------------------
// A dedicated image's memory as a texture on a global IOSurface, the way Direct3D 11 shares textures between processes
// (d3d11_shared.m): imported from a shared handle (the surface opened as this image's texture), or made here and given a
// handle (vkGetMemoryWin32HandleKHR) that Direct3D's OpenSharedResource or another Vulkan device imports. SteamVR's
// client shares a Vulkan game's eye images with its compositor this way.
enum { TD_WIDTH, TD_HEIGHT, TD_MIPS, TD_ARRAY, TD_FORMAT, TD_SAMPLES, TD_QUALITY, TD_USAGE, TD_BIND, TD_CPU, TD_MISC, TD_COUNT };
int vx_shared_texture_find(uintptr_t h, uint32_t *surface, uint32_t desc[TD_COUNT]);
int vx_shared_texture_add(uint32_t surface, const uint32_t desc[TD_COUNT]);
#define VX_SHARED_HANDLE_BASE 0x40000000u
static uint32_t dxgi_format_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return 28;
    case VK_FORMAT_R8G8B8A8_SRGB: return 29;
    case VK_FORMAT_B8G8R8A8_UNORM: return 87;
    case VK_FORMAT_B8G8R8A8_SRGB: return 91;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 10;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 24;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return 26;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 2;
    case VK_FORMAT_R16G16B16A16_UNORM: return 11;
    case VK_FORMAT_R32_SFLOAT: return 41;
    case VK_FORMAT_D32_SFLOAT: return 40;
    default: return 0;
    }
}
static id<MTLTexture> surface_texture(VkDevice dev, VkImage img, IOSurfaceRef surface) {
    MTLTextureDescriptor *d = [img->desc copy];
    d.storageMode = MTLStorageModeShared;
    d.mipmapLevelCount = 1;
    d.usage |= MTLTextureUsagePixelFormatView;
    return [dev->mtl newTextureWithDescriptor:d iosurface:surface plane:0];
}
static VkResult external_memory(VkDevice dev, const VkMemoryAllocateInfo *info, const VkImportMemoryWin32HandleInfoKHR *imp,
                                const VkExportMemoryAllocateInfo *exp, VkDeviceMemory *out) {
    const VkMemoryDedicatedAllocateInfo *ded = vk_find_struct(info->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
    VkImage img = ded ? ded->image : VK_NULL_HANDLE;
    if (!img || img->type != VK_IMAGE_TYPE_2D || img->samples > 1 || img->mips != 1 || img->layers != 1 ||
        img->tiling != VK_IMAGE_TILING_OPTIMAL) {
        if (vk_log_once("external memory that is not a dedicated 2D image", 0))
            vx_log("vulkan: external memory that is not a dedicated single 2D image is not supported (logged once)");
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    id<MTLTexture> tex = nil;
    int slot = -1, keyed = 0;
    if (imp && imp->handleType) {
        uint32_t sid = 0, desc[TD_COUNT];
        slot = imp->handle ? vx_shared_texture_find((uintptr_t)imp->handle, &sid, desc) : -1;
        IOSurfaceRef surface = slot >= 0 ? IOSurfaceLookup(sid) : NULL;
        if (!surface) { TRACE("import of handle %p (type %#x) failed", imp->handle, imp->handleType); return VK_ERROR_INVALID_EXTERNAL_HANDLE; }
        if (desc[TD_WIDTH] < img->extent.width || desc[TD_HEIGHT] < img->extent.height)
            TRACE("imported surface %ux%u for an image of %ux%u", desc[TD_WIDTH], desc[TD_HEIGHT], img->extent.width, img->extent.height);
        tex = surface_texture(dev, img, surface);
        CFRelease(surface);
        keyed = slot + 1;
        slot = -1;                                                 // (the slot stays its creator's)
    } else {
        uint32_t bpp = img->fmt->bytes, dxgi = dxgi_format_of(img->format);
        if (!dxgi || img->fmt->compressed) {
            if (vk_log_once("export of image format", img->format)) vx_log("vulkan: export of an image of format %d is not supported (logged once)", img->format);
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        NSDictionary *props = @{ (id)kIOSurfaceWidth: @(img->extent.width), (id)kIOSurfaceHeight: @(img->extent.height),
                                 (id)kIOSurfaceBytesPerElement: @(bpp),
                                 (id)kIOSurfaceBytesPerRow: @(IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, img->extent.width * bpp)),
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                                 (id)kIOSurfaceIsGlobal: @YES };
#pragma clang diagnostic pop
        IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
        if (!surface) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        tex = surface_texture(dev, img, surface);
        uint32_t desc[TD_COUNT] = { 0 };
        desc[TD_WIDTH] = img->extent.width; desc[TD_HEIGHT] = img->extent.height; desc[TD_MIPS] = 1; desc[TD_ARRAY] = 1;
        desc[TD_FORMAT] = dxgi; desc[TD_SAMPLES] = 1;
        desc[TD_BIND] = 0x8 | 0x20;                                // D3D11_BIND_SHADER_RESOURCE | RENDER_TARGET
        desc[TD_MISC] = 0x2;                                       // D3D11_RESOURCE_MISC_SHARED
        if (tex) slot = vx_shared_texture_add(IOSurfaceGetID(surface), desc);
        keyed = slot + 1;
        CFRelease(surface);
        if (tex && slot < 0) tex = nil;
    }
    if (!tex) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    struct VkDeviceMemory_T *m = calloc(1, sizeof *m);
    if (!m) return VK_ERROR_OUT_OF_HOST_MEMORY;
    m->size = info->allocationSize;
    m->type = VK_MEMTYPE_DEVICE;
    m->dedicated_image = img;
    m->external = tex;
    m->shared_slot = slot + 1;
    m->keyed_slot = keyed;
    *out = m;
    return VK_SUCCESS;
}
VKFN VkResult VKAPI_CALL vkGetMemoryWin32HandleKHR(VkDevice dev, const VkMemoryGetWin32HandleInfoKHR *info, void **handle) {
    (void)dev;
    VkDeviceMemory m = info->memory;
    if (!m || !m->shared_slot) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    *handle = (void *)(uintptr_t)(VX_SHARED_HANDLE_BASE + 4u * (uint32_t)(m->shared_slot - 1));   // (the same in every process)
    return VK_SUCCESS;
}
VKFN VkResult VKAPI_CALL vkGetMemoryWin32HandlePropertiesKHR(VkDevice dev, VkExternalMemoryHandleTypeFlagBits type, void *handle,
                                                            VkMemoryWin32HandlePropertiesKHR *p) {
    (void)dev;
    uint32_t sid, desc[TD_COUNT];
    if (!(type & VK_WIN32_IMAGE_HANDLES) || vx_shared_texture_find((uintptr_t)handle, &sid, desc) < 0) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    p->memoryTypeBits = 1u << VK_MEMTYPE_DEVICE;
    return VK_SUCCESS;
}

VKFN VkResult VKAPI_CALL vkAllocateMemory(VkDevice dev, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *alloc, VkDeviceMemory *out) {
    (void)alloc;
    *out = VK_NULL_HANDLE;
    if (info->memoryTypeIndex >= VK_MEMTYPE_COUNT) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VK_STAT(VKS_ALLOCS, 1);
    VK_STAT(VKS_ALLOC_BYTES, info->allocationSize);
    const VkImportMemoryWin32HandleInfoKHR *imp = vk_find_struct(info->pNext, VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR);
    const VkExportMemoryAllocateInfo *exp = vk_find_struct(info->pNext, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
    if ((imp && imp->handleType) || (exp && exp->handleTypes)) return external_memory(dev, info, imp, exp, out);
    uint64_t size = (info->allocationSize + 65535) & ~65535ull;
    if (!size) size = 65536;
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.type = MTLHeapTypePlacement;
    hd.storageMode = info->memoryTypeIndex == VK_MEMTYPE_DEVICE ? MTLStorageModePrivate : MTLStorageModeShared;
    hd.cpuCacheMode = MTLCPUCacheModeDefaultCache;
    hd.hazardTrackingMode = MTLHazardTrackingModeUntracked;
    hd.size = size;
    id<MTLHeap> heap = [dev->mtl newHeapWithDescriptor:hd];
    if (!heap) { TRACE("heap of %llu bytes failed", (unsigned long long)size); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    struct VkDeviceMemory_T *m = calloc(1, sizeof *m);
    if (!m) return VK_ERROR_OUT_OF_HOST_MEMORY;
    m->size = info->allocationSize;
    m->type = info->memoryTypeIndex;
    m->heap = heap;
    if (info->memoryTypeIndex != VK_MEMTYPE_DEVICE) {
        m->backing = [heap newBufferWithLength:size options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked offset:0];
        if (!m->backing) { m->heap = nil; free(m); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
        m->map = m->backing.contents;
    }
    const VkMemoryDedicatedAllocateInfo *ded = vk_find_struct(info->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
    if (ded) m->dedicated_image = ded->image;
    vk_device_add_heap(dev, heap);
    *out = m;
    return VK_SUCCESS;
}

VKFN void VKAPI_CALL vkFreeMemory(VkDevice dev, VkDeviceMemory mem, const VkAllocationCallbacks *alloc) {
    (void)alloc;
    if (!mem) return;
    if (mem->external) {
        void d3d_shared_release(int slot);
        if (mem->shared_slot) d3d_shared_release(mem->shared_slot - 1);
        mem->external = nil;
        free(mem);
        return;
    }
    vk_device_remove_heap(dev, mem->heap);
    mem->backing = nil;
    mem->heap = nil;
    free(mem);
}

VKFN VkResult VKAPI_CALL vkMapMemory(VkDevice dev, VkDeviceMemory mem, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void **data) {
    (void)dev; (void)size; (void)flags;
    VK_STAT(VKS_MAPS, 1);
    if (!mem->map) return VK_ERROR_MEMORY_MAP_FAILED;
    *data = (uint8_t *)mem->map + offset;
    return VK_SUCCESS;
}
VKFN VkResult VKAPI_CALL vkMapMemory2KHR(VkDevice dev, const VkMemoryMapInfoKHR *info, void **data) {
    return vkMapMemory(dev, info->memory, info->offset, info->size, info->flags, data);
}
VKFN void VKAPI_CALL vkUnmapMemory(VkDevice dev, VkDeviceMemory mem) { (void)dev; (void)mem; }
VKFN VkResult VKAPI_CALL vkUnmapMemory2KHR(VkDevice dev, const VkMemoryUnmapInfoKHR *info) { (void)dev; (void)info; return VK_SUCCESS; }
VKFN VkResult VKAPI_CALL vkFlushMappedMemoryRanges(VkDevice dev, uint32_t n, const VkMappedMemoryRange *r) { (void)dev; (void)n; (void)r; return VK_SUCCESS; }
VKFN VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges(VkDevice dev, uint32_t n, const VkMappedMemoryRange *r) { (void)dev; (void)n; (void)r; return VK_SUCCESS; }
VKFN void VKAPI_CALL vkGetDeviceMemoryCommitment(VkDevice dev, VkDeviceMemory mem, VkDeviceSize *bytes) { (void)dev; *bytes = mem->size; }
VKFN void VKAPI_CALL vkSetDeviceMemoryPriorityEXT(VkDevice dev, VkDeviceMemory mem, float priority) { (void)dev; (void)mem; (void)priority; }

// ---- buffers -----------------------------------------------------------------------------------------------------
static uint64_t buffer_alignment(VkDevice dev, uint64_t size) {
    MTLSizeAndAlign sa = [dev->mtl heapBufferSizeAndAlignWithLength:size ? size : 1 options:MTLResourceStorageModePrivate];
    return sa.align > 256 ? sa.align : 256;
}

VKFN VkResult VKAPI_CALL vkCreateBuffer(VkDevice dev, const VkBufferCreateInfo *info, const VkAllocationCallbacks *alloc, VkBuffer *out) {
    (void)dev; (void)alloc;
    VK_STAT(VKS_BUFFERS, 1);
    struct VkBuffer_T *b = calloc(1, sizeof *b);
    if (!b) return VK_ERROR_OUT_OF_HOST_MEMORY;
    b->size = info->size;
    b->usage = info->usage;
    b->flags = info->flags;
    *out = b;
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkDestroyBuffer(VkDevice dev, VkBuffer b, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    if (!b) return;
    b->mtl = nil;
    free(b);
}
static void buffer_requirements(VkDevice dev, uint64_t size, VkMemoryRequirements *r) {
    uint64_t align = buffer_alignment(dev, size);
    r->alignment = align;
    r->size = (size + align - 1) & ~(align - 1);
    r->memoryTypeBits = (1u << VK_MEMTYPE_COUNT) - 1;
}
VKFN void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice dev, VkBuffer b, VkMemoryRequirements *r) { buffer_requirements(dev, b->size, r); }
static void dedicated_reqs(void *chain, int prefer) {
    VkMemoryDedicatedRequirements *d = vk_find_struct_mut(chain, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS);
    if (d) { d->prefersDedicatedAllocation = prefer; d->requiresDedicatedAllocation = 0; }
}
VKFN void VKAPI_CALL vkGetBufferMemoryRequirements2(VkDevice dev, const VkBufferMemoryRequirementsInfo2 *info, VkMemoryRequirements2 *r) {
    buffer_requirements(dev, info->buffer->size, &r->memoryRequirements);
    dedicated_reqs(r->pNext, 0);
}
VKFN void VKAPI_CALL vkGetDeviceBufferMemoryRequirements(VkDevice dev, const VkDeviceBufferMemoryRequirements *info, VkMemoryRequirements2 *r) {
    buffer_requirements(dev, info->pCreateInfo->size, &r->memoryRequirements);
    dedicated_reqs(r->pNext, 0);
}

static VkResult bind_buffer(VkDevice dev, VkBuffer b, VkDeviceMemory mem, uint64_t offset) {
    b->memory = mem;
    b->memory_offset = offset;
    if (mem->backing) {
        b->mtl = mem->backing;
        b->offset = offset;
    } else {
        uint64_t size = b->size ? b->size : 1;
        b->mtl = [mem->heap newBufferWithLength:size options:MTLResourceStorageModePrivate | MTLResourceHazardTrackingModeUntracked offset:offset];
        b->offset = 0;
        if (!b->mtl) { TRACE("buffer of %llu bytes at %llu in a heap of %llu failed", (unsigned long long)size, (unsigned long long)offset, (unsigned long long)mem->size); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    }
    b->gpu = b->mtl.gpuAddress + b->offset;
    (void)dev;
    return VK_SUCCESS;
}
VKFN VkResult VKAPI_CALL vkBindBufferMemory(VkDevice dev, VkBuffer b, VkDeviceMemory mem, VkDeviceSize offset) {
    return bind_buffer(dev, b, mem, offset);
}
VKFN VkResult VKAPI_CALL vkBindBufferMemory2(VkDevice dev, uint32_t n, const VkBindBufferMemoryInfo *infos) {
    for (uint32_t i = 0; i < n; i++) {
        VkResult r = bind_buffer(dev, infos[i].buffer, infos[i].memory, infos[i].memoryOffset);
        VkBindMemoryStatusKHR *st = (VkBindMemoryStatusKHR *)vk_find_struct(infos[i].pNext, VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS_KHR);
        if (st && st->pResult) *st->pResult = r;
        if (r) return r;
    }
    return VK_SUCCESS;
}
VKFN VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddress(VkDevice dev, const VkBufferDeviceAddressInfo *info) { (void)dev; return info->buffer->gpu; }

// ---- images ------------------------------------------------------------------------------------------------------
static MTLTextureType texture_type(VkImageType type, uint32_t layers, uint32_t samples) {
    if (type == VK_IMAGE_TYPE_1D) return layers > 1 ? MTLTextureType1DArray : MTLTextureType1D;
    if (type == VK_IMAGE_TYPE_3D) return MTLTextureType3D;
    if (samples > 1) return layers > 1 ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample;
    return layers > 1 ? MTLTextureType2DArray : MTLTextureType2D;
}

MTLTextureUsage vk_texture_usage(const vk_format_info *f, VkImageUsageFlags usage, VkImageCreateFlags flags);
MTLTextureUsage vk_texture_usage(const vk_format_info *f, VkImageUsageFlags usage, VkImageCreateFlags flags) {
    MTLTextureUsage u = MTLTextureUsageShaderRead;
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT) u |= MTLTextureUsageShaderWrite;
    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        if (!f->compressed) u |= MTLTextureUsageRenderTarget;   // (clears and blits draw into it)
    if ((flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) || (f->aspects & VKF_STENCIL) || (f->aspects & VKF_COLOR))
        u |= MTLTextureUsagePixelFormatView;                    // (sRGB/UNORM views, swizzles, stencil views)
    return u;
}

VKFN VkResult VKAPI_CALL vkCreateImage(VkDevice dev, const VkImageCreateInfo *info, const VkAllocationCallbacks *alloc, VkImage *out) {
    (void)alloc;
    const vk_format_info *f = vk_format(info->format);
    if (!f || f->mtl == MTLPixelFormatInvalid) { TRACE("image format %d unsupported", info->format); return VK_ERROR_FORMAT_NOT_SUPPORTED; }
    VK_STAT(VKS_IMAGES, 1);
    struct VkImage_T *img = calloc(1, sizeof *img);
    if (!img) return VK_ERROR_OUT_OF_HOST_MEMORY;
    img->type = info->imageType;
    img->format = info->format;
    img->fmt = f;
    img->extent = info->extent;
    img->mips = info->mipLevels;
    img->layers = info->arrayLayers;
    img->samples = info->samples;
    img->usage = info->usage;
    img->flags = info->flags;
    img->tiling = info->tiling;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = texture_type(info->imageType, info->arrayLayers, info->samples);
    d.pixelFormat = f->mtl;
    d.width = info->extent.width;
    d.height = info->imageType == VK_IMAGE_TYPE_1D ? 1 : info->extent.height;
    d.depth = info->imageType == VK_IMAGE_TYPE_3D ? info->extent.depth : 1;
    d.mipmapLevelCount = info->mipLevels;
    d.arrayLength = info->imageType == VK_IMAGE_TYPE_3D ? 1 : info->arrayLayers;
    d.sampleCount = info->samples;
    d.usage = vk_texture_usage(f, info->usage, info->flags);
    d.storageMode = info->tiling == VK_IMAGE_TILING_LINEAR ? MTLStorageModeShared : MTLStorageModePrivate;
    d.hazardTrackingMode = MTLHazardTrackingModeUntracked;
    if (info->tiling == VK_IMAGE_TILING_LINEAR) {
        NSUInteger align = [dev->mtl minimumLinearTextureAlignmentForPixelFormat:f->mtl];
        uint64_t row = (uint64_t)info->extent.width * f->bytes;
        img->row_pitch = (row + align - 1) / align * align;
    }
    img->desc = d;
    *out = img;
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkDestroyImage(VkDevice dev, VkImage img, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    if (!img || img->swapchain) return;
    img->mtl = nil;
    img->desc = nil;
    free(img);
}

static void image_requirements(VkDevice dev, VkImage img, VkMemoryRequirements *r) {
    if (img->tiling == VK_IMAGE_TILING_LINEAR) {
        uint64_t size = img->row_pitch * img->extent.height;
        r->size = (size + 255) & ~255ull;
        r->alignment = 256;
        r->memoryTypeBits = (1u << VK_MEMTYPE_HOST) | (1u << VK_MEMTYPE_HOST_CACHED);
        return;
    }
    MTLSizeAndAlign sa = [dev->mtl heapTextureSizeAndAlignWithDescriptor:img->desc];
    r->size = sa.size;
    r->alignment = sa.align;
    r->memoryTypeBits = 1u << VK_MEMTYPE_DEVICE;
}
VKFN void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice dev, VkImage img, VkMemoryRequirements *r) { image_requirements(dev, img, r); }
VKFN void VKAPI_CALL vkGetImageMemoryRequirements2(VkDevice dev, const VkImageMemoryRequirementsInfo2 *info, VkMemoryRequirements2 *r) {
    image_requirements(dev, info->image, &r->memoryRequirements);
    dedicated_reqs(r->pNext, (info->image->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0);
}
VKFN void VKAPI_CALL vkGetDeviceImageMemoryRequirements(VkDevice dev, const VkDeviceImageMemoryRequirements *info, VkMemoryRequirements2 *r) {
    VkImage img = NULL;
    if (vkCreateImage(dev, info->pCreateInfo, NULL, &img) != VK_SUCCESS) { memset(&r->memoryRequirements, 0, sizeof r->memoryRequirements); return; }
    image_requirements(dev, img, &r->memoryRequirements);
    dedicated_reqs(r->pNext, 0);
    vkDestroyImage(dev, img, NULL);
}
VKFN void VKAPI_CALL vkGetImageSparseMemoryRequirements(VkDevice dev, VkImage img, uint32_t *count, VkSparseImageMemoryRequirements *r) {
    (void)dev; (void)img; (void)r;
    *count = 0;
}
VKFN void VKAPI_CALL vkGetImageSparseMemoryRequirements2(VkDevice dev, const VkImageSparseMemoryRequirementsInfo2 *info, uint32_t *count,
                                                        VkSparseImageMemoryRequirements2 *r) {
    (void)dev; (void)info; (void)r;
    *count = 0;
}
VKFN void VKAPI_CALL vkGetDeviceImageSparseMemoryRequirements(VkDevice dev, const VkDeviceImageMemoryRequirements *info, uint32_t *count,
                                                             VkSparseImageMemoryRequirements2 *r) {
    (void)dev; (void)info; (void)r;
    *count = 0;
}

static VkResult bind_image(VkDevice dev, VkImage img, VkDeviceMemory mem, uint64_t offset) {
    (void)dev;
    img->memory = mem;
    img->memory_offset = offset;
    if (mem->external) img->mtl = mem->external;               // (external memory: the image's own texture)
    else if (img->tiling == VK_IMAGE_TILING_LINEAR) {
        if (!mem->backing) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        img->mtl = [mem->backing newTextureWithDescriptor:img->desc offset:offset bytesPerRow:img->row_pitch];
    } else {
        if (mem->backing) img->desc.storageMode = MTLStorageModeShared;
        img->mtl = [mem->heap newTextureWithDescriptor:img->desc offset:offset];
    }
    if (!img->mtl) {
        TRACE("texture %ux%u fmt %d failed at %llu", img->extent.width, img->extent.height, img->format, (unsigned long long)offset);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    return VK_SUCCESS;
}
VKFN VkResult VKAPI_CALL vkBindImageMemory(VkDevice dev, VkImage img, VkDeviceMemory mem, VkDeviceSize offset) {
    return bind_image(dev, img, mem, offset);
}
VKFN VkResult VKAPI_CALL vkBindImageMemory2(VkDevice dev, uint32_t n, const VkBindImageMemoryInfo *infos) {
    for (uint32_t i = 0; i < n; i++) {
        VkResult r = bind_image(dev, infos[i].image, infos[i].memory, infos[i].memoryOffset);
        VkBindMemoryStatusKHR *st = (VkBindMemoryStatusKHR *)vk_find_struct(infos[i].pNext, VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS_KHR);
        if (st && st->pResult) *st->pResult = r;
        if (r) return r;
    }
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkGetImageSubresourceLayout(VkDevice dev, VkImage img, const VkImageSubresource *sub, VkSubresourceLayout *l) {
    (void)dev; (void)sub;
    l->offset = 0;
    l->rowPitch = img->row_pitch ? img->row_pitch : (uint64_t)img->extent.width * img->fmt->bytes;
    l->size = l->rowPitch * img->extent.height;
    l->arrayPitch = l->size;
    l->depthPitch = l->size;
}
VKFN void VKAPI_CALL vkGetImageSubresourceLayout2KHR(VkDevice dev, VkImage img, const VkImageSubresource2KHR *sub, VkSubresourceLayout2KHR *l) {
    vkGetImageSubresourceLayout(dev, img, &sub->imageSubresource, &l->subresourceLayout);
}

// ---- views and samplers ------------------------------------------------------------------------------------------
static MTLTextureSwizzle swizzle(VkComponentSwizzle s, MTLTextureSwizzle identity) {
    switch (s) {
    case VK_COMPONENT_SWIZZLE_ZERO: return MTLTextureSwizzleZero;
    case VK_COMPONENT_SWIZZLE_ONE: return MTLTextureSwizzleOne;
    case VK_COMPONENT_SWIZZLE_R: return MTLTextureSwizzleRed;
    case VK_COMPONENT_SWIZZLE_G: return MTLTextureSwizzleGreen;
    case VK_COMPONENT_SWIZZLE_B: return MTLTextureSwizzleBlue;
    case VK_COMPONENT_SWIZZLE_A: return MTLTextureSwizzleAlpha;
    default: return identity;
    }
}
static MTLTextureType view_type(VkImageViewType t, uint32_t samples) {
    switch (t) {
    case VK_IMAGE_VIEW_TYPE_1D: return MTLTextureType1D;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return MTLTextureType1DArray;
    case VK_IMAGE_VIEW_TYPE_2D: return samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return samples > 1 ? MTLTextureType2DMultisampleArray : MTLTextureType2DArray;
    case VK_IMAGE_VIEW_TYPE_3D: return MTLTextureType3D;
    case VK_IMAGE_VIEW_TYPE_CUBE: return MTLTextureTypeCube;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return MTLTextureTypeCubeArray;
    default: return MTLTextureType2D;
    }
}

VKFN VkResult VKAPI_CALL vkCreateImageView(VkDevice dev, const VkImageViewCreateInfo *info, const VkAllocationCallbacks *alloc, VkImageView *out) {
    (void)dev; (void)alloc;
    VkImage img = info->image;
    VK_STAT(VKS_VIEWS, 1);
    struct VkImageView_T *v = calloc(1, sizeof *v);
    if (!v) return VK_ERROR_OUT_OF_HOST_MEMORY;
    v->image = img;
    v->view_type = info->viewType;
    v->format = info->format;
    v->range = info->subresourceRange;
    if (v->range.levelCount == VK_REMAINING_MIP_LEVELS) v->range.levelCount = img->mips - v->range.baseMipLevel;
    if (v->range.layerCount == VK_REMAINING_ARRAY_LAYERS) v->range.layerCount = img->layers - v->range.baseArrayLayer;
    v->width = img->extent.width >> v->range.baseMipLevel; if (!v->width) v->width = 1;
    v->height = img->extent.height >> v->range.baseMipLevel; if (!v->height) v->height = 1;
    if (!img->mtl) { *out = v; return VK_SUCCESS; }   // (bound later: views of swapchain images are made at once)
    const vk_format_info *f = vk_format(info->format);
    MTLPixelFormat pf = f ? f->mtl : img->fmt->mtl;
    int stencil_only = info->subresourceRange.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT;
    if (stencil_only && img->fmt->mtl == MTLPixelFormatDepth32Float_Stencil8) pf = MTLPixelFormatX32_Stencil8;
    if ((img->fmt->aspects & VKF_DEPTH) && !stencil_only) pf = img->fmt->mtl;   // (a depth view samples the depth)
    MTLTextureType tt = view_type(info->viewType, img->samples);
    if (img->type == VK_IMAGE_TYPE_3D && tt != MTLTextureType3D) {
        // (a 2D view of a 3D image's slices: not in Metal; the 3D texture itself)
        tt = MTLTextureType3D;
    }
    MTLTextureSwizzleChannels sw = { swizzle(info->components.r, MTLTextureSwizzleRed), swizzle(info->components.g, MTLTextureSwizzleGreen),
                                     swizzle(info->components.b, MTLTextureSwizzleBlue), swizzle(info->components.a, MTLTextureSwizzleAlpha) };
    if (img->format == VK_FORMAT_B4G4R4A4_UNORM_PACK16) {
        // (ABGR4 holds R in the high nibble: B4G4R4A4 is it with red and blue exchanged)
        MTLTextureSwizzle t = sw.red; sw.red = sw.blue; sw.blue = t;
    }
    int identity = sw.red == MTLTextureSwizzleRed && sw.green == MTLTextureSwizzleGreen && sw.blue == MTLTextureSwizzleBlue &&
                   sw.alpha == MTLTextureSwizzleAlpha;
    int whole = v->range.baseMipLevel == 0 && v->range.levelCount == img->mips && v->range.baseArrayLayer == 0 &&
                v->range.layerCount == img->layers;
    if (whole && identity && pf == img->mtl.pixelFormat && tt == img->mtl.textureType) v->mtl = img->mtl;
    else {
        NSRange levels = NSMakeRange(v->range.baseMipLevel, v->range.levelCount);
        NSRange slices = NSMakeRange(img->type == VK_IMAGE_TYPE_3D ? 0 : v->range.baseArrayLayer,
                                     img->type == VK_IMAGE_TYPE_3D ? 1 : v->range.layerCount);
        v->mtl = identity ? [img->mtl newTextureViewWithPixelFormat:pf textureType:tt levels:levels slices:slices] :
                            [img->mtl newTextureViewWithPixelFormat:pf textureType:tt levels:levels slices:slices swizzle:sw];
        if (!v->mtl) {
            TRACE("view of %d (type %lu, levels %u+%u, layers %u+%u) as %lu failed", img->format, (unsigned long)img->mtl.textureType,
                  v->range.baseMipLevel, v->range.levelCount, v->range.baseArrayLayer, v->range.layerCount, (unsigned long)tt);
            free(v);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    *out = v;
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkDestroyImageView(VkDevice dev, VkImageView v, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    if (!v) return;
    v->mtl = nil;
    v->stencil = nil;
    free(v);
}

VKFN VkResult VKAPI_CALL vkCreateBufferView(VkDevice dev, const VkBufferViewCreateInfo *info, const VkAllocationCallbacks *alloc, VkBufferView *out) {
    (void)alloc;
    const vk_format_info *f = vk_format(info->format);
    if (!f || f->mtl == MTLPixelFormatInvalid || !info->buffer->mtl) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    struct VkBufferView_T *v = calloc(1, sizeof *v);
    if (!v) return VK_ERROR_OUT_OF_HOST_MEMORY;
    v->buffer = info->buffer;
    v->format = info->format;
    v->offset = info->offset;
    v->range = info->range == VK_WHOLE_SIZE ? info->buffer->size - info->offset : info->range;
    NSUInteger elements = (NSUInteger)(v->range / f->bytes);
    MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:f->mtl width:elements ? elements : 1
                                   resourceOptions:info->buffer->mtl.resourceOptions usage:MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite];
    v->mtl = [info->buffer->mtl newTextureWithDescriptor:d offset:info->buffer->offset + info->offset bytesPerRow:(NSUInteger)v->range];
    if (!v->mtl) { TRACE("buffer view of format %d at %llu failed", info->format, (unsigned long long)info->offset); free(v); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    (void)dev;
    *out = v;
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkDestroyBufferView(VkDevice dev, VkBufferView v, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    if (!v) return;
    v->mtl = nil;
    free(v);
}

static MTLSamplerAddressMode address_mode(VkSamplerAddressMode m) {
    switch (m) {
    case VK_SAMPLER_ADDRESS_MODE_REPEAT: return MTLSamplerAddressModeRepeat;
    case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return MTLSamplerAddressModeMirrorRepeat;
    case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE: return MTLSamplerAddressModeClampToEdge;
    case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER: return MTLSamplerAddressModeClampToBorderColor;
    case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}
MTLCompareFunction vk_compare(VkCompareOp op);
MTLCompareFunction vk_compare(VkCompareOp op) {
    static const MTLCompareFunction map[] = { MTLCompareFunctionNever, MTLCompareFunctionLess, MTLCompareFunctionEqual,
        MTLCompareFunctionLessEqual, MTLCompareFunctionGreater, MTLCompareFunctionNotEqual, MTLCompareFunctionGreaterEqual,
        MTLCompareFunctionAlways };
    return (uint32_t)op < 8 ? map[op] : MTLCompareFunctionAlways;
}

VKFN VkResult VKAPI_CALL vkCreateSampler(VkDevice dev, const VkSamplerCreateInfo *info, const VkAllocationCallbacks *alloc, VkSampler *out) {
    (void)alloc;
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.minFilter = info->minFilter == VK_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.magFilter = info->magFilter == VK_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = info->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    d.sAddressMode = address_mode(info->addressModeU);
    d.tAddressMode = address_mode(info->addressModeV);
    d.rAddressMode = address_mode(info->addressModeW);
    d.maxAnisotropy = info->anisotropyEnable ? (NSUInteger)fmaxf(1, fminf(16, info->maxAnisotropy)) : 1;
    d.lodMinClamp = info->minLod;
    d.lodMaxClamp = info->maxLod;
    d.normalizedCoordinates = !info->unnormalizedCoordinates;
    if (info->unnormalizedCoordinates) { d.mipFilter = MTLSamplerMipFilterNotMipmapped; d.lodMinClamp = 0; d.lodMaxClamp = 0; }
    if (info->compareEnable) d.compareFunction = vk_compare(info->compareOp);
    switch (info->borderColor) {
    case VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK: case VK_BORDER_COLOR_INT_OPAQUE_BLACK: d.borderColor = MTLSamplerBorderColorOpaqueBlack; break;
    case VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE: case VK_BORDER_COLOR_INT_OPAQUE_WHITE: d.borderColor = MTLSamplerBorderColorOpaqueWhite; break;
    case VK_BORDER_COLOR_FLOAT_CUSTOM_EXT: case VK_BORDER_COLOR_INT_CUSTOM_EXT: {
        const VkSamplerCustomBorderColorCreateInfoEXT *c = vk_find_struct(info->pNext, VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT);
        float a = c ? c->customBorderColor.float32[3] : 0, r = c ? c->customBorderColor.float32[0] : 0;
        d.borderColor = a == 0 ? MTLSamplerBorderColorTransparentBlack : r > 0.5f ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorOpaqueBlack;
        break;
    }
    default: d.borderColor = MTLSamplerBorderColorTransparentBlack;
    }
    d.supportArgumentBuffers = YES;
    struct VkSampler_T *s = calloc(1, sizeof *s);
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->mtl = [dev->mtl newSamplerStateWithDescriptor:d];
    s->info = *info;
    s->info.pNext = NULL;
    if (!s->mtl) { free(s); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    *out = s;
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkDestroySampler(VkDevice dev, VkSampler s, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    if (!s) return;
    s->mtl = nil;
    free(s);
}

// Private data (VK_EXT_private_data): a small table per device would be the full answer; slots keep one value per
// object here, which is what applications use them for.
struct VkPrivateDataSlot_T { pthread_mutex_t lock; uint64_t *keys, *values; uint32_t count, cap; };
VKFN VkResult VKAPI_CALL vkCreatePrivateDataSlot(VkDevice dev, const VkPrivateDataSlotCreateInfo *info, const VkAllocationCallbacks *alloc, VkPrivateDataSlot *out) {
    (void)dev; (void)info; (void)alloc;
    struct VkPrivateDataSlot_T *s = calloc(1, sizeof *s);
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pthread_mutex_init(&s->lock, NULL);
    *out = s;
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkDestroyPrivateDataSlot(VkDevice dev, VkPrivateDataSlot s, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    if (!s) return;
    free(s->keys); free(s->values);
    pthread_mutex_destroy(&s->lock);
    free(s);
}
VKFN VkResult VKAPI_CALL vkSetPrivateData(VkDevice dev, VkObjectType type, uint64_t object, VkPrivateDataSlot s, uint64_t data) {
    (void)dev; (void)type;
    pthread_mutex_lock(&s->lock);
    for (uint32_t i = 0; i < s->count; i++) if (s->keys[i] == object) { s->values[i] = data; pthread_mutex_unlock(&s->lock); return VK_SUCCESS; }
    if (s->count == s->cap) {
        uint32_t cap = s->cap ? s->cap * 2 : 16;
        s->keys = realloc(s->keys, cap * sizeof *s->keys);
        s->values = realloc(s->values, cap * sizeof *s->values);
        s->cap = cap;
    }
    s->keys[s->count] = object;
    s->values[s->count++] = data;
    pthread_mutex_unlock(&s->lock);
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkGetPrivateData(VkDevice dev, VkObjectType type, uint64_t object, VkPrivateDataSlot s, uint64_t *data) {
    (void)dev; (void)type;
    *data = 0;
    pthread_mutex_lock(&s->lock);
    for (uint32_t i = 0; i < s->count; i++) if (s->keys[i] == object) { *data = s->values[i]; break; }
    pthread_mutex_unlock(&s->lock);
}

#ifdef __aarch64__
#include "vx.h"   // (vx_low, vx_unlow_at for the thunks)
#include "thunks/vk_device.m.inc"
#endif
