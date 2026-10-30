// Controlled compiler and draw callbacks: no game, GPU, Steam or downloads.
#include <stddef.h>
#include <stdint.h>
// Reuse the bundled Khronos declarations with the native, rather than Windows, ABI.
#define VK_PLATFORM_H_
#define VKAPI_ATTR
#define VKAPI_CALL
#define VKAPI_PTR
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double kl_pipe_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static int kl_env_on(const char *name, int fallback) {
    const char *value = getenv(name);
    return value ? strcmp(value, "0") != 0 : fallback;
}
#define VKI(...) fprintf(stderr, __VA_ARGS__)

typedef struct { VkDevice dev; } fake_device;
static fake_device test_device;
static fake_device *g_devs[] = { &test_device };
static pthread_mutex_t test_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_cv = PTHREAD_COND_INITIALIZER;
static int compiling, release_compile, bind_entered, bind_done, draws, destroyed;
static VkResult compile_result;
static uint64_t real_pipeline;  // not a placeholder
static uint64_t test_cache;
static uint64_t other_cache;
static int cache_created, cache_destroyed, cache_reads, cache_merges, query_entered, query_done, compile_count;
static int compile_order[16];
static unsigned char cache_value;
static int private_created, private_destroyed;
static int archive_import_entered, release_archive_import;
static int block_public_publish, public_publish_merges;
static int block_engine_seed, engine_seeds;
static int block_private_create, private_create_entered;
#define PRIVATE_CACHE_MAGIC 0x5052495643414348ull

static int is_private_cache(VkPipelineCache cache) {
    return cache && cache != (VkPipelineCache)&test_cache && cache != (VkPipelineCache)&other_cache &&
        *(uint64_t *)cache == PRIVATE_CACHE_MAGIC;
}

static VkResult VKAPI_CALL fake_create(VkDevice dev, VkPipelineCache cache, uint32_t n,
        const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *alloc, VkPipeline *out) {
    (void)dev; (void)alloc;
    assert(n == 1);
    assert(is_private_cache(cache));
    pthread_mutex_lock(&test_mu);
    compiling = 1;
    pthread_cond_broadcast(&test_cv);
    while (!release_compile) pthread_cond_wait(&test_cv, &test_mu);
    // The engine has already reused its original creation structures.
    assert(ci->pDepthStencilState->depthWriteEnable == VK_TRUE);
    assert(ci->stageCount >= 1 && !strcmp(ci->pStages[0].pName, "shadow"));
    VkResult result = compile_result;
    compile_order[compile_count++] = (int)ci->subpass;
    pthread_mutex_unlock(&test_mu);
    *out = result == VK_SUCCESS ? (VkPipeline)&real_pipeline : VK_NULL_HANDLE;
    return result;
}
static void VKAPI_CALL fake_destroy(VkDevice dev, VkPipeline pipeline, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    assert(pipeline == (VkPipeline)&real_pipeline);
    destroyed++;
}
static VkResult VKAPI_CALL fake_create_cache(VkDevice dev, const VkPipelineCacheCreateInfo *ci,
        const VkAllocationCallbacks *alloc, VkPipelineCache *out) {
    (void)dev; (void)alloc;
    assert(!(ci->flags & VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT));
    pthread_mutex_lock(&test_mu);
    if (block_private_create) {
        private_create_entered++; pthread_cond_broadcast(&test_cv);
        while (block_private_create) pthread_cond_wait(&test_cv,&test_mu);
    }
    if (ci->initialDataSize) {
        archive_import_entered++;
        pthread_cond_broadcast(&test_cv);
        while (!release_archive_import) pthread_cond_wait(&test_cv, &test_mu);
    }
    if (cache_created++ == 0) *out = (VkPipelineCache)&test_cache;
    else {
        uint64_t *private_cache = malloc(sizeof *private_cache); assert(private_cache);
        *private_cache = PRIVATE_CACHE_MAGIC; *out = (VkPipelineCache)private_cache; private_created++;
    }
    pthread_mutex_unlock(&test_mu);
    return VK_SUCCESS;
}
static void VKAPI_CALL fake_destroy_cache(VkDevice dev, VkPipelineCache cache, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    pthread_mutex_lock(&test_mu);
    if (cache == (VkPipelineCache)&test_cache) cache_destroyed++;
    else { assert(is_private_cache(cache)); private_destroyed++; free(cache); }
    pthread_mutex_unlock(&test_mu);
}
static VkResult VKAPI_CALL fake_cache_data(VkDevice dev, VkPipelineCache cache, size_t *size, void *data) {
    (void)dev;
    assert(cache == (VkPipelineCache)&test_cache || is_private_cache(cache));
    pthread_mutex_lock(&test_mu);
    cache_reads++;
    if (data) { assert(*size >= 32); memset(data, cache_value, 32); }
    *size = 32;
    pthread_mutex_unlock(&test_mu);
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL fake_merge_caches(VkDevice dev, VkPipelineCache dst, uint32_t n, const VkPipelineCache *src) {
    (void)dev;
    assert(n == 1);
    if (is_private_cache(dst)) {
        if (src[0] == (VkPipelineCache)&test_cache) {
            pthread_mutex_lock(&test_mu); engine_seeds++;
            while (block_engine_seed) pthread_cond_wait(&test_cv,&test_mu);
            pthread_mutex_unlock(&test_mu);
        }
        assert(src[0] == (VkPipelineCache)&test_cache || is_private_cache(src[0])); return VK_SUCCESS;
    }
    if (is_private_cache(src[0])) {
        assert(dst == (VkPipelineCache)&test_cache);
        pthread_mutex_lock(&test_mu); public_publish_merges++;
        while (block_public_publish) pthread_cond_wait(&test_cv,&test_mu);
        pthread_mutex_unlock(&test_mu); return VK_SUCCESS;
    }
    assert((dst == (VkPipelineCache)&other_cache && src[0] == (VkPipelineCache)&test_cache) ||
           (dst == (VkPipelineCache)&test_cache && src[0] == (VkPipelineCache)&other_cache));
    pthread_mutex_lock(&test_mu);
    cache_merges++;
    pthread_mutex_unlock(&test_mu);
    return VK_SUCCESS;
}
static void fake_draw(void *cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    (void)cb; (void)vc; (void)ic; (void)fv; (void)fi;
    pthread_mutex_lock(&test_mu); draws++; pthread_mutex_unlock(&test_mu);
}
static PFN_vkVoidFunction VKAPI_CALL fake_gdpa(VkDevice dev, const char *name) {
    (void)dev;
    if (!strcmp(name, "vkDestroyPipeline")) return (PFN_vkVoidFunction)fake_destroy;
    if (!strcmp(name, "vkCmdDraw")) return (PFN_vkVoidFunction)fake_draw;
    if (!strcmp(name, "vkCreatePipelineCache")) return (PFN_vkVoidFunction)fake_create_cache;
    if (!strcmp(name, "vkDestroyPipelineCache")) return (PFN_vkVoidFunction)fake_destroy_cache;
    if (!strcmp(name, "vkGetPipelineCacheData")) return (PFN_vkVoidFunction)fake_cache_data;
    if (!strcmp(name, "vkMergePipelineCaches")) return (PFN_vkVoidFunction)fake_merge_caches;
    return NULL;
}
static PFN_vkGetDeviceProcAddr real_gdpa = fake_gdpa;

#include "../klepton-patches/runtime/kl_vk_async.inc"

static struct timespec deadline(long ms) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    t.tv_nsec += ms * 1000000;
    t.tv_sec += t.tv_nsec / 1000000000; t.tv_nsec %= 1000000000;
    return t;
}
static void wait_for(int *counter, int value) {
    struct timespec end = deadline(5000);
    while (*counter < value) assert(pthread_cond_timedwait(&test_cv, &test_mu, &end) == 0);
}

typedef struct { void *cb; uint64_t pipeline, bound; } bind_request;
static void *bind_and_draw(void *arg) {
    bind_request *request = arg;
    pthread_mutex_lock(&test_mu);
    bind_entered++; pthread_cond_broadcast(&test_cv);
    pthread_mutex_unlock(&test_mu);
    request->bound = kva_bind(request->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, request->pipeline);
    if (request->bound) klvk_CmdDraw(request->cb, 3, 1, 0, 0);
    pthread_mutex_lock(&test_mu);
    bind_done++; pthread_cond_broadcast(&test_cv);
    pthread_mutex_unlock(&test_mu);
    return NULL;
}
static void *query_cache(void *arg) {
    pthread_mutex_lock(&test_mu);
    query_entered++; pthread_cond_broadcast(&test_cv);
    pthread_mutex_unlock(&test_mu);
    if ((uintptr_t)arg == 0) {
        size_t size;
        assert(klvk_GetPipelineCacheData(test_device.dev, (VkPipelineCache)&test_cache, &size, NULL) == VK_SUCCESS);
        assert(size > 0);
    } else {
        VkPipelineCache cache = (uintptr_t)arg == 1 ? (VkPipelineCache)&test_cache : (VkPipelineCache)&other_cache;
        VkPipelineCache target = (uintptr_t)arg == 1 ? (VkPipelineCache)&other_cache : (VkPipelineCache)&test_cache;
        assert(klvk_MergePipelineCaches(test_device.dev, target, 1, &cache) == VK_SUCCESS);
    }
    pthread_mutex_lock(&test_mu);
    query_done++; pthread_cond_broadcast(&test_cv);
    pthread_mutex_unlock(&test_mu);
    return NULL;
}

static void test_pending_bind(VkResult result, int destroy_cache, int colour) {
    pthread_mutex_lock(&test_mu);
    compiling = release_compile = bind_entered = bind_done = query_entered = query_done = 0;
    compile_result = result;
    int previous_draws = draws;
    int previous_destroyed = cache_destroyed;
    pthread_mutex_unlock(&test_mu);

    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE
    };
    char name[] = "shadow";
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = (VkShaderModule)1, .pName = name },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = (VkShaderModule)2, .pName = name }
    };
    VkPipelineColorBlendAttachmentState attachment = { .colorWriteMask = 15 };
    VkPipelineColorBlendStateCreateInfo blend = { .attachmentCount = 1, .pAttachments = &attachment };
    VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = colour ? 2 : 1, .pStages = stages, .pDepthStencilState = &depth,
        .pColorBlendState = colour ? &blend : NULL };
    VkPipeline pipeline;
    assert(kva_create(test_device.dev, (VkPipelineCache)&test_cache, 1, &ci, &pipeline, fake_create));
    pthread_mutex_lock(&test_mu);
    wait_for(&compiling, 1);
    memset(name, 'x', sizeof name); depth.depthWriteEnable = VK_FALSE;
    pthread_mutex_unlock(&test_mu);

    pthread_t queries[3];
    if (destroy_cache) {
        klvk_DestroyPipelineCache(test_device.dev, (uint64_t)&test_cache, NULL);
        assert(cache_destroyed == previous_destroyed);
    } else {
        for (int i = 0; i < 3; i++) assert(!pthread_create(&queries[i], NULL, query_cache, (void *)(uintptr_t)i));
        pthread_mutex_lock(&test_mu); wait_for(&query_done, 3); pthread_mutex_unlock(&test_mu);
    }

    // Shadow pass and both eyes must all return without a draw, even if they
    // bind the same pending pipeline on different recording threads.
    bind_request requests[3]; pthread_t threads[3];
    for (int i = 0; i < 3; i++) {
        requests[i] = (bind_request){ .cb = &requests[i], .pipeline = (uint64_t)pipeline };
        assert(!pthread_create(&threads[i], NULL, bind_and_draw, &requests[i]));
    }
    pthread_mutex_lock(&test_mu);
    wait_for(&bind_done, 3); // compilation is still deliberately blocked
    assert(!release_compile && draws == previous_draws);
    pthread_mutex_unlock(&test_mu);
    for (int i = 0; i < 3; i++) {
        assert(!pthread_join(threads[i], NULL));
        assert(!requests[i].bound && kva_skip(requests[i].cb));
    }

    // Even a busy job queue cannot block a pending bind.
    pthread_mutex_lock(&g_kva_mu);
    assert(!kva_bind(&requests[0], VK_PIPELINE_BIND_POINT_GRAPHICS, (uint64_t)pipeline));
    pthread_mutex_unlock(&g_kva_mu);
    pthread_mutex_lock(&test_mu);
    release_compile = 1; pthread_cond_broadcast(&test_cv);
    pthread_mutex_unlock(&test_mu);
    pthread_mutex_lock(&g_kva_mu);
    while (g_kva_pending || g_kva_finishing) pthread_cond_wait(&g_kva_ready, &g_kva_mu);
    pthread_mutex_unlock(&g_kva_mu);

    // A completion between the eyes does not change this frame's draw set.
    for (int i = 0; i < 3; i++) {
        assert(!kva_bind(requests[i].cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (uint64_t)pipeline));
        klvk_CmdDraw(requests[i].cb, 3, 1, 0, 0);
    }
    assert(draws == previous_draws);
    kl_vulkan_async_begin_frame();
    for (int i = 0; i < 3; i++) {
        uint64_t bound = kva_bind(requests[i].cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (uint64_t)pipeline);
        if (result == VK_SUCCESS) {
            assert(bound == (uint64_t)&real_pipeline && !kva_skip(requests[i].cb));
        } else {
            assert(!bound && kva_skip(requests[i].cb));
            assert(kva_bind(requests[i].cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                (uint64_t)&real_pipeline) == (uint64_t)&real_pipeline);
        }
        klvk_CmdDraw(requests[i].cb, 3, 1, 0, 0);
    }
    assert(draws == previous_draws + 3);
    if (destroy_cache) assert(cache_destroyed == previous_destroyed + 1);
    else { for (int i = 0; i < 3; i++) assert(!pthread_join(queries[i], NULL)); assert(query_done == 3); }
    assert(kva_destroy_pipeline(test_device.dev, (uint64_t)pipeline, fake_destroy));
}

static void test_demand_priority(void) {
    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthWriteEnable = VK_TRUE
    };
    VkPipelineShaderStageCreateInfo stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = (VkShaderModule)1, .pName = "shadow" };
    VkGraphicsPipelineCreateInfo ci[3]; VkPipeline pipelines[3];
    for (int i = 0; i < 3; i++) ci[i] = (VkGraphicsPipelineCreateInfo){
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 1,
        .pStages = &stage, .pDepthStencilState = &depth, .subpass = (uint32_t)i };
    pthread_mutex_lock(&test_mu);
    compiling = release_compile = bind_entered = bind_done = compile_count = 0; compile_result = VK_SUCCESS;
    pthread_mutex_unlock(&test_mu);
    assert(kva_create(test_device.dev, (VkPipelineCache)&test_cache, 3, ci, pipelines, fake_create));
    pthread_mutex_lock(&test_mu); wait_for(&compiling, 1); pthread_mutex_unlock(&test_mu);
    bind_request request = { .cb = &request, .pipeline = (uint64_t)pipelines[2] };
    pthread_t thread; assert(!pthread_create(&thread, NULL, bind_and_draw, &request));
    pthread_mutex_lock(&test_mu);
    wait_for(&bind_done, 1);
    assert(!request.bound);
    release_compile = 1; pthread_cond_broadcast(&test_cv);
    pthread_mutex_unlock(&test_mu);
    assert(!pthread_join(thread, NULL));
    pthread_mutex_lock(&g_kva_mu);
    while (g_kva_pending || g_kva_finishing) pthread_cond_wait(&g_kva_ready, &g_kva_mu);
    pthread_mutex_unlock(&g_kva_mu);
    kl_vulkan_async_begin_frame();
    // The third pipeline is needed by a draw; the second is only warmup work.
    assert(compile_order[0] == 0 && compile_order[1] == 2);
    for (int i = 0; i < 3; i++) {
        assert(kva_bind(&request, VK_PIPELINE_BIND_POINT_GRAPHICS, (uint64_t)pipelines[i]));
        assert(kva_destroy_pipeline(test_device.dev, (uint64_t)pipelines[i], fake_destroy));
    }
}

static void test_queue_during_cache_work(void) {
    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthWriteEnable = VK_TRUE
    };
    VkPipelineShaderStageCreateInfo stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = (VkShaderModule)1, .pName = "shadow" };
    VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 1, .pStages = &stage, .pDepthStencilState = &depth };
    pthread_mutex_lock(&test_mu); compile_result = VK_SUCCESS; release_compile = 1; pthread_mutex_unlock(&test_mu);
    pthread_mutex_lock(&g_kva_cache_mu);
    VkPipeline pipelines[2];
    assert(kva_create(test_device.dev, (VkPipelineCache)&test_cache, 1, &ci, &pipelines[0], fake_create));
    // Let the worker reach the blocked driver-cache operation. It must release
    // the queue first, so the next material can still be enqueued and skipped.
    double end = kl_pipe_ms() + 3000;
    for (;;) {
        pthread_mutex_lock(&g_kva_mu);
        int started = kva_of((uint64_t)pipelines[0])->started_ms > 0;
        pthread_mutex_unlock(&g_kva_mu);
        if (started) break;
        assert(kl_pipe_ms() < end); usleep(100);
    }
    assert(kva_create(test_device.dev, (VkPipelineCache)&test_cache, 1, &ci, &pipelines[1], fake_create));
    kl_vulkan_async_begin_frame();
    for (int i = 0; i < 2; i++) assert(!kva_bind(&pipelines[i], VK_PIPELINE_BIND_POINT_GRAPHICS, (uint64_t)pipelines[i]));
    pthread_mutex_unlock(&g_kva_cache_mu);
    pthread_mutex_lock(&g_kva_mu);
    while (g_kva_pending || g_kva_finishing) pthread_cond_wait(&g_kva_ready, &g_kva_mu);
    pthread_mutex_unlock(&g_kva_mu);
    kl_vulkan_async_begin_frame();
    for (int i = 0; i < 2; i++) {
        assert(kva_bind(&pipelines[i], VK_PIPELINE_BIND_POINT_GRAPHICS, (uint64_t)pipelines[i]));
        assert(kva_destroy_pipeline(test_device.dev, (uint64_t)pipelines[i], fake_destroy));
    }
}

static void test_snapshot_pair(void) {
    size_t size = 0;
    pthread_mutex_lock(&test_mu); cache_value = 11; pthread_mutex_unlock(&test_mu);
    assert(klvk_GetPipelineCacheData(test_device.dev, (VkPipelineCache)&test_cache, &size, NULL) == VK_SUCCESS && size == 32);
    pthread_mutex_lock(&test_mu); cache_value = 22; pthread_mutex_unlock(&test_mu);
    unsigned char data[32];
    assert(klvk_GetPipelineCacheData(test_device.dev, (VkPipelineCache)&test_cache, &size, data) == VK_SUCCESS);
    for (size_t i = 0; i < size; i++) assert(data[i] == 11);
    assert(klvk_GetPipelineCacheData(test_device.dev, (VkPipelineCache)&test_cache, &size, NULL) == VK_SUCCESS);
    size = 8; memset(data, 99, sizeof data);
    assert(klvk_GetPipelineCacheData(test_device.dev, (VkPipelineCache)&test_cache, &size, data) == VK_INCOMPLETE && size == 0);
    for (size_t i = 0; i < sizeof data; i++) assert(data[i] == 99);
    size = sizeof data;
    assert(klvk_GetPipelineCacheData(test_device.dev, (VkPipelineCache)&test_cache, &size, data) == VK_SUCCESS);
    for (size_t i = 0; i < size; i++) assert(data[i] == 22);
    klvk_DestroyPipelineCache(test_device.dev, (uint64_t)&test_cache, NULL);
}

static void test_pending_archive(void) {
    char directory[] = "build/tests/archive-stall.XXXXXX";
    assert(mkdtemp(directory));
    char absolute[4096], path[4096], file_path[4096];
    assert(realpath(directory, absolute));
    snprintf(path, sizeof path, "%s/cache", absolute);
    snprintf(file_path, sizeof file_path, "%s.0", path);
    unsigned char original[32]; memset(original, 77, sizeof original);
    FILE *file = fopen(file_path, "wb"); assert(file);
    assert(fwrite(original, 1, sizeof original, file) == sizeof original); fclose(file);
    setenv("KL_VK_PIPELINE_ARCHIVE", path, 1);

    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthWriteEnable = VK_TRUE
    };
    VkPipelineShaderStageCreateInfo stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = (VkShaderModule)1, .pName = "shadow" };
    VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 1, .pStages = &stage, .pDepthStencilState = &depth };
    pthread_mutex_lock(&test_mu);
    archive_import_entered = release_archive_import = compiling = 0;
    release_compile = 1; compile_result = VK_SUCCESS;
    pthread_mutex_unlock(&test_mu);
    VkPipeline pipeline;
    assert(kva_create(test_device.dev, (VkPipelineCache)&test_cache, 1, &ci, &pipeline, fake_create));
    pthread_mutex_lock(&test_mu);
    wait_for(&archive_import_entered, 1);
    wait_for(&compiling, 1); // must compile while disk import is still blocked
    assert(!release_archive_import);
    pthread_mutex_unlock(&test_mu);
    pthread_mutex_lock(&g_kva_mu);
    while (g_kva_pending || g_kva_finishing) pthread_cond_wait(&g_kva_ready, &g_kva_mu);
    pthread_mutex_unlock(&g_kva_mu);
    kl_vulkan_async_begin_frame();
    assert(kva_bind(&pipeline, VK_PIPELINE_BIND_POINT_GRAPHICS, (uint64_t)pipeline));
    int before = draws;
    klvk_CmdDraw(&pipeline, 3, 1, 0, 0);
    assert(draws == before + 1);
    assert(kva_destroy_pipeline(test_device.dev, (uint64_t)pipeline, fake_destroy));
    // Publication while restoring must preserve the old archive on disk.
    unsigned char readback[32]; file = fopen(file_path, "rb"); assert(file);
    assert(fread(readback, 1, sizeof readback, file) == sizeof readback); fclose(file);
    assert(!memcmp(original, readback, sizeof original));
    // A busy saver/merge must not stop a worker from seeding or publishing.
    pthread_mutex_lock(&g_kva_archive_mu);
    kva_archive_seed(test_device.dev, (VkPipelineCache)&test_cache);
    kva_archive_publish(test_device.dev, (VkPipelineCache)&test_cache);
    pthread_mutex_unlock(&g_kva_archive_mu);
    pthread_mutex_lock(&test_mu);
    release_archive_import = 1; pthread_cond_broadcast(&test_cv);
    pthread_mutex_unlock(&test_mu);
    kva_archive_close(test_device.dev);
    unsetenv("KL_VK_PIPELINE_ARCHIVE");
    assert(!unlink(file_path)); assert(!rmdir(directory));
}

static void test_actual_render_targets(void) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .stage = VK_SHADER_STAGE_VERTEX_BIT }, { .stage = VK_SHADER_STAGE_FRAGMENT_BIT }
    };
    VkPipelineColorBlendAttachmentState ignored_color = { .colorWriteMask = 15 };
    VkPipelineColorBlendStateCreateInfo blend = { .attachmentCount = 1, .pAttachments = &ignored_color };
    VkPipelineRenderingCreateInfo rendering = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .depthAttachmentFormat = VK_FORMAT_D32_SFLOAT };
    VkGraphicsPipelineCreateInfo ci = { .pNext = &rendering, .stageCount = 2, .pStages = stages, .pColorBlendState = &blend };
    // No colour attachment: these blend fields are ignored by Vulkan.
    assert(kva_depth_only(&ci));
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &format;
    rendering.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
    ci.pColorBlendState = NULL; // valid when all blend state is dynamic
    assert(!kva_depth_only(&ci));

    VkAttachmentReference unused = { VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };
    VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpasses[2] = {
        { .colorAttachmentCount = 1, .pColorAttachments = &unused, .pDepthStencilAttachment = &depth },
        { .colorAttachmentCount = 1, .pColorAttachments = &color }
    };
    VkRenderPassCreateInfo pass = { .subpassCount = 2, .pSubpasses = subpasses };
    ci.renderPass = (VkRenderPass)&pass; ci.pNext = NULL;
    kva_record_pass(ci.renderPass, &pass, NULL);
    stages[0].module = (VkShaderModule)1; stages[1].module = (VkShaderModule)2;
    ci.pColorBlendState = (const VkPipelineColorBlendStateCreateInfo *)1;
    assert(kva_depth_only(&ci));
    kva_arena arena = {0};
    VkGraphicsPipelineCreateInfo *copy = kva_copy_gpci(&arena, &ci);
    assert(copy && !copy->pColorBlendState); kva_free(&arena);
    ci.subpass = 1; ci.pColorBlendState = NULL;
    ci.pDepthStencilState = (const VkPipelineDepthStencilStateCreateInfo *)1;
    assert(!kva_depth_only(&ci));
    copy = kva_copy_gpci(&arena, &ci);
    assert(copy && !copy->pDepthStencilState); kva_free(&arena);
    kva_forget_pass(ci.renderPass);
    VkAttachmentReference2 depth2 = { .attachment = 0 };
    VkSubpassDescription2 subpass2 = { .pDepthStencilAttachment = &depth2 };
    VkRenderPassCreateInfo2 pass2 = { .subpassCount = 1, .pSubpasses = &subpass2 };
    kva_record_pass(ci.renderPass, NULL, &pass2);
    ci.subpass = 0; ci.pDepthStencilState = NULL;
    ci.pColorBlendState = &blend;
    assert(kva_depth_only(&ci));
    kva_forget_pass(ci.renderPass);
}

static void test_worker_archive_owns_cache(void) {
    char directory[] = "build/tests/private-cache.XXXXXX", path[4096], absolute[4096];
    assert(mkdtemp(directory) && realpath(directory,absolute));
    snprintf(path,sizeof path,"%s/cache",absolute);
    setenv("KL_VK_PIPELINE_ARCHIVE",path,1);
    setenv("KL_VK_PRIVATE_CACHE","1",1);
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthWriteEnable=VK_TRUE
    };
    VkPipelineShaderStageCreateInfo stage = { .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage=VK_SHADER_STAGE_VERTEX_BIT, .module=(VkShaderModule)1, .pName="shadow" };
    VkGraphicsPipelineCreateInfo ci = { .sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount=1, .pStages=&stage, .pDepthStencilState=&ds };
    pthread_mutex_lock(&test_mu);
    block_public_publish=block_engine_seed=1; release_compile=1; compile_result=VK_SUCCESS;
    int before=public_publish_merges;
    int seeds=engine_seeds;
    pthread_mutex_unlock(&test_mu);
    VkPipeline pipeline; assert(kva_create(test_device.dev,(VkPipelineCache)&test_cache,1,&ci,&pipeline,fake_create));
    // Publishing to an engine cache is deliberately blocked. Completion,
    // drawing and export must still progress using the worker-owned archive.
    struct timespec end=deadline(2000);
    pthread_mutex_lock(&g_kva_mu);
    while (g_kva_pending || g_kva_finishing) assert(!pthread_cond_timedwait(&g_kva_ready,&g_kva_mu,&end));
    pthread_mutex_unlock(&g_kva_mu);
    assert(public_publish_merges==before);
    assert(engine_seeds==seeds);
    kl_vulkan_async_begin_frame();
    assert(kva_bind(&pipeline,VK_PIPELINE_BIND_POINT_GRAPHICS,(uint64_t)pipeline));
    size_t size=0; unsigned char bytes[32];
    assert(klvk_GetPipelineCacheData(test_device.dev,(VkPipelineCache)&test_cache,&size,NULL)==VK_SUCCESS && size==32);
    assert(klvk_GetPipelineCacheData(test_device.dev,(VkPipelineCache)&test_cache,&size,bytes)==VK_SUCCESS);
    assert(kva_destroy_pipeline(test_device.dev,(uint64_t)pipeline,fake_destroy));
    kva_archive_close(test_device.dev);
    pthread_mutex_lock(&test_mu); block_public_publish=block_engine_seed=0; pthread_cond_broadcast(&test_cv); pthread_mutex_unlock(&test_mu);
    unsetenv("KL_VK_PRIVATE_CACHE"); unsetenv("KL_VK_PIPELINE_ARCHIVE");
    snprintf(path,sizeof path,"%s/cache.0",absolute);
    assert(!unlink(path) && !rmdir(directory));
}

static void test_unchanged_private_snapshot(void) {
    setenv("KL_VK_PRIVATE_CACHE","1",1);
    setenv("KL_VK_PIPELINE_ARCHIVE","/unused-private-archive",1);
    pthread_mutex_lock(&test_mu); cache_value=31; pthread_mutex_unlock(&test_mu);
    VkPipelineCache other=(VkPipelineCache)&other_cache;
    assert(klvk_MergePipelineCaches(test_device.dev,(VkPipelineCache)&test_cache,1,&other)==VK_SUCCESS);
    size_t size=0; unsigned char bytes[32];
    assert(klvk_GetPipelineCacheData(test_device.dev,(VkPipelineCache)&test_cache,&size,NULL)==VK_SUCCESS);
    // A caller mutation after the size query must not change that pair.
    pthread_mutex_lock(&test_mu); cache_value=42; pthread_mutex_unlock(&test_mu);
    assert(klvk_MergePipelineCaches(test_device.dev,(VkPipelineCache)&test_cache,1,&other)==VK_SUCCESS);
    assert(klvk_GetPipelineCacheData(test_device.dev,(VkPipelineCache)&test_cache,&size,bytes)==VK_SUCCESS);
    for (size_t i=0;i<size;i++) assert(bytes[i]==31);
    assert(klvk_GetPipelineCacheData(test_device.dev,(VkPipelineCache)&test_cache,&size,NULL)==VK_SUCCESS);
    assert(klvk_GetPipelineCacheData(test_device.dev,(VkPipelineCache)&test_cache,&size,bytes)==VK_SUCCESS);
    for (size_t i=0;i<size;i++) assert(bytes[i]==42);
    int reads=cache_reads;
    assert(klvk_GetPipelineCacheData(test_device.dev,(VkPipelineCache)&test_cache,&size,NULL)==VK_SUCCESS);
    assert(klvk_GetPipelineCacheData(test_device.dev,(VkPipelineCache)&test_cache,&size,bytes)==VK_SUCCESS);
    assert(cache_reads==reads); // unchanged exports never re-enter the driver
    unsetenv("KL_VK_PRIVATE_CACHE"); unsetenv("KL_VK_PIPELINE_ARCHIVE");
}

static void test_private_creation_keeps_exports_running(void) {
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthWriteEnable=VK_TRUE
    };
    VkPipelineShaderStageCreateInfo stage = { .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage=VK_SHADER_STAGE_VERTEX_BIT, .module=(VkShaderModule)1, .pName="shadow" };
    VkGraphicsPipelineCreateInfo ci = { .sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount=1, .pStages=&stage, .pDepthStencilState=&ds };
    pthread_mutex_lock(&test_mu);
    block_private_create=1; private_create_entered=0; query_entered=query_done=0; release_compile=1;
    pthread_mutex_unlock(&test_mu);
    VkPipeline pipeline; assert(kva_create(test_device.dev,(VkPipelineCache)&test_cache,1,&ci,&pipeline,fake_create));
    pthread_mutex_lock(&test_mu); wait_for(&private_create_entered,1); pthread_mutex_unlock(&test_mu);
    pthread_t query; assert(!pthread_create(&query,NULL,query_cache,NULL));
    struct timespec end=deadline(2000);
    pthread_mutex_lock(&test_mu);
    while (!query_done) assert(!pthread_cond_timedwait(&test_cv,&test_mu,&end));
    block_private_create=0; pthread_cond_broadcast(&test_cv);
    pthread_mutex_unlock(&test_mu);
    assert(!pthread_join(query,NULL));
    pthread_mutex_lock(&g_kva_mu);
    while (g_kva_pending || g_kva_finishing) pthread_cond_wait(&g_kva_ready,&g_kva_mu);
    pthread_mutex_unlock(&g_kva_mu);
    assert(kva_destroy_pipeline(test_device.dev,(uint64_t)pipeline,fake_destroy));
}

int main(void) {
    alarm(20);
    setenv("KL_VK_ASYNC_PIPELINES", "1", 1);
    setenv("KL_VK_ASYNC_THREADS", "1", 1);
    assert(kva_on());
    test_actual_render_targets();
    // Initialise the real draw callback before recording on several threads.
    klvk_CmdDraw(&test_device, 3, 1, 0, 0);
    VkPipelineCacheCreateInfo cache_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .flags = VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT };
    VkPipelineCache cache;
    assert(klvk_CreatePipelineCache(test_device.dev, &cache_info, NULL, &cache) == VK_SUCCESS);
    assert(cache_created == 1 && cache_info.flags == VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT);
    test_pending_bind(VK_SUCCESS, 0, 0);
    test_pending_bind(VK_ERROR_OUT_OF_DEVICE_MEMORY, 0, 0);
    test_pending_bind(VK_SUCCESS, 1, 0);
    assert(destroyed == 2 && cache_reads == 4 && cache_merges == 4 && cache_destroyed == 1);
    test_demand_priority();
    // Colour passes can write cached lighting, stencil or shadow masks too.
    test_pending_bind(VK_SUCCESS, 0, 1);
    test_pending_bind(VK_ERROR_OUT_OF_DEVICE_MEMORY, 0, 1);
    test_queue_during_cache_work();
    test_snapshot_pair();
    test_pending_archive();
    test_worker_archive_owns_cache();
    test_unchanged_private_snapshot();
    test_private_creation_keeps_exports_running();
    kva_archive_close(test_device.dev);
    assert(private_created > 0 && private_created == private_destroyed);
    assert(kva_bind(&test_device, VK_PIPELINE_BIND_POINT_COMPUTE,
        (uint64_t)&real_pipeline) == (uint64_t)&real_pipeline);
    puts("async pipelines: nonblocking colour/depth binds, stereo activation, demand priority, cache snapshots and lifetime OK");
    return 0;
}
