// Driver feature negotiation without a game or GPU.
#include <stdint.h>
#include <stddef.h>
#define VK_PLATFORM_H_
#define VKAPI_ATTR
#define VKAPI_CALL
#define VKAPI_PTR
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int kl_env_on(const char *name, int fallback) {
    const char *s = getenv(name); return s ? strcmp(s,"0") != 0 : fallback;
}
#include "../klepton-patches/runtime/kl_vk_compat.inc"
static int masks;
static void VKAPI_CALL features(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *out) {
    (void)pd;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT *d = out->pNext;
    assert(d->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT);
    d->extendedDynamicState3ColorWriteMask = masks;
}
static VkResult VKAPI_CALL enumerate(VkPhysicalDevice pd, const char *layer, uint32_t *count, VkExtensionProperties *out) {
    (void)pd; (void)layer;
    const char *names[] = {"VK_KHR_swapchain", VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, "VK_KHR_dynamic_rendering"};
    if (!out) { *count = 3; return VK_SUCCESS; }
    uint32_t n = *count < 3 ? *count : 3;
    for (uint32_t i = 0; i < n; i++) { memset(&out[i],0,sizeof out[i]); strcpy(out[i].extensionName,names[i]); out[i].specVersion = i+1; }
    *count = n; return n < 3 ? VK_INCOMPLETE : VK_SUCCESS;
}
int main(void) {
    setenv("KL_VK_ALYX_STATIC_COLOR_WRITE","1",1);
    uint32_t n = 0;
    assert(kvc_enumerate_extensions(enumerate,features,NULL,NULL,&n,NULL) == VK_SUCCESS && n == 2);
    VkExtensionProperties out[4]; memset(out,0x5a,sizeof out);
    n = 1;
    assert(kvc_enumerate_extensions(enumerate,features,NULL,NULL,&n,out) == VK_INCOMPLETE && n == 1);
    assert(out[1].specVersion == 0x5a5a5a5a);
    n = 4;
    assert(kvc_enumerate_extensions(enumerate,features,NULL,NULL,&n,out) == VK_SUCCESS && n == 2);
    assert(!strcmp(out[0].extensionName,"VK_KHR_swapchain") && out[0].specVersion == 1);
    assert(!strcmp(out[1].extensionName,"VK_KHR_dynamic_rendering") && out[1].specVersion == 3);
    assert(out[2].specVersion == 0x5a5a5a5a);
    n = 0;
    assert(kvc_enumerate_extensions(enumerate,features,NULL,NULL,&n,out) == VK_INCOMPLETE && n == 0);
    masks = 1; n = 0;
    assert(kvc_enumerate_extensions(enumerate,features,NULL,NULL,&n,NULL) == VK_SUCCESS && n == 3);
    masks = 0; setenv("KL_VK_ALYX_STATIC_COLOR_WRITE","0",1);
    assert(kvc_enumerate_extensions(enumerate,features,NULL,NULL,&n,NULL) == VK_SUCCESS && n == 3);
    puts("Vulkan compatibility: unsupported dynamic masks select static pipeline state; enumeration bounds and supported drivers retained");
}
