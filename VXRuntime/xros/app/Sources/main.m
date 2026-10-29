// VX Runtime: VXRuntime on Vision Pro as it is (../../VISIONOS_PORT.md). One flat window; the Windows program named by
// the launch arguments runs in it, from the bundle, on the AOT modules linked into the app (no JIT on visionOS).
//
//   xcrun devicectl device process launch --device <id> marimokodai.VXRuntimeHost -test d3d12
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
void vxrt_xros_attach(UIView *display);                     // (src/xros_ui.m)

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
    int arena = xa_reserve();
    jlog("arena", "\"addr\":\"0xf00000000\",\"size_mb\":512,\"reserved\":%d", arena);
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
    metal_probe();
    vxrt_xros_attach(self.view);
    pthread_t t;
    pthread_create(&t, NULL, memory_ticker, NULL);
    if (!strcmp(g_test, "none")) { status(@"VX Runtime: probes only (Documents/host_log.jsonl)"); return; }
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8u << 20);
    pthread_create(&t, &a, runtime_thread, NULL);
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    [self.view bringSubviewToFront:g_status];
}
@end

@interface VXSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@end
@implementation VXSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    UIWindowScene *ws = (UIWindowScene *)scene;
    ws.sizeRestrictions.minimumSize = CGSizeMake(1280, 720);
    self.window = [[UIWindow alloc] initWithWindowScene:ws];
    self.window.rootViewController = [VXRootController new];
    [self.window makeKeyAndVisible];
}
@end

@interface VXAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation VXAppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options { return YES; }
@end

int main(int argc, char *argv[]) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass([VXAppDelegate class])); }
}
