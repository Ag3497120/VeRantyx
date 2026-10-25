// Queue submission and synchronization: each submission is one Metal command buffer; semaphores (binary and
// timeline) are Metal shared events, waited and signaled in the command buffer; fences are signaled by its completion.
#include "vk_impl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

void vk_encode(VkQueue q, id<MTLCommandBuffer> mcb, const VkCommandBuffer *cbs, uint32_t n);
void vk_mark_queries_available(VkQueryPool pool);
#define TRACE(...) do { if (vk_trace()) vx_log("vulkan: " __VA_ARGS__); } while (0)

// ---- VXRT_STATS ---------------------------------------------------------------------------------------------------
// Every 5 seconds of presents, one log line: the frame rate and frame times (between presents), and per frame what the
// Vulkan calls cost: submissions and the CPU time spent encoding them into Metal, time waiting (acquire, the next
// drawable, fences, semaphores, idle waits), pipelines created and the shader compiles they needed, descriptor updates,
// objects created, draws and passes, bytes copied, and GPU time (the union of the command buffers' GPU spans).
_Atomic uint64_t vk_stats[VKS_COUNT];
int vk_stats_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("VXRT_STATS") && atoi(getenv("VXRT_STATS"));
    return on;
}
// ---- tuning switches (vk_option) ------------------------------------------------------------------------------------
static struct { char name[32]; int value; } g_live[16];
static int g_live_count;
static uint64_t g_live_read;
static pthread_mutex_t g_live_lock = PTHREAD_MUTEX_INITIALIZER;
static void live_read(const char *path) {     // (the switches in the file; each change is logged: where a run switched)
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    int n = 0;
    while (n < 16 && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        int v = atoi(eq + 1), was = -1;
        for (int k = 0; k < g_live_count; k++) if (!strcmp(g_live[k].name, line)) was = g_live[k].value;
        if (was != v) vx_log("vulkan: VXRT_VK_LIVE: %s=%d", line, v);
        snprintf(g_live[n].name, sizeof g_live[n].name, "%s", line);
        g_live[n++].value = v;
    }
    fclose(f);
    g_live_count = n;
}
int vk_option(const char *name, int dflt) {
    const char *live = getenv("VXRT_VK_LIVE");
    if (live && *live) {
        pthread_mutex_lock(&g_live_lock);
        uint64_t now = vk_now_ns();
        if (now - g_live_read > 1000000000ull) { g_live_read = now; live_read(live); }
        for (int i = 0; i < g_live_count; i++)
            if (!strcmp(g_live[i].name, name)) { int v = g_live[i].value; pthread_mutex_unlock(&g_live_lock); return v; }
        pthread_mutex_unlock(&g_live_lock);
    }
    char env[64];
    snprintf(env, sizeof env, "VXRT_VK_%s", name);
    const char *v = getenv(env);
    return v && *v ? atoi(v) : dflt;
}

static uint64_t g_gpu_last_end;                 // (the GPU time already counted ends there; completions come in order)
static pthread_mutex_t g_gpu_lock = PTHREAD_MUTEX_INITIALIZER;
void vk_stats_gpu(id<MTLCommandBuffer> mcb) {
    if (!vk_stats_on()) return;
    [mcb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        CFTimeInterval a = done.GPUStartTime, b = done.GPUEndTime;
        if (b <= a || a <= 0) return;
        uint64_t s = (uint64_t)(a * 1e9), e = (uint64_t)(b * 1e9);
        pthread_mutex_lock(&g_gpu_lock);
        if (s < g_gpu_last_end) s = g_gpu_last_end;
        if (e > s) { atomic_fetch_add(&vk_stats[VKS_GPU_NS], e - s); g_gpu_last_end = e; }
        pthread_mutex_unlock(&g_gpu_lock);
    }];
}
static int cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
void vk_stats_frame(void) {
    if (!vk_stats_on()) return;
    static uint64_t last_report, last_frame, frames, times[4096];
    static uint32_t ntimes;
    static uint64_t prev[VKS_COUNT];
    uint64_t now = vk_now_ns();
    if (!last_report) { last_report = last_frame = now; for (int i = 0; i < VKS_COUNT; i++) prev[i] = atomic_load(&vk_stats[i]); return; }
    if (ntimes < 4096) times[ntimes++] = now - last_frame;
    last_frame = now;
    frames++;
    if (now - last_report < 5000000000ull) return;
    uint64_t d[VKS_COUNT];
    for (int i = 0; i < VKS_COUNT; i++) { uint64_t v = atomic_load(&vk_stats[i]); d[i] = v - prev[i]; prev[i] = v; }
    double secs = (now - last_report) / 1e9, f = (double)frames;
    qsort(times, ntimes, sizeof times[0], cmp_u64);
    #define MS(x) ((x) / 1e6)
    #define PF(i) (d[i] / f)
    #define PFMS(i) (d[i] / f / 1e6)
    vx_log("vulkan stats: %.1f fps, frame ms p50 %.1f p95 %.1f p99 %.1f max %.1f; GPU busy %.0f%% (%.1f ms/frame)",
           f / secs, MS(times[ntimes / 2]), MS(times[ntimes * 95 / 100]), MS(times[ntimes * 99 / 100]), MS(times[ntimes - 1]),
           d[VKS_GPU_NS] / 1e7 / secs, PFMS(VKS_GPU_NS));
    vx_log("vulkan stats per frame: submit %.1f (cmdbufs %.1f, Metal cbs %.1f) %.2f ms [encode %.2f ms, keyed %.2f ms]; present %.2f ms "
           "[drawable %.2f ms]; acquire wait %.2f ms; fence waits %.1f %.2f ms; semaphore waits %.1f %.2f ms; idle waits %.2f %.2f ms (query %llu)",
           PF(VKS_SUBMITS), PF(VKS_CMDBUFS), PF(VKS_MTL_CBS), PFMS(VKS_SUBMIT_NS), PFMS(VKS_ENCODE_NS), PFMS(VKS_KEYED_NS),
           PFMS(VKS_PRESENT_NS), PFMS(VKS_DRAWABLE_NS), PFMS(VKS_ACQUIRE_NS), PF(VKS_FENCE_WAITS), PFMS(VKS_FENCE_NS),
           PF(VKS_SEM_WAITS), PFMS(VKS_SEM_NS), PF(VKS_IDLE_WAITS), PFMS(VKS_IDLE_NS), (unsigned long long)d[VKS_QUERY_IDLE]);
    vx_log("vulkan stats per frame: draws %.0f (indirect %.0f, gs/tess %.0f), dispatches %.0f, passes %.1f, encoders %.1f, copy %.2f MB, "
           "update %.1f KB; descriptor updates %.1f (%.1f writes, %.1f templates, %.1f sets allocated)",
           PF(VKS_DRAWS), PF(VKS_INDIRECT), PF(VKS_GS_TESS), PF(VKS_DISPATCHES), PF(VKS_PASSES), PF(VKS_ENCODERS), PF(VKS_COPY_BYTES) / 1e6,
           PF(VKS_UPDATE_BYTES) / 1e3, PF(VKS_DESC_UPDATES), PF(VKS_DESC_WRITES), PF(VKS_DESC_TEMPLATES), PF(VKS_DESC_ALLOCS));
    vx_log("vulkan stats per frame: barriers %.1f (%.1f inside render passes); render passes waiting at their vertex stage %.1f",
           PF(VKS_BARRIERS), PF(VKS_BARRIERS_IN_PASS), PF(VKS_VERTEX_WAITS));
    if (d[VKS_TESS_PATCHES])
        vx_log("vulkan stats per frame: tessellation kernels: %.0f vertices, %.0f patches, %.0f control points out",
               PF(VKS_TESS_VERTS), PF(VKS_TESS_PATCHES), PF(VKS_TESS_CP));
    vx_log("vulkan stats in %.1f s: pipelines %llu graphics %.0f ms, %llu compute %.0f ms (translate %.0f ms; MSL compiles %llu %.0f ms, "
           "%llu cached); created %llu buffers, %llu images, %llu views, %llu allocations %.1f MB, %llu maps",
           secs, (unsigned long long)d[VKS_GFX_PIPES], MS(d[VKS_GFX_PIPE_NS]), (unsigned long long)d[VKS_COMP_PIPES], MS(d[VKS_COMP_PIPE_NS]),
           MS(d[VKS_TRANSLATE_NS]), (unsigned long long)d[VKS_MSL_COMPILES], MS(d[VKS_MSL_COMPILE_NS]), (unsigned long long)d[VKS_MSL_CACHED],
           (unsigned long long)d[VKS_BUFFERS], (unsigned long long)d[VKS_IMAGES], (unsigned long long)d[VKS_VIEWS],
           (unsigned long long)d[VKS_ALLOCS], d[VKS_ALLOC_BYTES] / 1e6, (unsigned long long)d[VKS_MAPS]);
    #undef MS
    #undef PF
    #undef PFMS
    last_report = now;
    frames = 0;
    ntimes = 0;
}

// ---- fences ------------------------------------------------------------------------------------------------------
VKFN VkResult VKAPI_CALL vkCreateFence(VkDevice dev, const VkFenceCreateInfo *info, const VkAllocationCallbacks *alloc, VkFence *out) {
    (void)dev; (void)alloc;
    struct VkFence_T *f = calloc(1, sizeof *f);
    if (!f) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pthread_mutex_init(&f->lock, NULL);
    pthread_cond_init(&f->cond, NULL);
    f->signaled = (info->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0;
    *out = f;
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkDestroyFence(VkDevice dev, VkFence f, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    if (!f) return;
    pthread_mutex_lock(&f->lock);
    while (f->pending) pthread_cond_wait(&f->cond, &f->lock);   // (its submission still refers to it)
    pthread_mutex_unlock(&f->lock);
    pthread_mutex_destroy(&f->lock);
    pthread_cond_destroy(&f->cond);
    free(f);
}
static void fence_signal(VkFence f) {
    pthread_mutex_lock(&f->lock);
    f->signaled = 1;
    if (f->pending) f->pending--;
    pthread_cond_broadcast(&f->cond);
    pthread_mutex_unlock(&f->lock);
}
VKFN VkResult VKAPI_CALL vkGetFenceStatus(VkDevice dev, VkFence f) {
    (void)dev;
    pthread_mutex_lock(&f->lock);
    int s = f->signaled;
    pthread_mutex_unlock(&f->lock);
    return s ? VK_SUCCESS : VK_NOT_READY;
}
VKFN VkResult VKAPI_CALL vkResetFences(VkDevice dev, uint32_t n, const VkFence *fs) {
    (void)dev;
    for (uint32_t i = 0; i < n; i++) {
        pthread_mutex_lock(&fs[i]->lock);
        fs[i]->signaled = 0;
        pthread_mutex_unlock(&fs[i]->lock);
    }
    return VK_SUCCESS;
}
static void deadline_of(uint64_t timeout, struct timespec *ts) {
    clock_gettime(CLOCK_REALTIME, ts);
    uint64_t ns = timeout > 3600ull * 1000000000ull ? 3600ull * 1000000000ull : timeout;
    ts->tv_sec += (time_t)(ns / 1000000000ull);
    ts->tv_nsec += (long)(ns % 1000000000ull);
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}
static VkResult wait_for_fences(VkDevice dev, uint32_t n, const VkFence *fs, VkBool32 all, uint64_t timeout);
VKFN VkResult VKAPI_CALL vkWaitForFences(VkDevice dev, uint32_t n, const VkFence *fs, VkBool32 all, uint64_t timeout) {
    VK_STAT_T0(t);
    VkResult r = wait_for_fences(dev, n, fs, all, timeout);
    if (timeout) { VK_STAT(VKS_FENCE_WAITS, 1); VK_STAT_NS(VKS_FENCE_NS, t); }
    return r;
}
static VkResult wait_for_fences(VkDevice dev, uint32_t n, const VkFence *fs, VkBool32 all, uint64_t timeout) {
    (void)dev;
    struct timespec ts;
    deadline_of(timeout, &ts);
    int forever = timeout == UINT64_MAX;
    for (;;) {
        uint32_t done = 0;
        for (uint32_t i = 0; i < n; i++) if (vkGetFenceStatus(dev, fs[i]) == VK_SUCCESS) done++;
        if (all ? done == n : done > 0) return VK_SUCCESS;
        if (!timeout) return VK_TIMEOUT;
        // (waits on the first unsignaled fence; any of them signaling wakes its waiter)
        VkFence f = NULL;
        for (uint32_t i = 0; i < n && !f; i++) if (vkGetFenceStatus(dev, fs[i]) != VK_SUCCESS) f = fs[i];
        pthread_mutex_lock(&f->lock);
        int r = 0;
        if (!f->signaled) {
            if (forever || !all) {
                struct timespec slice;
                deadline_of(all ? 1000000000ull : 1000000ull, &slice);   // (any: poll the others every millisecond)
                if (!forever && (slice.tv_sec > ts.tv_sec || (slice.tv_sec == ts.tv_sec && slice.tv_nsec > ts.tv_nsec))) slice = ts;
                r = pthread_cond_timedwait(&f->cond, &f->lock, &slice);
                if (r == ETIMEDOUT && forever) r = 0;
            } else r = pthread_cond_timedwait(&f->cond, &f->lock, &ts);
        }
        pthread_mutex_unlock(&f->lock);
        if (r == ETIMEDOUT) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (!forever && (now.tv_sec > ts.tv_sec || (now.tv_sec == ts.tv_sec && now.tv_nsec >= ts.tv_nsec))) {
                done = 0;
                for (uint32_t i = 0; i < n; i++) if (vkGetFenceStatus(dev, fs[i]) == VK_SUCCESS) done++;
                return (all ? done == n : done > 0) ? VK_SUCCESS : VK_TIMEOUT;
            }
        }
    }
}

// ---- semaphores --------------------------------------------------------------------------------------------------
VKFN VkResult VKAPI_CALL vkCreateSemaphore(VkDevice dev, const VkSemaphoreCreateInfo *info, const VkAllocationCallbacks *alloc, VkSemaphore *out) {
    (void)alloc;
    struct VkSemaphore_T *s = calloc(1, sizeof *s);
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    const VkSemaphoreTypeCreateInfo *t = vk_find_struct(info->pNext, VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
    s->timeline = t && t->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE;
    s->event = [dev->mtl newSharedEvent];
    if (s->timeline) { s->event.signaledValue = t->initialValue; atomic_store(&s->value, t->initialValue); }
    *out = s;
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkDestroySemaphore(VkDevice dev, VkSemaphore s, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    if (!s) return;
    s->event = nil;
    free(s);
}
VKFN VkResult VKAPI_CALL vkGetSemaphoreCounterValue(VkDevice dev, VkSemaphore s, uint64_t *v) {
    (void)dev;
    *v = s->event.signaledValue;
    return VK_SUCCESS;
}
VKFN VkResult VKAPI_CALL vkSignalSemaphore(VkDevice dev, const VkSemaphoreSignalInfo *info) {
    (void)dev;
    VkSemaphore s = info->semaphore;
    uint64_t cur = atomic_load(&s->value);
    while (cur < info->value && !atomic_compare_exchange_weak(&s->value, &cur, info->value)) {}
    s->event.signaledValue = info->value;
    return VK_SUCCESS;
}
static MTLSharedEventListener *listener(void) {
    static MTLSharedEventListener *l;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ l = [[MTLSharedEventListener alloc] initWithDispatchQueue:dispatch_queue_create("vxrt.vulkan.events", NULL)]; });
    return l;
}
// Waits until the event reaches the value, or the deadline: 1 reached, 0 timed out.
static int event_wait(id<MTLSharedEvent> ev, uint64_t value, uint64_t timeout_ns) {
    if (ev.signaledValue >= value) return 1;
    if (!timeout_ns) return 0;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [ev notifyListener:listener() atValue:value block:^(id<MTLSharedEvent> e, uint64_t v) { (void)e; (void)v; dispatch_semaphore_signal(sem); }];
    dispatch_time_t t = timeout_ns == UINT64_MAX ? DISPATCH_TIME_FOREVER : dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeout_ns > INT64_MAX ? INT64_MAX : timeout_ns));
    return dispatch_semaphore_wait(sem, t) == 0 || ev.signaledValue >= value;
}
static VkResult wait_semaphores(VkDevice dev, const VkSemaphoreWaitInfo *info, uint64_t timeout);
VKFN VkResult VKAPI_CALL vkWaitSemaphores(VkDevice dev, const VkSemaphoreWaitInfo *info, uint64_t timeout) {
    VK_STAT_T0(t);
    VkResult r = wait_semaphores(dev, info, timeout);
    if (timeout) { VK_STAT(VKS_SEM_WAITS, 1); VK_STAT_NS(VKS_SEM_NS, t); }
    return r;
}
static VkResult wait_semaphores(VkDevice dev, const VkSemaphoreWaitInfo *info, uint64_t timeout) {
    (void)dev;
    int any = (info->flags & VK_SEMAPHORE_WAIT_ANY_BIT) != 0;
    if (any) {
        uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        for (;;) {
            for (uint32_t i = 0; i < info->semaphoreCount; i++)
                if (info->pSemaphores[i]->event.signaledValue >= info->pValues[i]) return VK_SUCCESS;
            uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            if (timeout != UINT64_MAX && now - start >= timeout) return VK_TIMEOUT;
            event_wait(info->pSemaphores[0]->event, info->pValues[0], 1000000);
        }
    }
    uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    for (uint32_t i = 0; i < info->semaphoreCount; i++) {
        uint64_t used = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
        uint64_t left = timeout == UINT64_MAX ? UINT64_MAX : used >= timeout ? 0 : timeout - used;
        if (!event_wait(info->pSemaphores[i]->event, info->pValues[i], left)) return VK_TIMEOUT;
    }
    return VK_SUCCESS;
}

int vx_shared_keyed_acquire(int slot, uint64_t key, uint32_t ms);   // d3d11_shared.m
void vx_shared_keyed_release(int slot, uint64_t key);
void vx_shared_keyed_gpu_done(int slot);

// ---- submission --------------------------------------------------------------------------------------------------
VkResult vk_queue_submit_internal(VkQueue q, uint32_t n, const VkSubmitInfo2 *submits, VkFence fence) {
    VK_STAT_T0(t_submit);
    pthread_mutex_lock(&q->lock);
    if (fence) {
        pthread_mutex_lock(&fence->lock);
        fence->pending++;
        pthread_mutex_unlock(&fence->lock);
    }
    for (uint32_t i = 0; i < n || (i == 0 && n == 0); i++) @autoreleasepool {
        id<MTLCommandBuffer> mcb = [q->mtl commandBuffer];
        VK_STAT(VKS_MTL_CBS, 1);
        vk_stats_gpu(mcb);
        const VkWin32KeyedMutexAcquireReleaseInfoKHR *km = NULL;
        if (n) {
            const VkSubmitInfo2 *s = &submits[i];
            // VK_KHR_win32_keyed_mutex: the shared images' mutexes are taken before the work (the CPU waits here, where
            // Windows lets the GPU wait) and given back after it (d3d11_shared.m).
            km = vk_find_struct(s->pNext, VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR);
            VK_STAT_T0(t_keyed);
            for (uint32_t k = 0; km && k < km->acquireCount; k++) {
                VkDeviceMemory m = km->pAcquireSyncs[k];
                if (m && m->keyed_slot && !vx_shared_keyed_acquire(m->keyed_slot - 1, km->pAcquireKeys[k], km->pAcquireTimeouts ? km->pAcquireTimeouts[k] : 0xffffffffu))
                    TRACE("keyed mutex of slot %d not acquired with key %llu", m->keyed_slot - 1, (unsigned long long)km->pAcquireKeys[k]);
            }
            if (km && km->acquireCount) VK_STAT_NS(VKS_KEYED_NS, t_keyed);
            for (uint32_t k = 0; k < s->waitSemaphoreInfoCount; k++) {
                VkSemaphore sem = s->pWaitSemaphoreInfos[k].semaphore;
                uint64_t v = sem->timeline ? s->pWaitSemaphoreInfos[k].value : atomic_load(&sem->value);
                if (v) [mcb encodeWaitForEvent:sem->event value:v];
            }
            VkCommandBuffer cbs[256];
            uint32_t nc = s->commandBufferInfoCount < 256 ? s->commandBufferInfoCount : 256;
            for (uint32_t k = 0; k < nc; k++) cbs[k] = s->pCommandBufferInfos[k].commandBuffer;
            VK_STAT_T0(t_encode);
            vk_encode(q, mcb, cbs, nc);
            VK_STAT_NS(VKS_ENCODE_NS, t_encode);
            VK_STAT(VKS_CMDBUFS, nc);
            for (uint32_t k = 0; k < s->signalSemaphoreInfoCount; k++) {
                VkSemaphore sem = s->pSignalSemaphoreInfos[k].semaphore;
                uint64_t v = sem->timeline ? s->pSignalSemaphoreInfos[k].value : atomic_fetch_add(&sem->value, 1) + 1;
                if (sem->timeline) { uint64_t cur = atomic_load(&sem->value); while (cur < v && !atomic_compare_exchange_weak(&sem->value, &cur, v)) {} }
                [mcb encodeSignalEvent:sem->event value:v];
            }
        }
        uint64_t number = ++q->submitted;
        [mcb encodeSignalEvent:q->done value:number];
        if (fence && (n == 0 || i == n - 1)) {
            VkFence f = fence;
            [mcb addCompletedHandler:^(id<MTLCommandBuffer> done) {
                if (done.status == MTLCommandBufferStatusError)
                    vk_log_unique("vulkan: GPU work failed: %s", done.error.localizedDescription.UTF8String);
                fence_signal(f);
            }];
        } else if (vk_trace()) {
            [mcb addCompletedHandler:^(id<MTLCommandBuffer> done) {
                if (done.status == MTLCommandBufferStatusError) vk_log_unique("vulkan: GPU work failed: %s", done.error.localizedDescription.UTF8String);
            }];
        }
        int released[64], nr = 0;
        for (uint32_t k = 0; km && k < km->releaseCount && nr < 64; k++) {
            VkDeviceMemory m = km->pReleaseSyncs[k];
            if (!m || !m->keyed_slot) continue;
            released[nr++] = m->keyed_slot - 1;
            vx_shared_keyed_release(m->keyed_slot - 1, km->pReleaseKeys[k]);
        }
        if (nr) {
            int *slots = malloc(sizeof(int) * (size_t)nr);
            memcpy(slots, released, sizeof(int) * (size_t)nr);
            int count = nr;
            [mcb addCompletedHandler:^(id<MTLCommandBuffer> done) { (void)done; for (int k = 0; k < count; k++) vx_shared_keyed_gpu_done(slots[k]); free(slots); }];
        }
        [mcb commit];
        if (n == 0) break;
    }
    pthread_mutex_unlock(&q->lock);
    if (n) { VK_STAT(VKS_SUBMITS, 1); VK_STAT_NS(VKS_SUBMIT_NS, t_submit); }
    return VK_SUCCESS;
}

VKFN VkResult VKAPI_CALL vkQueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *submits, VkFence fence) {
    return vk_queue_submit_internal(q, n, submits, fence);
}
VKFN VkResult VKAPI_CALL vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *submits, VkFence fence) {
    // (as version 2 submissions)
    VkSubmitInfo2 s2[64];
    if (n > 64) {
        for (uint32_t i = 0; i < n; i += 64) {
            VkResult r = vkQueueSubmit(q, n - i < 64 ? n - i : 64, submits + i, i + 64 >= n ? fence : VK_NULL_HANDLE);
            if (r) return r;
        }
        return VK_SUCCESS;
    }
    uint32_t total_w = 0, total_c = 0, total_s = 0;
    for (uint32_t i = 0; i < n; i++) { total_w += submits[i].waitSemaphoreCount; total_c += submits[i].commandBufferCount; total_s += submits[i].signalSemaphoreCount; }
    VkSemaphoreSubmitInfo *w = calloc(total_w + total_s + 1, sizeof *w);
    VkCommandBufferSubmitInfo *c = calloc(total_c + 1, sizeof *c);
    uint32_t wi = 0, ci = 0;
    for (uint32_t i = 0; i < n; i++) {
        const VkSubmitInfo *s = &submits[i];
        const VkTimelineSemaphoreSubmitInfo *tl = vk_find_struct(s->pNext, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
        // (the keyed mutexes of VK_KHR_win32_keyed_mutex go along: VkSubmitInfo2 takes the same structure)
        s2[i] = (VkSubmitInfo2){ VK_STRUCTURE_TYPE_SUBMIT_INFO_2, vk_find_struct(s->pNext, VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR),
                                 0, s->waitSemaphoreCount, w + wi, s->commandBufferCount, c + ci,
                                 s->signalSemaphoreCount, NULL };
        for (uint32_t k = 0; k < s->waitSemaphoreCount; k++, wi++)
            w[wi] = (VkSemaphoreSubmitInfo){ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, s->pWaitSemaphores[k],
                tl && k < tl->waitSemaphoreValueCount && tl->pWaitSemaphoreValues ? tl->pWaitSemaphoreValues[k] : 0, 0, 0 };
        for (uint32_t k = 0; k < s->commandBufferCount; k++, ci++)
            c[ci] = (VkCommandBufferSubmitInfo){ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, s->pCommandBuffers[k], 0 };
        s2[i].pSignalSemaphoreInfos = w + wi;
        for (uint32_t k = 0; k < s->signalSemaphoreCount; k++, wi++)
            w[wi] = (VkSemaphoreSubmitInfo){ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, s->pSignalSemaphores[k],
                tl && k < tl->signalSemaphoreValueCount && tl->pSignalSemaphoreValues ? tl->pSignalSemaphoreValues[k] : 0, 0, 0 };
    }
    VkResult r = vk_queue_submit_internal(q, n, s2, fence);
    free(w);
    free(c);
    return r;
}

VKFN VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue q) {
    pthread_mutex_lock(&q->lock);
    uint64_t target = q->submitted;
    pthread_mutex_unlock(&q->lock);
    VK_STAT_T0(t);
    event_wait(q->done, target, UINT64_MAX);
    VK_STAT(VKS_IDLE_WAITS, 1);
    VK_STAT_NS(VKS_IDLE_NS, t);
    return VK_SUCCESS;
}
VKFN VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice dev) {
    for (uint32_t i = 0; i < dev->queue_count; i++) vkQueueWaitIdle(&dev->queues[i]);
    return VK_SUCCESS;
}
VKFN VkResult VKAPI_CALL vkQueueBindSparse(VkQueue q, uint32_t n, const VkBindSparseInfo *info, VkFence fence) {
    (void)n; (void)info;
    // (sparse binding is not offered; the fence still signals)
    return vk_queue_submit_internal(q, 0, NULL, fence);
}

// ---- events ------------------------------------------------------------------------------------------------------
VKFN VkResult VKAPI_CALL vkCreateEvent(VkDevice dev, const VkEventCreateInfo *info, const VkAllocationCallbacks *alloc, VkEvent *out) {
    (void)dev; (void)info; (void)alloc;
    *out = calloc(1, sizeof(struct VkEvent_T));
    return *out ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
VKFN void VKAPI_CALL vkDestroyEvent(VkDevice dev, VkEvent e, const VkAllocationCallbacks *alloc) { (void)dev; (void)alloc; free(e); }
VKFN VkResult VKAPI_CALL vkGetEventStatus(VkDevice dev, VkEvent e) { (void)dev; return atomic_load(&e->set) ? VK_EVENT_SET : VK_EVENT_RESET; }
VKFN VkResult VKAPI_CALL vkSetEvent(VkDevice dev, VkEvent e) { (void)dev; atomic_store(&e->set, 1); return VK_SUCCESS; }
VKFN VkResult VKAPI_CALL vkResetEvent(VkDevice dev, VkEvent e) { (void)dev; atomic_store(&e->set, 0); return VK_SUCCESS; }

// ---- query pools -------------------------------------------------------------------------------------------------
VKFN VkResult VKAPI_CALL vkCreateQueryPool(VkDevice dev, const VkQueryPoolCreateInfo *info, const VkAllocationCallbacks *alloc, VkQueryPool *out) {
    (void)alloc;
    if (info->queryType != VK_QUERY_TYPE_OCCLUSION && info->queryType != VK_QUERY_TYPE_TIMESTAMP) {
        TRACE("query type %d not supported", info->queryType);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    struct VkQueryPool_T *p = calloc(1, sizeof *p);
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->type = info->queryType;
    p->count = info->queryCount;
    p->results = [dev->mtl newBufferWithLength:(info->queryCount ? info->queryCount : 1) * 8 options:MTLResourceStorageModeShared];
    p->available = calloc(info->queryCount ? info->queryCount : 1, 1);
    *out = p;
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkDestroyQueryPool(VkDevice dev, VkQueryPool p, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    if (!p) return;
    p->results = nil;
    free(p->available);
    free(p);
}
VKFN void VKAPI_CALL vkResetQueryPool(VkDevice dev, VkQueryPool p, uint32_t first, uint32_t count) {
    (void)dev;
    for (uint32_t i = 0; i < count && first + i < p->count; i++) {
        p->available[first + i] = 0;
        ((uint64_t *)p->results.contents)[first + i] = 0;
    }
}
VKFN VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice dev, VkQueryPool p, uint32_t first, uint32_t count, size_t size, void *data,
                                              VkDeviceSize stride, VkQueryResultFlags flags) {
    (void)size;
    int pending = 0;
    for (uint32_t i = 0; i < count; i++) if (first + i < p->count && p->available[first + i] != 1) pending = 1;
    if (pending && (flags & VK_QUERY_RESULT_WAIT_BIT)) {
        VK_STAT(VKS_QUERY_IDLE, 1);
        vkDeviceWaitIdle(dev);
        vk_mark_queries_available(p);
    }
    VkResult r = VK_SUCCESS;
    const uint64_t *vals = p->results.contents;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t q = first + i;
        int avail = q < p->count && p->available[q] == 1;
        uint8_t *out = (uint8_t *)data + i * stride;
        if (!avail) r = VK_NOT_READY;
        if (avail || (flags & VK_QUERY_RESULT_PARTIAL_BIT)) {
            uint64_t v = q < p->count ? vals[q] : 0;
            if (flags & VK_QUERY_RESULT_64_BIT) *(uint64_t *)out = v; else *(uint32_t *)out = (uint32_t)v;
        }
        if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
            if (flags & VK_QUERY_RESULT_64_BIT) ((uint64_t *)out)[1] = avail; else ((uint32_t *)out)[1] = avail;
        }
    }
    return r;
}

#ifdef __aarch64__
#include "vx.h"   // (vx_low, vx_unlow_at for the thunks)
#include "thunks/vk_queue.m.inc"
#endif
