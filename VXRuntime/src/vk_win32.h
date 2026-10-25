// The Win32 parts of the Vulkan API that vulkan_core.h leaves to the platform header (VK_KHR_win32_surface,
// VK_KHR_external_memory_win32, VK_KHR_external_semaphore_win32), as the Vulkan specification defines them.
#ifndef VX_VK_WIN32_H
#define VX_VK_WIN32_H

#define VK_KHR_WIN32_SURFACE_EXTENSION_NAME "VK_KHR_win32_surface"
#define VK_KHR_WIN32_SURFACE_SPEC_VERSION 6
typedef VkFlags VkWin32SurfaceCreateFlagsKHR;
typedef struct VkWin32SurfaceCreateInfoKHR {
    VkStructureType sType;
    const void *pNext;
    VkWin32SurfaceCreateFlagsKHR flags;
    void *hinstance;
    void *hwnd;
} VkWin32SurfaceCreateInfoKHR;

#define VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME "VK_KHR_external_memory_win32"
#define VK_KHR_EXTERNAL_MEMORY_WIN32_SPEC_VERSION 1
typedef struct VkImportMemoryWin32HandleInfoKHR {
    VkStructureType sType;
    const void *pNext;
    VkExternalMemoryHandleTypeFlagBits handleType;
    void *handle;
    const uint16_t *name;
} VkImportMemoryWin32HandleInfoKHR;
typedef struct VkExportMemoryWin32HandleInfoKHR {
    VkStructureType sType;
    const void *pNext;
    const void *pAttributes;
    uint32_t dwAccess;
    const uint16_t *name;
} VkExportMemoryWin32HandleInfoKHR;
typedef struct VkMemoryWin32HandlePropertiesKHR {
    VkStructureType sType;
    void *pNext;
    uint32_t memoryTypeBits;
} VkMemoryWin32HandlePropertiesKHR;
typedef struct VkMemoryGetWin32HandleInfoKHR {
    VkStructureType sType;
    const void *pNext;
    VkDeviceMemory memory;
    VkExternalMemoryHandleTypeFlagBits handleType;
} VkMemoryGetWin32HandleInfoKHR;

#define VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME "VK_KHR_win32_keyed_mutex"
#define VK_KHR_WIN32_KEYED_MUTEX_SPEC_VERSION 1
typedef struct VkWin32KeyedMutexAcquireReleaseInfoKHR {
    VkStructureType sType;
    const void *pNext;
    uint32_t acquireCount;
    const VkDeviceMemory *pAcquireSyncs;
    const uint64_t *pAcquireKeys;
    const uint32_t *pAcquireTimeouts;
    uint32_t releaseCount;
    const VkDeviceMemory *pReleaseSyncs;
    const uint64_t *pReleaseKeys;
} VkWin32KeyedMutexAcquireReleaseInfoKHR;

VKAPI_ATTR VkResult VKAPI_CALL vkCreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR *pCreateInfo,
                                                      const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface);
VKAPI_ATTR VkBool32 VKAPI_CALL vkGetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex);
VKAPI_ATTR VkResult VKAPI_CALL vkGetMemoryWin32HandleKHR(VkDevice device, const VkMemoryGetWin32HandleInfoKHR *pGetWin32HandleInfo, void **pHandle);
VKAPI_ATTR VkResult VKAPI_CALL vkGetMemoryWin32HandlePropertiesKHR(VkDevice device, VkExternalMemoryHandleTypeFlagBits handleType, void *handle,
                                                                  VkMemoryWin32HandlePropertiesKHR *pMemoryWin32HandleProperties);

#endif
