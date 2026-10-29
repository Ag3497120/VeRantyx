// kl_vk_async.inc — asynchronous graphics-pipeline creation (VeRantyx).
// Included into kl_vulkan.c by klepton-patches/apply.py.
//
// MoltenVK compiles a pipeline inside vkCreateGraphicsPipelines; the first
// time Source 2 meets a material that is 250-500 ms on its render thread (the
// hitches when characters or new areas appear). With KL_VK_ASYNC_PIPELINES=1
// the create info is deep-copied, the guest gets a placeholder handle at once,
// and worker threads compile ahead of use. Pending graphics draws are omitted
// without waiting, including depth passes: uninterrupted head motion takes
// priority over complete cold frames. Ready materials activate together at
// the next stereo frame boundary. Cached shadow contents may be incomplete
// until the engine redraws them; this policy is not a shadow-correctness fix.
// Anything the copy does not understand is compiled synchronously as before.
//
// While compiles are in flight, destroys of shader modules, pipeline layouts,
// render passes and descriptor-set layouts are deferred (the pending compiles
// still reference them); they run when the queue drains. Workers compile into
// private caches, seeding from and merging completed results into engine caches
// under a short lock. Engine cache operations never drain background jobs.
// The native host with a persistent archive keeps completed work in that
// archive alone: Source 2 serializes its caches on a frame worker, so copying
// every shader back into them reintroduces head-turn stalls during exports.

#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <unistd.h>

#define KVA_MAGIC 0x4b4c41535950524full   /* "ORPYSALK" — never an MVK vtable */
static int kva_render_usage(const VkGraphicsPipelineCreateInfo *ci);

typedef struct kva_chunk { struct kva_chunk *next; size_t used, cap; char data[]; } kva_chunk;
typedef struct { kva_chunk *head; } kva_arena;

static void *kva_alloc(kva_arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > 4096 ? n : 4096;
        kva_chunk *c = (kva_chunk *)malloc(sizeof *c + cap);
        if (!c) return NULL;
        c->next = a->head; c->used = 0; c->cap = cap; a->head = c;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += n;
    return p;
}
static void *kva_dup(kva_arena *a, const void *p, size_t n) {
    if (!p || !n) return NULL;
    void *d = kva_alloc(a, n);
    if (d) memcpy(d, p, n);
    return d;
}
static void kva_free(kva_arena *a) {
    kva_chunk *c = a->head;
    while (c) { kva_chunk *n = c->next; free(c); c = n; }
    a->head = NULL;
}

// pNext chains: known, pointer-free (or handled) structures only; 0 = give up.
static int kva_copy_chain(kva_arena *a, const void *src, const void **dst) {
    const VkBaseInStructure *s = (const VkBaseInStructure *)src;
    VkBaseOutStructure *head = NULL, *tail = NULL;
    for (; s; s = s->pNext) {
        size_t sz = 0;
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO:
            continue;   // output pointers: meaningless after the call returns
        case VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO: sz = sizeof(VkPipelineRenderingCreateInfo); break;
        case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT: sz = sizeof(VkPipelineRasterizationDepthClipStateCreateInfoEXT); break;
        case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT: sz = sizeof(VkPipelineRasterizationProvokingVertexStateCreateInfoEXT); break;
        case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT: sz = sizeof(VkPipelineRasterizationLineStateCreateInfoEXT); break;
        case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT: sz = sizeof(VkPipelineRasterizationConservativeStateCreateInfoEXT); break;
        case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO: sz = sizeof(VkPipelineShaderStageRequiredSubgroupSizeCreateInfo); break;
        case VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT: sz = sizeof(VkPipelineVertexInputDivisorStateCreateInfoEXT); break;
        default: {
            static int said; if (said < 8) { said++;
                VKI("[async] pipeline pNext sType %d not copied: that pipeline compiles synchronously\n", (int)s->sType); }
            return 0;
        }
        }
        VkBaseOutStructure *c = (VkBaseOutStructure *)kva_dup(a, s, sz);
        if (!c) return 0;
        c->pNext = NULL;
        if ((int)s->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
            VkPipelineRenderingCreateInfo *r = (VkPipelineRenderingCreateInfo *)c;
            r->pColorAttachmentFormats = (const VkFormat *)kva_dup(a, r->pColorAttachmentFormats, r->colorAttachmentCount * sizeof(VkFormat));
        } else if ((int)s->sType == VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT) {
            VkPipelineVertexInputDivisorStateCreateInfoEXT *d = (VkPipelineVertexInputDivisorStateCreateInfoEXT *)c;
            d->pVertexBindingDivisors = (const VkVertexInputBindingDivisorDescriptionEXT *)kva_dup(a, d->pVertexBindingDivisors,
                d->vertexBindingDivisorCount * sizeof(VkVertexInputBindingDivisorDescriptionEXT));
        }
        if (tail) tail->pNext = c; else head = c;
        tail = c;
    }
    *dst = head;
    return 1;
}

#define KVA_STATE(T, field)                                                           \
    if (ci->field) {                                                                  \
        T *x = (T *)kva_dup(a, ci->field, sizeof(T));                                 \
        if (!x || !kva_copy_chain(a, ci->field->pNext, &x->pNext)) return NULL;       \
        c->field = x;                                                                 \
    }

static VkGraphicsPipelineCreateInfo *kva_copy_gpci(kva_arena *a, const VkGraphicsPipelineCreateInfo *ci) {
    VkGraphicsPipelineCreateInfo *c = (VkGraphicsPipelineCreateInfo *)kva_dup(a, ci, sizeof *ci);
    if (!c || !kva_copy_chain(a, ci->pNext, &c->pNext)) return NULL;
    if (ci->stageCount) {
        VkPipelineShaderStageCreateInfo *st = (VkPipelineShaderStageCreateInfo *)kva_dup(a, ci->pStages, ci->stageCount * sizeof *st);
        if (!st) return NULL;
        for (uint32_t i = 0; i < ci->stageCount; i++) {
            if (!kva_copy_chain(a, ci->pStages[i].pNext, &st[i].pNext)) return NULL;
            if (!st[i].module) return NULL;   // inline code (maintenance5): not handled
            const char *nm = ci->pStages[i].pName ? ci->pStages[i].pName : "main";
            st[i].pName = (const char *)kva_dup(a, nm, strlen(nm) + 1);
            const VkSpecializationInfo *sp = ci->pStages[i].pSpecializationInfo;
            if (sp) {
                VkSpecializationInfo *spc = (VkSpecializationInfo *)kva_dup(a, sp, sizeof *sp);
                if (!spc) return NULL;
                spc->pMapEntries = (const VkSpecializationMapEntry *)kva_dup(a, sp->pMapEntries, sp->mapEntryCount * sizeof(VkSpecializationMapEntry));
                spc->pData = kva_dup(a, sp->pData, sp->dataSize);
                st[i].pSpecializationInfo = spc;
            }
        }
        c->pStages = st;
    }
    KVA_STATE(VkPipelineVertexInputStateCreateInfo, pVertexInputState)
    if (c->pVertexInputState) {
        VkPipelineVertexInputStateCreateInfo *v = (VkPipelineVertexInputStateCreateInfo *)c->pVertexInputState;
        v->pVertexBindingDescriptions = (const VkVertexInputBindingDescription *)kva_dup(a, v->pVertexBindingDescriptions,
            v->vertexBindingDescriptionCount * sizeof(VkVertexInputBindingDescription));
        v->pVertexAttributeDescriptions = (const VkVertexInputAttributeDescription *)kva_dup(a, v->pVertexAttributeDescriptions,
            v->vertexAttributeDescriptionCount * sizeof(VkVertexInputAttributeDescription));
    }
    KVA_STATE(VkPipelineInputAssemblyStateCreateInfo, pInputAssemblyState)
    KVA_STATE(VkPipelineTessellationStateCreateInfo, pTessellationState)
    KVA_STATE(VkPipelineViewportStateCreateInfo, pViewportState)
    if (c->pViewportState) {
        VkPipelineViewportStateCreateInfo *v = (VkPipelineViewportStateCreateInfo *)c->pViewportState;
        v->pViewports = (const VkViewport *)kva_dup(a, v->pViewports, v->viewportCount * sizeof(VkViewport));
        v->pScissors = (const VkRect2D *)kva_dup(a, v->pScissors, v->scissorCount * sizeof(VkRect2D));
    }
    KVA_STATE(VkPipelineRasterizationStateCreateInfo, pRasterizationState)
    KVA_STATE(VkPipelineMultisampleStateCreateInfo, pMultisampleState)
    if (c->pMultisampleState && c->pMultisampleState->pSampleMask) {
        VkPipelineMultisampleStateCreateInfo *m = (VkPipelineMultisampleStateCreateInfo *)c->pMultisampleState;
        uint32_t words = ((uint32_t)m->rasterizationSamples + 31) / 32;
        m->pSampleMask = (const VkSampleMask *)kva_dup(a, m->pSampleMask, words * sizeof(VkSampleMask));
    }
    int usage = kva_render_usage(ci);
    if (usage >= 0 && !(usage & 2)) c->pDepthStencilState = NULL;
    else { KVA_STATE(VkPipelineDepthStencilStateCreateInfo, pDepthStencilState) }
    if (usage >= 0 && !(usage & 1)) c->pColorBlendState = NULL;
    else { KVA_STATE(VkPipelineColorBlendStateCreateInfo, pColorBlendState) }
    if (c->pColorBlendState) {
        VkPipelineColorBlendStateCreateInfo *b = (VkPipelineColorBlendStateCreateInfo *)c->pColorBlendState;
        b->pAttachments = (const VkPipelineColorBlendAttachmentState *)kva_dup(a, b->pAttachments,
            b->attachmentCount * sizeof(VkPipelineColorBlendAttachmentState));
    }
    KVA_STATE(VkPipelineDynamicStateCreateInfo, pDynamicState)
    if (c->pDynamicState) {
        VkPipelineDynamicStateCreateInfo *d = (VkPipelineDynamicStateCreateInfo *)c->pDynamicState;
        d->pDynamicStates = (const VkDynamicState *)kva_dup(a, d->pDynamicStates, d->dynamicStateCount * sizeof(VkDynamicState));
    }
    // Derivatives of a placeholder: drop the derivative link (an optimisation hint).
    c->flags &= ~(VkPipelineCreateFlags)VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    c->basePipelineHandle = VK_NULL_HANDLE;
    c->basePipelineIndex = -1;
    return c;
}

typedef struct kva_pipe {
    uint64_t magic;
    _Atomic uint64_t real;          // the MoltenVK pipeline once compiled
    _Atomic int state;              // 0 pending, 1 ready, 2 failed
    uint64_t ready_serial;          // immutable after state becomes ready
    _Atomic int demanded;           // priority hint; never locks the recording thread
    int depth_only;
    int destroy_requested;          // under g_kva_mu
    VkDevice dev;
    VkPipelineCache cache;
    PFN_vkCreateGraphicsPipelines create;
    PFN_vkDestroyPipeline destroy;
    kva_arena arena;
    VkGraphicsPipelineCreateInfo *ci;
    struct kva_pipe *next_job;
    struct kva_pipe *next_active;
    double queued_ms, started_ms, compiled_ms;
} kva_pipe;

static pthread_mutex_t g_kva_mu = PTHREAD_MUTEX_INITIALIZER;
// Driver cache merges/snapshots can be expensive. They must not own the job
// queue mutex needed to enqueue newly encountered graphics materials.
static pthread_mutex_t g_kva_cache_mu = PTHREAD_MUTEX_INITIALIZER;
static void kva_dirty_snapshot_locked(VkDevice dev, VkPipelineCache cache);
static int kva_on(void);
static int kva_private_archive(void) {
    const char *path = getenv("KL_VK_PIPELINE_ARCHIVE");
    return kva_on() && kl_env_on("KL_VK_PRIVATE_CACHE",0) && path && path[0]=='/' && strlen(path)<=4000;
}
static pthread_cond_t g_kva_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_kva_ready = PTHREAD_COND_INITIALIZER;
static kva_pipe *g_kva_head, *g_kva_tail, *g_kva_active;
static int g_kva_pending;                   // queued + compiling
static unsigned long long g_kva_done, g_kva_failed, g_kva_sync, g_kva_deferred_binds, g_kva_failed_binds;
static double g_kva_ms;

// Only readiness publication uses this mutex; compilation, cache operations
// and filesystem I/O cannot hold up WaitGetPoses. Snapshot after publication
// so a worker finishing between two eyes cannot activate a one-eye material.
static pthread_mutex_t g_kva_visibility_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_kva_completed_serial;
static _Atomic uint64_t g_kva_visible_serial;
void kl_vulkan_async_begin_frame(void) {
    pthread_mutex_lock(&g_kva_visibility_mu);
    atomic_store(&g_kva_visible_serial, g_kva_completed_serial);
    pthread_mutex_unlock(&g_kva_visibility_mu);
}

typedef struct kva_pass {
    VkRenderPass pass;
    struct kva_pass *next;
    uint32_t count;
    unsigned char usage[]; // bit 0 colour, bit 1 depth/stencil
} kva_pass;
static kva_pass *g_kva_passes;

static void kva_forget_pass(VkRenderPass pass) {
    pthread_mutex_lock(&g_kva_mu);
    kva_pass **at = &g_kva_passes;
    while (*at && (*at)->pass != pass) at = &(*at)->next;
    if (*at) { kva_pass *old = *at; *at = old->next; free(old); }
    pthread_mutex_unlock(&g_kva_mu);
}
static void kva_record_pass(VkRenderPass pass, const VkRenderPassCreateInfo *a, const VkRenderPassCreateInfo2 *b) {
    uint32_t count = a ? a->subpassCount : b->subpassCount;
    if (!pass || count > 4096) return;
    kva_pass *p = calloc(1, sizeof *p + count);
    if (!p) return;
    p->pass = pass; p->count = count;
    for (uint32_t i = 0; i < count; i++) {
        if (a) {
            const VkSubpassDescription *s = &a->pSubpasses[i];
            for (uint32_t j = 0; j < s->colorAttachmentCount; j++)
                if (s->pColorAttachments[j].attachment != VK_ATTACHMENT_UNUSED) p->usage[i] |= 1;
            if (s->pDepthStencilAttachment && s->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED) p->usage[i] |= 2;
        } else {
            const VkSubpassDescription2 *s = &b->pSubpasses[i];
            for (uint32_t j = 0; j < s->colorAttachmentCount; j++)
                if (s->pColorAttachments[j].attachment != VK_ATTACHMENT_UNUSED) p->usage[i] |= 1;
            if (s->pDepthStencilAttachment && s->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED) p->usage[i] |= 2;
        }
    }
    kva_forget_pass(pass);
    pthread_mutex_lock(&g_kva_mu); p->next = g_kva_passes; g_kva_passes = p; pthread_mutex_unlock(&g_kva_mu);
}
static int kva_render_usage(const VkGraphicsPipelineCreateInfo *ci) {
    if (ci->renderPass) {
        int usage = -1;
        pthread_mutex_lock(&g_kva_mu);
        for (kva_pass *p = g_kva_passes; p; p = p->next) if (p->pass == ci->renderPass && ci->subpass < p->count) {
            usage = p->usage[ci->subpass]; break;
        }
        pthread_mutex_unlock(&g_kva_mu);
        return usage;
    }
    for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext) if (p->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
        const VkPipelineRenderingCreateInfo *r = (const VkPipelineRenderingCreateInfo *)p;
        if (r->colorAttachmentCount && !r->pColorAttachmentFormats) return -1;
        int usage = r->depthAttachmentFormat != VK_FORMAT_UNDEFINED || r->stencilAttachmentFormat != VK_FORMAT_UNDEFINED ? 2 : 0;
        for (uint32_t i = 0; i < r->colorAttachmentCount; i++) if (r->pColorAttachmentFormats[i] != VK_FORMAT_UNDEFINED) usage |= 1;
        return usage;
    }
    return -1;
}

static int kva_depth_only(const VkGraphicsPipelineCreateInfo *ci) {
    int fragment = 0;
    for (uint32_t i = 0; i < ci->stageCount; i++) fragment |= ci->pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT;
    if (!fragment) return 1; // pre-rasterization side effects must not be dropped
    int usage = kva_render_usage(ci);
    if (usage >= 0 && !(usage & 1)) return 1;
    if (usage >= 0 && !(usage & 2)) return 0;
    if (!fragment || !ci->pColorBlendState || !ci->pColorBlendState->attachmentCount || !ci->pColorBlendState->pAttachments) return 1;
    for (uint32_t i = 0; i < ci->pColorBlendState->attachmentCount; i++)
        if (ci->pColorBlendState->pAttachments[i].colorWriteMask) return 0;
    return 1;
}

typedef struct { int kind; VkDevice dev; uint64_t h; } kva_deferred;   // 0 module 1 layout 2 renderpass 3 dsl 4 cache
static kva_deferred *g_kva_def; static size_t g_kva_ndef, g_kva_capdef;

static int g_kva_enabled;
static void kva_init_enabled(void) { g_kva_enabled = kl_env_on("KL_VK_ASYNC_PIPELINES", 0); }
static int kva_on(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, kva_init_enabled);
    return g_kva_enabled;
}
static inline kva_pipe *kva_of(uint64_t h) {
    if (!h) return NULL;
    const kva_pipe *p = (const kva_pipe *)(uintptr_t)h;
    return p->magic == KVA_MAGIC ? (kva_pipe *)p : NULL;
}

static void kva_flush_deferred(kva_deferred *items, size_t count);

// Keep completed, rewritten shaders across sessions, independently of Source
// 2's per-thread caches (which can all start empty). Never serialize this cache
// under the engine/queue mutex, or on the thread recording a draw.
typedef struct kva_archive {
    VkDevice dev;
    VkPipelineCache cache;
    double saved_ms;
    int dirty;
    int loading;
    char path[4096];
    struct kva_archive *next;
} kva_archive;
static kva_archive *g_kva_archives;
static pthread_mutex_t g_kva_archive_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_kva_archive_ready = PTHREAD_COND_INITIALIZER;
static int g_kva_finishing;

// MoltenVK rebuilds Metal libraries while importing a disk cache. A large
// archive can take longer than a minute: never put that work ahead of the
// first visible material, or hold the archive lock while importing it.
static void *kva_archive_load(void *arg) {
    kva_archive *a = arg;
    pthread_setname_np("kl-shader-cache-load");
    VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    void *data = NULL;
    FILE *file = fopen(a->path, "rb");
    if (file) {
        if (!fseek(file, 0, SEEK_END)) {
            long size = ftell(file);
            if (size >= 32 && size <= 256 * 1024 * 1024 && !fseek(file, 0, SEEK_SET)) {
                data = malloc((size_t)size);
                if (data && fread(data, 1, size, file) == (size_t)size) {
                    ci.initialDataSize = (size_t)size; ci.pInitialData = data;
                }
            }
        }
        fclose(file);
    }
    VkPipelineCache imported = VK_NULL_HANDLE;
    VkResult result = VK_SUCCESS;
    double start = kl_pipe_ms();
    if (ci.initialDataSize) {
        VKI("[async] persistent shader cache: importing %zu bytes independently of material workers\n", ci.initialDataSize);
        PFN_vkCreatePipelineCache create = (PFN_vkCreatePipelineCache)real_gdpa(a->dev, "vkCreatePipelineCache");
        result = create ? create(a->dev, &ci, NULL, &imported) : VK_ERROR_INITIALIZATION_FAILED;
    }
    free(data);
    pthread_mutex_lock(&g_kva_archive_mu);
    if (imported && result == VK_SUCCESS) {
        PFN_vkMergePipelineCaches merge = (PFN_vkMergePipelineCaches)real_gdpa(a->dev, "vkMergePipelineCaches");
        if (merge) result = merge(a->dev, a->cache, 1, &imported);
    }
    if (imported) {
        PFN_vkDestroyPipelineCache destroy = (PFN_vkDestroyPipelineCache)real_gdpa(a->dev, "vkDestroyPipelineCache");
        if (destroy) destroy(a->dev, imported, NULL);
    }
    VKI("[async] persistent shader cache: import finished, %zu bytes, %.1f ms, result %d\n",
        ci.initialDataSize, kl_pipe_ms() - start, (int)result);
    a->loading = 0;
    pthread_cond_broadcast(&g_kva_archive_ready);
    pthread_mutex_unlock(&g_kva_archive_mu);
    return NULL;
}

static kva_archive *kva_archive_locked(VkDevice dev) {
    const char *path = getenv("KL_VK_PIPELINE_ARCHIVE");
    if (!path || path[0] != '/' || strlen(path) > 4000 || !real_gdpa) return NULL;
    unsigned index = 0;
    for (kva_archive *a = g_kva_archives; a; a = a->next, index++) if (a->dev == dev) return a;
    kva_archive *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->dev = dev;
    snprintf(a->path, sizeof a->path, "%s.%u", path, index);
    PFN_vkCreatePipelineCache create = (PFN_vkCreatePipelineCache)real_gdpa(dev, "vkCreatePipelineCache");
    VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    VkResult result = create ? create(dev, &ci, NULL, &a->cache) : VK_ERROR_INITIALIZATION_FAILED;
    if (result != VK_SUCCESS) { free(a); return NULL; }
    a->next = g_kva_archives; g_kva_archives = a;
    a->loading = 1;
    pthread_t thread;
    int err = pthread_create(&thread, NULL, kva_archive_load, a);
    if (!err) pthread_detach(thread);
    else {
        a->loading = 0;
        VKI("[async] persistent shader cache: import thread unavailable (%d); using current-session shaders\n", err);
    }
    return a;
}
static void kva_archive_save_locked(kva_archive *a, int force) {
    // Do not replace the old file with a partial cold cache before import ends.
    if (a->loading || !a->dirty || (!force && a->saved_ms && kl_pipe_ms() - a->saved_ms < 5000)) return;
    PFN_vkGetPipelineCacheData get = (PFN_vkGetPipelineCacheData)real_gdpa(a->dev, "vkGetPipelineCacheData");
    size_t size = 0;
    if (!get || get(a->dev, a->cache, &size, NULL) != VK_SUCCESS || size < 32 || size > 256 * 1024 * 1024) return;
    void *bytes = malloc(size);
    if (!bytes) return;
    if (get(a->dev, a->cache, &size, bytes) == VK_SUCCESS) {
        char temporary[4096]; snprintf(temporary, sizeof temporary, "%s.XXXXXX", a->path);
        int fd = mkstemp(temporary);
        if (fd >= 0) {
            size_t written = 0;
            while (written < size) {
                ssize_t n = write(fd, (char *)bytes + written, size - written);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                written += (size_t)n;
            }
            int closed = close(fd) == 0;
            if (written == size && closed && rename(temporary, a->path) == 0) {
                a->dirty = 0; a->saved_ms = kl_pipe_ms();
            }
            unlink(temporary);
        }
    }
    free(bytes);
}
static void kva_archive_seed(VkDevice dev, VkPipelineCache cache) {
    if (!cache) return;
    // Restoring/saving the archive is optional; busy cache work must never
    // monopolize the material workers. Their private/engine caches still work.
    if (pthread_mutex_trylock(&g_kva_archive_mu)) return;
    kva_archive *a = kva_archive_locked(dev);
    PFN_vkMergePipelineCaches merge = (PFN_vkMergePipelineCaches)real_gdpa(dev, "vkMergePipelineCaches");
    if (a && merge) merge(dev, cache, 1, &a->cache);
    pthread_mutex_unlock(&g_kva_archive_mu);
}
static void kva_archive_publish(VkDevice dev, VkPipelineCache cache) {
    if (!cache) return;
    if (pthread_mutex_trylock(&g_kva_archive_mu)) return;
    kva_archive *a = kva_archive_locked(dev);
    PFN_vkMergePipelineCaches merge = (PFN_vkMergePipelineCaches)real_gdpa(dev, "vkMergePipelineCaches");
    if (a && merge && merge(dev, a->cache, 1, &cache) == VK_SUCCESS) {
        a->dirty = 1;
        kva_archive_save_locked(a, 0);
    }
    pthread_mutex_unlock(&g_kva_archive_mu);
}
static void kva_archive_close(VkDevice dev) {
    pthread_mutex_lock(&g_kva_mu);
    while (g_kva_pending || g_kva_finishing) pthread_cond_wait(&g_kva_ready, &g_kva_mu);
    pthread_mutex_unlock(&g_kva_mu);
    pthread_mutex_lock(&g_kva_archive_mu);
    kva_archive **at = &g_kva_archives;
    while (*at && (*at)->dev != dev) at = &(*at)->next;
    if (*at) {
        kva_archive *a = *at;
        while (a->loading) pthread_cond_wait(&g_kva_archive_ready, &g_kva_archive_mu);
        at = &g_kva_archives;
        while (*at && *at != a) at = &(*at)->next;
        *at = a->next;
        kva_archive_save_locked(a, 1);
        PFN_vkDestroyPipelineCache destroy = (PFN_vkDestroyPipelineCache)real_gdpa(dev, "vkDestroyPipelineCache");
        if (destroy) destroy(dev, a->cache, NULL);
        free(a);
    }
    pthread_mutex_unlock(&g_kva_archive_mu);
}
static void VKAPI_CALL klvk_DestroyDevice(VkDevice dev, const VkAllocationCallbacks *alloc) {
    kva_archive_close(dev);
    PFN_vkDestroyDevice real = real_gdpa ? (PFN_vkDestroyDevice)real_gdpa(dev, "vkDestroyDevice") : NULL;
    if (real) real(dev, alloc);
}

// Metal may hold a pipeline cache's own lock during compilation. Workers use
// private caches so an engine snapshot or merge never waits for that compiler.
static VkPipelineCache kva_private_cache(kva_pipe *p) {
    if (!real_gdpa) return VK_NULL_HANDLE;
    PFN_vkCreatePipelineCache create = (PFN_vkCreatePipelineCache)real_gdpa(p->dev, "vkCreatePipelineCache");
    PFN_vkMergePipelineCaches merge = (PFN_vkMergePipelineCaches)real_gdpa(p->dev, "vkMergePipelineCaches");
    VkPipelineCache cache = VK_NULL_HANDLE;
    VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    if (!create || !merge || create(p->dev, &ci, NULL, &cache) != VK_SUCCESS) return VK_NULL_HANDLE;
    // The archive supplies native materials independently of engine caches.
    // Even seeding from a small engine cache can wait inside the driver while
    // holding the lock its frame-worker exports need.
    VkResult r = VK_SUCCESS;
    if (p->cache && !kva_private_archive()) {
        pthread_mutex_lock(&g_kva_cache_mu);
        r = merge(p->dev, cache, 1, &p->cache);
        pthread_mutex_unlock(&g_kva_cache_mu);
    }
    if (r != VK_SUCCESS) VKI("[async] private cache seed failed (%d); compiling with an empty cache\n", (int)r);
    return cache;
}

static void kva_merge_private_locked(kva_pipe *p, VkPipelineCache cache) {
    if (!cache || !real_gdpa) return;
    if (kva_private_archive()) return;
    PFN_vkMergePipelineCaches merge = (PFN_vkMergePipelineCaches)real_gdpa(p->dev, "vkMergePipelineCaches");
    if (merge && p->cache) {
        VkResult r = merge(p->dev, p->cache, 1, &cache);
        if (r != VK_SUCCESS) VKI("[async] completed cache merge failed (%d)\n", (int)r);
    }
}

static VkResult kva_inline_graphics(PFN_vkCreateGraphicsPipelines real, VkDevice dev, VkPipelineCache cache,
        uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (kva_on() && cache) pthread_mutex_lock(&g_kva_cache_mu);
    VkResult r = real(dev, cache, n, ci, a, out);
    if (kva_on() && cache) kva_dirty_snapshot_locked(dev,cache);
    if (kva_on() && cache) pthread_mutex_unlock(&g_kva_cache_mu);
    return r;
}
static VkResult kva_inline_compute(PFN_vkCreateComputePipelines real, VkDevice dev, VkPipelineCache cache,
        uint32_t n, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (kva_on() && cache) pthread_mutex_lock(&g_kva_cache_mu);
    VkResult r = real(dev, cache, n, ci, a, out);
    if (kva_on() && cache) kva_dirty_snapshot_locked(dev,cache);
    if (kva_on() && cache) pthread_mutex_unlock(&g_kva_cache_mu);
    return r;
}

static void kva_prioritize_locked(kva_pipe *p);

static void *kva_worker(void *arg) {
    (void)arg;
    pthread_setname_np("kl-pipeline-compile");
    for (;;) {
        pthread_mutex_lock(&g_kva_mu);
        while (!g_kva_head) pthread_cond_wait(&g_kva_cv, &g_kva_mu);
        for (kva_pipe *job = g_kva_head; job; job = job->next_job) {
            if (atomic_load(&job->demanded)) { kva_prioritize_locked(job); break; }
        }
        kva_pipe *p = g_kva_head;
        g_kva_head = p->next_job;
        if (!g_kva_head) g_kva_tail = NULL;
        p->started_ms = kl_pipe_ms();
        pthread_mutex_unlock(&g_kva_mu);

        VkPipelineCache private_cache = kva_private_cache(p);
        kva_archive_seed(p->dev, private_cache);
        double t0 = kl_pipe_ms();
        VkPipeline real = VK_NULL_HANDLE;
        VkResult r = p->create(p->dev, private_cache, 1, p->ci, NULL, &real);
        double dt = kl_pipe_ms() - t0;

        if (!kva_private_archive()) {
            pthread_mutex_lock(&g_kva_cache_mu);
            kva_merge_private_locked(p, private_cache);
            pthread_mutex_unlock(&g_kva_cache_mu);
        }
        kva_free(&p->arena); p->ci = NULL;
        pthread_mutex_lock(&g_kva_mu);
        p->compiled_ms = dt;
        g_kva_ms += dt;
        if (r == VK_SUCCESS && real) {
            pthread_mutex_lock(&g_kva_visibility_mu);
            p->ready_serial = ++g_kva_completed_serial;
            atomic_store(&p->real, (uint64_t)real); atomic_store(&p->state, 1);
            pthread_mutex_unlock(&g_kva_visibility_mu);
            g_kva_done++;
        }
        else {
            atomic_store(&p->state, 2); g_kva_failed++;
            VKI("[async] graphics pipeline compilation failed (VkResult %d); its draws are suppressed\n", (int)r);
        }
        int destroy = p->destroy_requested;
        kva_pipe **active = &g_kva_active;
        while (*active && *active != p) active = &(*active)->next_active;
        if (*active) *active = p->next_active;
        VkDevice dev = p->dev;
        g_kva_finishing++;
        g_kva_pending--;
        kva_deferred *deferred = NULL; size_t deferred_count = 0;
        if (g_kva_pending == 0) {
            deferred = g_kva_def; deferred_count = g_kva_ndef;
            g_kva_def = NULL; g_kva_ndef = g_kva_capdef = 0;
        }
        unsigned long long done = g_kva_done;
        unsigned long long failed = g_kva_failed;
        double ms = g_kva_ms;
        int pending = g_kva_pending;
        pthread_cond_broadcast(&g_kva_ready);
        pthread_mutex_unlock(&g_kva_mu);
        if (destroy) {
            if (real && p->destroy) p->destroy(p->dev, real, NULL);
            p->magic = 0; free(p);
        }
        kva_flush_deferred(deferred, deferred_count);
        // Publish outside the recording/queue mutex; no frame waits for the disk cache.
        if (r == VK_SUCCESS) kva_archive_publish(dev, private_cache);
        PFN_vkDestroyPipelineCache destroy_cache = real_gdpa ?
            (PFN_vkDestroyPipelineCache)real_gdpa(dev, "vkDestroyPipelineCache") : NULL;
        if (private_cache && destroy_cache) destroy_cache(dev, private_cache, NULL);
        pthread_mutex_lock(&g_kva_mu);
        g_kva_finishing--; pthread_cond_broadcast(&g_kva_ready);
        pthread_mutex_unlock(&g_kva_mu);
        if ((done % 50 == 0 && done) || pending == 0) {
            VKI("[async] %llu pipelines compiled off the render thread (%.1f s), %llu failed, %llu compiled inline, "
                "%llu pending binds deferred without waiting, %llu failed binds suppressed, %d pending\n",
                done, ms / 1000.0, failed, __atomic_load_n(&g_kva_sync, __ATOMIC_RELAXED),
                __atomic_load_n(&g_kva_deferred_binds, __ATOMIC_RELAXED),
                __atomic_load_n(&g_kva_failed_binds, __ATOMIC_RELAXED), pending);
        }
    }
    return NULL;
}

static void kva_spawn_workers(void) {
    const char *e = getenv("KL_VK_ASYNC_THREADS");
    int n = e && *e ? atoi(e) : 3;
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) { pthread_t t; pthread_create(&t, NULL, kva_worker, NULL); pthread_detach(t); }
    VKI("[async] pipeline compiles moved off the render thread (%d workers) with private caches; nonblocking colour/depth binds; materials activate at stereo frame boundaries\n", n);
}
static void kva_start_workers(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, kva_spawn_workers);
}

// Returns 1 and fills out[i] with placeholders when it took the pipelines.
static int kva_create(VkDevice dev, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, VkPipeline *out,
                      PFN_vkCreateGraphicsPipelines real) {
    if (!kva_on() || !real || !ci || !n || !out) return 0;
    for (uint32_t i = 0; i < n; i++)   // a batch with an in-batch base index: keep it synchronous
        if ((ci[i].flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) && ci[i].basePipelineIndex >= 0) return 0;
    kva_pipe **made = (kva_pipe **)calloc(n, sizeof *made);
    if (!made) return 0;
    for (uint32_t i = 0; i < n; i++) {
        kva_pipe *p = (kva_pipe *)calloc(1, sizeof *p);
        if (!p) goto fail;
        p->magic = KVA_MAGIC; p->dev = dev; p->cache = cache;
        p->depth_only = kva_depth_only(&ci[i]);
        if (p->depth_only) {
            static unsigned long long targets;
            if (__atomic_add_fetch(&targets, 1, __ATOMIC_RELAXED) <= 32)
                VKI("[async] depth/stencil pipeline: renderPass=%#llx subpass=%u targets=%d\n",
                    (unsigned long long)(uintptr_t)ci[i].renderPass, ci[i].subpass, kva_render_usage(&ci[i]));
        }
        p->create = real;
        p->destroy = real_gdpa ? (PFN_vkDestroyPipeline)real_gdpa(dev, "vkDestroyPipeline") : NULL;
        p->ci = kva_copy_gpci(&p->arena, &ci[i]);
        if (!p->ci) { kva_free(&p->arena); free(p); goto fail; }
        made[i] = p;
    }
    kva_start_workers();
    pthread_mutex_lock(&g_kva_mu);
    for (uint32_t i = 0; i < n; i++) {
        kva_pipe *p = made[i];
        out[i] = (VkPipeline)(uintptr_t)p;
        p->queued_ms = kl_pipe_ms();
        p->next_active = g_kva_active; g_kva_active = p;
        if (g_kva_tail) g_kva_tail->next_job = p; else g_kva_head = p;
        g_kva_tail = p;
        g_kva_pending++;
    }
    pthread_cond_broadcast(&g_kva_cv);
    pthread_mutex_unlock(&g_kva_mu);
    free(made);
    return 1;
fail:
    for (uint32_t i = 0; i < n; i++) if (made[i]) { kva_free(&made[i]->arena); free(made[i]); }
    free(made);
    __atomic_add_fetch(&g_kva_sync, 1, __ATOMIC_RELAXED);
    return 0;
}

// Draw suppression is scoped to a command buffer until its next graphics bind.
#define KVA_CB_SLOTS 8192
static struct { _Atomic(uintptr_t) cb; _Atomic int skip; } g_kva_cb[KVA_CB_SLOTS];
static inline int kva_cb_slot(void *cb, int insert) {
    uintptr_t k = (uintptr_t)cb;
    unsigned h = (unsigned)((k >> 4) * 2654435761u) & (KVA_CB_SLOTS - 1);
    for (unsigned i = 0; i < KVA_CB_SLOTS; i++, h = (h + 1) & (KVA_CB_SLOTS - 1)) {
        uintptr_t cur = atomic_load(&g_kva_cb[h].cb);
        if (cur == k) return (int)h;
        if (cur == 0) {
            if (!insert) return -1;
            uintptr_t z = 0;
            if (atomic_compare_exchange_strong(&g_kva_cb[h].cb, &z, k) || z == k) return (int)h;
        }
    }
    return -1;
}
static inline void kva_set_skip(void *cb, int skip) {
    int s = kva_cb_slot(cb, skip);   // clearing a never-seen cb needs no slot
    if (s >= 0) atomic_store(&g_kva_cb[s].skip, skip);
}
static inline int kva_skip(void *cb) {
    if (!kva_on()) return 0;
    int s = kva_cb_slot(cb, 0);
    return s >= 0 && atomic_load(&g_kva_cb[s].skip);
}
// A draw must not sit behind unrelated warmup work that has not started yet.
static void kva_prioritize_locked(kva_pipe *p) {
    kva_pipe **at = &g_kva_head, *previous = NULL;
    while (*at && *at != p) { previous = *at; at = &(*at)->next_job; }
    if (*at && previous) {
        *at = p->next_job;
        if (g_kva_tail == p) g_kva_tail = previous;
        p->next_job = g_kva_head; g_kva_head = p;
        pthread_cond_signal(&g_kva_cv);
    }
}

// Never wait for a graphics pipeline on the thread recording a frame.
static inline uint64_t kva_bind(void *cb, uint32_t bindPoint, uint64_t pipeline) {
    if (!kva_on() || bindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS) return pipeline;
    kva_pipe *p = kva_of(pipeline);
    if (!p) { kva_set_skip(cb, 0); return pipeline; }
    int state = atomic_load(&p->state);
    if (state == 1 && p->ready_serial <= atomic_load(&g_kva_visible_serial)) {
        kva_set_skip(cb, 0); return atomic_load(&p->real);
    }
    kva_set_skip(cb, 1);
    if (state == 2) __atomic_add_fetch(&g_kva_failed_binds, 1, __ATOMIC_RELAXED);
    else {
        atomic_store(&p->demanded, 1);
        __atomic_add_fetch(&g_kva_deferred_binds, 1, __ATOMIC_RELAXED);
    }
    return 0;
}
// DestroyPipeline: returns 1 when it handled a placeholder.
static int kva_destroy_pipeline(VkDevice dev, uint64_t pipeline, PFN_vkDestroyPipeline real) {
    kva_pipe *p = kva_of(pipeline);
    if (!p) return 0;
    pthread_mutex_lock(&g_kva_mu);
    int st = atomic_load(&p->state);
    if (st == 0) { p->destroy_requested = 1; pthread_mutex_unlock(&g_kva_mu); return 1; }
    pthread_mutex_unlock(&g_kva_mu);
    uint64_t r = atomic_load(&p->real);
    if (r && real) real(dev, (VkPipeline)r, NULL);
    p->magic = 0; free(p);
    return 1;
}
// Destroys of objects pending compiles may use: deferred while any are pending.
static int kva_defer_destroy(int kind, VkDevice dev, uint64_t h) {
    if (!kva_on() || !h) return 0;
    pthread_mutex_lock(&g_kva_mu);
    if (g_kva_pending == 0) { pthread_mutex_unlock(&g_kva_mu); return 0; }
    if (g_kva_ndef == g_kva_capdef) {
        size_t cap = g_kva_capdef ? g_kva_capdef * 2 : 64;
        kva_deferred *n = (kva_deferred *)realloc(g_kva_def, cap * sizeof *n);
        if (!n) { pthread_mutex_unlock(&g_kva_mu); return 0; }
        g_kva_def = n; g_kva_capdef = cap;
    }
    g_kva_def[g_kva_ndef++] = (kva_deferred){ kind, dev, h };
    pthread_mutex_unlock(&g_kva_mu);
    return 1;
}
static void kva_flush_deferred(kva_deferred *items, size_t count) {
    static const char *names[] = { "vkDestroyShaderModule", "vkDestroyPipelineLayout", "vkDestroyRenderPass", "vkDestroyDescriptorSetLayout", "vkDestroyPipelineCache" };
    for (size_t i = 0; i < count; i++) {
        kva_deferred *d = &items[i];
        void (*fn)(VkDevice, uint64_t, const void *) = real_gdpa ? (void (*)(VkDevice, uint64_t, const void *))real_gdpa(d->dev, names[d->kind]) : NULL;
        if (fn) fn(d->dev, d->h, NULL);
    }
    free(items);
}

// ---- entry points (added to kl_vulkan.c's dispatch table by apply.py) ----
typedef void (*kva_draw_fn)(void *, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void (*kva_indirect_fn)(void *, uint64_t, uint64_t, uint32_t, uint32_t);
typedef void (*kva_count_fn)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint32_t, uint32_t);
#define KVA_CMD_REAL(T, name) \
    static T real; if (!real && real_gdpa) real = (T)real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, name)
static void VKAPI_CALL klvk_CmdDraw(void *cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    KVA_CMD_REAL(kva_draw_fn, "vkCmdDraw");
    if (kva_skip(cb)) return;
    if (real) real(cb, vc, ic, fv, fi);
}
static void VKAPI_CALL klvk_CmdDrawIndirect(void *cb, uint64_t buf, uint64_t off, uint32_t n, uint32_t stride) {
    KVA_CMD_REAL(kva_indirect_fn, "vkCmdDrawIndirect");
    if (kva_skip(cb)) return;
    if (real) real(cb, buf, off, n, stride);
}
static void VKAPI_CALL klvk_CmdDrawIndexedIndirect(void *cb, uint64_t buf, uint64_t off, uint32_t n, uint32_t stride) {
    KVA_CMD_REAL(kva_indirect_fn, "vkCmdDrawIndexedIndirect");
    if (kva_skip(cb)) return;
    if (real) real(cb, buf, off, n, stride);
}
static void VKAPI_CALL klvk_CmdDrawIndirectCount(void *cb, uint64_t buf, uint64_t off, uint64_t cbuf, uint64_t coff, uint32_t max, uint32_t stride) {
    KVA_CMD_REAL(kva_count_fn, "vkCmdDrawIndirectCount");
    if (kva_skip(cb)) return;
    if (real) real(cb, buf, off, cbuf, coff, max, stride);
}
static void VKAPI_CALL klvk_CmdDrawIndexedIndirectCount(void *cb, uint64_t buf, uint64_t off, uint64_t cbuf, uint64_t coff, uint32_t max, uint32_t stride) {
    KVA_CMD_REAL(kva_count_fn, "vkCmdDrawIndexedIndirectCount");
    if (kva_skip(cb)) return;
    if (real) real(cb, buf, off, cbuf, coff, max, stride);
}
static void VKAPI_CALL klvk_DestroyPipeline(VkDevice dev, uint64_t p, const VkAllocationCallbacks *a) {
    static PFN_vkDestroyPipeline real;
    if (!real && real_gdpa) real = (PFN_vkDestroyPipeline)real_gdpa(dev, "vkDestroyPipeline");
    if (kva_destroy_pipeline(dev, p, real)) return;
    if (real) real(dev, (VkPipeline)p, a);
}
#define KVA_DEFERRED_DESTROY(fn, kind, name)                                              \
    static void VKAPI_CALL klvk_##fn(VkDevice dev, uint64_t h, const VkAllocationCallbacks *a) { \
        static void (*real)(VkDevice, uint64_t, const VkAllocationCallbacks *);            \
        if (!real && real_gdpa) real = (void (*)(VkDevice, uint64_t, const VkAllocationCallbacks *))real_gdpa(dev, name); \
        if (kva_defer_destroy(kind, dev, h)) return;                                       \
        if (real) real(dev, h, a);                                                         \
    }
KVA_DEFERRED_DESTROY(DestroyShaderModule, 0, "vkDestroyShaderModule")
KVA_DEFERRED_DESTROY(DestroyPipelineLayout, 1, "vkDestroyPipelineLayout")
KVA_DEFERRED_DESTROY(DestroyDescriptorSetLayout, 3, "vkDestroyDescriptorSetLayout")

static void VKAPI_CALL klvk_DestroyRenderPass(VkDevice dev, uint64_t h, const VkAllocationCallbacks *a) {
    kva_forget_pass((VkRenderPass)h);
    if (kva_defer_destroy(2, dev, h)) return;
    PFN_vkDestroyRenderPass real = real_gdpa ? (PFN_vkDestroyRenderPass)real_gdpa(dev, "vkDestroyRenderPass") : NULL;
    if (real) real(dev, (VkRenderPass)h, a);
}

typedef struct kva_snapshot {
    VkDevice dev;
    VkPipelineCache cache;
    void *bytes;
    size_t size;
    int dirty;
    struct kva_snapshot *next;
} kva_snapshot;
static kva_snapshot *g_kva_snapshots; // under g_kva_cache_mu
static void kva_dirty_snapshot_locked(VkDevice dev, VkPipelineCache cache) {
    for (kva_snapshot *s=g_kva_snapshots; s; s=s->next)
        if (s->dev==dev && s->cache==cache) s->dirty=1;
}

static void VKAPI_CALL klvk_DestroyPipelineCache(VkDevice dev, uint64_t h, const VkAllocationCallbacks *a) {
    pthread_mutex_lock(&g_kva_cache_mu);
    kva_snapshot **at = &g_kva_snapshots;
    while (*at && ((*at)->dev != dev || (uint64_t)(*at)->cache != h)) at = &(*at)->next;
    if (*at) { kva_snapshot *old = *at; *at = old->next; free(old->bytes); free(old); }
    pthread_mutex_unlock(&g_kva_cache_mu);
    if (kva_defer_destroy(4, dev, h)) return;
    PFN_vkDestroyPipelineCache real = real_gdpa ? (PFN_vkDestroyPipelineCache)real_gdpa(dev, "vkDestroyPipelineCache") : NULL;
    if (real) real(dev, (VkPipelineCache)h, a);
}

static VkResult VKAPI_CALL klvk_CreatePipelineCache(VkDevice dev, const VkPipelineCacheCreateInfo *ci,
        const VkAllocationCallbacks *a, VkPipelineCache *out) {
    PFN_vkCreatePipelineCache real = real_gdpa ? (PFN_vkCreatePipelineCache)real_gdpa(dev, "vkCreatePipelineCache") : NULL;
    VkPipelineCacheCreateInfo safe;
    if (ci && kva_on()) {
        safe = *ci;
        // The engine's per-thread caches now also have a compilation worker.
        safe.flags &= ~(VkPipelineCacheCreateFlags)VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT;
        ci = &safe;
    }
    VkResult r = real ? real(dev, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (r == VK_SUCCESS && ci) VKI("[async] engine pipeline cache: %zu initial bytes\n", ci->initialDataSize);
    return r;
}
static VkResult VKAPI_CALL klvk_GetPipelineCacheData(VkDevice dev, VkPipelineCache cache, size_t *size, void *data) {
    PFN_vkGetPipelineCacheData real = real_gdpa ? (PFN_vkGetPipelineCacheData)real_gdpa(dev, "vkGetPipelineCacheData") : NULL;
    if (!real || !size) return VK_ERROR_INITIALIZATION_FAILED;
    double started = kl_pipe_ms();
    pthread_mutex_lock(&g_kva_cache_mu);
    kva_snapshot *snapshot = g_kva_snapshots;
    while (snapshot && (snapshot->dev != dev || snapshot->cache != cache)) snapshot = snapshot->next;
    if (!snapshot) {
        snapshot = calloc(1, sizeof *snapshot);
        if (!snapshot) { pthread_mutex_unlock(&g_kva_cache_mu); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        snapshot->dev = dev; snapshot->cache = cache; snapshot->next = g_kva_snapshots; g_kva_snapshots = snapshot;
    }
    VkResult r = VK_SUCCESS;
    int reusable=kva_private_archive();
    if (!snapshot->bytes || (!data && (!reusable || snapshot->dirty))) {
        // Keep the size/data pair coherent while workers add entries. A Vulkan
        // cache snapshot may contain only work completed at the time of query.
        size_t length = 0;
        r = real(dev, cache, &length, NULL);
        if (r == VK_SUCCESS && length <= 128u * 1024u * 1024u) {
            void *bytes = malloc(length ? length : 1);
            if (!bytes) r = VK_ERROR_OUT_OF_HOST_MEMORY;
            else {
                r = real(dev, cache, &length, bytes);
                if (r == VK_SUCCESS || r == VK_INCOMPLETE) {
                    free(snapshot->bytes); snapshot->bytes = bytes; snapshot->size = length; snapshot->dirty=0; r = VK_SUCCESS;
                } else free(bytes);
            }
        } else if (r == VK_SUCCESS) r = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (r == VK_SUCCESS) {
        if (!data) *size = snapshot->size;
        else {
            // An arbitrary byte prefix need not be a valid driver cache blob.
            if (*size < snapshot->size) { *size = 0; r = VK_INCOMPLETE; }
            else {
                memcpy(data, snapshot->bytes, snapshot->size); *size = snapshot->size;
                if (!reusable) { free(snapshot->bytes); snapshot->bytes = NULL; snapshot->size = 0; }
            }
        }
    }
    pthread_mutex_unlock(&g_kva_cache_mu);
    double elapsed = kl_pipe_ms() - started;
    if (elapsed >= 8.0) VKI("[async] engine cache export: %zu bytes, %.1f ms, result %d\n", *size, elapsed, (int)r);
    return r;
}
static VkResult VKAPI_CALL klvk_MergePipelineCaches(VkDevice dev, VkPipelineCache dst, uint32_t n, const VkPipelineCache *src) {
    PFN_vkMergePipelineCaches real = real_gdpa ? (PFN_vkMergePipelineCaches)real_gdpa(dev, "vkMergePipelineCaches") : NULL;
    pthread_mutex_lock(&g_kva_cache_mu);
    // Only completed worker results touch engine caches, under this mutex.
    VkResult r = real ? real(dev, dst, n, src) : VK_ERROR_INITIALIZATION_FAILED;
    kva_dirty_snapshot_locked(dev,dst);
    pthread_mutex_unlock(&g_kva_cache_mu);
    return r;
}
