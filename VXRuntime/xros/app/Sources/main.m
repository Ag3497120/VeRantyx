// VX Runtime: VXRuntime on Vision Pro (../../VISIONOS_PORT.md). The Windows program named by the launch arguments runs
// from the bundle on the AOT modules linked into or embedded in the app (no JIT on visionOS); its swap chain is shown
// in an immersive space (CompositorServices, both eyes warped from the game's frame: VXImmersive.swift,
// src/xros_vr.m), or with -present flat only in the window (debug).
//
//   xcrun devicectl device process launch --device <id> marimokodai.VXRuntimeHost -test d3d12 [-present flat]
//
// Tests (bundle/exe): xros_clear (a swap chain cleared through the hues, 10 s), d3d12 (default), suite, hello_crt, seh_try, unwind, cxx_exceptions (stops: one address has no
// module), none (only the memory and address-space probes). Documents/host_log.jsonl gets the probes, the
// available memory every 2 s and the program's exit; Documents/vxrt.log the runtime's log (stderr, this launch).
#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#include <crt_externs.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <os/proc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

int vx_loader_main(int argc, char **argv, char **envp);   // (src/loader.c, its main)
int xa_reserve(void);                                       // (translator/aot.c: the modules' image arena)
uint64_t xa_arena_base(void), xa_arena_size(void);
void vxrt_xros_attach(UIView *display);                     // (src/xros_ui.m)
void vx_xros_vr_enable(int on);                             // (src/xros_vr.m: the immersive presenter)
int vxrt_present_vr(void);

static char g_docs[1024];
static int g_log = -1;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_test[64] = "d3d12";
static double t0;

static double now(void) { return CFAbsoluteTimeGetCurrent() + kCFAbsoluteTimeIntervalSince1970; }   // (Unix time)
static uint64_t footprint_mb(void) {
    task_vm_info_data_t info; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &n) == KERN_SUCCESS ? info.phys_footprint >> 20 : 0;
}
static void jlog(const char *event, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jlog(const char *event, const char *fmt, ...) {
    char extra[1536] = "";
    if (fmt) { va_list ap; va_start(ap, fmt); vsnprintf(extra, sizeof extra, fmt, ap); va_end(ap); }
    char line[2048];
    int n = snprintf(line, sizeof line, "{\"t\":%.3f,\"since_start\":%.3f,\"test\":\"%s\",\"event\":\"%s\",\"available_mb\":%llu,\"footprint_mb\":%llu%s%s}\n",
                     now(), now() - t0, g_test, event, (unsigned long long)(os_proc_available_memory() >> 20),
                     (unsigned long long)footprint_mb(), *extra ? "," : "", extra);
    pthread_mutex_lock(&g_log_lock);
    if (g_log >= 0) write(g_log, line, (size_t)n);
    pthread_mutex_unlock(&g_log_lock);
}
void vxrt_xros_program_exited(int rc) { jlog("exit", "\"rc\":%d", rc); }

// The address space as the system left it at launch (after the arena): every mapped region of 16 MB or more and every
// gap of 64 MB or more up to 1 TB, then whether 1 GB can be had exactly at the candidate arena bases (each freed again).
static void vm_map_probe(void) {
    vm_address_t r = 0, prev_end = 0; vm_size_t rs = 0;
    char buf[1400]; int k = 0, nreg = 0;
    for (;;) {
        natural_t depth = 0; vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &r, &rs, &depth, (vm_region_recurse_info_64_t)&info, &cnt) != KERN_SUCCESS) break;
        if (r >= (1ull << 40)) break;
        if (r > prev_end && r - prev_end >= (64ull << 20)) jlog("va_gap", "\"lo\":\"%#llx\",\"hi\":\"%#llx\",\"mb\":%llu",
            (unsigned long long)prev_end, (unsigned long long)r, (unsigned long long)((r - prev_end) >> 20));
        if (rs >= (16ull << 20)) {
            k += snprintf(buf + k, sizeof buf - k, "%s{\"lo\":\"%#llx\",\"mb\":%llu,\"prot\":%d,\"max\":%d,\"tag\":%u,\"share\":%u,\"submap\":%d}",
                          nreg ? "," : "", (unsigned long long)r, (unsigned long long)(rs >> 20), info.protection, info.max_protection,
                          info.user_tag, info.share_mode, info.is_submap);
            if (++nreg == 8 || k > 1100) { jlog("va_regions", "\"regions\":[%s]", buf); k = 0; nreg = 0; }
        }
        prev_end = r + rs; r += rs;
    }
    if (nreg) jlog("va_regions", "\"regions\":[%s]", buf);
    jlog("va_end", "\"last_end\":\"%#llx\"", (unsigned long long)prev_end);
    static const uint64_t cand[] = { 0x140000000ull, 0x180000000ull, 0x200000000ull, 0x240000000ull, 0x280000000ull, 0x2a0000000ull,
                                     0xfc0000000ull, 0x1000000000ull, 0x2000000000ull, 0x4000000000ull, 0x6000000000ull, 0x7000000000ull, 0x7400000000ull,
                                     0x7800000000ull, 0x7c00000000ull, 0x8000000000ull, 0x10000000000ull };
    k = 0;
    for (unsigned i = 0; i < sizeof cand / sizeof *cand; i++) {
        vm_address_t a = cand[i];
        kern_return_t kr = vm_allocate(mach_task_self(), &a, 1ull << 30, VM_FLAGS_FIXED);
        if (kr == KERN_SUCCESS) vm_deallocate(mach_task_self(), a, 1ull << 30);
        k += snprintf(buf + k, sizeof buf - k, "%s\"%#llx\":%d", i ? "," : "", (unsigned long long)cand[i], kr);
    }
    jlog("va_fixed_1gb", "\"kr\":{%s}", buf);
    // where the system puts large reservations (the guard region from 0xfc0000000 up is not taken by vm_allocate
    // fixed): mmap without a fixed address, with a hint, and fixed over the guard (each freed again)
    k = 0;
    static const uint64_t sizes_gb[] = { 1, 8, 64 };
    for (unsigned i = 0; i < 3; i++) {
        void *r = mmap(NULL, sizes_gb[i] << 30, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        k += snprintf(buf + k, sizeof buf - k, "%s\"any_%llugb\":\"%p\"", i ? "," : "", (unsigned long long)sizes_gb[i], r);
        if (r != MAP_FAILED) munmap(r, sizes_gb[i] << 30);
    }
    static const uint64_t hints[] = { 0x1000000000ull, 0x2000000000ull, 0x4000000000ull };
    for (unsigned i = 0; i < 3; i++) {
        void *r = mmap((void *)(uintptr_t)hints[i], 1ull << 30, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        k += snprintf(buf + k, sizeof buf - k, ",\"hint_%#llx\":\"%p\"", (unsigned long long)hints[i], r);
        if (r != MAP_FAILED) munmap(r, 1ull << 30);
        r = mmap((void *)(uintptr_t)hints[i], 1ull << 30, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        k += snprintf(buf + k, sizeof buf - k, ",\"fixed_%#llx\":\"%p\"", (unsigned long long)hints[i], r);
        if (r != MAP_FAILED) {
            void *rw = mmap(r, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);   // (usable?)
            int ok = rw != MAP_FAILED; if (ok) { ((volatile char *)rw)[0] = 1; }
            k += snprintf(buf + k, sizeof buf - k, ",\"fixed_%#llx_rw\":%d", (unsigned long long)hints[i], ok);
            munmap(r, 1ull << 30);
        }
    }
    jlog("va_large", "%s", buf);
}

// Before anything else maps memory (a constructor, before main): the modules' arena, then what the address space and
// the memory limit look like. (On the Mac the runtime starts itself again until the arena is free; an app cannot.)
__attribute__((constructor)) static void early(void) {
    t0 = now();
    const char *home = getenv("HOME");
    snprintf(g_docs, sizeof g_docs, "%s/Documents", home ? home : ".");
    mkdir(g_docs, 0755);
    char p[1200];
    snprintf(p, sizeof p, "%s/host_log.jsonl", g_docs);
    g_log = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
    snprintf(p, sizeof p, "%s/vxrt.log", g_docs);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, 2); dup2(fd, 1); close(fd); }
    setvbuf(stdout, NULL, _IOLBF, 0);
    int argc = *_NSGetArgc(); char **argv = *_NSGetArgv();
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "-test")) snprintf(g_test, sizeof g_test, "%s", argv[i + 1]);
    uint64_t ram = 0; size_t sz = sizeof ram; sysctlbyname("hw.memsize", &ram, &sz, NULL, 0);
    jlog("launch", "\"device_ram_mb\":%llu,\"pid\":%d", (unsigned long long)(ram >> 20), getpid());
    uint64_t abase = xa_arena_base(), asize = xa_arena_size();
    int arena = xa_reserve();
    jlog("arena", "\"addr\":\"%#llx\",\"size_mb\":%llu,\"reserved\":%d", (unsigned long long)abase, (unsigned long long)(asize >> 20), arena);
    vm_map_probe();
    // the exe's preferred base (0x140000000) and the DLL range above it: free now?
    vm_address_t a = 0x140000000ull;
    kern_return_t kr = vm_allocate(mach_task_self(), &a, 256ull << 20, VM_FLAGS_FIXED);
    if (kr == KERN_SUCCESS) vm_deallocate(mach_task_self(), a, 256ull << 20);
    jlog("exe_base", "\"addr\":\"0x140000000\",\"size_mb\":256,\"free\":%d,\"kr\":%d", kr == KERN_SUCCESS, kr);
    // the largest PROT_NONE reservation (extended-virtual-addressing)
    uint64_t best = 0;
    for (uint64_t g = 128; g >= 1 && !best; g /= 2) {
        void *r = mmap(NULL, g << 30, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (r != MAP_FAILED) { best = g; munmap(r, g << 30); }
    }
    jlog("va_probe", "\"largest_reservation_gb\":%llu", (unsigned long long)best);
}

// What the GPU offers that Direct3D 12 games need (BC textures, argument buffers, sparse pages, Metal 4), for the log.
static void metal_probe(void) {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    if (!d) { jlog("metal", "\"device\":null"); return; }
    int mtl4 = 0, sparse = 0;
#if !TARGET_OS_SIMULATOR
    if (@available(visionOS 26.0, *)) {
        mtl4 = [d newMTL4CommandQueue] != nil;
        sparse = [d newBufferWithLength:65536 options:MTLResourceStorageModePrivate placementSparsePageSize:MTLSparsePageSize64] != nil;
    }
#endif
    jlog("metal", "\"device\":\"%s\",\"apple7\":%d,\"apple8\":%d,\"apple9\":%d,\"metal3\":%d,\"bc\":%d,\"argbuf_tier\":%d,"
         "\"working_set_mb\":%llu,\"max_buffer_mb\":%llu,\"float32_filter\":%d,\"raytracing\":%d,\"mtl4_queue\":%d,\"placement_sparse\":%d,"
         "\"sparse_tile\":%llu",
         d.name.UTF8String, [d supportsFamily:MTLGPUFamilyApple7], [d supportsFamily:MTLGPUFamilyApple8], [d supportsFamily:MTLGPUFamilyApple9],
         [d supportsFamily:MTLGPUFamilyMetal3], d.supportsBCTextureCompression, (int)d.argumentBuffersSupport,
         (unsigned long long)(d.recommendedMaxWorkingSetSize >> 20), (unsigned long long)(d.maxBufferLength >> 20),
         d.supports32BitFloatFiltering, d.supportsRaytracing, mtl4, sparse, (unsigned long long)d.sparseTileSizeInBytes);
}

static void *memory_ticker(void *arg) {
    for (;;) { sleep(2); jlog("mem", NULL); }
    return NULL;
}

static UILabel *g_status;
static void status(NSString *s) { dispatch_async(dispatch_get_main_queue(), ^{ g_status.text = s; }); }

static void *runtime_thread(void *arg) {
    NSString *res = NSBundle.mainBundle.resourcePath;
    NSString *bundle = [res stringByAppendingPathComponent:@"bundle"];
    NSString *exe = [bundle stringByAppendingFormat:@"/exe/%s.exe", g_test];
    char root[1200];
    snprintf(root, sizeof root, "%s/root", g_docs);
    mkdir(root, 0755);
    setenv("VXRT_ROOT", root, 0);
    setenv("VXRT_AOT_DIR", "/linked", 0);                  // (any folder: translator/aot.c finds the modules by file name)
    setenv("VXRT_AOT_STRICT", "1", 0);
    setenv("VXRT_LOG_TIME", "1", 0);
    setenv("VXRT_MESSAGEBOX", "auto", 0);
    setenv("VXRT_NPU_INTERP", "race", 0);                  // (the NPU race path, as in every Cyberpunk run: logged if off)
    chdir(bundle.fileSystemRepresentation);                  // (the tests read tests/shaders12/... from here)
    vx_xros_vr_enable(vxrt_present_vr());
    if (![NSFileManager.defaultManager fileExistsAtPath:exe]) {
        jlog("no_test", "\"exe\":\"%s\"", exe.UTF8String);
        status([NSString stringWithFormat:@"VX Runtime: no test %s", g_test]);
        return NULL;
    }
    jlog("start", "\"exe\":\"%s\"", exe.UTF8String);
    status([NSString stringWithFormat:@"VX Runtime: running %s.exe (AOT, no JIT)", g_test]);
    char *argv[] = { "vxrt", (char *)exe.fileSystemRepresentation, NULL };
    extern char **environ;
    int rc = vx_loader_main(2, argv, environ);
    jlog("loader_returned", "\"rc\":%d", rc);
    status([NSString stringWithFormat:@"VX Runtime: %s.exe: loader returned %d", g_test, rc]);
    return NULL;
}

static void vxrt_boot(UIView *display);
static int g_booted;
@interface VXRootController : UIViewController
@end
@implementation VXRootController {
    BOOL started;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    g_status = [UILabel new];
    g_status.textColor = UIColor.whiteColor;
    g_status.font = [UIFont monospacedSystemFontOfSize:18 weight:UIFontWeightRegular];
    g_status.text = @"VX Runtime";
    g_status.frame = CGRectMake(16, 8, 1000, 28);
    g_status.layer.zPosition = 1000;
    [self.view addSubview:g_status];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (started) return;
    started = YES;
    CGSize s = self.view.bounds.size;
    jlog("window", "\"w\":%.0f,\"h\":%.0f,\"scale\":%.1f", s.width, s.height, self.view.traitCollection.displayScale);
    vxrt_boot(self.view);
}
@end

// The program's start: the display (the window's view, or a view in no window when none appeared: the immersive space
// shows the swap chain anyway), the probes, the memory log, the program's thread. Once.
static void vxrt_boot(UIView *display) {
    if (g_booted++) return;
    metal_probe();
    vxrt_xros_attach(display);
    pthread_t t;
    pthread_create(&t, NULL, memory_ticker, NULL);
    if (!strcmp(g_test, "none")) { status(@"VX Runtime: probes only (Documents/host_log.jsonl)"); return; }
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8u << 20);
    pthread_create(&t, &a, runtime_thread, NULL);
}
void vxrt_boot_fallback(void) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        static UIView *detached;
        if (detached || g_booted) return;
        detached = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 1280, 720)];
        jlog("window_fallback", "\"why\":\"no window appeared in 4 s: a view in no window is the display\"");
        vxrt_boot(detached);
    });
}

// The SwiftUI app (VXApp.swift) hosts this controller in its window and opens the immersive space (VXImmersive.swift).
UIViewController *vxrt_make_root_controller(void) { return [VXRootController new]; }
// -present vr (the default): the program's swap chain is shown in the immersive space (src/xros_vr.m); -present flat:
// only in the window (a debug fallback).
int vxrt_present_vr(void) {
    int argc = *_NSGetArgc(); char **argv = *_NSGetArgv();
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "-present")) return strcmp(argv[i + 1], "flat") != 0;
    return 1;
}
void vxrt_jlog(const char *event, const char *fields) { jlog(event, "%s", fields ? fields : ""); }
