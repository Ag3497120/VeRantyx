// Vulkan 1.0 ABI subset used by vxrt's built-in vulkan-1.dll frontend.
// These are public Khronos ABI layouts for currently implemented commands; this is intentionally
// not a replacement for the Vulkan SDK headers.
#ifndef VX_VULKAN_H
#define VX_VULKAN_H

#include "vx.h"

typedef int32_t VkResult;
typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;
#define VK_WHOLE_SIZE (~(VkDeviceSize)0)
typedef uint32_t VkInstanceCreateFlags;
typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef struct VkDevice_T *VkDevice;
typedef struct VkQueue_T *VkQueue;
typedef struct VkCommandBuffer_T *VkCommandBuffer;
// Vulkan non-dispatchable handles are 64-bit values on this x86-64 runtime.
typedef uint64_t VkDeviceMemory;
typedef uint64_t VkBuffer;
typedef uint64_t VkImage;
typedef uint64_t VkImageView;
typedef uint64_t VkShaderModule;
typedef uint64_t VkDescriptorSetLayout;
typedef uint64_t VkDescriptorPool;
typedef uint64_t VkDescriptorSet;
typedef uint64_t VkPipelineLayout;
typedef uint64_t VkPipeline;
typedef uint64_t VkPipelineCache;
typedef uint64_t VkSampler;
typedef uint64_t VkCommandPool;
typedef uint64_t VkFence;
typedef uint64_t VkSemaphore;
typedef int32_t VkStructureType;
typedef int32_t VkSystemAllocationScope;
typedef int32_t VkInternalAllocationType;
typedef uint32_t VkSampleCountFlags;
typedef uint32_t VkQueueFlags;
typedef uint32_t VkSharingMode;
typedef uint32_t VkMemoryPropertyFlags;
typedef uint32_t VkMemoryHeapFlags;
typedef uint32_t VkBufferUsageFlags;
typedef uint32_t VkBufferCreateFlags;
typedef int32_t VkFormat;
typedef uint32_t VkImageType;
typedef uint32_t VkImageTiling;
typedef uint32_t VkImageUsageFlags;
typedef uint32_t VkImageCreateFlags;
typedef uint32_t VkImageViewCreateFlags;
typedef uint32_t VkImageViewType;
typedef uint32_t VkComponentSwizzle;
typedef int32_t VkImageLayout;
typedef uint32_t VkImageAspectFlags;
typedef uint32_t VkCommandPoolCreateFlags;
typedef uint32_t VkCommandBufferUsageFlags;
typedef uint32_t VkCommandBufferResetFlags;
typedef uint32_t VkCommandBufferLevel;
typedef uint32_t VkFenceCreateFlags;
typedef uint32_t VkPipelineStageFlags;
typedef uint32_t VkAccessFlags;
typedef uint32_t VkDependencyFlags;
typedef uint32_t VkShaderStageFlags;
typedef uint32_t VkPipelineCreateFlags;
typedef uint32_t VkPipelineShaderStageCreateFlags;
typedef uint32_t VkPipelineBindPoint;
typedef uint32_t VkDescriptorType;
typedef uint32_t VkDescriptorSetLayoutCreateFlags;
typedef uint32_t VkDescriptorPoolCreateFlags;
typedef uint32_t VkDescriptorPoolResetFlags;
typedef uint32_t VkSamplerCreateFlags;
typedef uint32_t VkFilter;
typedef uint32_t VkSamplerMipmapMode;
typedef uint32_t VkSamplerAddressMode;
typedef int32_t VkCompareOp;
typedef uint32_t VkBorderColor;

typedef void (WINAPI *PFN_vkVoidFunction)(void);
typedef void *(WINAPI *PFN_vkAllocationFunction)(void *pUserData, size_t size, size_t alignment,
                                                 VkSystemAllocationScope allocationScope);
typedef void *(WINAPI *PFN_vkReallocationFunction)(void *pUserData, void *pOriginal, size_t size,
                                                   size_t alignment, VkSystemAllocationScope allocationScope);
typedef void (WINAPI *PFN_vkFreeFunction)(void *pUserData, void *pMemory);
typedef void (WINAPI *PFN_vkInternalAllocationNotification)(void *pUserData, size_t size,
                                                             VkInternalAllocationType allocationType,
                                                             VkSystemAllocationScope allocationScope);
typedef void (WINAPI *PFN_vkInternalFreeNotification)(void *pUserData, size_t size,
                                                       VkInternalAllocationType allocationType,
                                                       VkSystemAllocationScope allocationScope);

typedef struct VkAllocationCallbacks {
    void *pUserData;
    PFN_vkAllocationFunction pfnAllocation;
    PFN_vkReallocationFunction pfnReallocation;
    PFN_vkFreeFunction pfnFree;
    PFN_vkInternalAllocationNotification pfnInternalAllocation;
    PFN_vkInternalFreeNotification pfnInternalFree;
} VkAllocationCallbacks;

typedef struct VkApplicationInfo {
    VkStructureType sType;
    const void *pNext;
    const char *pApplicationName;
    uint32_t applicationVersion;
    const char *pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
} VkApplicationInfo;

typedef struct VkInstanceCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkInstanceCreateFlags flags;
    const VkApplicationInfo *pApplicationInfo;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct VkExtensionProperties {
    char extensionName[256];
    uint32_t specVersion;
} VkExtensionProperties;

typedef struct VkLayerProperties {
    char layerName[256];
    uint32_t specVersion;
    uint32_t implementationVersion;
    char description[256];
} VkLayerProperties;

// Public Vulkan 1.0 ABI layouts used by the compute-only physical-device frontend.
typedef struct VkExtent3D { uint32_t width, height, depth; } VkExtent3D;
typedef struct VkPhysicalDeviceFeatures {
    VkBool32 robustBufferAccess;
    VkBool32 fullDrawIndexUint32;
    VkBool32 imageCubeArray;
    VkBool32 independentBlend;
    VkBool32 geometryShader;
    VkBool32 tessellationShader;
    VkBool32 sampleRateShading;
    VkBool32 dualSrcBlend;
    VkBool32 logicOp;
    VkBool32 multiDrawIndirect;
    VkBool32 drawIndirectFirstInstance;
    VkBool32 depthClamp;
    VkBool32 depthBiasClamp;
    VkBool32 fillModeNonSolid;
    VkBool32 depthBounds;
    VkBool32 wideLines;
    VkBool32 largePoints;
    VkBool32 alphaToOne;
    VkBool32 multiViewport;
    VkBool32 samplerAnisotropy;
    VkBool32 textureCompressionETC2;
    VkBool32 textureCompressionASTC_LDR;
    VkBool32 textureCompressionBC;
    VkBool32 occlusionQueryPrecise;
    VkBool32 pipelineStatisticsQuery;
    VkBool32 vertexPipelineStoresAndAtomics;
    VkBool32 fragmentStoresAndAtomics;
    VkBool32 shaderTessellationAndGeometryPointSize;
    VkBool32 shaderImageGatherExtended;
    VkBool32 shaderStorageImageExtendedFormats;
    VkBool32 shaderStorageImageMultisample;
    VkBool32 shaderStorageImageReadWithoutFormat;
    VkBool32 shaderStorageImageWriteWithoutFormat;
    VkBool32 shaderUniformBufferArrayDynamicIndexing;
    VkBool32 shaderSampledImageArrayDynamicIndexing;
    VkBool32 shaderStorageBufferArrayDynamicIndexing;
    VkBool32 shaderStorageImageArrayDynamicIndexing;
    VkBool32 shaderClipDistance;
    VkBool32 shaderCullDistance;
    VkBool32 shaderFloat64;
    VkBool32 shaderInt64;
    VkBool32 shaderInt16;
    VkBool32 shaderResourceResidency;
    VkBool32 shaderResourceMinLod;
    VkBool32 sparseBinding;
    VkBool32 sparseResidencyBuffer;
    VkBool32 sparseResidencyImage2D;
    VkBool32 sparseResidencyImage3D;
    VkBool32 sparseResidency2Samples;
    VkBool32 sparseResidency4Samples;
    VkBool32 sparseResidency8Samples;
    VkBool32 sparseResidency16Samples;
    VkBool32 sparseResidencyAliased;
    VkBool32 variableMultisampleRate;
    VkBool32 inheritedQueries;
} VkPhysicalDeviceFeatures;

typedef struct VkPhysicalDeviceLimits {
    uint32_t maxImageDimension1D, maxImageDimension2D, maxImageDimension3D, maxImageDimensionCube;
    uint32_t maxImageArrayLayers, maxTexelBufferElements, maxUniformBufferRange, maxStorageBufferRange;
    uint32_t maxPushConstantsSize, maxMemoryAllocationCount, maxSamplerAllocationCount;
    VkDeviceSize bufferImageGranularity, sparseAddressSpaceSize;
    uint32_t maxBoundDescriptorSets, maxPerStageDescriptorSamplers, maxPerStageDescriptorUniformBuffers;
    uint32_t maxPerStageDescriptorStorageBuffers, maxPerStageDescriptorSampledImages, maxPerStageDescriptorStorageImages;
    uint32_t maxPerStageDescriptorInputAttachments, maxPerStageResources, maxDescriptorSetSamplers;
    uint32_t maxDescriptorSetUniformBuffers, maxDescriptorSetUniformBuffersDynamic;
    uint32_t maxDescriptorSetStorageBuffers, maxDescriptorSetStorageBuffersDynamic;
    uint32_t maxDescriptorSetSampledImages, maxDescriptorSetStorageImages, maxDescriptorSetInputAttachments;
    uint32_t maxVertexInputAttributes, maxVertexInputBindings, maxVertexInputAttributeOffset, maxVertexInputBindingStride;
    uint32_t maxVertexOutputComponents, maxTessellationGenerationLevel, maxTessellationPatchSize;
    uint32_t maxTessellationControlPerVertexInputComponents, maxTessellationControlPerVertexOutputComponents;
    uint32_t maxTessellationControlPerPatchOutputComponents, maxTessellationControlTotalOutputComponents;
    uint32_t maxTessellationEvaluationInputComponents, maxTessellationEvaluationOutputComponents;
    uint32_t maxGeometryShaderInvocations, maxGeometryInputComponents, maxGeometryOutputComponents;
    uint32_t maxGeometryOutputVertices, maxGeometryTotalOutputComponents, maxFragmentInputComponents;
    uint32_t maxFragmentOutputAttachments, maxFragmentDualSrcAttachments, maxFragmentCombinedOutputResources;
    uint32_t maxComputeSharedMemorySize, maxComputeWorkGroupCount[3], maxComputeWorkGroupInvocations;
    uint32_t maxComputeWorkGroupSize[3], subPixelPrecisionBits, subTexelPrecisionBits, mipmapPrecisionBits;
    uint32_t maxDrawIndexedIndexValue, maxDrawIndirectCount;
    float maxSamplerLodBias, maxSamplerAnisotropy;
    uint32_t maxViewports, maxViewportDimensions[2];
    float viewportBoundsRange[2];
    uint32_t viewportSubPixelBits;
    size_t minMemoryMapAlignment;
    VkDeviceSize minTexelBufferOffsetAlignment, minUniformBufferOffsetAlignment, minStorageBufferOffsetAlignment;
    int32_t minTexelOffset;
    uint32_t maxTexelOffset;
    int32_t minTexelGatherOffset;
    uint32_t maxTexelGatherOffset;
    float minInterpolationOffset, maxInterpolationOffset;
    uint32_t subPixelInterpolationOffsetBits, maxFramebufferWidth, maxFramebufferHeight, maxFramebufferLayers;
    VkSampleCountFlags framebufferColorSampleCounts, framebufferDepthSampleCounts, framebufferStencilSampleCounts;
    VkSampleCountFlags framebufferNoAttachmentsSampleCounts;
    uint32_t maxColorAttachments;
    VkSampleCountFlags sampledImageColorSampleCounts, sampledImageIntegerSampleCounts, sampledImageDepthSampleCounts;
    VkSampleCountFlags sampledImageStencilSampleCounts, storageImageSampleCounts;
    uint32_t maxSampleMaskWords;
    VkBool32 timestampComputeAndGraphics;
    float timestampPeriod;
    uint32_t maxClipDistances, maxCullDistances, maxCombinedClipAndCullDistances, discreteQueuePriorities;
    float pointSizeRange[2], lineWidthRange[2], pointSizeGranularity, lineWidthGranularity;
    VkBool32 strictLines, standardSampleLocations;
    VkDeviceSize optimalBufferCopyOffsetAlignment, optimalBufferCopyRowPitchAlignment, nonCoherentAtomSize;
} VkPhysicalDeviceLimits;

typedef struct VkPhysicalDeviceSparseProperties {
    VkBool32 residencyStandard2DBlockShape;
    VkBool32 residencyStandard2DMultisampleBlockShape;
    VkBool32 residencyStandard3DBlockShape;
    VkBool32 residencyAlignedMipSize;
    VkBool32 residencyNonResidentStrict;
} VkPhysicalDeviceSparseProperties;

typedef struct VkPhysicalDeviceProperties {
    uint32_t apiVersion, driverVersion, vendorID, deviceID;
    int32_t deviceType;
    char deviceName[256];
    uint8_t pipelineCacheUUID[16];
    VkPhysicalDeviceLimits limits;
    VkPhysicalDeviceSparseProperties sparseProperties;
} VkPhysicalDeviceProperties;

typedef struct VkQueueFamilyProperties {
    VkQueueFlags queueFlags;
    uint32_t queueCount;
    uint32_t timestampValidBits;
    VkExtent3D minImageTransferGranularity;
} VkQueueFamilyProperties;

typedef struct VkMemoryType { VkMemoryPropertyFlags propertyFlags; uint32_t heapIndex; } VkMemoryType;
typedef struct VkMemoryHeap { VkDeviceSize size; VkMemoryHeapFlags flags; } VkMemoryHeap;
typedef struct VkPhysicalDeviceMemoryProperties {
    uint32_t memoryTypeCount;
    VkMemoryType memoryTypes[32];
    uint32_t memoryHeapCount;
    VkMemoryHeap memoryHeaps[16];
} VkPhysicalDeviceMemoryProperties;

typedef struct VkDeviceQueueCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    uint32_t queueFamilyIndex, queueCount;
    const float *pQueuePriorities;
} VkDeviceQueueCreateInfo;

typedef struct VkDeviceCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    uint32_t queueCreateInfoCount;
    const VkDeviceQueueCreateInfo *pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
    const VkPhysicalDeviceFeatures *pEnabledFeatures;
} VkDeviceCreateInfo;

typedef struct VkMemoryRequirements {
    VkDeviceSize size, alignment;
    uint32_t memoryTypeBits;
} VkMemoryRequirements;

typedef struct VkMemoryAllocateInfo {
    VkStructureType sType;
    const void *pNext;
    VkDeviceSize allocationSize;
    uint32_t memoryTypeIndex;
} VkMemoryAllocateInfo;

typedef struct VkMappedMemoryRange {
    VkStructureType sType;
    const void *pNext;
    VkDeviceMemory memory;
    VkDeviceSize offset;
    VkDeviceSize size;
} VkMappedMemoryRange;

typedef struct VkBufferCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkBufferCreateFlags flags;
    VkDeviceSize size;
    VkBufferUsageFlags usage;
    VkSharingMode sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
} VkBufferCreateInfo;

typedef struct VkImageCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkImageCreateFlags flags;
    VkImageType imageType;
    VkFormat format;
    VkExtent3D extent;
    uint32_t mipLevels, arrayLayers;
    VkSampleCountFlags samples;
    VkImageTiling tiling;
    VkImageUsageFlags usage;
    VkSharingMode sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
    VkImageLayout initialLayout;
} VkImageCreateInfo;

typedef struct VkComponentMapping {
    VkComponentSwizzle r, g, b, a;
} VkComponentMapping;

typedef struct VkImageSubresource { VkImageAspectFlags aspectMask; uint32_t mipLevel, arrayLayer; } VkImageSubresource;
typedef struct VkImageSubresourceLayers {
    VkImageAspectFlags aspectMask;
    uint32_t mipLevel, baseArrayLayer, layerCount;
} VkImageSubresourceLayers;
typedef struct VkImageSubresourceRange {
    VkImageAspectFlags aspectMask;
    uint32_t baseMipLevel, levelCount, baseArrayLayer, layerCount;
} VkImageSubresourceRange;
typedef struct VkImageViewCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkImageViewCreateFlags flags;
    VkImage image;
    VkImageViewType viewType;
    VkFormat format;
    VkComponentMapping components;
    VkImageSubresourceRange subresourceRange;
} VkImageViewCreateInfo;
typedef struct VkSamplerCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkSamplerCreateFlags flags;
    VkFilter magFilter, minFilter;
    VkSamplerMipmapMode mipmapMode;
    VkSamplerAddressMode addressModeU, addressModeV, addressModeW;
    float mipLodBias;
    VkBool32 anisotropyEnable;
    float maxAnisotropy;
    VkBool32 compareEnable;
    VkCompareOp compareOp;
    float minLod, maxLod;
    VkBorderColor borderColor;
    VkBool32 unnormalizedCoordinates;
} VkSamplerCreateInfo;
typedef union VkClearColorValue {
    float float32[4];
    int32_t int32[4];
    uint32_t uint32[4];
} VkClearColorValue;
typedef struct VkSubresourceLayout {
    VkDeviceSize offset, size, rowPitch, arrayPitch, depthPitch;
} VkSubresourceLayout;
typedef struct VkOffset3D { int32_t x, y, z; } VkOffset3D;
typedef struct VkBufferImageCopy {
    VkDeviceSize bufferOffset;
    uint32_t bufferRowLength, bufferImageHeight;
    VkImageSubresourceLayers imageSubresource;
    VkOffset3D imageOffset;
    VkExtent3D imageExtent;
} VkBufferImageCopy;
typedef struct VkImageCopy {
    VkImageSubresourceLayers srcSubresource;
    VkOffset3D srcOffset;
    VkImageSubresourceLayers dstSubresource;
    VkOffset3D dstOffset;
    VkExtent3D extent;
} VkImageCopy;
typedef struct VkImageMemoryBarrier {
    VkStructureType sType;
    const void *pNext;
    VkAccessFlags srcAccessMask, dstAccessMask;
    VkImageLayout oldLayout, newLayout;
    uint32_t srcQueueFamilyIndex, dstQueueFamilyIndex;
    VkImage image;
    VkImageSubresourceRange subresourceRange;
} VkImageMemoryBarrier;
typedef struct VkFormatProperties {
    uint32_t linearTilingFeatures, optimalTilingFeatures, bufferFeatures;
} VkFormatProperties;
typedef struct VkImageFormatProperties {
    VkExtent3D maxExtent;
    uint32_t maxMipLevels, maxArrayLayers;
    VkSampleCountFlags sampleCounts;
    VkDeviceSize maxResourceSize;
} VkImageFormatProperties;

typedef struct VkCommandPoolCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkCommandPoolCreateFlags flags;
    uint32_t queueFamilyIndex;
} VkCommandPoolCreateInfo;

typedef struct VkCommandBufferAllocateInfo {
    VkStructureType sType;
    const void *pNext;
    VkCommandPool commandPool;
    VkCommandBufferLevel level;
    uint32_t commandBufferCount;
} VkCommandBufferAllocateInfo;

typedef struct VkCommandBufferBeginInfo {
    VkStructureType sType;
    const void *pNext;
    VkCommandBufferUsageFlags flags;
    const void *pInheritanceInfo;
} VkCommandBufferBeginInfo;

typedef struct VkBufferCopy {
    VkDeviceSize srcOffset, dstOffset, size;
} VkBufferCopy;

typedef struct VkDispatchIndirectCommand {
    uint32_t x, y, z;
} VkDispatchIndirectCommand;

typedef struct VkShaderModuleCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    size_t codeSize;
    const uint32_t *pCode;
} VkShaderModuleCreateInfo;

typedef struct VkDescriptorSetLayoutBinding {
    uint32_t binding;
    VkDescriptorType descriptorType;
    uint32_t descriptorCount;
    VkShaderStageFlags stageFlags;
    const VkSampler *pImmutableSamplers;
} VkDescriptorSetLayoutBinding;

typedef struct VkDescriptorSetLayoutCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkDescriptorSetLayoutCreateFlags flags;
    uint32_t bindingCount;
    const VkDescriptorSetLayoutBinding *pBindings;
} VkDescriptorSetLayoutCreateInfo;

typedef struct VkPushConstantRange {
    VkShaderStageFlags stageFlags;
    uint32_t offset;
    uint32_t size;
} VkPushConstantRange;

typedef struct VkPipelineLayoutCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    uint32_t setLayoutCount;
    const VkDescriptorSetLayout *pSetLayouts;
    uint32_t pushConstantRangeCount;
    const VkPushConstantRange *pPushConstantRanges;
} VkPipelineLayoutCreateInfo;

typedef struct VkSpecializationInfo {
    uint32_t mapEntryCount;
    const void *pMapEntries;
    size_t dataSize;
    const void *pData;
} VkSpecializationInfo;

typedef struct VkSpecializationMapEntry {
    uint32_t constantID;
    uint32_t offset;
    size_t size;
} VkSpecializationMapEntry;

typedef struct VkPipelineShaderStageCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkPipelineShaderStageCreateFlags flags;
    VkShaderStageFlags stage;
    VkShaderModule module;
    const char *pName;
    const VkSpecializationInfo *pSpecializationInfo;
} VkPipelineShaderStageCreateInfo;

typedef struct VkComputePipelineCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkPipelineCreateFlags flags;
    VkPipelineShaderStageCreateInfo stage;
    VkPipelineLayout layout;
    VkPipeline basePipelineHandle;
    int32_t basePipelineIndex;
} VkComputePipelineCreateInfo;

typedef struct VkDescriptorPoolSize {
    VkDescriptorType type;
    uint32_t descriptorCount;
} VkDescriptorPoolSize;

typedef struct VkDescriptorPoolCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkDescriptorPoolCreateFlags flags;
    uint32_t maxSets;
    uint32_t poolSizeCount;
    const VkDescriptorPoolSize *pPoolSizes;
} VkDescriptorPoolCreateInfo;

typedef struct VkDescriptorSetAllocateInfo {
    VkStructureType sType;
    const void *pNext;
    VkDescriptorPool descriptorPool;
    uint32_t descriptorSetCount;
    const VkDescriptorSetLayout *pSetLayouts;
} VkDescriptorSetAllocateInfo;

typedef struct VkDescriptorBufferInfo {
    VkBuffer buffer;
    VkDeviceSize offset;
    VkDeviceSize range;
} VkDescriptorBufferInfo;

typedef struct VkDescriptorImageInfo {
    VkSampler sampler;
    VkImageView imageView;
    VkImageLayout imageLayout;
} VkDescriptorImageInfo;

typedef struct VkWriteDescriptorSet {
    VkStructureType sType;
    const void *pNext;
    VkDescriptorSet dstSet;
    uint32_t dstBinding;
    uint32_t dstArrayElement;
    uint32_t descriptorCount;
    VkDescriptorType descriptorType;
    const VkDescriptorImageInfo *pImageInfo;
    const VkDescriptorBufferInfo *pBufferInfo;
    const void *pTexelBufferView;
} VkWriteDescriptorSet;

typedef struct VkCopyDescriptorSet {
    VkStructureType sType;
    const void *pNext;
    VkDescriptorSet srcSet;
    uint32_t srcBinding;
    uint32_t srcArrayElement;
    VkDescriptorSet dstSet;
    uint32_t dstBinding;
    uint32_t dstArrayElement;
    uint32_t descriptorCount;
} VkCopyDescriptorSet;

typedef struct VkSubmitInfo {
    VkStructureType sType;
    const void *pNext;
    uint32_t waitSemaphoreCount;
    const VkSemaphore *pWaitSemaphores;
    const VkPipelineStageFlags *pWaitDstStageMask;
    uint32_t commandBufferCount;
    const VkCommandBuffer *pCommandBuffers;
    uint32_t signalSemaphoreCount;
    const VkSemaphore *pSignalSemaphores;
} VkSubmitInfo;

typedef struct VkFenceCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkFenceCreateFlags flags;
} VkFenceCreateInfo;

typedef struct VkSemaphoreCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
} VkSemaphoreCreateInfo;

typedef struct VkMemoryBarrier {
    VkStructureType sType;
    const void *pNext;
    VkAccessFlags srcAccessMask, dstAccessMask;
} VkMemoryBarrier;

typedef struct VkBufferMemoryBarrier {
    VkStructureType sType;
    const void *pNext;
    VkAccessFlags srcAccessMask, dstAccessMask;
    uint32_t srcQueueFamilyIndex, dstQueueFamilyIndex;
    VkBuffer buffer;
    VkDeviceSize offset, size;
} VkBufferMemoryBarrier;

enum {
    VK_PHYSICAL_DEVICE_TYPE_OTHER = 0,
    VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1,
    VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2,
    VK_QUEUE_GRAPHICS_BIT = 1u << 0,
    VK_QUEUE_COMPUTE_BIT = 1u << 1,
    VK_QUEUE_TRANSFER_BIT = 1u << 2,
    VK_SHARING_MODE_EXCLUSIVE = 0,
    VK_SHARING_MODE_CONCURRENT = 1,
    VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0,
    VK_COMMAND_BUFFER_LEVEL_SECONDARY = 1,
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 1u << 0,
    VK_BUFFER_USAGE_TRANSFER_DST_BIT = 1u << 1,
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT = 1u << 2,
    VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT = 1u << 3,
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT = 1u << 4,
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT = 1u << 5,
    VK_BUFFER_USAGE_INDEX_BUFFER_BIT = 1u << 6,
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT = 1u << 7,
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT = 1u << 8,
    VK_IMAGE_TYPE_1D = 0,
    VK_IMAGE_TYPE_2D = 1,
    VK_IMAGE_TYPE_3D = 2,
    VK_IMAGE_VIEW_TYPE_1D = 0,
    VK_IMAGE_VIEW_TYPE_2D = 1,
    VK_IMAGE_VIEW_TYPE_3D = 2,
    VK_IMAGE_VIEW_TYPE_CUBE = 3,
    VK_IMAGE_VIEW_TYPE_1D_ARRAY = 4,
    VK_IMAGE_VIEW_TYPE_2D_ARRAY = 5,
    VK_IMAGE_VIEW_TYPE_CUBE_ARRAY = 6,
    VK_COMPONENT_SWIZZLE_IDENTITY = 0,
    VK_COMPONENT_SWIZZLE_ZERO = 1,
    VK_COMPONENT_SWIZZLE_ONE = 2,
    VK_COMPONENT_SWIZZLE_R = 3,
    VK_COMPONENT_SWIZZLE_G = 4,
    VK_COMPONENT_SWIZZLE_B = 5,
    VK_COMPONENT_SWIZZLE_A = 6,
    VK_IMAGE_TILING_OPTIMAL = 0,
    VK_IMAGE_TILING_LINEAR = 1,
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 1u << 0,
    VK_IMAGE_USAGE_TRANSFER_DST_BIT = 1u << 1,
    VK_IMAGE_USAGE_SAMPLED_BIT = 1u << 2,
    VK_IMAGE_USAGE_STORAGE_BIT = 1u << 3,
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 1u << 4,
    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT = 1u << 5,
    VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT = 1u << 6,
    VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT = 1u << 7,
    VK_IMAGE_ASPECT_COLOR_BIT = 1u << 0,
    VK_IMAGE_ASPECT_DEPTH_BIT = 1u << 1,
    VK_IMAGE_ASPECT_STENCIL_BIT = 1u << 2,
    VK_FORMAT_UNDEFINED = 0,
    VK_FORMAT_R8_UNORM = 9,
    VK_FORMAT_R8G8_UNORM = 16,
    VK_FORMAT_R8G8B8A8_UNORM = 37,
    VK_FORMAT_B8G8R8A8_UNORM = 44,
    VK_FORMAT_R16G16B16A16_SFLOAT = 97,
    VK_FORMAT_R32_SFLOAT = 100,
    VK_IMAGE_LAYOUT_UNDEFINED = 0,
    VK_IMAGE_LAYOUT_GENERAL = 1,
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2,
    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL = 3,
    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL = 4,
    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL = 5,
    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 6,
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 7,
    VK_IMAGE_LAYOUT_PREINITIALIZED = 8,
    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR = 1000001002,
    VK_REMAINING_MIP_LEVELS = 0xffffffffu,
    VK_REMAINING_ARRAY_LAYERS = 0xffffffffu,
    VK_SHADER_STAGE_COMPUTE_BIT = 1u << 5,
    VK_PIPELINE_BIND_POINT_COMPUTE = 1,
    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER = 6,
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER = 7,
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC = 9,
    VK_DESCRIPTOR_TYPE_SAMPLER = 0,
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 1,
    VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE = 2,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE = 3,
    VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER = 4,
    VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER = 5,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT = 1u << 0,
    VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT = 1u << 1,
    VK_FILTER_NEAREST = 0,
    VK_FILTER_LINEAR = 1,
    VK_COMPARE_OP_NEVER = 0,
    VK_COMPARE_OP_LESS = 1,
    VK_COMPARE_OP_EQUAL = 2,
    VK_COMPARE_OP_LESS_OR_EQUAL = 3,
    VK_COMPARE_OP_GREATER = 4,
    VK_COMPARE_OP_NOT_EQUAL = 5,
    VK_COMPARE_OP_GREATER_OR_EQUAL = 6,
    VK_COMPARE_OP_ALWAYS = 7,
    VK_SAMPLER_MIPMAP_MODE_NEAREST = 0,
    VK_SAMPLER_MIPMAP_MODE_LINEAR = 1,
    VK_SAMPLER_ADDRESS_MODE_REPEAT = 0,
    VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT = 1,
    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE = 2,
    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER = 3,
    VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE = 4,
    VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK = 0,
    VK_BORDER_COLOR_INT_TRANSPARENT_BLACK = 1,
    VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK = 2,
    VK_BORDER_COLOR_INT_OPAQUE_BLACK = 3,
    VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE = 4,
    VK_BORDER_COLOR_INT_OPAQUE_WHITE = 5,
    VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT = 1u << 0,
    VK_COMMAND_POOL_CREATE_TRANSIENT_BIT = 1u << 0,
    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT = 1u << 1,
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 1u << 0,
    VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT = 1u << 1,
    VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT = 1u << 2,
    VK_FENCE_CREATE_SIGNALED_BIT = 1u << 0,
    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT = 1u << 0,
    VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT = 1u << 1,
    VK_PIPELINE_STAGE_VERTEX_INPUT_BIT = 1u << 2,
    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT = 1u << 3,
    VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT = 1u << 4,
    VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT = 1u << 5,
    VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT = 1u << 6,
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT = 1u << 7,
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT = 1u << 8,
    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT = 1u << 9,
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT = 1u << 10,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT = 1u << 11,
    VK_PIPELINE_STAGE_TRANSFER_BIT = 1u << 12,
    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT = 1u << 13,
    VK_PIPELINE_STAGE_HOST_BIT = 1u << 14,
    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT = 1u << 15,
    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT = 1u << 16,
    VK_ACCESS_INDIRECT_COMMAND_READ_BIT = 1u << 0,
    VK_ACCESS_INDEX_READ_BIT = 1u << 1,
    VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT = 1u << 2,
    VK_ACCESS_UNIFORM_READ_BIT = 1u << 3,
    VK_ACCESS_INPUT_ATTACHMENT_READ_BIT = 1u << 4,
    VK_ACCESS_SHADER_READ_BIT = 1u << 5,
    VK_ACCESS_SHADER_WRITE_BIT = 1u << 6,
    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT = 1u << 7,
    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT = 1u << 8,
    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT = 1u << 9,
    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT = 1u << 10,
    VK_ACCESS_TRANSFER_READ_BIT = 1u << 11,
    VK_ACCESS_TRANSFER_WRITE_BIT = 1u << 12,
    VK_ACCESS_HOST_READ_BIT = 1u << 13,
    VK_ACCESS_HOST_WRITE_BIT = 1u << 14,
    VK_ACCESS_MEMORY_READ_BIT = 1u << 15,
    VK_ACCESS_MEMORY_WRITE_BIT = 1u << 16,
    VK_DEPENDENCY_BY_REGION_BIT = 1u << 0,
    VK_QUEUE_FAMILY_IGNORED = 0xffffffffu,
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 1u << 0,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 1u << 1,
    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 1u << 2,
    VK_MEMORY_PROPERTY_HOST_CACHED_BIT = 1u << 3,
    VK_MEMORY_HEAP_DEVICE_LOCAL_BIT = 1u << 0,
    VK_SAMPLE_COUNT_1_BIT = 1u << 0
};

enum {
    VK_SUCCESS = 0,
    VK_INCOMPLETE = 5,
    VK_ERROR_OUT_OF_HOST_MEMORY = -1,
    VK_ERROR_OUT_OF_DEVICE_MEMORY = -2,
    VK_ERROR_INITIALIZATION_FAILED = -3,
    VK_ERROR_DEVICE_LOST = -4,
    VK_ERROR_MEMORY_MAP_FAILED = -5,
    VK_ERROR_LAYER_NOT_PRESENT = -6,
    VK_ERROR_EXTENSION_NOT_PRESENT = -7,
    VK_ERROR_FEATURE_NOT_PRESENT = -8,
    VK_ERROR_INCOMPATIBLE_DRIVER = -9,
    VK_ERROR_TOO_MANY_OBJECTS = -10,
    VK_ERROR_FORMAT_NOT_SUPPORTED = -11,
    VK_ERROR_UNKNOWN = -13,
    VK_NOT_READY = 1,
    VK_TIMEOUT = 2,
    VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
    VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2,
    VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3,
    VK_STRUCTURE_TYPE_SUBMIT_INFO = 4,
    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5,
    VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE = 6,
    VK_STRUCTURE_TYPE_FENCE_CREATE_INFO = 8,
    VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO = 9,
    VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12,
    VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14,
    VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO = 15,
    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16,
    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO = 18,
    VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO = 29,
    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO = 30,
    VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO = 31,
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32,
    VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO = 33,
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO = 34,
    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET = 35,
    VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET = 36,
    VK_STRUCTURE_TYPE_MEMORY_BARRIER = 46,
    VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER = 44,
    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER = 45,
    VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39,
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40,
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42,
    VK_SYSTEM_ALLOCATION_SCOPE_COMMAND = 0,
    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT = 1,
    VK_SYSTEM_ALLOCATION_SCOPE_DEVICE = 3,
    VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE = 4,
    VK_API_VERSION_1_0 = (1u << 22)
};

typedef VkResult (WINAPI *PFN_vkEnumerateInstanceVersion)(uint32_t *pApiVersion);
typedef VkResult (WINAPI *PFN_vkEnumerateInstanceExtensionProperties)(const char *pLayerName,
                                                                       uint32_t *pPropertyCount,
                                                                       VkExtensionProperties *pProperties);
typedef VkResult (WINAPI *PFN_vkEnumerateInstanceLayerProperties)(uint32_t *pPropertyCount,
                                                                   VkLayerProperties *pProperties);
typedef VkResult (WINAPI *PFN_vkEnumerateDeviceExtensionProperties)(VkPhysicalDevice physicalDevice,
    const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties);
typedef VkResult (WINAPI *PFN_vkEnumerateDeviceLayerProperties)(VkPhysicalDevice physicalDevice,
    uint32_t *pPropertyCount, VkLayerProperties *pProperties);
typedef VkResult (WINAPI *PFN_vkCreateInstance)(const VkInstanceCreateInfo *pCreateInfo,
                                                 const VkAllocationCallbacks *pAllocator,
                                                 VkInstance *pInstance);
typedef VkResult (WINAPI *PFN_vkCreateSampler)(VkDevice device, const VkSamplerCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSampler *pSampler);
typedef void (WINAPI *PFN_vkDestroySampler)(VkDevice device, VkSampler sampler,
    const VkAllocationCallbacks *pAllocator);
typedef void (WINAPI *PFN_vkDestroyInstance)(VkInstance instance, const VkAllocationCallbacks *pAllocator);
typedef PFN_vkVoidFunction (WINAPI *PFN_vkGetInstanceProcAddr)(VkInstance instance, const char *pName);
typedef VkResult (WINAPI *PFN_vkEnumeratePhysicalDevices)(VkInstance instance, uint32_t *pPhysicalDeviceCount,
                                                           VkPhysicalDevice *pPhysicalDevices);
typedef void (WINAPI *PFN_vkGetPhysicalDeviceFeatures)(VkPhysicalDevice physicalDevice,
                                                        VkPhysicalDeviceFeatures *pFeatures);
typedef void (WINAPI *PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice physicalDevice,
                                                          VkPhysicalDeviceProperties *pProperties);
typedef void (WINAPI *PFN_vkGetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice physicalDevice,
    uint32_t *pQueueFamilyPropertyCount, VkQueueFamilyProperties *pQueueFamilyProperties);
typedef void (WINAPI *PFN_vkGetPhysicalDeviceMemoryProperties)(VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties *pMemoryProperties);
typedef void (WINAPI *PFN_vkGetPhysicalDeviceFormatProperties)(VkPhysicalDevice physicalDevice, VkFormat format,
    VkFormatProperties *pFormatProperties);
typedef VkResult (WINAPI *PFN_vkGetPhysicalDeviceImageFormatProperties)(VkPhysicalDevice physicalDevice,
    VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags,
    VkImageFormatProperties *pImageFormatProperties);
typedef VkResult (WINAPI *PFN_vkCreateDevice)(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDevice *pDevice);
typedef void (WINAPI *PFN_vkDestroyDevice)(VkDevice device, const VkAllocationCallbacks *pAllocator);
typedef PFN_vkVoidFunction (WINAPI *PFN_vkGetDeviceProcAddr)(VkDevice device, const char *pName);
typedef void (WINAPI *PFN_vkGetDeviceQueue)(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex,
    VkQueue *pQueue);
typedef VkResult (WINAPI *PFN_vkCreateBuffer)(VkDevice device, const VkBufferCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer);
typedef void (WINAPI *PFN_vkDestroyBuffer)(VkDevice device, VkBuffer buffer,
    const VkAllocationCallbacks *pAllocator);
typedef void (WINAPI *PFN_vkGetBufferMemoryRequirements)(VkDevice device, VkBuffer buffer,
    VkMemoryRequirements *pMemoryRequirements);
typedef VkResult (WINAPI *PFN_vkAllocateMemory)(VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
    const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory);
typedef void (WINAPI *PFN_vkFreeMemory)(VkDevice device, VkDeviceMemory memory,
    const VkAllocationCallbacks *pAllocator);
typedef VkResult (WINAPI *PFN_vkBindBufferMemory)(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
    VkDeviceSize memoryOffset);
typedef VkResult (WINAPI *PFN_vkCreateImage)(VkDevice device, const VkImageCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkImage *pImage);
typedef void (WINAPI *PFN_vkDestroyImage)(VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator);
typedef VkResult (WINAPI *PFN_vkCreateImageView)(VkDevice device, const VkImageViewCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkImageView *pView);
typedef void (WINAPI *PFN_vkDestroyImageView)(VkDevice device, VkImageView imageView,
    const VkAllocationCallbacks *pAllocator);
typedef void (WINAPI *PFN_vkGetImageMemoryRequirements)(VkDevice device, VkImage image,
    VkMemoryRequirements *pMemoryRequirements);
typedef VkResult (WINAPI *PFN_vkBindImageMemory)(VkDevice device, VkImage image, VkDeviceMemory memory,
    VkDeviceSize memoryOffset);
typedef void (WINAPI *PFN_vkGetImageSubresourceLayout)(VkDevice device, VkImage image,
    const VkImageSubresource *pSubresource, VkSubresourceLayout *pLayout);
typedef VkResult (WINAPI *PFN_vkMapMemory)(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
    VkDeviceSize size, VkFlags flags, void **ppData);
typedef void (WINAPI *PFN_vkUnmapMemory)(VkDevice device, VkDeviceMemory memory);
typedef VkResult (WINAPI *PFN_vkFlushMappedMemoryRanges)(VkDevice device, uint32_t memoryRangeCount,
    const VkMappedMemoryRange *pMemoryRanges);
typedef VkResult (WINAPI *PFN_vkInvalidateMappedMemoryRanges)(VkDevice device, uint32_t memoryRangeCount,
    const VkMappedMemoryRange *pMemoryRanges);
typedef VkResult (WINAPI *PFN_vkCreateShaderModule)(VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule);
typedef void (WINAPI *PFN_vkDestroyShaderModule)(VkDevice device, VkShaderModule shaderModule,
    const VkAllocationCallbacks *pAllocator);
typedef VkResult (WINAPI *PFN_vkCreateDescriptorSetLayout)(VkDevice device,
    const VkDescriptorSetLayoutCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
    VkDescriptorSetLayout *pSetLayout);
typedef void (WINAPI *PFN_vkDestroyDescriptorSetLayout)(VkDevice device, VkDescriptorSetLayout setLayout,
    const VkAllocationCallbacks *pAllocator);
typedef VkResult (WINAPI *PFN_vkCreatePipelineLayout)(VkDevice device, const VkPipelineLayoutCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkPipelineLayout *pPipelineLayout);
typedef void (WINAPI *PFN_vkDestroyPipelineLayout)(VkDevice device, VkPipelineLayout pipelineLayout,
    const VkAllocationCallbacks *pAllocator);
typedef VkResult (WINAPI *PFN_vkCreateComputePipelines)(VkDevice device, VkPipelineCache pipelineCache,
    uint32_t createInfoCount, const VkComputePipelineCreateInfo *pCreateInfos,
    const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines);
typedef void (WINAPI *PFN_vkDestroyPipeline)(VkDevice device, VkPipeline pipeline,
    const VkAllocationCallbacks *pAllocator);
typedef VkResult (WINAPI *PFN_vkCreateDescriptorPool)(VkDevice device, const VkDescriptorPoolCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDescriptorPool *pDescriptorPool);
typedef void (WINAPI *PFN_vkDestroyDescriptorPool)(VkDevice device, VkDescriptorPool descriptorPool,
    const VkAllocationCallbacks *pAllocator);
typedef VkResult (WINAPI *PFN_vkResetDescriptorPool)(VkDevice device, VkDescriptorPool descriptorPool, VkFlags flags);
typedef VkResult (WINAPI *PFN_vkAllocateDescriptorSets)(VkDevice device,
    const VkDescriptorSetAllocateInfo *pAllocateInfo, VkDescriptorSet *pDescriptorSets);
typedef VkResult (WINAPI *PFN_vkFreeDescriptorSets)(VkDevice device, VkDescriptorPool descriptorPool,
    uint32_t descriptorSetCount, const VkDescriptorSet *pDescriptorSets);
typedef void (WINAPI *PFN_vkUpdateDescriptorSets)(VkDevice device, uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet *pDescriptorWrites, uint32_t descriptorCopyCount,
    const VkCopyDescriptorSet *pDescriptorCopies);
typedef VkResult (WINAPI *PFN_vkCreateCommandPool)(VkDevice device, const VkCommandPoolCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkCommandPool *pCommandPool);
typedef void (WINAPI *PFN_vkDestroyCommandPool)(VkDevice device, VkCommandPool commandPool,
    const VkAllocationCallbacks *pAllocator);
typedef VkResult (WINAPI *PFN_vkResetCommandPool)(VkDevice device, VkCommandPool commandPool, VkFlags flags);
typedef VkResult (WINAPI *PFN_vkAllocateCommandBuffers)(VkDevice device,
    const VkCommandBufferAllocateInfo *pAllocateInfo, VkCommandBuffer *pCommandBuffers);
typedef void (WINAPI *PFN_vkFreeCommandBuffers)(VkDevice device, VkCommandPool commandPool,
    uint32_t commandBufferCount, const VkCommandBuffer *pCommandBuffers);
typedef VkResult (WINAPI *PFN_vkBeginCommandBuffer)(VkCommandBuffer commandBuffer,
    const VkCommandBufferBeginInfo *pBeginInfo);
typedef VkResult (WINAPI *PFN_vkEndCommandBuffer)(VkCommandBuffer commandBuffer);
typedef VkResult (WINAPI *PFN_vkResetCommandBuffer)(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags);
typedef void (WINAPI *PFN_vkCmdCopyBuffer)(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer,
    uint32_t regionCount, const VkBufferCopy *pRegions);
typedef void (WINAPI *PFN_vkCmdCopyBufferToImage)(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage,
    VkImageLayout dstImageLayout, uint32_t regionCount, const VkBufferImageCopy *pRegions);
typedef void (WINAPI *PFN_vkCmdCopyImageToBuffer)(VkCommandBuffer commandBuffer, VkImage srcImage,
    VkImageLayout srcImageLayout, VkBuffer dstBuffer, uint32_t regionCount, const VkBufferImageCopy *pRegions);
typedef void (WINAPI *PFN_vkCmdCopyImage)(VkCommandBuffer commandBuffer, VkImage srcImage,
    VkImageLayout srcImageLayout, VkImage dstImage, VkImageLayout dstImageLayout,
    uint32_t regionCount, const VkImageCopy *pRegions);
typedef void (WINAPI *PFN_vkCmdClearColorImage)(VkCommandBuffer commandBuffer, VkImage image,
    VkImageLayout imageLayout, const VkClearColorValue *pColor, uint32_t rangeCount,
    const VkImageSubresourceRange *pRanges);
typedef void (WINAPI *PFN_vkCmdUpdateBuffer)(VkCommandBuffer commandBuffer, VkBuffer dstBuffer,
    VkDeviceSize dstOffset, VkDeviceSize dataSize, const void *pData);
typedef void (WINAPI *PFN_vkCmdFillBuffer)(VkCommandBuffer commandBuffer, VkBuffer dstBuffer,
    VkDeviceSize dstOffset, VkDeviceSize size, uint32_t data);
typedef void (WINAPI *PFN_vkCmdPipelineBarrier)(VkCommandBuffer commandBuffer,
    VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags,
    uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier *pBufferMemoryBarriers,
    uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier *pImageMemoryBarriers);
typedef VkResult (WINAPI *PFN_vkQueueSubmit)(VkQueue queue, uint32_t submitCount,
    const VkSubmitInfo *pSubmits, VkFence fence);
typedef VkResult (WINAPI *PFN_vkQueueWaitIdle)(VkQueue queue);
typedef VkResult (WINAPI *PFN_vkDeviceWaitIdle)(VkDevice device);
typedef VkResult (WINAPI *PFN_vkCreateFence)(VkDevice device, const VkFenceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkFence *pFence);
typedef void (WINAPI *PFN_vkDestroyFence)(VkDevice device, VkFence fence,
    const VkAllocationCallbacks *pAllocator);
typedef VkResult (WINAPI *PFN_vkGetFenceStatus)(VkDevice device, VkFence fence);
typedef VkResult (WINAPI *PFN_vkWaitForFences)(VkDevice device, uint32_t fenceCount, const VkFence *pFences,
    VkBool32 waitAll, uint64_t timeout);
typedef VkResult (WINAPI *PFN_vkResetFences)(VkDevice device, uint32_t fenceCount, const VkFence *pFences);
typedef VkResult (WINAPI *PFN_vkCreateSemaphore)(VkDevice device, const VkSemaphoreCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSemaphore *pSemaphore);
typedef void (WINAPI *PFN_vkDestroySemaphore)(VkDevice device, VkSemaphore semaphore,
    const VkAllocationCallbacks *pAllocator);
typedef void (WINAPI *PFN_vkCmdBindPipeline)(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
    VkPipeline pipeline);
typedef void (WINAPI *PFN_vkCmdBindDescriptorSets)(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout, uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet *pDescriptorSets,
    uint32_t dynamicOffsetCount, const uint32_t *pDynamicOffsets);
typedef void (WINAPI *PFN_vkCmdPushConstants)(VkCommandBuffer commandBuffer, VkPipelineLayout layout,
    VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size, const void *pValues);
typedef void (WINAPI *PFN_vkCmdDispatch)(VkCommandBuffer commandBuffer, uint32_t groupCountX,
    uint32_t groupCountY, uint32_t groupCountZ);
typedef void (WINAPI *PFN_vkCmdDispatchIndirect)(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset);

VkResult WINAPI vkEnumerateInstanceVersion(uint32_t *pApiVersion);
VkResult WINAPI vkEnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pPropertyCount,
                                                         VkExtensionProperties *pProperties);
VkResult WINAPI vkEnumerateInstanceLayerProperties(uint32_t *pPropertyCount, VkLayerProperties *pProperties);
VkResult WINAPI vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char *pLayerName,
    uint32_t *pPropertyCount, VkExtensionProperties *pProperties);
VkResult WINAPI vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
    uint32_t *pPropertyCount, VkLayerProperties *pProperties);
VkResult WINAPI vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                  const VkAllocationCallbacks *pAllocator, VkInstance *pInstance);
void WINAPI vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator);
PFN_vkVoidFunction WINAPI vkGetInstanceProcAddr(VkInstance instance, const char *pName);
VkResult WINAPI vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *pPhysicalDeviceCount,
                                             VkPhysicalDevice *pPhysicalDevices);
void WINAPI vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures *pFeatures);
void WINAPI vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties *pProperties);
void WINAPI vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
    uint32_t *pQueueFamilyPropertyCount, VkQueueFamilyProperties *pQueueFamilyProperties);
void WINAPI vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties *pMemoryProperties);
void WINAPI vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format,
    VkFormatProperties *pFormatProperties);
VkResult WINAPI vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format,
    VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags,
    VkImageFormatProperties *pImageFormatProperties);
VkResult WINAPI vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDevice *pDevice);
void WINAPI vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator);
PFN_vkVoidFunction WINAPI vkGetDeviceProcAddr(VkDevice device, const char *pName);
void WINAPI vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue);
VkResult WINAPI vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer);
void WINAPI vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator);
void WINAPI vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements *pMemoryRequirements);
VkResult WINAPI vkCreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkImage *pImage);
void WINAPI vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator);
VkResult WINAPI vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkImageView *pView);
void WINAPI vkDestroyImageView(VkDevice device, VkImageView imageView,
    const VkAllocationCallbacks *pAllocator);
VkResult WINAPI vkCreateSampler(VkDevice device, const VkSamplerCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSampler *pSampler);
void WINAPI vkDestroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks *pAllocator);
void WINAPI vkGetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements *pMemoryRequirements);
VkResult WINAPI vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset);
void WINAPI vkGetImageSubresourceLayout(VkDevice device, VkImage image,
    const VkImageSubresource *pSubresource, VkSubresourceLayout *pLayout);
VkResult WINAPI vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
    const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory);
void WINAPI vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *pAllocator);
VkResult WINAPI vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset);
VkResult WINAPI vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
    VkFlags flags, void **ppData);
void WINAPI vkUnmapMemory(VkDevice device, VkDeviceMemory memory);
VkResult WINAPI vkFlushMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount,
    const VkMappedMemoryRange *pMemoryRanges);
VkResult WINAPI vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount,
    const VkMappedMemoryRange *pMemoryRanges);
VkResult WINAPI vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule);
void WINAPI vkDestroyShaderModule(VkDevice device, VkShaderModule shaderModule,
    const VkAllocationCallbacks *pAllocator);
VkResult WINAPI vkCreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDescriptorSetLayout *pSetLayout);
void WINAPI vkDestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout setLayout,
    const VkAllocationCallbacks *pAllocator);
VkResult WINAPI vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkPipelineLayout *pPipelineLayout);
void WINAPI vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout pipelineLayout,
    const VkAllocationCallbacks *pAllocator);
VkResult WINAPI vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkComputePipelineCreateInfo *pCreateInfos, const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines);
void WINAPI vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *pAllocator);
VkResult WINAPI vkCreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDescriptorPool *pDescriptorPool);
void WINAPI vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool,
    const VkAllocationCallbacks *pAllocator);
VkResult WINAPI vkResetDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool, VkFlags flags);
VkResult WINAPI vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *pAllocateInfo,
    VkDescriptorSet *pDescriptorSets);
VkResult WINAPI vkFreeDescriptorSets(VkDevice device, VkDescriptorPool descriptorPool, uint32_t descriptorSetCount,
    const VkDescriptorSet *pDescriptorSets);
void WINAPI vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet *pDescriptorWrites, uint32_t descriptorCopyCount,
    const VkCopyDescriptorSet *pDescriptorCopies);
VkResult WINAPI vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkCommandPool *pCommandPool);
void WINAPI vkDestroyCommandPool(VkDevice device, VkCommandPool commandPool,
    const VkAllocationCallbacks *pAllocator);
VkResult WINAPI vkResetCommandPool(VkDevice device, VkCommandPool commandPool, VkFlags flags);
VkResult WINAPI vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *pAllocateInfo,
    VkCommandBuffer *pCommandBuffers);
void WINAPI vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount,
    const VkCommandBuffer *pCommandBuffers);
VkResult WINAPI vkBeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo);
VkResult WINAPI vkEndCommandBuffer(VkCommandBuffer commandBuffer);
VkResult WINAPI vkResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags);
void WINAPI vkCmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer,
    uint32_t regionCount, const VkBufferCopy *pRegions);
void WINAPI vkCmdCopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage,
    VkImageLayout dstImageLayout, uint32_t regionCount, const VkBufferImageCopy *pRegions);
void WINAPI vkCmdCopyImageToBuffer(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout,
    VkBuffer dstBuffer, uint32_t regionCount, const VkBufferImageCopy *pRegions);
void WINAPI vkCmdCopyImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout,
    VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageCopy *pRegions);
void WINAPI vkCmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout,
    const VkClearColorValue *pColor, uint32_t rangeCount, const VkImageSubresourceRange *pRanges);
void WINAPI vkCmdUpdateBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset,
    VkDeviceSize dataSize, const void *pData);
void WINAPI vkCmdFillBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset,
                            VkDeviceSize size, uint32_t data);
void WINAPI vkCmdPipelineBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags, uint32_t memoryBarrierCount,
    const VkMemoryBarrier *pMemoryBarriers, uint32_t bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier *pBufferMemoryBarriers, uint32_t imageMemoryBarrierCount,
    const VkImageMemoryBarrier *pImageMemoryBarriers);
void WINAPI vkCmdBindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline);
void WINAPI vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout, uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet *pDescriptorSets,
    uint32_t dynamicOffsetCount, const uint32_t *pDynamicOffsets);
void WINAPI vkCmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout layout,
    VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size, const void *pValues);
void WINAPI vkCmdDispatch(VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY,
    uint32_t groupCountZ);
void WINAPI vkCmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset);
VkResult WINAPI vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence);
VkResult WINAPI vkQueueWaitIdle(VkQueue queue);
VkResult WINAPI vkDeviceWaitIdle(VkDevice device);
VkResult WINAPI vkCreateFence(VkDevice device, const VkFenceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkFence *pFence);
void WINAPI vkDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks *pAllocator);
VkResult WINAPI vkGetFenceStatus(VkDevice device, VkFence fence);
VkResult WINAPI vkWaitForFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences,
    VkBool32 waitAll, uint64_t timeout);
VkResult WINAPI vkResetFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences);
VkResult WINAPI vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSemaphore *pSemaphore);
void WINAPI vkDestroySemaphore(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *pAllocator);

void vx_vulkan_init(void);

#endif
