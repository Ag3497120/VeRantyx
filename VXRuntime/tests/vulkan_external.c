// Vulkan images shared with Direct3D 11 through Win32 handles (VK_KHR_external_memory_win32, src/vk_device.m), as
// SteamVR's client shares a Vulkan game's eye images with its compositor: the Vulkan device's LUID is the DXGI
// adapter's; a Direct3D 11 shared texture (made-up texels) imported into a Vulkan image reads back the same; a Vulkan
// image made for export and cleared on the GPU is opened by Direct3D 11 with that handle and reads back the clear
// colour; a keyed-mutex texture is handed over both ways (VK_KHR_win32_keyed_mutex: Direct3D releases with key 1,
// Vulkan's submission takes it with 1, copies, and gives it back with 0, which Direct3D then takes). No Man's Sky in VR showed nothing: "Driver lacks support for KHR vulkan interop" (vrclient).
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <string.h>
#include "vulkan/vulkan_core.h"

typedef struct { VkStructureType sType; const void *pNext; VkExternalMemoryHandleTypeFlagBits handleType; HANDLE handle; LPCWSTR name; } ImportWin32;
typedef struct { VkStructureType sType; const void *pNext; VkDeviceMemory memory; VkExternalMemoryHandleTypeFlagBits handleType; } GetWin32Info;
typedef VkResult (VKAPI_PTR *PFN_GetWin32)(VkDevice, const GetWin32Info *, HANDLE *);
typedef struct { VkStructureType sType; const void *pNext; uint32_t acquireCount; const VkDeviceMemory *pAcquireSyncs; const uint64_t *pAcquireKeys;
                 const uint32_t *pAcquireTimeouts; uint32_t releaseCount; const VkDeviceMemory *pReleaseSyncs; const uint64_t *pReleaseKeys; } KeyedMutexInfo;

static int bad;
#define CHECK(x) do { if (!(x)) { printf("vulkan_external: check failed (line %d): %s\n", __LINE__, #x); bad++; } } while (0)
static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static VkCommandPool pool;

static uint32_t memory_type(uint32_t bits, VkFlags want) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(gpu, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}
static VkImage external_image(VkExternalMemoryHandleTypeFlagBits type) {
    VkExternalMemoryImageCreateInfo ext = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL, type };
    VkImageCreateInfo info = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &ext, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { 4, 4, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = 1,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT };
    VkImage img = 0;
    CHECK(vkCreateImage(device, &info, NULL, &img) == VK_SUCCESS);
    return img;
}
static VkCommandBuffer begin(void) {
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(device, &ai, &cb);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cb, &bi);
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    return cb;
}
static void submit(VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
}

int main(void) {
    // Direct3D 11: the adapter's LUID, and a shared texture of made-up texels.
    ID3D11Device *d3d; ID3D11DeviceContext *ctx;
    CHECK(SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &d3d, NULL, &ctx)));
    if (bad) return 1;
    IDXGIDevice *dxgi; IDXGIAdapter *adapter; DXGI_ADAPTER_DESC ad;
    ID3D11Device_QueryInterface(d3d, &IID_IDXGIDevice, (void **)&dxgi);
    IDXGIDevice_GetAdapter(dxgi, &adapter);
    IDXGIAdapter_GetDesc(adapter, &ad);
    uint32_t texels[16];
    for (int i = 0; i < 16; i++) texels[i] = 0x01020304u * (uint32_t)(i + 1) ^ 0x80402010u;
    D3D11_TEXTURE2D_DESC td = { 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, D3D11_RESOURCE_MISC_SHARED };
    D3D11_SUBRESOURCE_DATA sd = { texels, 16, 0 };
    ID3D11Texture2D *shared; HANDLE h = NULL;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(d3d, &td, &sd, &shared)));
    IDXGIResource *res;
    ID3D11Texture2D_QueryInterface(shared, &IID_IDXGIResource, (void **)&res);
    CHECK(SUCCEEDED(IDXGIResource_GetSharedHandle(res, &h)) && h);
    ID3D11DeviceContext_Flush(ctx);

    // Vulkan: the device of that LUID, with external memory.
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst;
    CHECK(vkCreateInstance(&ii, NULL, &inst) == VK_SUCCESS);
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &gpu);
    VkPhysicalDeviceIDProperties idp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp };
    vkGetPhysicalDeviceProperties2(gpu, &p2);
    CHECK(idp.deviceLUIDValid && !memcmp(idp.deviceLUID, &ad.AdapterLuid, 8));
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &ne, NULL);
    VkExtensionProperties ep[256];
    if (ne > 256) ne = 256;
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &ne, ep);
    int has = 0;
    for (uint32_t i = 0; i < ne; i++) if (!strcmp(ep[i].extensionName, "VK_KHR_external_memory_win32")) has = 1;
    CHECK(has);
    VkPhysicalDeviceExternalImageFormatInfo xi = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT };
    VkPhysicalDeviceImageFormatInfo2 fi = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &xi, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                                            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0 };
    VkExternalImageFormatProperties xp = { VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
    VkImageFormatProperties2 fp = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &xp };
    CHECK(vkGetPhysicalDeviceImageFormatProperties2(gpu, &fi, &fp) == VK_SUCCESS);
    CHECK(xp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
    float prio = 1;
    VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
    const char *exts[] = { "VK_KHR_external_memory", "VK_KHR_external_memory_win32", "VK_KHR_win32_keyed_mutex" };
    VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
                              .enabledExtensionCount = 3, .ppEnabledExtensionNames = exts };
    CHECK(vkCreateDevice(gpu, &di, NULL, &device) == VK_SUCCESS);
    if (bad) return 1;
    vkGetDeviceQueue(device, 0, 0, &queue);
    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    vkCreateCommandPool(device, &pi, NULL, &pool);
    PFN_GetWin32 get_handle = (PFN_GetWin32)vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR");
    CHECK(get_handle != NULL);
    VkBuffer rb; VkDeviceMemory rbm; void *map;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    vkCreateBuffer(device, &bci, NULL, &rb);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, rb, &mr);
    VkMemoryAllocateInfo bai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) };
    vkAllocateMemory(device, &bai, NULL, &rbm);
    vkBindBufferMemory(device, rb, rbm, 0);
    vkMapMemory(device, rbm, 0, 64, 0, &map);
    VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 4, 4, 1 } };

    // 1. Import: the Direct3D texture as a Vulkan image.
    VkImage in = external_image(VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT);
    vkGetImageMemoryRequirements(device, in, &mr);
    VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, in, VK_NULL_HANDLE };
    ImportWin32 imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &ded, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT, h, NULL };
    VkMemoryAllocateInfo iai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, mr.size, memory_type(mr.memoryTypeBits, 0) };
    VkDeviceMemory inm = 0;
    CHECK(vkAllocateMemory(device, &iai, NULL, &inm) == VK_SUCCESS);
    CHECK(vkBindImageMemory(device, in, inm, 0) == VK_SUCCESS);
    VkCommandBuffer cb = begin();
    vkCmdCopyImageToBuffer(cb, in, VK_IMAGE_LAYOUT_GENERAL, rb, 1, &region);
    submit(cb);
    CHECK(!memcmp(map, texels, sizeof texels));
    printf("vulkan_external: Direct3D 11 shared texture imported: %s\n", memcmp(map, texels, sizeof texels) ? "wrong texels" : "same texels");

    // 2. Export: a Vulkan image cleared on the GPU, opened by Direct3D 11.
    VkImage out = external_image(VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT);
    vkGetImageMemoryRequirements(device, out, &mr);
    VkMemoryDedicatedAllocateInfo ded2 = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, out, VK_NULL_HANDLE };
    VkExportMemoryAllocateInfo exp = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded2, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT };
    VkMemoryAllocateInfo eai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp, mr.size, memory_type(mr.memoryTypeBits, 0) };
    VkDeviceMemory outm = 0;
    CHECK(vkAllocateMemory(device, &eai, NULL, &outm) == VK_SUCCESS);
    CHECK(vkBindImageMemory(device, out, outm, 0) == VK_SUCCESS);
    cb = begin();
    VkClearColorValue c = { .float32 = { 1.0f, 0.5f, 0.0f, 1.0f } };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, out, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &range);
    submit(cb);
    HANDLE oh = NULL;
    GetWin32Info gi = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, NULL, outm, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT };
    CHECK(get_handle && get_handle(device, &gi, &oh) == VK_SUCCESS && oh);
    ID3D11Texture2D *opened = NULL, *staging = NULL;
    CHECK(SUCCEEDED(ID3D11Device_OpenSharedResource(d3d, oh, &IID_ID3D11Texture2D, (void **)&opened)));
    if (opened) {
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        ID3D11Device_CreateTexture2D(d3d, &td, NULL, &staging);
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)opened);
        D3D11_MAPPED_SUBRESOURCE m;
        CHECK(SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m)));
        const uint8_t *px = m.pData;
        int ok = px[0] == 255 && (px[1] == 127 || px[1] == 128) && px[2] == 0 && px[3] == 255;
        CHECK(ok);
        printf("vulkan_external: Vulkan image exported, read by Direct3D 11: %d %d %d %d\n", px[0], px[1], px[2], px[3]);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    }

    // 3. A keyed mutex: Direct3D clears and releases with key 1, Vulkan takes it with 1, copies, releases with 0.
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; td.CPUAccessFlags = 0;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    ID3D11Texture2D *kt; HANDLE kh = NULL;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(d3d, &td, NULL, &kt)));
    IDXGIResource *kres; IDXGIKeyedMutex *kmx; ID3D11RenderTargetView *rtv;
    ID3D11Texture2D_QueryInterface(kt, &IID_IDXGIResource, (void **)&kres);
    IDXGIResource_GetSharedHandle(kres, &kh);
    ID3D11Texture2D_QueryInterface(kt, &IID_IDXGIKeyedMutex, (void **)&kmx);
    ID3D11Device_CreateRenderTargetView(d3d, (ID3D11Resource *)kt, NULL, &rtv);
    CHECK(IDXGIKeyedMutex_AcquireSync(kmx, 0, 1000) == S_OK);
    const float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, green);
    CHECK(IDXGIKeyedMutex_ReleaseSync(kmx, 1) == S_OK);
    VkImage ki = external_image(VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT);
    vkGetImageMemoryRequirements(device, ki, &mr);
    VkMemoryDedicatedAllocateInfo ded3 = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, ki, VK_NULL_HANDLE };
    ImportWin32 imp3 = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &ded3, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT, kh, NULL };
    VkMemoryAllocateInfo kai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp3, mr.size, memory_type(mr.memoryTypeBits, 0) };
    VkDeviceMemory kmem = 0;
    CHECK(vkAllocateMemory(device, &kai, NULL, &kmem) == VK_SUCCESS);
    CHECK(vkBindImageMemory(device, ki, kmem, 0) == VK_SUCCESS);
    memset(map, 0, 64);
    cb = begin();
    vkCmdCopyImageToBuffer(cb, ki, VK_IMAGE_LAYOUT_GENERAL, rb, 1, &region);
    vkEndCommandBuffer(cb);
    uint64_t akey = 1, rkey = 0;
    uint32_t timeout = 1000;
    KeyedMutexInfo kmi = { 1000075000 /* VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR */, NULL, 1, &kmem, &akey, &timeout, 1, &kmem, &rkey };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &kmi, .commandBufferCount = 1, .pCommandBuffers = &cb };
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS);
    CHECK(IDXGIKeyedMutex_AcquireSync(kmx, 0, 2000) == S_OK);    // (given back by Vulkan with key 0, once its copy is done)
    vkQueueWaitIdle(queue);
    const uint8_t *g = map;
    CHECK(g[0] == 0 && g[1] == 255 && g[2] == 0 && g[3] == 255);
    printf("vulkan_external: keyed mutex handed over Direct3D -> Vulkan -> Direct3D, texel %d %d %d %d\n", g[0], g[1], g[2], g[3]);
    IDXGIKeyedMutex_ReleaseSync(kmx, 0);

    vkDeviceWaitIdle(device);
    printf("vulkan external memory (Win32 handles with Direct3D 11): %s\n", bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}
