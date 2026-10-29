// Win32 surfaces and swapchains: a window's CAMetalLayer. Swapchain images are textures of our own that stay valid as
// Vulkan requires; presenting copies one into the layer's next drawable on the presenting queue.
#include "vk_impl.h"
#include "vx.h"
#include "ui.h"
#import <QuartzCore/QuartzCore.h>
#import <ImageIO/ImageIO.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

MTLTextureUsage vk_texture_usage(const vk_format_info *f, VkImageUsageFlags usage, VkImageCreateFlags flags);
VkResult vk_present_blit(VkQueue q, id<MTLCommandBuffer> cb, VkImage img, id<MTLTexture> target);
void vk_gpu_profile_frame(void);   // (vk_command.m)
#define TRACE(...) do { if (vk_trace()) vx_log("vulkan: " __VA_ARGS__); } while (0)

static vx_window *top_window(void *hwnd) {
    vx_window *w = vx_window_get(hwnd);
    while (w && !w->native && w->parent) w = vx_window_get(w->parent);
    return w && w->native ? w : NULL;
}

VKFN VkResult VKAPI_CALL vkCreateWin32SurfaceKHR(VkInstance inst, const VkWin32SurfaceCreateInfoKHR *info, const VkAllocationCallbacks *alloc,
                                                 VkSurfaceKHR *out) {
    (void)alloc;
    vx_window *w = top_window(info->hwnd);
    if (!w) { TRACE("surface for window %p: not a window with a view", info->hwnd); return VK_ERROR_INITIALIZATION_FAILED; }
    vk_surface *s = calloc(1, sizeof *s);
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->instance = inst;
    s->hwnd = info->hwnd;
    s->layer = (__bridge CAMetalLayer *)vx_cocoa_metal_layer(w, (__bridge void *)inst->mtl, (int)MTLPixelFormatBGRA8Unorm);
    *out = (VkSurfaceKHR)s;
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkDestroySurfaceKHR(VkInstance inst, VkSurfaceKHR surface, const VkAllocationCallbacks *alloc) {
    (void)inst; (void)alloc;
    vk_surface *s = (vk_surface *)surface;
    if (!s) return;
    s->layer = nil;
    free(s);
}
VKFN VkBool32 VKAPI_CALL vkGetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice phys, uint32_t family) { (void)phys; (void)family; return VK_TRUE; }
VKFN VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice phys, uint32_t family, VkSurfaceKHR s, VkBool32 *ok) {
    (void)phys; (void)family; (void)s;
    *ok = VK_TRUE;
    return VK_SUCCESS;
}

static VkExtent2D window_extent(vk_surface *s) {
    vx_window *w = vx_window_get(s->hwnd);
    if (!w) return (VkExtent2D){ 0, 0 };
    return (VkExtent2D){ (uint32_t)(w->client_w > 0 ? w->client_w : 0), (uint32_t)(w->client_h > 0 ? w->client_h : 0) };
}

VKFN VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice phys, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *c) {
    (void)phys;
    vk_surface *s = (vk_surface *)surface;
    VkExtent2D e = window_extent(s);
    memset(c, 0, sizeof *c);
    c->minImageCount = 2;
    c->maxImageCount = 8;
    c->currentExtent = e;
    c->minImageExtent = (VkExtent2D){ 1, 1 };
    c->maxImageExtent = (VkExtent2D){ 16384, 16384 };
    c->maxImageArrayLayers = 1;
    c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    return VK_SUCCESS;
}
VKFN VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice phys, const VkPhysicalDeviceSurfaceInfo2KHR *info,
                                                                    VkSurfaceCapabilities2KHR *c) {
    return vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, info->surface, &c->surfaceCapabilities);
}

static const VkSurfaceFormatKHR g_formats[] = {
    { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    { VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
};
VKFN VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice phys, VkSurfaceKHR s, uint32_t *count, VkSurfaceFormatKHR *f) {
    (void)phys; (void)s;
    uint32_t n = sizeof g_formats / sizeof g_formats[0];
    if (!f) { *count = n; return VK_SUCCESS; }
    uint32_t k = *count < n ? *count : n;
    memcpy(f, g_formats, k * sizeof *f);
    *count = k;
    return k < n ? VK_INCOMPLETE : VK_SUCCESS;
}
VKFN VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice phys, const VkPhysicalDeviceSurfaceInfo2KHR *info, uint32_t *count,
                                                               VkSurfaceFormat2KHR *f) {
    (void)phys; (void)info;
    uint32_t n = sizeof g_formats / sizeof g_formats[0];
    if (!f) { *count = n; return VK_SUCCESS; }
    uint32_t k = *count < n ? *count : n;
    for (uint32_t i = 0; i < k; i++) f[i].surfaceFormat = g_formats[i];
    *count = k;
    return k < n ? VK_INCOMPLETE : VK_SUCCESS;
}
VKFN VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice phys, VkSurfaceKHR s, uint32_t *count, VkPresentModeKHR *m) {
    (void)phys; (void)s;
    static const VkPresentModeKHR modes[] = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR };
    if (!m) { *count = 3; return VK_SUCCESS; }
    uint32_t k = *count < 3 ? *count : 3;
    memcpy(m, modes, k * sizeof *m);
    *count = k;
    return k < 3 ? VK_INCOMPLETE : VK_SUCCESS;
}

// ---- swapchains --------------------------------------------------------------------------------------------------
VKFN VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *alloc,
                                              VkSwapchainKHR *out) {
    (void)alloc;
    vk_surface *surf = (vk_surface *)info->surface;
    const vk_format_info *f = vk_format(info->imageFormat);
    if (!f || f->mtl == MTLPixelFormatInvalid) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (info->oldSwapchain) ((vk_swapchain *)info->oldSwapchain)->retired = 1;
    vk_swapchain *sc = calloc(1, sizeof *sc);
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
    sc->device = dev;
    sc->surface = surf;
    sc->info = *info;
    sc->info.pNext = NULL;
    sc->count = info->minImageCount < 2 ? 2 : info->minImageCount > 8 ? 8 : info->minImageCount;
    sc->images = calloc(sc->count, sizeof *sc->images);
    sc->handles = calloc(sc->count, sizeof *sc->handles);
    sc->presented = calloc(sc->count, sizeof *sc->presented);
    sc->present_queue = calloc(sc->count, sizeof *sc->present_queue);
    // The layer presents in the swapchain's format when Metal's layers have it, else BGRA8 (and a converting draw).
    MTLPixelFormat lf = f->mtl == MTLPixelFormatBGRA8Unorm || f->mtl == MTLPixelFormatBGRA8Unorm_sRGB || f->mtl == MTLPixelFormatRGBA16Float ||
                        f->mtl == MTLPixelFormatRGB10A2Unorm || f->mtl == MTLPixelFormatBGR10A2Unorm ? f->mtl : MTLPixelFormatBGRA8Unorm;
    CAMetalLayer *layer = surf->layer;
    uint32_t w = info->imageExtent.width, h = info->imageExtent.height;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.pixelFormat = lf;
    layer.drawableSize = CGSizeMake(w, h);
    layer.maximumDrawableCount = sc->count >= 3 ? 3 : 2;
#if !VX_XROS   // (visionOS: the system paces CAMetalLayer)
    layer.displaySyncEnabled = info->presentMode == VK_PRESENT_MODE_FIFO_KHR || info->presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
#endif
    layer.framebufferOnly = NO;
    [CATransaction commit];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f->mtl width:w height:h mipmapped:NO];
    d.usage = vk_texture_usage(f, info->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0) | MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
    d.hazardTrackingMode = MTLHazardTrackingModeUntracked;
    MTLSizeAndAlign sa = [dev->mtl heapTextureSizeAndAlignWithDescriptor:d];
    uint64_t stride = (sa.size + sa.align - 1) / sa.align * sa.align;
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.type = MTLHeapTypePlacement;
    hd.storageMode = MTLStorageModePrivate;
    hd.hazardTrackingMode = MTLHazardTrackingModeUntracked;
    hd.size = stride * sc->count;
    id<MTLHeap> heap = [dev->mtl newHeapWithDescriptor:hd];
    if (!heap) { free(sc->images); free(sc->handles); free(sc->presented); free(sc->present_queue); free(sc); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    for (uint32_t i = 0; i < sc->count; i++) {
        struct VkImage_T *img = &sc->images[i];
        img->type = VK_IMAGE_TYPE_2D;
        img->format = info->imageFormat;
        img->fmt = f;
        img->extent = (VkExtent3D){ w, h, 1 };
        img->mips = 1; img->layers = 1; img->samples = 1;
        img->usage = info->imageUsage;
        img->tiling = VK_IMAGE_TILING_OPTIMAL;
        img->desc = d;
        img->mtl = [heap newTextureWithDescriptor:d offset:stride * i];
        img->swapchain = sc;
        img->swapchain_index = i;
        sc->handles[i] = img;
    }
    vk_device_add_heap(dev, heap);
    sc->heap = heap;
    TRACE("swapchain %ux%u, %u images, format %d, present mode %d", w, h, sc->count, info->imageFormat, info->presentMode);
    *out = (VkSwapchainKHR)sc;
    return VK_SUCCESS;
}

VKFN void VKAPI_CALL vkDestroySwapchainKHR(VkDevice dev, VkSwapchainKHR swapchain, const VkAllocationCallbacks *alloc) {
    (void)alloc;
    vk_swapchain *sc = (vk_swapchain *)swapchain;
    if (!sc) return;
    vkDeviceWaitIdle(dev);
    if (sc->heap) vk_device_remove_heap(dev, sc->heap);
    for (uint32_t i = 0; i < sc->count; i++) { sc->images[i].mtl = nil; sc->images[i].desc = nil; }
    sc->heap = nil;
    free(sc->images); free(sc->handles); free(sc->presented); free(sc->present_queue);
    free(sc);
}

VKFN VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice dev, VkSwapchainKHR swapchain, uint32_t *count, VkImage *images) {
    (void)dev;
    vk_swapchain *sc = (vk_swapchain *)swapchain;
    if (!images) { *count = sc->count; return VK_SUCCESS; }
    uint32_t k = *count < sc->count ? *count : sc->count;
    memcpy(images, sc->handles, k * sizeof *images);
    *count = k;
    return k < sc->count ? VK_INCOMPLETE : VK_SUCCESS;
}

VKFN VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice dev, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore sem, VkFence fence,
                                               uint32_t *index) {
    vk_swapchain *sc = (vk_swapchain *)swapchain;
    if (sc->retired) return VK_ERROR_OUT_OF_DATE_KHR;
    uint32_t i = sc->next;
    // The image is free once its last present's copy has run.
    VkQueue q = sc->present_queue[i];
    VK_STAT(VKS_ACQUIRES, 1);
    if (q && sc->presented[i] && q->done.signaledValue < sc->presented[i]) {
        VK_STAT_T0(t_wait);
        uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        while (q->done.signaledValue < sc->presented[i]) {
            if (timeout != UINT64_MAX && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start > timeout) return timeout ? VK_TIMEOUT : VK_NOT_READY;
            usleep(200);
        }
        VK_STAT_NS(VKS_ACQUIRE_NS, t_wait);
    }
    sc->next = (i + 1) % sc->count;
    *index = i;
    if (sem) { uint64_t v = atomic_fetch_add(&sem->value, 1) + 1; sem->event.signaledValue = v; }
    if (fence) vk_queue_submit_internal(&dev->queues[0], 0, NULL, fence);
    VkExtent2D e = window_extent(sc->surface);
    if (e.width && e.height && (e.width != sc->info.imageExtent.width || e.height != sc->info.imageExtent.height)) return VK_SUBOPTIMAL_KHR;
    return VK_SUCCESS;
}
VKFN VkResult VKAPI_CALL vkAcquireNextImage2KHR(VkDevice dev, const VkAcquireNextImageInfoKHR *info, uint32_t *index) {
    return vkAcquireNextImageKHR(dev, info->swapchain, info->timeout, info->semaphore, info->fence, index);
}

// VXRT_PRESENT_SNAPSHOT=file.png (with VXRT_PRESENT_SNAPSHOT_AT=n, default 300; VXRT_PRESENT_SNAPSHOT_EVERY=k: also
// every k-th present after it, as file-<n>.png): a presented image as a PNG, to see what a program draws.
static void snapshot(VkQueue q, VkImage img, uint64_t present) {
    const char *path = getenv("VXRT_PRESENT_SNAPSHOT");
    if (!path) return;
    uint64_t at = getenv("VXRT_PRESENT_SNAPSHOT_AT") ? strtoull(getenv("VXRT_PRESENT_SNAPSHOT_AT"), NULL, 10) : 300;
    uint64_t every = getenv("VXRT_PRESENT_SNAPSHOT_EVERY") ? strtoull(getenv("VXRT_PRESENT_SNAPSHOT_EVERY"), NULL, 10) : 0;
    if (present != at && !(every && present > at && (present - at) % every == 0)) return;
    id<MTLTexture> tex = img->mtl;
    MTLPixelFormat f = tex.pixelFormat;
    int bgra = f == MTLPixelFormatBGRA8Unorm || f == MTLPixelFormatBGRA8Unorm_sRGB;
    if (!bgra && f != MTLPixelFormatRGBA8Unorm && f != MTLPixelFormatRGBA8Unorm_sRGB) { vx_log("vulkan: snapshot of format %d not written", (int)f); return; }
    NSUInteger w = tex.width, h = tex.height, row = w * 4;
    id<MTLBuffer> buf = [q->device->mtl newBufferWithLength:row * h options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q->mtl commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b waitForFence:q->last_fence];
    [b copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
              toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
    [b updateFence:q->last_fence];
    [b endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    char name[1024];
    if (present == at) snprintf(name, sizeof name, "%s", path);
    else {
        const char *dot = strrchr(path, '.');
        int stem = dot ? (int)(dot - path) : (int)strlen(path);
        snprintf(name, sizeof name, "%.*s-%llu%s", stem, path, (unsigned long long)present, dot ? dot : "");
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGBitmapInfo bi = bgra ? (kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst) : (kCGBitmapByteOrder32Big | kCGImageAlphaNoneSkipLast);
    CGContextRef ctx = CGBitmapContextCreate(buf.contents, w, h, 8, row, cs, bi);
    CGImageRef im = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:name]];
    CGImageDestinationRef dst = im ? CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, NULL) : NULL;
    if (dst) { CGImageDestinationAddImage(dst, im, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); }
    if (im) CGImageRelease(im);
    if (ctx) CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    vx_log("vulkan: present %llu (%lux%lu) written to %s", (unsigned long long)present, (unsigned long)w, (unsigned long)h, name);
}

// The present rate in the log every 5 seconds with VXRT_TRACE_VULKAN (or VXRT_STATS).
static void count_present(void) {
    static uint64_t last, count;
    static int on = -1;
    if (on < 0) on = vk_trace() || (getenv("VXRT_STATS") && atoi(getenv("VXRT_STATS")));
    if (!on) return;
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    count++;
    if (!last) last = now;
    if (now - last >= 5000000000ull) {
        vx_log("vulkan: %.1f presents/s", count * 1e9 / (double)(now - last));
        last = now;
        count = 0;
    }
}

VKFN VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *info) {
    VkResult result = VK_SUCCESS;
    VK_STAT_T0(t_present);
    pthread_mutex_lock(&q->lock);
    for (uint32_t k = 0; k < info->swapchainCount; k++) @autoreleasepool {
        vk_swapchain *sc = (vk_swapchain *)info->pSwapchains[k];
        uint32_t i = info->pImageIndices[k];
        VkImage img = &sc->images[i];
        id<MTLCommandBuffer> cb = [q->mtl commandBuffer];
        VK_STAT(VKS_MTL_CBS, 1);
        vk_stats_gpu(cb);
        for (uint32_t w = 0; w < info->waitSemaphoreCount; w++) {
            VkSemaphore s = info->pWaitSemaphores[w];
            uint64_t v = atomic_load(&s->value);
            if (v) [cb encodeWaitForEvent:s->event value:v];
        }
        VK_STAT_T0(t_drawable);
        id<CAMetalDrawable> drawable = [sc->surface->layer nextDrawable];
        VK_STAT_NS(VKS_DRAWABLE_NS, t_drawable);
        if (drawable) {
            if (drawable.texture.pixelFormat == img->mtl.pixelFormat && drawable.texture.width == img->mtl.width &&
                drawable.texture.height == img->mtl.height) {
                if (!q->last_fence) q->last_fence = [q->device->mtl newFence];
                id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
                [b waitForFence:q->last_fence];
                [b copyFromTexture:img->mtl toTexture:drawable.texture];
                [b updateFence:q->last_fence];
                [b endEncoding];
            } else vk_present_blit(q, cb, img, drawable.texture);
            [cb presentDrawable:drawable];
        }
        uint64_t number = ++q->submitted;
        [cb encodeSignalEvent:q->done value:number];
        [cb commit];
        sc->presented[i] = number;
        sc->present_queue[i] = q;
        static _Atomic uint64_t presents;
        snapshot(q, img, atomic_fetch_add(&presents, 1) + 1);
        count_present();
        VkResult r = sc->retired ? VK_ERROR_OUT_OF_DATE_KHR : VK_SUCCESS;
        if (info->pResults) info->pResults[k] = r;
        if (r) result = r;
    }
    pthread_mutex_unlock(&q->lock);
    VK_STAT(VKS_PRESENTS, 1);
    VK_STAT_NS(VKS_PRESENT_NS, t_present);
    vk_stats_frame();
    vk_gpu_profile_frame();
    return result;
}

VKFN void VKAPI_CALL vkSetHdrMetadataEXT(VkDevice dev, uint32_t n, const VkSwapchainKHR *sc, const VkHdrMetadataEXT *m) {
    (void)dev; (void)n; (void)sc; (void)m;
}

#ifdef __aarch64__
#include "thunks/vk_wsi.m.inc"
#endif
