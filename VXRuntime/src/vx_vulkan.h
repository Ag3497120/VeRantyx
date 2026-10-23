// Vulkan 1.0 ABI subset used by vxrt's built-in vulkan-1.dll frontend.
// These are public Khronos ABI layouts for currently implemented commands; this is intentionally
// not a replacement for the Vulkan SDK headers.
#ifndef VX_VULKAN_H
#define VX_VULKAN_H

#include "vx.h"

#define VK_KHR_MAINTENANCE1_EXTENSION_NAME "VK_KHR_maintenance1"
#define VK_KHR_MAINTENANCE1_SPEC_VERSION 2u
#define VK_KHR_SURFACE_EXTENSION_NAME "VK_KHR_surface"
#define VK_KHR_SURFACE_SPEC_VERSION 25u
#define VK_KHR_WIN32_SURFACE_EXTENSION_NAME "VK_KHR_win32_surface"
#define VK_KHR_WIN32_SURFACE_SPEC_VERSION 6u
#define VK_KHR_SWAPCHAIN_EXTENSION_NAME "VK_KHR_swapchain"
#define VK_KHR_SWAPCHAIN_SPEC_VERSION 70u

typedef int32_t VkResult;
typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
#define VK_TRUE 1u
#define VK_FALSE 0u
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
typedef uint64_t VkRenderPass;
typedef uint64_t VkFramebuffer;
typedef uint64_t VkFence;
typedef uint64_t VkSemaphore;
typedef uint64_t VkSurfaceKHR;
typedef uint64_t VkSwapchainKHR;
typedef int32_t VkStructureType;
typedef int32_t VkSystemAllocationScope;
typedef int32_t VkInternalAllocationType;
typedef uint32_t VkSampleCountFlags;
typedef uint32_t VkFormatFeatureFlags;
typedef uint32_t VkQueueFlags;
typedef uint32_t VkSharingMode;
typedef uint32_t VkMemoryPropertyFlags;
typedef uint32_t VkMemoryHeapFlags;
typedef uint32_t VkBufferUsageFlags;
typedef int32_t VkIndexType;
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
typedef uint32_t VkCommandPoolTrimFlagsKHR;
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
typedef uint32_t VkColorComponentFlags;
typedef uint32_t VkCullModeFlags;
typedef uint32_t VkSampleMask;
typedef int32_t VkPrimitiveTopology;
typedef int32_t VkPolygonMode;
typedef int32_t VkFrontFace;
typedef int32_t VkLogicOp;
typedef int32_t VkBlendFactor;
typedef int32_t VkBlendOp;
typedef int32_t VkDynamicState;
typedef uint32_t VkDescriptorSetLayoutCreateFlags;
typedef uint32_t VkDescriptorPoolCreateFlags;
typedef uint32_t VkDescriptorPoolResetFlags;
typedef uint32_t VkSamplerCreateFlags;
typedef uint32_t VkFilter;
typedef uint32_t VkSamplerMipmapMode;
typedef uint32_t VkSamplerAddressMode;
typedef int32_t VkCompareOp;
typedef uint32_t VkBorderColor;
typedef int32_t VkColorSpaceKHR;
typedef int32_t VkPresentModeKHR;
typedef uint32_t VkSurfaceTransformFlagsKHR;
typedef uint32_t VkSurfaceTransformFlagBitsKHR;
typedef uint32_t VkCompositeAlphaFlagsKHR;
typedef uint32_t VkAttachmentLoadOp;
typedef uint32_t VkAttachmentStoreOp;
typedef uint32_t VkSubpassContents;
typedef uint32_t VkSwapchainCreateFlagsKHR;

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

typedef struct VkWin32SurfaceCreateInfoKHR {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    HANDLE hinstance;
    HANDLE hwnd;
} VkWin32SurfaceCreateInfoKHR;

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
typedef struct VkOffset2D { int32_t x, y; } VkOffset2D;
typedef struct VkExtent2D { uint32_t width, height; } VkExtent2D;
typedef struct VkSurfaceCapabilitiesKHR {
    uint32_t minImageCount, maxImageCount;
    VkExtent2D currentExtent, minImageExtent, maxImageExtent;
    uint32_t maxImageArrayLayers;
    VkSurfaceTransformFlagsKHR supportedTransforms;
    VkSurfaceTransformFlagBitsKHR currentTransform;
    VkCompositeAlphaFlagsKHR supportedCompositeAlpha;
    VkImageUsageFlags supportedUsageFlags;
} VkSurfaceCapabilitiesKHR;
typedef struct VkSurfaceFormatKHR { VkFormat format; VkColorSpaceKHR colorSpace; } VkSurfaceFormatKHR;
typedef struct VkSwapchainCreateInfoKHR {
    VkStructureType sType;
    const void *pNext;
    VkSwapchainCreateFlagsKHR flags;
    VkSurfaceKHR surface;
    uint32_t minImageCount;
    VkFormat imageFormat;
    VkColorSpaceKHR imageColorSpace;
    VkExtent2D imageExtent;
    uint32_t imageArrayLayers;
    VkImageUsageFlags imageUsage;
    VkSharingMode imageSharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
    VkSurfaceTransformFlagBitsKHR preTransform;
    VkCompositeAlphaFlagsKHR compositeAlpha;
    VkPresentModeKHR presentMode;
    VkBool32 clipped;
    VkSwapchainKHR oldSwapchain;
} VkSwapchainCreateInfoKHR;
typedef struct VkPresentInfoKHR {
    VkStructureType sType;
    const void *pNext;
    uint32_t waitSemaphoreCount;
    const VkSemaphore *pWaitSemaphores;
    uint32_t swapchainCount;
    const VkSwapchainKHR *pSwapchains;
    const uint32_t *pImageIndices;
    VkResult *pResults;
} VkPresentInfoKHR;
typedef struct VkRect2D { VkOffset2D offset; VkExtent2D extent; } VkRect2D;
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
typedef struct VkAttachmentDescription {
    VkFlags flags;
    VkFormat format;
    VkSampleCountFlags samples;
    VkAttachmentLoadOp loadOp;
    VkAttachmentStoreOp storeOp;
    VkAttachmentLoadOp stencilLoadOp;
    VkAttachmentStoreOp stencilStoreOp;
    VkImageLayout initialLayout, finalLayout;
} VkAttachmentDescription;
typedef struct VkAttachmentReference { uint32_t attachment; VkImageLayout layout; } VkAttachmentReference;
typedef struct VkSubpassDescription {
    VkFlags flags;
    VkPipelineBindPoint pipelineBindPoint;
    uint32_t inputAttachmentCount;
    const VkAttachmentReference *pInputAttachments;
    uint32_t colorAttachmentCount;
    const VkAttachmentReference *pColorAttachments;
    const VkAttachmentReference *pResolveAttachments;
    const VkAttachmentReference *pDepthStencilAttachment;
    uint32_t preserveAttachmentCount;
    const uint32_t *pPreserveAttachments;
} VkSubpassDescription;
typedef struct VkSubpassDependency {
    uint32_t srcSubpass, dstSubpass;
    VkPipelineStageFlags srcStageMask, dstStageMask;
    VkAccessFlags srcAccessMask, dstAccessMask;
    VkDependencyFlags dependencyFlags;
} VkSubpassDependency;
typedef struct VkRenderPassCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    uint32_t attachmentCount;
    const VkAttachmentDescription *pAttachments;
    uint32_t subpassCount;
    const VkSubpassDescription *pSubpasses;
    uint32_t dependencyCount;
    const VkSubpassDependency *pDependencies;
} VkRenderPassCreateInfo;
typedef struct VkFramebufferCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    VkRenderPass renderPass;
    uint32_t attachmentCount;
    const VkImageView *pAttachments;
    uint32_t width, height, layers;
} VkFramebufferCreateInfo;
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
typedef struct VkClearDepthStencilValue { float depth; uint32_t stencil; } VkClearDepthStencilValue;
typedef union VkClearValue { VkClearColorValue color; VkClearDepthStencilValue depthStencil; } VkClearValue;
typedef struct VkRenderPassBeginInfo {
    VkStructureType sType;
    const void *pNext;
    VkRenderPass renderPass;
    VkFramebuffer framebuffer;
    VkRect2D renderArea;
    uint32_t clearValueCount;
    const VkClearValue *pClearValues;
} VkRenderPassBeginInfo;
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

typedef struct VkDrawIndirectCommand {
    uint32_t vertexCount, instanceCount, firstVertex, firstInstance;
} VkDrawIndirectCommand;

typedef struct VkDrawIndexedIndirectCommand {
    uint32_t indexCount, instanceCount, firstIndex;
    int32_t vertexOffset;
    uint32_t firstInstance;
} VkDrawIndexedIndirectCommand;

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

typedef struct VkVertexInputBindingDescription {
    uint32_t binding, stride, inputRate;
} VkVertexInputBindingDescription;
typedef struct VkVertexInputAttributeDescription {
    uint32_t location, binding;
    VkFormat format;
    uint32_t offset;
} VkVertexInputAttributeDescription;
typedef struct VkPipelineVertexInputStateCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    uint32_t vertexBindingDescriptionCount;
    const VkVertexInputBindingDescription *pVertexBindingDescriptions;
    uint32_t vertexAttributeDescriptionCount;
    const VkVertexInputAttributeDescription *pVertexAttributeDescriptions;
} VkPipelineVertexInputStateCreateInfo;
typedef struct VkPipelineInputAssemblyStateCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    VkPrimitiveTopology topology;
    VkBool32 primitiveRestartEnable;
} VkPipelineInputAssemblyStateCreateInfo;
typedef struct VkViewport {
    float x, y, width, height, minDepth, maxDepth;
} VkViewport;
typedef struct VkPipelineViewportStateCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    uint32_t viewportCount;
    const VkViewport *pViewports;
    uint32_t scissorCount;
    const VkRect2D *pScissors;
} VkPipelineViewportStateCreateInfo;
typedef struct VkPipelineRasterizationStateCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    VkBool32 depthClampEnable, rasterizerDiscardEnable;
    VkPolygonMode polygonMode;
    VkCullModeFlags cullMode;
    VkFrontFace frontFace;
    VkBool32 depthBiasEnable;
    float depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor, lineWidth;
} VkPipelineRasterizationStateCreateInfo;
typedef struct VkPipelineMultisampleStateCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    VkSampleCountFlags rasterizationSamples;
    VkBool32 sampleShadingEnable;
    float minSampleShading;
    const VkSampleMask *pSampleMask;
    VkBool32 alphaToCoverageEnable, alphaToOneEnable;
} VkPipelineMultisampleStateCreateInfo;
typedef struct VkStencilOpState {
    int32_t failOp, passOp, depthFailOp, compareOp;
    uint32_t compareMask, writeMask, reference;
} VkStencilOpState;
typedef struct VkPipelineDepthStencilStateCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    VkBool32 depthTestEnable, depthWriteEnable;
    VkCompareOp depthCompareOp;
    VkBool32 depthBoundsTestEnable, stencilTestEnable;
    VkStencilOpState front, back;
    float minDepthBounds, maxDepthBounds;
} VkPipelineDepthStencilStateCreateInfo;
typedef struct VkPipelineColorBlendAttachmentState {
    VkBool32 blendEnable;
    VkBlendFactor srcColorBlendFactor, dstColorBlendFactor;
    VkBlendOp colorBlendOp;
    VkBlendFactor srcAlphaBlendFactor, dstAlphaBlendFactor;
    VkBlendOp alphaBlendOp;
    VkColorComponentFlags colorWriteMask;
} VkPipelineColorBlendAttachmentState;
typedef struct VkPipelineColorBlendStateCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    VkBool32 logicOpEnable;
    VkLogicOp logicOp;
    uint32_t attachmentCount;
    const VkPipelineColorBlendAttachmentState *pAttachments;
    float blendConstants[4];
} VkPipelineColorBlendStateCreateInfo;
typedef struct VkPipelineDynamicStateCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    uint32_t dynamicStateCount;
    const VkDynamicState *pDynamicStates;
} VkPipelineDynamicStateCreateInfo;
typedef struct VkPipelineTessellationStateCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    uint32_t patchControlPoints;
} VkPipelineTessellationStateCreateInfo;
typedef struct VkGraphicsPipelineCreateInfo {
    VkStructureType sType;
    const void *pNext;
    VkPipelineCreateFlags flags;
    uint32_t stageCount;
    const VkPipelineShaderStageCreateInfo *pStages;
    const VkPipelineVertexInputStateCreateInfo *pVertexInputState;
    const VkPipelineInputAssemblyStateCreateInfo *pInputAssemblyState;
    const VkPipelineTessellationStateCreateInfo *pTessellationState;
    const VkPipelineViewportStateCreateInfo *pViewportState;
    const VkPipelineRasterizationStateCreateInfo *pRasterizationState;
    const VkPipelineMultisampleStateCreateInfo *pMultisampleState;
    const VkPipelineDepthStencilStateCreateInfo *pDepthStencilState;
    const VkPipelineColorBlendStateCreateInfo *pColorBlendState;
    const VkPipelineDynamicStateCreateInfo *pDynamicState;
    VkPipelineLayout layout;
    VkRenderPass renderPass;
    uint32_t subpass;
    VkPipeline basePipelineHandle;
    int32_t basePipelineIndex;
} VkGraphicsPipelineCreateInfo;

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
    VK_VERTEX_INPUT_RATE_VERTEX = 0,
    VK_VERTEX_INPUT_RATE_INSTANCE = 1,
    VK_INDEX_TYPE_UINT16 = 0,
    VK_INDEX_TYPE_UINT32 = 1,
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
    VK_FORMAT_R8_SNORM = 10,
    VK_FORMAT_R8_UINT = 13,
    VK_FORMAT_R8_SINT = 14,
    VK_FORMAT_R8G8_UNORM = 16,
    VK_FORMAT_R8G8_SNORM = 17,
    VK_FORMAT_R8G8_UINT = 20,
    VK_FORMAT_R8G8_SINT = 21,
    VK_FORMAT_R8G8B8_UNORM = 23,
    VK_FORMAT_R8G8B8_SNORM = 24,
    VK_FORMAT_R8G8B8_UINT = 27,
    VK_FORMAT_R8G8B8_SINT = 28,
    VK_FORMAT_R8G8B8A8_UNORM = 37,
    VK_FORMAT_R8G8B8A8_SNORM = 38,
    VK_FORMAT_R8G8B8A8_UINT = 41,
    VK_FORMAT_R8G8B8A8_SINT = 42,
    VK_FORMAT_B8G8R8A8_UNORM = 44,
    VK_FORMAT_R16_UNORM = 70,
    VK_FORMAT_R16_SNORM = 71,
    VK_FORMAT_R16_UINT = 74,
    VK_FORMAT_R16_SINT = 75,
    VK_FORMAT_R16_SFLOAT = 76,
    VK_FORMAT_R16G16_UNORM = 77,
    VK_FORMAT_R16G16_SNORM = 78,
    VK_FORMAT_R16G16_UINT = 81,
    VK_FORMAT_R16G16_SINT = 82,
    VK_FORMAT_R16G16_SFLOAT = 83,
    VK_FORMAT_R16G16B16_UNORM = 84,
    VK_FORMAT_R16G16B16_SNORM = 85,
    VK_FORMAT_R16G16B16_UINT = 88,
    VK_FORMAT_R16G16B16_SINT = 89,
    VK_FORMAT_R16G16B16_SFLOAT = 90,
    VK_FORMAT_R16G16B16A16_UNORM = 91,
    VK_FORMAT_R16G16B16A16_SNORM = 92,
    VK_FORMAT_R16G16B16A16_UINT = 95,
    VK_FORMAT_R16G16B16A16_SINT = 96,
    VK_FORMAT_R16G16B16A16_SFLOAT = 97,
    VK_FORMAT_R32_UINT = 98,
    VK_FORMAT_R32_SINT = 99,
    VK_FORMAT_R32_SFLOAT = 100,
    VK_FORMAT_R32G32_UINT = 101,
    VK_FORMAT_R32G32_SINT = 102,
    VK_FORMAT_R32G32_SFLOAT = 103,
    VK_FORMAT_R32G32B32_UINT = 104,
    VK_FORMAT_R32G32B32_SINT = 105,
    VK_FORMAT_R32G32B32_SFLOAT = 106,
    VK_FORMAT_R32G32B32A32_UINT = 107,
    VK_FORMAT_R32G32B32A32_SINT = 108,
    VK_FORMAT_R32G32B32A32_SFLOAT = 109,
    VK_FORMAT_D16_UNORM = 124,
    VK_FORMAT_D32_SFLOAT = 126,
    VK_FORMAT_D24_UNORM_S8_UINT = 129,
    VK_FORMAT_D32_SFLOAT_S8_UINT = 130,
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
    VK_SHADER_STAGE_VERTEX_BIT = 1u << 0,
    VK_SHADER_STAGE_GEOMETRY_BIT = 1u << 3,
    VK_SHADER_STAGE_FRAGMENT_BIT = 1u << 4,
    VK_SHADER_STAGE_COMPUTE_BIT = 1u << 5,
    VK_PIPELINE_BIND_POINT_GRAPHICS = 0,
    VK_PIPELINE_BIND_POINT_COMPUTE = 1,
    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER = 6,
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER = 7,
    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC = 8,
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC = 9,
    VK_DESCRIPTOR_TYPE_SAMPLER = 0,
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 1,
    VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE = 2,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE = 3,
    VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER = 4,
    VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER = 5,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT = 1u << 0,
    VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT = 1u << 1,
    VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT = 1u << 6,
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT = 1u << 7,
    VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT = 1u << 9,
    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT_KHR = 1u << 14,
    VK_FORMAT_FEATURE_TRANSFER_DST_BIT_KHR = 1u << 15,
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
    VK_PRIMITIVE_TOPOLOGY_POINT_LIST = 0,
    VK_PRIMITIVE_TOPOLOGY_LINE_LIST = 1,
    VK_PRIMITIVE_TOPOLOGY_LINE_STRIP = 2,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP = 4,
    VK_DYNAMIC_STATE_VIEWPORT = 0,
    VK_DYNAMIC_STATE_SCISSOR = 1,
    VK_DYNAMIC_STATE_BLEND_CONSTANTS = 4,
    VK_POLYGON_MODE_FILL = 0,
    VK_CULL_MODE_NONE = 0,
    VK_CULL_MODE_FRONT_BIT = 1,
    VK_CULL_MODE_BACK_BIT = 2,
    VK_CULL_MODE_FRONT_AND_BACK = 3,
    VK_FRONT_FACE_COUNTER_CLOCKWISE = 0,
    VK_FRONT_FACE_CLOCKWISE = 1,
    VK_BLEND_FACTOR_ZERO = 0,
    VK_BLEND_FACTOR_ONE = 1,
    VK_BLEND_FACTOR_SRC_COLOR = 2,
    VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR = 3,
    VK_BLEND_FACTOR_DST_COLOR = 4,
    VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR = 5,
    VK_BLEND_FACTOR_SRC_ALPHA = 6,
    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA = 7,
    VK_BLEND_FACTOR_DST_ALPHA = 8,
    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA = 9,
    VK_BLEND_FACTOR_CONSTANT_COLOR = 10,
    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR = 11,
    VK_BLEND_FACTOR_CONSTANT_ALPHA = 12,
    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA = 13,
    VK_BLEND_FACTOR_SRC_ALPHA_SATURATE = 14,
    VK_BLEND_FACTOR_SRC1_COLOR = 15,
    VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR = 16,
    VK_BLEND_FACTOR_SRC1_ALPHA = 17,
    VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA = 18,
    VK_BLEND_OP_ADD = 0,
    VK_BLEND_OP_SUBTRACT = 1,
    VK_BLEND_OP_REVERSE_SUBTRACT = 2,
    VK_BLEND_OP_MIN = 3,
    VK_BLEND_OP_MAX = 4,
    VK_LOGIC_OP_CLEAR = 0,
    VK_LOGIC_OP_AND = 1,
    VK_LOGIC_OP_AND_REVERSE = 2,
    VK_LOGIC_OP_COPY = 3,
    VK_LOGIC_OP_AND_INVERTED = 4,
    VK_LOGIC_OP_NO_OP = 5,
    VK_LOGIC_OP_XOR = 6,
    VK_LOGIC_OP_OR = 7,
    VK_LOGIC_OP_NOR = 8,
    VK_LOGIC_OP_EQUIVALENT = 9,
    VK_LOGIC_OP_INVERT = 10,
    VK_LOGIC_OP_OR_REVERSE = 11,
    VK_LOGIC_OP_COPY_INVERTED = 12,
    VK_LOGIC_OP_OR_INVERTED = 13,
    VK_LOGIC_OP_NAND = 14,
    VK_LOGIC_OP_SET = 15,
    VK_COLOR_COMPONENT_R_BIT = 1u << 0,
    VK_COLOR_COMPONENT_G_BIT = 1u << 1,
    VK_COLOR_COMPONENT_B_BIT = 1u << 2,
    VK_COLOR_COMPONENT_A_BIT = 1u << 3,
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
    VK_SAMPLE_COUNT_1_BIT = 1u << 0,
    VK_ATTACHMENT_LOAD_OP_LOAD = 0,
    VK_ATTACHMENT_LOAD_OP_CLEAR = 1,
    VK_ATTACHMENT_LOAD_OP_DONT_CARE = 2,
    VK_ATTACHMENT_STORE_OP_STORE = 0,
    VK_ATTACHMENT_STORE_OP_DONT_CARE = 1,
    VK_SUBPASS_CONTENTS_INLINE = 0,
    VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS = 1,
    VK_ATTACHMENT_UNUSED = 0xffffffffu
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
    VK_ERROR_OUT_OF_POOL_MEMORY_KHR = -1000069000,
    VK_ERROR_OUT_OF_POOL_MEMORY = VK_ERROR_OUT_OF_POOL_MEMORY_KHR,
    VK_ERROR_UNKNOWN = -13,
    VK_ERROR_SURFACE_LOST_KHR = -1000000000,
    VK_ERROR_NATIVE_WINDOW_IN_USE_KHR = -1000000001,
    VK_ERROR_OUT_OF_DATE_KHR = -1000001004,
    VK_NOT_READY = 1,
    VK_TIMEOUT = 2,
    VK_SUBOPTIMAL_KHR = 1000001003,
    VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
    VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
    VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR = 1000009000,
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
    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO = 19,
    VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO = 20,
    VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO = 21,
    VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO = 22,
    VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO = 23,
    VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO = 24,
    VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO = 25,
    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO = 26,
    VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO = 27,
    VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO = 28,
    VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO = 29,
    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO = 30,
    VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO = 31,
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32,
    VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO = 33,
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO = 34,
    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET = 35,
    VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET = 36,
    VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO = 37,
    VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO = 38,
    VK_STRUCTURE_TYPE_MEMORY_BARRIER = 46,
    VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER = 44,
    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER = 45,
    VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39,
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40,
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42,
    VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO = 43,
    VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR = 1000001000,
    VK_STRUCTURE_TYPE_PRESENT_INFO_KHR = 1000001001,
    VK_SYSTEM_ALLOCATION_SCOPE_COMMAND = 0,
    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT = 1,
    VK_SYSTEM_ALLOCATION_SCOPE_DEVICE = 3,
    VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE = 4,
    VK_API_VERSION_1_0 = (1u << 22)
};

enum {
    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR = 0,
    VK_PRESENT_MODE_FIFO_KHR = 2,
    VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR = 1u << 0,
    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR = 1u << 0
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
typedef VkResult (WINAPI *PFN_vkCreateWin32SurfaceKHR)(VkInstance instance,
    const VkWin32SurfaceCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator,
    VkSurfaceKHR *pSurface);
typedef void (WINAPI *PFN_vkDestroySurfaceKHR)(VkInstance instance, VkSurfaceKHR surface,
    const VkAllocationCallbacks *pAllocator);
typedef VkResult (WINAPI *PFN_vkGetPhysicalDeviceSurfaceSupportKHR)(VkPhysicalDevice physicalDevice,
    uint32_t queueFamilyIndex, VkSurfaceKHR surface, VkBool32 *pSupported);
typedef VkResult (WINAPI *PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *pSurfaceCapabilities);
typedef VkResult (WINAPI *PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)(VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface, uint32_t *pSurfaceFormatCount, VkSurfaceFormatKHR *pSurfaceFormats);
typedef VkResult (WINAPI *PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)(VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface, uint32_t *pPresentModeCount, VkPresentModeKHR *pPresentModes);
typedef VkResult (WINAPI *PFN_vkCreateSwapchainKHR)(VkDevice device,
    const VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator,
    VkSwapchainKHR *pSwapchain);
typedef void (WINAPI *PFN_vkDestroySwapchainKHR)(VkDevice device, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks *pAllocator);
typedef VkResult (WINAPI *PFN_vkGetSwapchainImagesKHR)(VkDevice device, VkSwapchainKHR swapchain,
    uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages);
typedef VkResult (WINAPI *PFN_vkAcquireNextImageKHR)(VkDevice device, VkSwapchainKHR swapchain,
    uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex);
typedef VkResult (WINAPI *PFN_vkQueuePresentKHR)(VkQueue queue, const VkPresentInfoKHR *pPresentInfo);
typedef VkBool32 (WINAPI *PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR)(
    VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex);
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
typedef VkResult (WINAPI *PFN_vkCreateRenderPass)(VkDevice device, const VkRenderPassCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass);
typedef void (WINAPI *PFN_vkDestroyRenderPass)(VkDevice device, VkRenderPass renderPass,
    const VkAllocationCallbacks *pAllocator);
typedef VkResult (WINAPI *PFN_vkCreateFramebuffer)(VkDevice device, const VkFramebufferCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkFramebuffer *pFramebuffer);
typedef void (WINAPI *PFN_vkDestroyFramebuffer)(VkDevice device, VkFramebuffer framebuffer,
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
typedef VkResult (WINAPI *PFN_vkCreateGraphicsPipelines)(VkDevice device, VkPipelineCache pipelineCache,
    uint32_t createInfoCount, const VkGraphicsPipelineCreateInfo *pCreateInfos,
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
typedef void (WINAPI *PFN_vkTrimCommandPoolKHR)(VkDevice device, VkCommandPool commandPool,
    VkCommandPoolTrimFlagsKHR flags);
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
typedef void (WINAPI *PFN_vkCmdSetViewport)(VkCommandBuffer commandBuffer, uint32_t firstViewport,
    uint32_t viewportCount, const VkViewport *pViewports);
typedef void (WINAPI *PFN_vkCmdSetScissor)(VkCommandBuffer commandBuffer, uint32_t firstScissor,
    uint32_t scissorCount, const VkRect2D *pScissors);
typedef void (WINAPI *PFN_vkCmdSetBlendConstants)(VkCommandBuffer commandBuffer, const float blendConstants[4]);
typedef void (WINAPI *PFN_vkCmdBindVertexBuffers)(VkCommandBuffer commandBuffer, uint32_t firstBinding,
    uint32_t bindingCount, const VkBuffer *pBuffers, const VkDeviceSize *pOffsets);
typedef void (WINAPI *PFN_vkCmdDraw)(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount,
    uint32_t firstVertex, uint32_t firstInstance);
typedef void (WINAPI *PFN_vkCmdDrawIndirect)(VkCommandBuffer commandBuffer, VkBuffer buffer,
    VkDeviceSize offset, uint32_t drawCount, uint32_t stride);
typedef void (WINAPI *PFN_vkCmdDrawIndexedIndirect)(VkCommandBuffer commandBuffer, VkBuffer buffer,
    VkDeviceSize offset, uint32_t drawCount, uint32_t stride);
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
VkResult WINAPI vkCreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface);
void WINAPI vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
    const VkAllocationCallbacks *pAllocator);
VkResult WINAPI vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
    VkSurfaceKHR surface, VkBool32 *pSupported);
VkResult WINAPI vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR *pSurfaceCapabilities);
VkResult WINAPI vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
    uint32_t *pSurfaceFormatCount, VkSurfaceFormatKHR *pSurfaceFormats);
VkResult WINAPI vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
    uint32_t *pPresentModeCount, VkPresentModeKHR *pPresentModes);
VkResult WINAPI vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain);
void WINAPI vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks *pAllocator);
VkResult WINAPI vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
    uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages);
VkResult WINAPI vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex);
VkBool32 WINAPI vkGetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice physicalDevice,
    uint32_t queueFamilyIndex);
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
VkResult WINAPI vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass);
void WINAPI vkDestroyRenderPass(VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks *pAllocator);
VkResult WINAPI vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkFramebuffer *pFramebuffer);
void WINAPI vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks *pAllocator);
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
VkResult WINAPI vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo *pCreateInfos, const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines);
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
void WINAPI vkTrimCommandPoolKHR(VkDevice device, VkCommandPool commandPool, VkCommandPoolTrimFlagsKHR flags);
VkResult WINAPI vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *pAllocateInfo,
    VkCommandBuffer *pCommandBuffers);
void WINAPI vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount,
    const VkCommandBuffer *pCommandBuffers);
VkResult WINAPI vkBeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo);
VkResult WINAPI vkEndCommandBuffer(VkCommandBuffer commandBuffer);
VkResult WINAPI vkResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags);
void WINAPI vkCmdBeginRenderPass(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo *pRenderPassBegin,
    VkSubpassContents contents);
void WINAPI vkCmdEndRenderPass(VkCommandBuffer commandBuffer);
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
void WINAPI vkCmdSetViewport(VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount,
    const VkViewport *pViewports);
void WINAPI vkCmdSetScissor(VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount,
    const VkRect2D *pScissors);
void WINAPI vkCmdSetBlendConstants(VkCommandBuffer commandBuffer, const float blendConstants[4]);
void WINAPI vkCmdBindVertexBuffers(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount,
    const VkBuffer *pBuffers, const VkDeviceSize *pOffsets);
void WINAPI vkCmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
                                 VkIndexType indexType);
void WINAPI vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount,
    uint32_t firstVertex, uint32_t firstInstance);
void WINAPI vkCmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
    uint32_t drawCount, uint32_t stride);
void WINAPI vkCmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount,
    uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance);
void WINAPI vkCmdDrawIndexedIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
    uint32_t drawCount, uint32_t stride);
void WINAPI vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout, uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet *pDescriptorSets,
    uint32_t dynamicOffsetCount, const uint32_t *pDynamicOffsets);
void WINAPI vkCmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout layout,
    VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size, const void *pValues);
void WINAPI vkCmdDispatch(VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY,
    uint32_t groupCountZ);
void WINAPI vkCmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset);
VkResult WINAPI vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence);
VkResult WINAPI vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo);
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
