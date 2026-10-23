// VXRuntime's own platform header for the Khronos Vulkan headers (vulkan_core.h includes it): the Vulkan commands
// and callbacks of Windows programs use the Windows x64 calling convention, which this macOS process calls ms_abi.
#ifndef VK_PLATFORM_H_
#define VK_PLATFORM_H_
#include <stddef.h>
#include <stdint.h>
#define VKAPI_ATTR
#define VKAPI_CALL __attribute__((ms_abi))
#define VKAPI_PTR  __attribute__((ms_abi))
#endif
