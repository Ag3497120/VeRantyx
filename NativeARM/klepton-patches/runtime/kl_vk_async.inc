// kl_vk_async.inc — asynchronous graphics-pipeline creation (VeRantyx).
// Included into kl_vulkan.c by klepton-patches/apply.py.
//
// MoltenVK compiles a pipeline inside vkCreateGraphicsPipelines; the first
// time Source 2 meets a material that is 250-500 ms on its render thread (the
// hitches when characters or new areas appear). With KL_VK_ASYNC_PIPELINES=1
// the create info is deep-copied, the guest gets a placeholder handle at once,
// and worker threads compile. Binding a placeholder that is not ready yet makes
// the command buffer skip draws until its next bind: the object appears a few
// frames late instead of the whole frame freezing. Anything the copy does not
// understand (an unknown pNext) is compiled synchronously as before.
//
// While compiles are in flight, destroys of shader modules, pipeline layouts,
// render passes and descriptor-set layouts are deferred (the pending compiles
// still reference them); they run when the queue drains.

#include <pthread.h>
#include <stdatomic.h>

#define KVA_MAGIC 0x4b4c41535950524full   /* "ORPYSALK" — never an MVK vtable */

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
    KVA_STATE(VkPipelineDepthStencilStateCreateInfo, pDepthStencilState)
    KVA_STATE(VkPipelineColorBlendStateCreateInfo, pColorBlendState)
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
    int destroy_requested;          // under g_kva_mu
    VkDevice dev;
    kva_arena arena;
    VkGraphicsPipelineCreateInfo *ci;
    struct kva_pipe *next_job;
    double queued_ms;
} kva_pipe;

static pthread_mutex_t g_kva_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_kva_cv = PTHREAD_COND_INITIALIZER;
static kva_pipe *g_kva_head, *g_kva_tail;
static int g_kva_pending;                   // queued + compiling
static unsigned long long g_kva_done, g_kva_failed, g_kva_sync, g_kva_skipped_binds;
static double g_kva_ms;
static PFN_vkCreateGraphicsPipelines g_kva_create;
static PFN_vkDestroyPipeline g_kva_destroy_pipeline;

typedef struct { int kind; VkDevice dev; uint64_t h; } kva_deferred;   // kind: 0 module 1 layout 2 renderpass 3 dsl
static kva_deferred *g_kva_def; static size_t g_kva_ndef, g_kva_capdef;

static int kva_on(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_ASYNC_PIPELINES", 0);
    return on;
}
static inline kva_pipe *kva_of(uint64_t h) {
    if (!h) return NULL;
    const kva_pipe *p = (const kva_pipe *)(uintptr_t)h;
    return p->magic == KVA_MAGIC ? (kva_pipe *)p : NULL;
}

static void kva_flush_deferred_locked(void);

static void *kva_worker(void *arg) {
    (void)arg;
    pthread_setname_np("kl-pipeline-compile");
    for (;;) {
        pthread_mutex_lock(&g_kva_mu);
        while (!g_kva_head) pthread_cond_wait(&g_kva_cv, &g_kva_mu);
        kva_pipe *p = g_kva_head;
        g_kva_head = p->next_job;
        if (!g_kva_head) g_kva_tail = NULL;
        pthread_mutex_unlock(&g_kva_mu);

        double t0 = kl_pipe_ms();
        VkPipeline real = VK_NULL_HANDLE;
        VkResult r = g_kva_create(p->dev, VK_NULL_HANDLE, 1, p->ci, NULL, &real);
        double dt = kl_pipe_ms() - t0;

        pthread_mutex_lock(&g_kva_mu);
        kva_free(&p->arena); p->ci = NULL;
        g_kva_ms += dt;
        if (r == VK_SUCCESS && real) { atomic_store(&p->real, (uint64_t)real); atomic_store(&p->state, 1); g_kva_done++; }
        else { atomic_store(&p->state, 2); g_kva_failed++; }
        int destroy = p->destroy_requested;
        g_kva_pending--;
        if (g_kva_pending == 0) kva_flush_deferred_locked();
        unsigned long long done = g_kva_done;
        pthread_mutex_unlock(&g_kva_mu);
        if (destroy) {
            if (real && g_kva_destroy_pipeline) g_kva_destroy_pipeline(p->dev, real, NULL);
            p->magic = 0; free(p);
        }
        if (done % 50 == 0 && done) {
            VKI("[async] %llu pipelines compiled off the render thread (%.1f s), %llu failed, %llu compiled inline, "
                "%llu binds skipped while compiling, %d pending\n",
                done, g_kva_ms / 1000.0, g_kva_failed, g_kva_sync, g_kva_skipped_binds, g_kva_pending);
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
    VKI("[async] pipeline compiles moved off the render thread (%d workers)\n", n);
}
static void kva_start_workers(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, kva_spawn_workers);
}

// Returns 1 and fills out[i] with placeholders when it took the pipelines.
static int kva_create(VkDevice dev, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, VkPipeline *out,
                      PFN_vkCreateGraphicsPipelines real) {
    if (!kva_on() || !real || !ci || !n || !out) return 0;
    for (uint32_t i = 0; i < n; i++)   // a batch with an in-batch base index: keep it synchronous
        if ((ci[i].flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) && ci[i].basePipelineIndex >= 0) return 0;
    kva_pipe **made = (kva_pipe **)calloc(n, sizeof *made);
    if (!made) return 0;
    for (uint32_t i = 0; i < n; i++) {
        kva_pipe *p = (kva_pipe *)calloc(1, sizeof *p);
        if (!p) goto fail;
        p->magic = KVA_MAGIC; p->dev = dev;
        p->ci = kva_copy_gpci(&p->arena, &ci[i]);
        if (!p->ci) { kva_free(&p->arena); free(p); goto fail; }
        made[i] = p;
    }
    g_kva_create = real;
    if (!g_kva_destroy_pipeline && real_gdpa) g_kva_destroy_pipeline = (PFN_vkDestroyPipeline)real_gdpa(dev, "vkDestroyPipeline");
    kva_start_workers();
    pthread_mutex_lock(&g_kva_mu);
    for (uint32_t i = 0; i < n; i++) {
        kva_pipe *p = made[i];
        out[i] = (VkPipeline)(uintptr_t)p;
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

// Per-command-buffer "skip draws" flag (open addressing, never shrinks).
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
// Bind: returns the handle to bind, or 0 to bind nothing (draws skipped).
static inline uint64_t kva_bind(void *cb, uint32_t bindPoint, uint64_t pipeline) {
    if (!kva_on() || bindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS) return pipeline;
    kva_pipe *p = kva_of(pipeline);
    if (!p) { kva_set_skip(cb, 0); return pipeline; }
    if (atomic_load(&p->state) == 1) { kva_set_skip(cb, 0); return atomic_load(&p->real); }
    kva_set_skip(cb, 1);
    __atomic_add_fetch(&g_kva_skipped_binds, 1, __ATOMIC_RELAXED);
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
static void kva_flush_deferred_locked(void) {
    static const char *names[] = { "vkDestroyShaderModule", "vkDestroyPipelineLayout", "vkDestroyRenderPass", "vkDestroyDescriptorSetLayout" };
    for (size_t i = 0; i < g_kva_ndef; i++) {
        kva_deferred *d = &g_kva_def[i];
        void (*fn)(VkDevice, uint64_t, const void *) = real_gdpa ? (void (*)(VkDevice, uint64_t, const void *))real_gdpa(d->dev, names[d->kind]) : NULL;
        if (fn) fn(d->dev, d->h, NULL);
    }
    g_kva_ndef = 0;
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
KVA_DEFERRED_DESTROY(DestroyRenderPass, 2, "vkDestroyRenderPass")
KVA_DEFERRED_DESTROY(DestroyDescriptorSetLayout, 3, "vkDestroyDescriptorSetLayout")
