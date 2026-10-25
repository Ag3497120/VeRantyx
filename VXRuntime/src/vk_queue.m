// Queue submission and synchronization: each submission is one Metal command buffer; semaphores (binary and
// timeline) are Metal shared events, waited and signaled in the command buffer; fences are signaled by its completion.
#include "vk_impl.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

void vk_encode(VkQueue q, id<MTLCommandBuffer> mcb, const VkCommandBuffer *cbs, uint32_t n);
void vk_mark_queries_available(VkQueryPool pool);
#define TRACE(...) do { if (vk_trace()) vx_log("vulkan: " __VA_ARGS__); } while (0)

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
VKFN VkResult VKAPI_CALL vkWaitForFences(VkDevice dev, uint32_t n, const VkFence *fs, VkBool32 all, uint64_t timeout) {
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
VKFN VkResult VKAPI_CALL vkWaitSemaphores(VkDevice dev, const VkSemaphoreWaitInfo *info, uint64_t timeout) {
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
    pthread_mutex_lock(&q->lock);
    if (fence) {
        pthread_mutex_lock(&fence->lock);
        fence->pending++;
        pthread_mutex_unlock(&fence->lock);
    }
    for (uint32_t i = 0; i < n || (i == 0 && n == 0); i++) @autoreleasepool {
        id<MTLCommandBuffer> mcb = [q->mtl commandBuffer];
        const VkWin32KeyedMutexAcquireReleaseInfoKHR *km = NULL;
        if (n) {
            const VkSubmitInfo2 *s = &submits[i];
            // VK_KHR_win32_keyed_mutex: the shared images' mutexes are taken before the work (the CPU waits here, where
            // Windows lets the GPU wait) and given back after it (d3d11_shared.m).
            km = vk_find_struct(s->pNext, VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR);
            for (uint32_t k = 0; km && k < km->acquireCount; k++) {
                VkDeviceMemory m = km->pAcquireSyncs[k];
                if (m && m->keyed_slot && !vx_shared_keyed_acquire(m->keyed_slot - 1, km->pAcquireKeys[k], km->pAcquireTimeouts ? km->pAcquireTimeouts[k] : 0xffffffffu))
                    TRACE("keyed mutex of slot %d not acquired with key %llu", m->keyed_slot - 1, (unsigned long long)km->pAcquireKeys[k]);
            }
            for (uint32_t k = 0; k < s->waitSemaphoreInfoCount; k++) {
                VkSemaphore sem = s->pWaitSemaphoreInfos[k].semaphore;
                uint64_t v = sem->timeline ? s->pWaitSemaphoreInfos[k].value : atomic_load(&sem->value);
                if (v) [mcb encodeWaitForEvent:sem->event value:v];
            }
            VkCommandBuffer cbs[256];
            uint32_t nc = s->commandBufferInfoCount < 256 ? s->commandBufferInfoCount : 256;
            for (uint32_t k = 0; k < nc; k++) cbs[k] = s->pCommandBufferInfos[k].commandBuffer;
            vk_encode(q, mcb, cbs, nc);
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
    event_wait(q->done, target, UINT64_MAX);
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
