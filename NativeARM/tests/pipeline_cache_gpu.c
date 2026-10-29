// Optional local MoltenVK check; synthetic shaders only, no game or headset.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <MoltenVK/mvk_private_api.h>
#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../../VXRuntime/tests/vulkan_shaders/fullscreen.vert.h"
#include "../../VXRuntime/tests/vulkan_shaders/solid.frag.h"

static double kl_pipe_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static int kl_env_on(const char *name, int fallback) {
    const char *value = getenv(name);
    return value ? strcmp(value, "0") != 0 : fallback;
}
#define VKI(...) fprintf(stderr, __VA_ARGS__)
typedef struct { VkDevice dev; } test_device;
static test_device device;
static test_device *g_devs[] = { &device };
static PFN_vkGetDeviceProcAddr real_gdpa;
#include "../klepton-patches/runtime/kl_vk_async.inc"
#include "../klepton-patches/runtime/kl_vk_compat.inc"

#define INSTANCE_FN(name) PFN_vk##name p##name = (PFN_vk##name)gipa(instance, "vk" #name); assert(p##name)
#define DEVICE_FN(name) PFN_vk##name p##name = (PFN_vk##name)real_gdpa(device.dev, "vk" #name); assert(p##name)
static void *g_gpu_library;
#include "shadow_depth_gpu.inc"

int main(int argc, char **argv) {
    assert(argc == 3 || argc == 4);
    alarm(argc == 4 ? 180 : 50);
    setenv("KL_VK_ASYNC_PIPELINES", "1", 1);
    setenv("KL_VK_ASYNC_THREADS", "1", 1);
    setenv("MVK_CONFIG_PERFORMANCE_TRACKING", "1", 1);
    setenv("MVK_CONFIG_LOG_LEVEL", "1", 1);
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); assert(library);
    g_gpu_library = library;
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr"); assert(gipa);
    PFN_vkGetPerformanceStatisticsMVK stats = (PFN_vkGetPerformanceStatisticsMVK)dlsym(library, "vkGetPerformanceStatisticsMVK"); assert(stats);
    VkInstance instance = VK_NULL_HANDLE;
    INSTANCE_FN(CreateInstance); INSTANCE_FN(EnumerateInstanceExtensionProperties);
    const char *extension = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    uint32_t extension_count = 0;
    assert(pEnumerateInstanceExtensionProperties(NULL, &extension_count, NULL) == VK_SUCCESS);
    VkExtensionProperties *extensions = calloc(extension_count, sizeof *extensions); assert(extensions);
    assert(pEnumerateInstanceExtensionProperties(NULL, &extension_count, extensions) == VK_SUCCESS);
    for (uint32_t i = 0; i < extension_count; i++) if (!strcmp(extensions[i].extensionName, extension)) {
        ici.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        ici.enabledExtensionCount = 1; ici.ppEnabledExtensionNames = &extension;
    }
    free(extensions);
    assert(pCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);
    INSTANCE_FN(EnumeratePhysicalDevices); INSTANCE_FN(GetPhysicalDeviceQueueFamilyProperties); INSTANCE_FN(CreateDevice); INSTANCE_FN(DestroyInstance);
    INSTANCE_FN(EnumerateDeviceExtensionProperties);
    INSTANCE_FN(GetPhysicalDeviceFeatures2);
    uint32_t n = 1; VkPhysicalDevice physical;
    assert(pEnumeratePhysicalDevices(instance, &n, &physical) == VK_SUCCESS && n);
    // Same early capability decision as the native host, against the real GPU.
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamic = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT
    };
    VkPhysicalDeviceFeatures2 supported = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &dynamic };
    pGetPhysicalDeviceFeatures2(physical, &supported);
    setenv("KL_VK_ALYX_STATIC_COLOR_WRITE", "1", 1);
    uint32_t compatible_count = 0;
    assert(kvc_enumerate_extensions(pEnumerateDeviceExtensionProperties, pGetPhysicalDeviceFeatures2,
        physical, NULL, &compatible_count, NULL) == VK_SUCCESS);
    VkExtensionProperties *compatible = calloc(compatible_count, sizeof *compatible); assert(compatible);
    assert(kvc_enumerate_extensions(pEnumerateDeviceExtensionProperties, pGetPhysicalDeviceFeatures2,
        physical, NULL, &compatible_count, compatible) == VK_SUCCESS);
    for (uint32_t i = 0; i < compatible_count; i++)
        if (!strcmp(compatible[i].extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME))
            assert(dynamic.extendedDynamicState3ColorWriteMask);
    free(compatible);
    printf("GPU colour state: dynamic channel masks %s; compatible extensions verified before device creation\n",
        dynamic.extendedDynamicState3ColorWriteMask ? "supported" : "unsupported, static variants required");
    n = 0; pGetPhysicalDeviceQueueFamilyProperties(physical, &n, NULL);
    VkQueueFamilyProperties *families = calloc(n, sizeof *families); assert(families);
    pGetPhysicalDeviceQueueFamilyProperties(physical, &n, families);
    uint32_t family = 0; while (family < n && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family++;
    assert(family < n); free(families);
    float priority = 1;
    VkDeviceQueueCreateInfo queue = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
        .queueCount = 1, .pQueuePriorities = &priority };
    const char *portability = "VK_KHR_portability_subset";
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue };
    extension_count = 0;
    assert(pEnumerateDeviceExtensionProperties(physical, NULL, &extension_count, NULL) == VK_SUCCESS);
    extensions = calloc(extension_count, sizeof *extensions); assert(extensions);
    assert(pEnumerateDeviceExtensionProperties(physical, NULL, &extension_count, extensions) == VK_SUCCESS);
    for (uint32_t i = 0; i < extension_count; i++) if (!strcmp(extensions[i].extensionName, portability)) {
        dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = &portability;
    }
    free(extensions);
    assert(pCreateDevice(physical, &dci, NULL, &device.dev) == VK_SUCCESS);
    real_gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr"); assert(real_gdpa);
    DEVICE_FN(CreateShaderModule); DEVICE_FN(DestroyShaderModule); DEVICE_FN(CreatePipelineLayout); DEVICE_FN(DestroyPipelineLayout);
    DEVICE_FN(CreateRenderPass); DEVICE_FN(DestroyRenderPass); DEVICE_FN(CreateGraphicsPipelines); DEVICE_FN(DestroyDevice);
    VkPipelineLayout layout;
    VkPipelineLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    assert(pCreatePipelineLayout(device.dev, &lci, NULL, &layout) == VK_SUCCESS);
    VkAttachmentDescription attachment = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &color };
    VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
        .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass };
    VkRenderPass pass; assert(pCreateRenderPass(device.dev, &rpci, NULL, &pass) == VK_SUCCESS);
    VkPipelineVertexInputStateCreateInfo vertex = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport viewport = { 0, 0, 4, 4, 0, 1 }; VkRect2D scissor = { { 0, 0 }, { 4, 4 } };
    VkPipelineViewportStateCreateInfo view = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor };
    VkPipelineRasterizationStateCreateInfo raster = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo samples = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState blend = { .colorWriteMask = 15 };
    VkPipelineColorBlendStateCreateInfo colors = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend };
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .pName = "main" }
    };
    VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex, .pInputAssemblyState = &assembly,
        .pViewportState = &view, .pRasterizationState = &raster, .pMultisampleState = &samples,
        .pColorBlendState = &colors, .layout = layout, .renderPass = pass, .basePipelineIndex = -1 };
    void *data = NULL; size_t size = 0;
    char archive[4096], archive_file[4096];
    snprintf(archive, sizeof archive, "%s.archive", argv[2]);
    snprintf(archive_file, sizeof archive_file, "%s.0", archive);
    unlink(archive_file);
    uint32_t conversions[4]; double elapsed[4];
    for (int phase = 0; phase < 4; phase++) {
        if (phase >= 2) setenv("KL_VK_PRIVATE_CACHE","1",1);
        else unsetenv("KL_VK_PRIVATE_CACHE");
        if (phase == 1) unsetenv("KL_VK_PIPELINE_ARCHIVE");
        else {
            setenv("KL_VK_PIPELINE_ARCHIVE", archive, 1);
            // This check measures reuse after restoration. The controlled
            // async test separately proves draws progress during restoration.
            pthread_mutex_lock(&g_kva_archive_mu);
            kva_archive *restored = kva_archive_locked(device.dev); assert(restored);
            while (restored->loading) pthread_cond_wait(&g_kva_archive_ready, &g_kva_archive_mu);
            pthread_mutex_unlock(&g_kva_archive_mu);
        }
        // Recreate shader modules so the second phase cannot hit their local cache.
        VkShaderModuleCreateInfo sm = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof spv_fullscreen_vert, .pCode = spv_fullscreen_vert };
        assert(pCreateShaderModule(device.dev, &sm, NULL, &stages[0].module) == VK_SUCCESS);
        sm.codeSize = sizeof spv_solid_frag; sm.pCode = spv_solid_frag;
        assert(pCreateShaderModule(device.dev, &sm, NULL, &stages[1].module) == VK_SUCCESS);
        VkPipelineCache cache;
        VkPipelineCacheCreateInfo cci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            .initialDataSize = phase == 1 ? size : 0, .pInitialData = phase == 1 ? data : NULL };
        assert(klvk_CreatePipelineCache(device.dev, &cci, NULL, &cache) == VK_SUCCESS);
        size_t empty_size=0;
        PFN_vkGetPipelineCacheData raw_cache_data=(PFN_vkGetPipelineCacheData)real_gdpa(device.dev,"vkGetPipelineCacheData");
        if (phase >= 2) assert(raw_cache_data(device.dev,cache,&empty_size,NULL)==VK_SUCCESS);
        MVKPerformanceStatistics before = {0}, after = {0}; size_t stat_size = sizeof before;
        assert(stats(device.dev, &before, &stat_size) == VK_SUCCESS);
        VkPipeline pipeline; double start = kl_pipe_ms();
        assert(kva_create(device.dev, phase == 3 ? VK_NULL_HANDLE : cache, 1, &ci, &pipeline, pCreateGraphicsPipelines));
        pthread_mutex_lock(&g_kva_mu);
        while (g_kva_pending || g_kva_finishing) pthread_cond_wait(&g_kva_ready, &g_kva_mu);
        pthread_mutex_unlock(&g_kva_mu);
        kl_vulkan_async_begin_frame();
        assert(kva_bind(&ci, VK_PIPELINE_BIND_POINT_GRAPHICS, (uint64_t)pipeline));
        elapsed[phase] = kl_pipe_ms() - start;
        stat_size = sizeof after; assert(stats(device.dev, &after, &stat_size) == VK_SUCCESS);
        conversions[phase] = after.shaderCompilation.spirvToMSL.count - before.shaderCompilation.spirvToMSL.count;
        if (phase >= 2) {
            size_t exported_size=0;
            assert(klvk_GetPipelineCacheData(device.dev,cache,&exported_size,NULL)==VK_SUCCESS);
            assert(exported_size==empty_size); // compiled shaders live in the archive
        }
        if (!phase) {
            assert(klvk_GetPipelineCacheData(device.dev, cache, &size, NULL) == VK_SUCCESS && size > 32);
            data = malloc(size); assert(data);
            assert(klvk_GetPipelineCacheData(device.dev, cache, &size, data) == VK_SUCCESS);
            FILE *file = fopen(argv[2], "wb"); assert(file);
            assert(fwrite(data, 1, size, file) == size); assert(!fclose(file));
            free(data); data = malloc(size); assert(data);
            file = fopen(argv[2], "rb"); assert(file);
            assert(fread(data, 1, size, file) == size); assert(!fclose(file));
        }
        assert(kva_destroy_pipeline(device.dev, (uint64_t)pipeline,
            (PFN_vkDestroyPipeline)real_gdpa(device.dev, "vkDestroyPipeline")));
        klvk_DestroyPipelineCache(device.dev, (uint64_t)cache, NULL);
        for (int i = 0; i < 2; i++) pDestroyShaderModule(device.dev, stages[i].module, NULL);
        kva_archive_close(device.dev); // force a disk reload, with fresh shader modules, on the next pass
    }
    assert(conversions[0] > 0 && conversions[1] == 0 && conversions[2] == 0 && conversions[3] == 0);
    printf("Persistent cache: empty engine cache and null cache both reuse disk shaders (%u / %u conversions)\n", conversions[2], conversions[3]);
    unsetenv("KL_VK_PIPELINE_ARCHIVE");
    unsetenv("KL_VK_PRIVATE_CACHE");
    printf("{\"cache_bytes\":%zu,\"cold_conversions\":%u,\"warm_conversions\":%u,\"cold_ms\":%.3f,\"warm_ms\":%.3f}\n",
        size, conversions[0], conversions[1], elapsed[0], elapsed[1]);
    test_shadow_depth(physical, family, &ci);
    if (argc == 4) {
        // Optional read-only reproduction with an existing large archive.
        // Atomic saves replace a private symlink, never the source cache.
        char directory[4096], source[4096], cache_path[4096], cache_file[4096];
        snprintf(directory, sizeof directory, "%s.import.XXXXXX", argv[2]);
        assert(mkdtemp(directory) && realpath(argv[3], source));
        snprintf(cache_path, sizeof cache_path, "%s/cache", directory);
        snprintf(cache_file, sizeof cache_file, "%s.0", cache_path);
        assert(!symlink(source, cache_file));
        setenv("KL_VK_PIPELINE_ARCHIVE", cache_path, 1);
        double start = kl_pipe_ms();
        pthread_mutex_lock(&g_kva_archive_mu);
        kva_archive *restoring = kva_archive_locked(device.dev); assert(restoring);
        pthread_mutex_unlock(&g_kva_archive_mu);
        test_shadow_depth(physical, family, &ci);
        pthread_mutex_lock(&g_kva_archive_mu);
        int still_loading = restoring->loading;
        pthread_mutex_unlock(&g_kva_archive_mu);
        fprintf(stderr, "GPU draw/readback completed in %.1f ms; archive import still running: %s\n",
            kl_pipe_ms() - start, still_loading ? "yes" : "no");
        kva_archive_close(device.dev);
        unsetenv("KL_VK_PIPELINE_ARCHIVE");
        assert(!unlink(cache_file)); assert(!rmdir(directory));
    }
    free(data); pDestroyRenderPass(device.dev, pass, NULL); pDestroyPipelineLayout(device.dev, layout, NULL);
    pDestroyDevice(device.dev, NULL); pDestroyInstance(instance, NULL);
    return 0;
}
