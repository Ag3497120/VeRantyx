// Direct3D 11 resources shared between processes (D3D11_RESOURCE_MISC_SHARED, _SHARED_KEYEDMUTEX, _SHARED_NTHANDLE).
//
// Every shared resource has a slot in a table in shared memory that all vxrt processes of one C: root see; its
// shared handle is 0x40000000 + 4 * slot, the same value in every process (as Direct3D's legacy shared handles
// are). A texture lives in a global IOSurface (its ID is in the slot), so OpenSharedResource in another process
// makes a Metal texture of the same surface; a buffer lives in POSIX shared memory. A keyed mutex is a key and an
// owner in the slot; ReleaseSync waits for the GPU work so far, so the other process reads finished pixels.
#include <os/os_sync_wait_on_address.h>
#import <IOSurface/IOSurface.h>
#include "d3d_common.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "d3d_tables.h"
#pragma clang diagnostic pop

#define SLOTS 4096
typedef struct {
    volatile int32_t used;           // 0 free, 1 being filled, 2 ready
    int32_t kind;                    // 1 texture, 2 buffer
    int32_t creator;                 // the process that made it (a slot of a process that ended is free again)
    uint32_t surface;                // IOSurface ID
    char shm[32];                    // shared memory name (buffers)
    uint32_t desc[TD_COUNT];         // the texture's description (the 2D layout), or the buffer's
    uint64_t size;
    volatile int64_t key;            // keyed mutex: the key it was released with
    volatile int32_t owner;          // and who holds it (0: nobody)
    volatile uint32_t released;      // counts releases: waiters in other processes sleep on it
    volatile int32_t gpu_pending;    // releases whose GPU work has not finished yet (by gpu_pid; others wait for 0)
    volatile int32_t gpu_pid;
} shared_slot;
typedef struct { volatile uint32_t spin; shared_slot s[SLOTS]; } shared_table;
static shared_table *g_table;

static shared_table *table(void) {
    if (g_table) return g_table;
    uint64_t h = 1469598103934665603ull;
    for (const char *c = vx_root(); *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
    char name[32];
    snprintf(name, sizeof name, "/vxsh2.%08llx", (unsigned long long)(h & 0xffffffffull));   // (2: the slot layout)
    int fd = shm_open(name, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return NULL;
    struct stat st;
    if (!fstat(fd, &st) && st.st_size < (off_t)sizeof(shared_table)) ftruncate(fd, sizeof(shared_table));
    shared_table *t = mmap(NULL, sizeof(shared_table), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (t == MAP_FAILED) return NULL;
    g_table = t;
    return t;
}
static int take_slot(void) {
    shared_table *t = table();
    if (!t) return -1;
    while (__atomic_exchange_n(&t->spin, 1, __ATOMIC_ACQUIRE)) sched_yield();
    int found = -1;
    for (int i = 0; i < SLOTS && found < 0; i++) {
        if (t->s[i].used && t->s[i].creator && kill(t->s[i].creator, 0) && errno == ESRCH) t->s[i].used = 0;   // its process ended
        if (!t->s[i].used) found = i;
    }
    if (found >= 0) { memset(&t->s[found], 0, sizeof t->s[found]); t->s[found].used = 1; t->s[found].creator = getpid(); }
    __atomic_store_n(&t->spin, 0, __ATOMIC_RELEASE);
    return found;
}
void d3d_shared_release(int slot) {
    shared_table *t = table();
    if (t && slot >= 0 && slot < SLOTS && t->s[slot].creator == getpid()) __atomic_store_n(&t->s[slot].used, 0, __ATOMIC_RELEASE);
}
static void number(CFMutableDictionaryRef d, CFStringRef key, long v) {
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberLongType, &v);
    CFDictionarySetValue(d, key, n);
    CFRelease(n);
}

// A texture for sharing: one mip, one slice, one sample, on a global IOSurface.
void *d3d_shared_texture(d3d_device *dev, MTLTextureDescriptor *td, const uint32_t desc[TD_COUNT], uint32_t bpp, int *slot) {
    *slot = -1;
    if (!bpp || td.textureType != MTLTextureType2D || td.sampleCount > 1) { vx_log("Direct3D: this shared texture (format %u) cannot be shared yet", desc[TD_FORMAT]); return NULL; }
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    number(d, kIOSurfaceWidth, (long)td.width);
    number(d, kIOSurfaceHeight, (long)td.height);
    number(d, kIOSurfaceBytesPerElement, bpp);
    number(d, kIOSurfaceBytesPerRow, (long)IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, td.width * bpp));
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CFDictionarySetValue(d, kIOSurfaceIsGlobal, kCFBooleanTrue);  // so another process can look it up by its ID
#pragma clang diagnostic pop
    IOSurfaceRef surface = IOSurfaceCreate(d);
    CFRelease(d);
    if (!surface) return NULL;
    td.mipmapLevelCount = 1;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [(__bridge id<MTLDevice>)dev->mtl newTextureWithDescriptor:td iosurface:surface plane:0];
    CFRelease(surface);                                          // the texture keeps it
    if (!tex) return NULL;
    int s = take_slot();
    if (s >= 0) {
        shared_slot *e = &table()->s[s];
        e->kind = 1; e->surface = IOSurfaceGetID(tex.iosurface);
        memcpy(e->desc, desc, sizeof e->desc);
        e->desc[TD_MIPS] = 1;
        __atomic_store_n(&e->used, 2, __ATOMIC_RELEASE);
    }
    *slot = s;
    if (vx_trace) vx_log("Direct3D: shared texture %ux%u format %u: IOSurface %u, handle %#x", (unsigned)td.width, (unsigned)td.height, desc[TD_FORMAT], (unsigned)IOSurfaceGetID(tex.iosurface), D3D_SHARED_HANDLE_BASE + 4 * s);
    return (void *)CFBridgingRetain(tex);
}
// A buffer for sharing: shared memory (whole pages) that a Metal buffer uses without copying.
static void *map_shared_buffer(d3d_device *dev, const char *name, size_t size, int create, void **shm) {
    int fd = shm_open(name, O_RDWR | (create ? O_CREAT : 0), 0600);
    if (fd < 0) return NULL;
    if (create) ftruncate(fd, (off_t)size);
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    id<MTLBuffer> b = [(__bridge id<MTLDevice>)dev->mtl newBufferWithBytesNoCopy:p length:size options:MTLResourceStorageModeShared deallocator:nil];
    if (!b) { munmap(p, size); return NULL; }
    *shm = p;
    return (void *)CFBridgingRetain(b);
}
void *d3d_shared_buffer(d3d_device *dev, const uint32_t desc[6], void **shm, size_t *shm_size, int *slot) {
    *slot = take_slot();
    if (*slot < 0) return NULL;
    size_t page = (size_t)getpagesize(), size = (desc[0] + page - 1) / page * page;
    char name[32];
    snprintf(name, sizeof name, "/vxb.%d.%d", (int)getpid(), *slot);
    shm_unlink(name);
    void *b = map_shared_buffer(dev, name, size, 1, shm);
    if (!b) { d3d_shared_release(*slot); *slot = -1; return NULL; }
    shared_slot *e = &table()->s[*slot];
    e->kind = 2; e->size = size;
    snprintf(e->shm, sizeof e->shm, "%s", name);
    memcpy(e->desc, desc, sizeof(uint32_t) * 6);
    __atomic_store_n(&e->used, 2, __ATOMIC_RELEASE);
    *shm_size = size;
    if (vx_trace) vx_log("Direct3D: shared buffer of %u bytes: handle %#x", desc[0], D3D_SHARED_HANDLE_BASE + 4 * *slot);
    return b;
}

// OpenSharedResource(1): the resource behind a shared handle, as this device's object.
HRESULT d3d_open_shared(d3d_device *dev, HANDLE h, const GUID *iid, void **out) {
    uintptr_t v = (uintptr_t)h;
    *out = NULL;
    shared_table *t = table();
    if (!t || v < D3D_SHARED_HANDLE_BASE || (v - D3D_SHARED_HANDLE_BASE) % 4 || (v - D3D_SHARED_HANDLE_BASE) / 4 >= SLOTS) return E_INVALIDARG;
    int s = (int)((v - D3D_SHARED_HANDLE_BASE) / 4);
    shared_slot *e = &t->s[s];
    if (__atomic_load_n(&e->used, __ATOMIC_ACQUIRE) != 2) { vx_log("Direct3D: shared handle %p is not open", h); return E_INVALIDARG; }
    d3d_object *o = NULL;
    if (e->kind == 1) {
        IOSurfaceRef surface = IOSurfaceLookup(e->surface);
        if (!surface) { vx_log("Direct3D: the IOSurface of shared handle %p is gone", h); return E_INVALIDARG; }
        MTLPixelFormat f = d3d_mtl_format(e->desc[TD_FORMAT], NULL);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:e->desc[TD_WIDTH] height:e->desc[TD_HEIGHT] mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [(__bridge id<MTLDevice>)dev->mtl newTextureWithDescriptor:td iosurface:surface plane:0];
        CFRelease(surface);
        if (!tex) return E_INVALIDARG;
        o = &d3d_texture_wrap(dev, e->desc, (void *)CFBridgingRetain(tex), s + 1)->base;
        d3d_shared_note((d3d_texture *)o);
        if (getenv("VXRT_TRACE_KEYED")) vx_log("opened shared texture slot %d: %ux%u DXGI format %u, IOSurface %u", s, e->desc[TD_WIDTH], e->desc[TD_HEIGHT], e->desc[TD_FORMAT], e->surface);
    } else if (e->kind == 2) {
        void *shm = NULL;
        void *b = map_shared_buffer(dev, e->shm, (size_t)e->size, 0, &shm);
        if (!b) return E_INVALIDARG;
        o = &d3d_buffer_wrap(dev, e->desc, b, s + 1, shm, (size_t)e->size)->base;
    }
    if (!o) return E_INVALIDARG;
    HRESULT hr = ((HRESULT (WINAPI *)(void *, const GUID *, void **))o->vtbl[0])(o, iid, out);
    d3d_release(o);
    return hr;
}

// ---- IDXGIResource1 (the resources' second table) ----------------------------------------------------------------------------
static d3d_object *res_of(void *this) { return FROM_VTBL2(this); }
static int slot_of(d3d_object *o) { return o->kind == K_TEXTURE ? ((d3d_texture *)o)->shared : o->kind == K_BUFFER ? ((d3d_buffer *)o)->shared : 0; }
static WINAPI HRESULT res_qi(void *this, const GUID *iid, void **out) { d3d_object *o = res_of(this); return ((HRESULT (WINAPI *)(void *, const GUID *, void **))o->vtbl[0])(o, iid, out); }
static WINAPI uint32_t res_addref(void *this) { return d3d_addref(res_of(this)); }
static WINAPI uint32_t res_release(void *this) { return d3d_release(res_of(this)); }
static WINAPI HRESULT res_get_shared_handle(void *this, HANDLE *h) {
    int s = slot_of(res_of(this));
    *h = s ? (HANDLE)(uintptr_t)(D3D_SHARED_HANDLE_BASE + 4 * (s - 1)) : NULL;
    return s ? S_OK : E_INVALIDARG;
}
static WINAPI HRESULT res_create_shared_handle(void *this, const void *sa, DWORD access, const WCHAR *name, HANDLE *h) {
    (void)sa; (void)access; (void)name;
    return res_get_shared_handle(this, h);                       // the same value: it means the resource in every process
}
static WINAPI HRESULT res_get_usage(void *this, uint32_t *usage) { (void)this; *usage = 0x20 | 0x10; return S_OK; }   // RENDER_TARGET_OUTPUT | SHADER_INPUT
static WINAPI HRESULT res_set_eviction(void *this, uint32_t p) { (void)this; (void)p; return S_OK; }
static WINAPI HRESULT res_get_eviction(void *this, uint32_t *p) { (void)this; *p = 0; return S_OK; }
static WINAPI HRESULT res_get_device(void *this, const GUID *iid, void **out) {
    d3d_device *d = res_of(this)->dev;
    return ((HRESULT (WINAPI *)(void *, const GUID *, void **))d->base.vtbl[0])(d, iid, out);
}
static WINAPI HRESULT res_get_parent(void *this, const GUID *iid, void **out) { return res_get_device(this, iid, out); }
static WINAPI HRESULT res_set_private(void *this, const GUID *g, uint32_t n, const void *data) { return d3d_set_private(res_of(this), g, n, data); }
static WINAPI HRESULT res_get_private(void *this, const GUID *g, uint32_t *n, void *data) { return d3d_get_private(res_of(this), g, n, data); }
static WINAPI HRESULT res_set_private_interface(void *this, const GUID *g, void *unk) { (void)unk; return d3d_set_private(res_of(this), g, 0, NULL); }

// With VXRT_SHARED_SNAPSHOT set, every shared texture this process makes or opens is remembered (its Metal texture
// retained), and the snapshot writes them all: file.png.<pid>.<slot>.png.
static void *g_noted[64]; static int g_noted_slot[64], g_noted_count;
void d3d_shared_note(d3d_texture *t) {
    if (!getenv("VXRT_SHARED_SNAPSHOT") || !t->shared || g_noted_count >= 64) return;
    int i = __sync_fetch_and_add(&g_noted_count, 1);
    if (i >= 64) return;
    g_noted[i] = (void *)CFBridgingRetain((__bridge id)t->mtl); g_noted_slot[i] = t->shared - 1;
}

// VXRT_SHARED_COPY_SNAPSHOT=file.png (VXRT_SHARED_COPY_SNAPSHOT_AT=n, default 600): the n-th time this process
// copies or resolves into a shared texture, that texture after the copy (debugging what crosses processes).
void d3d_shared_written(d3d_device *dev, d3d_texture *t) {
    static int count;
    const char *snap = getenv("VXRT_SHARED_COPY_SNAPSHOT"), *at = getenv("VXRT_SHARED_COPY_SNAPSHOT_AT");
    if (!snap || !t->shared || __sync_add_and_fetch(&count, 1) != (at ? atoi(at) : 600)) return;
    d3d_flush(dev, 1);
    vx_log("shared copy snapshot: slot %d, %ux%u, DXGI format %u", t->shared - 1, t->desc[TD_WIDTH], t->desc[TD_HEIGHT], t->desc[TD_FORMAT]);
    d3d_texture_png(dev, t->mtl, snap);
}

// ---- IDXGIKeyedMutex (shared textures' third table) ----------------------------------------------------------------------------
static d3d_texture *keyed_of(void *this) { return (d3d_texture *)((char *)this - offsetof(d3d_texture, keyed_vtbl)); }
static WINAPI HRESULT keyed_qi(void *this, const GUID *iid, void **out) { d3d_texture *t = keyed_of(this); return ((HRESULT (WINAPI *)(void *, const GUID *, void **))t->base.vtbl[0])(t, iid, out); }
static WINAPI uint32_t keyed_addref(void *this) { return d3d_addref(&keyed_of(this)->base); }
static WINAPI uint32_t keyed_release(void *this) { return d3d_release(&keyed_of(this)->base); }
// VXRT_STATS=1: every 600 AcquireSyncs, how long they waited and for what.
__thread uint32_t vx_held_ops[7];    // (d3d_common.h)
static __thread uint64_t g_held_since;
static int g_keyed_stats = -1;
static uint64_t g_acquired[SLOTS];                   // when this process last took each slot's mutex
static void hold_stats(int slot) {                   // and how long it kept it, every 600 releases
    static uint64_t n, ns, slot_n[SLOTS], slot_ns[SLOTS];
    if (!g_acquired[slot]) return;
    uint64_t d = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - g_acquired[slot];
    ns += d; slot_n[slot]++; slot_ns[slot] += d;
    g_acquired[slot] = 0;
    if (++n % 600) return;
    char per[256] = ""; size_t len = 0;
    for (int i = 0; i < SLOTS && len < sizeof per - 32; i++)
        if (slot_n[i]) len += (size_t)snprintf(per + len, sizeof per - len, " slot %d %.2f ms (%llu)", i, slot_ns[i] / 1e6 / slot_n[i], (unsigned long long)slot_n[i]);
    vx_log("keyed mutex: held %.2f ms on average over %llu releases;%s", ns / 1e6 / n, (unsigned long long)n, per);
}
static void keyed_stats(int why, const struct timespec *start) {
    static uint64_t n, ns[4], waits[4];
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t d = (uint64_t)(now.tv_sec - start->tv_sec) * 1000000000ull + (uint64_t)(now.tv_nsec - start->tv_nsec);
    __atomic_add_fetch(&ns[why], d, __ATOMIC_RELAXED);
    __atomic_add_fetch(&waits[why], 1, __ATOMIC_RELAXED);
    if (__atomic_add_fetch(&n, 1, __ATOMIC_RELAXED) % 600) return;
    vx_log("keyed mutex: %llu acquires; waited for the other GPU %llu times (%llu ms), for the key %llu times (%llu ms), for the owner %llu times (%llu ms)",
           (unsigned long long)n, (unsigned long long)waits[1], (unsigned long long)(ns[1] / 1000000), (unsigned long long)waits[2],
           (unsigned long long)(ns[2] / 1000000), (unsigned long long)waits[3], (unsigned long long)(ns[3] / 1000000));
}
static WINAPI HRESULT keyed_acquire(void *this, uint64_t key, DWORD ms) {
    if (g_keyed_stats < 0) g_keyed_stats = getenv("VXRT_STATS") != NULL;
    d3d_texture *t = keyed_of(this);
    if (!t->shared) return S_OK;
    shared_slot *e = &table()->s[t->shared - 1];
    struct timespec start; clock_gettime(CLOCK_MONOTONIC, &start);
    int why = 0;                                       // what the first look found missing: 1 the GPU, 2 the key, 3 the owner
    int owned = 0;
    for (;;) {
        int32_t free_owner = 0;
        // The mutex is taken as soon as it is released with the key, and the GPU is waited for while holding it, as
        // on Windows (where only the GPU waits): waiting first let the releaser take it back meanwhile (a game that
        // acquires again right after releasing left the compositor without its frames).
        if (!owned && (uint64_t)__atomic_load_n(&e->key, __ATOMIC_ACQUIRE) == key && __atomic_compare_exchange_n(&e->owner, &free_owner, (int32_t)getpid(), 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            owned = 1;
        int gpu_ready = !__atomic_load_n(&e->gpu_pending, __ATOMIC_ACQUIRE) || e->gpu_pid == (int32_t)getpid();   // the releaser's own queue keeps the order
        if (owned && gpu_ready) {
            vx_profile_held(1);
            if (g_keyed_stats) { keyed_stats(why, &start); g_acquired[t->shared - 1] = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
            if (getenv("VXRT_TRACE_KEYED")) {
                char from[160] = ""; size_t len = 0;           // the program's return addresses on the stack
                uint64_t *sp = __builtin_frame_address(0);
                for (int i = 0, found = 0; i < 400 && found < 4; i++) {
                    const vx_image *m = vx_image_at(sp[i]);
                    if (m) { len += (size_t)snprintf(from + len, sizeof from - len, " %s+%#llx", m->name, (unsigned long long)(sp[i] - (uint64_t)(uintptr_t)m->base)); found++; }
                }
                vx_log("keyed mutex slot %d: AcquireSync(%llu) by %d, from%s", t->shared - 1, (unsigned long long)key, (int)getpid(), from);
                vx_keyed_held++; g_held_since = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            }
            return S_OK;
        }
        if (!why) why = owned || !gpu_ready ? 1 : (uint64_t)e->key != key ? 2 : 3;
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t elapsed = (uint64_t)(now.tv_sec - start.tv_sec) * 1000 + (uint64_t)((now.tv_nsec - start.tv_nsec) / 1000000);
        if (ms != 0xffffffffu && elapsed >= ms) {
            if (owned) {                                   // given back as it was (released with the key, nobody holding it)
                __atomic_store_n(&e->owner, 0, __ATOMIC_RELEASE);
                __atomic_add_fetch(&e->released, 1, __ATOMIC_RELEASE);
                if (__builtin_available(macOS 14.4, *)) os_sync_wake_by_address_all((void *)&e->released, 4, OS_SYNC_WAKE_BY_ADDRESS_SHARED);
            }
            if (getenv("VXRT_TRACE_KEYED")) vx_log("keyed mutex slot %d: AcquireSync(%llu, %u ms) timed out: released with key %lld, owner %d",
                                                   t->shared - 1, (unsigned long long)key, (unsigned)ms, (long long)e->key, (int)e->owner);
            return (HRESULT)0x102;   // WAIT_TIMEOUT
        }
        // Sleep until a release (any process) or 2 ms, then look again.
        uint32_t seen = __atomic_load_n(&e->released, __ATOMIC_ACQUIRE);
        int gpu_now = !__atomic_load_n(&e->gpu_pending, __ATOMIC_ACQUIRE) || e->gpu_pid == (int32_t)getpid();
        if (owned ? gpu_now : (uint64_t)__atomic_load_n(&e->key, __ATOMIC_ACQUIRE) == key && !__atomic_load_n(&e->owner, __ATOMIC_ACQUIRE)) continue;
        if (__builtin_available(macOS 14.4, *))
            os_sync_wait_on_address_with_timeout((void *)&e->released, seen, 4, OS_SYNC_WAIT_ON_ADDRESS_SHARED, OS_CLOCK_MACH_ABSOLUTE_TIME, 2000000);
        else usleep(200);
    }
}
// The release itself: the key and owner are published and other processes waiting in AcquireSync are woken.
static void publish_release(shared_slot *e, uint64_t key) {
    __atomic_store_n(&e->key, (int64_t)key, __ATOMIC_RELEASE);
    __atomic_store_n(&e->owner, 0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&e->released, 1, __ATOMIC_RELEASE);
    if (__builtin_available(macOS 14.4, *)) os_sync_wake_by_address_all((void *)&e->released, 4, OS_SYNC_WAKE_BY_ADDRESS_SHARED);
}
// ReleaseSync commits the work drawn so far and returns at once, giving up ownership at once as Windows does; the
// slot counts the release as pending on the GPU until a completion handler queued behind the work clears it, and
// only another process waits for that (this process's queue keeps the order). The CPU and GPU so keep working in
// parallel instead of stopping at every frame handed over. VXRT_SYNC_RELEASE=1 waits for the GPU instead.
static WINAPI HRESULT keyed_release_sync(void *this, uint64_t key) {
    d3d_texture *t = keyed_of(this);
    if (!t->shared) return S_OK;
    static int sync_release = -1;
    if (sync_release < 0) sync_release = getenv("VXRT_SYNC_RELEASE") && atoi(getenv("VXRT_SYNC_RELEASE"));
    shared_slot *slot_e = &table()->s[t->shared - 1];
    vx_profile_held(-1);
    if (g_keyed_stats > 0) hold_stats(t->shared - 1);
    if (vx_keyed_held) vx_keyed_held--;
    if (!sync_release && !getenv("VXRT_SHARED_SNAPSHOT")) {
        if (getenv("VXRT_TRACE_KEYED")) {
            uint32_t *o = vx_held_ops;
            vx_log("keyed mutex slot %d: ReleaseSync(%llu) by %d, published when the GPU is done; held %.2f ms: %u draws, %u dispatches, %u copies, %u region copies, %u resolves, %u maps, %u flushes",
                   t->shared - 1, (unsigned long long)key, (int)getpid(), (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - g_held_since) / 1e6,
                   o[0], o[1], o[2], o[3], o[4], o[5], o[6]);
            memset(vx_held_ops, 0, sizeof vx_held_ops);
        }
        __atomic_store_n(&slot_e->gpu_pid, (int32_t)getpid(), __ATOMIC_RELEASE);
        __atomic_add_fetch(&slot_e->gpu_pending, 1, __ATOMIC_ACQ_REL);
        publish_release(slot_e, key);
        d3d_flush(t->base.dev, 0);
        id<MTLCommandBuffer> fence = [(__bridge id<MTLCommandQueue>)t->base.dev->queue commandBuffer];
        [fence addCompletedHandler:^(id<MTLCommandBuffer> done) {
            (void)done;
            __atomic_sub_fetch(&slot_e->gpu_pending, 1, __ATOMIC_ACQ_REL);
            __atomic_add_fetch(&slot_e->released, 1, __ATOMIC_RELEASE);
            if (__builtin_available(macOS 14.4, *)) os_sync_wake_by_address_all((void *)&slot_e->released, 4, OS_SYNC_WAKE_BY_ADDRESS_SHARED);
        }];
        [fence commit];
        return S_OK;
    }
    d3d_flush(t->base.dev, 1);                                    // the other process sees what was drawn
    // VXRT_SHARED_SNAPSHOT=file.png (VXRT_SHARED_SNAPSHOT_AT=n, default 600): the n-th texture this process releases.
    // Or, with VXRT_SHARED_SNAPSHOT_AT=now, while the file <file.png>.now exists (every 30 releases; remove it after).
    static int releases;
    const char *snap = getenv("VXRT_SHARED_SNAPSHOT"), *at = getenv("VXRT_SHARED_SNAPSHOT_AT");
    int fire = 0;
    if (snap && at && !strcmp(at, "now")) {
        char trigger[1024]; snprintf(trigger, sizeof trigger, "%s.now", snap);
        static int last;
        int n = __sync_add_and_fetch(&releases, 1);
        if (n - last >= 30 && !access(trigger, F_OK)) { last = n; fire = 1; }
    } else if (snap) fire = __sync_add_and_fetch(&releases, 1) == (at ? atoi(at) : 600);
    if (fire) {
        vx_log("shared snapshot: slot %d, DXGI format %u", t->shared - 1, t->desc[TD_FORMAT]);
        d3d_texture_png(t->base.dev, t->mtl, snap);
        for (int i = 0; i < g_noted_count && i < 64; i++) {
            char path[1024];
            snprintf(path, sizeof path, "%s.%d.%d.png", snap, (int)getpid(), g_noted_slot[i]);
            d3d_texture_png(t->base.dev, g_noted[i], path);
        }
    }
    shared_slot *e = &table()->s[t->shared - 1];
    if (getenv("VXRT_TRACE_KEYED")) vx_log("keyed mutex slot %d: ReleaseSync(%llu) by %d (owner was %d)", t->shared - 1, (unsigned long long)key, (int)getpid(), (int)e->owner);
    publish_release(e, key);
    return S_OK;
}
static WINAPI HRESULT keyed_get_device(void *this, const GUID *iid, void **out) {
    d3d_device *d = keyed_of(this)->base.dev;
    return ((HRESULT (WINAPI *)(void *, const GUID *, void **))d->base.vtbl[0])(d, iid, out);
}

static void **g_res_vtbl, **g_keyed_vtbl;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void make_tables(void) {
    static const d3d_method res[] = { { "QueryInterface", (void *)res_qi }, { "AddRef", (void *)res_addref }, { "Release", (void *)res_release },
        { "SetPrivateData", (void *)res_set_private }, { "SetPrivateDataInterface", (void *)res_set_private_interface },
        { "GetPrivateData", (void *)res_get_private }, { "GetParent", (void *)res_get_parent }, { "GetDevice", (void *)res_get_device },
        { "GetSharedHandle", (void *)res_get_shared_handle }, { "GetUsage", (void *)res_get_usage },
        { "SetEvictionPriority", (void *)res_set_eviction }, { "GetEvictionPriority", (void *)res_get_eviction },
        { "CreateSharedHandle", (void *)res_create_shared_handle }, { NULL, NULL } };
    static const d3d_method keyed[] = { { "QueryInterface", (void *)keyed_qi }, { "AddRef", (void *)keyed_addref }, { "Release", (void *)keyed_release },
        { "GetDevice", (void *)keyed_get_device }, { "GetParent", (void *)keyed_get_device },
        { "AcquireSync", (void *)keyed_acquire }, { "ReleaseSync", (void *)keyed_release_sync }, { NULL, NULL } };
    g_res_vtbl = D3D_VTABLE(IDXGIResource1, res);
    g_keyed_vtbl = D3D_VTABLE(IDXGIKeyedMutex, keyed);
}
void **d3d_dxgi_resource_vtbl(void) { pthread_once(&g_once, make_tables); return g_res_vtbl; }
void **d3d_keyed_mutex_vtbl(void) { pthread_once(&g_once, make_tables); return g_keyed_vtbl; }

#ifdef __aarch64__
#include "thunks/d3d11_shared.m.inc"
#endif
