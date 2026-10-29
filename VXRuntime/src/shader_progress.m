#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include "vx.h"
#include "shader_progress.h"

typedef struct { uint64_t requested, completed, failed, ns; } compile_counts;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static compile_counts g_libraries, g_pipelines;
static NSMutableSet<NSData *> *g_sources;
static uint64_t g_translations, g_translation_failed, g_translate_ns, g_waits, g_wait_ns;
static double g_started, g_last_activity;
static NSString *g_path;
static dispatch_queue_t g_queue;
static dispatch_source_t g_timer;
static int g_initialized;
static void write_progress(int force);

static void progress_init(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        g_started = NSDate.date.timeIntervalSince1970;
        g_last_activity = g_started;
        g_sources = [NSMutableSet new];
        const char *path = getenv("VXRT_SHADER_PROGRESS_FILE"), *expected = getenv("VXRT_SHADER_PROGRESS_EXE");
        const char *exe = vx_proc.argv && vx_proc.argv[0] ? vx_proc.argv[0] : "";
        for (const char *at = exe; *at; at++) if (*at == '/' || *at == '\\') exe = at + 1;
        // Children inherit the environment: SteamVR helpers must not overwrite the game's counters.
        if (path && *path && (!expected || !strcasecmp(expected, exe))) g_path = @(path);
        g_queue = dispatch_queue_create("vxrt.shader-progress", DISPATCH_QUEUE_SERIAL);
        g_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, g_queue);
        dispatch_source_set_timer(g_timer, dispatch_time(DISPATCH_TIME_NOW, 0), 250 * NSEC_PER_MSEC, 25 * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(g_timer, ^{ @autoreleasepool { write_progress(0); } });
        dispatch_resume(g_timer);
        atexit(vx_shader_progress_flush);
        __atomic_store_n(&g_initialized, 1, __ATOMIC_RELEASE);
    });
}

static NSDictionary *counts(compile_counts c) {
    return @{ @"requested": @(c.requested), @"completed": @(c.completed), @"failed": @(c.failed),
              @"pending": @(c.requested - c.completed), @"elapsed_ms": @(c.ns / 1000000.0) };
}
NSDictionary *vx_shader_progress_snapshot(void) {
    progress_init();
    pthread_mutex_lock(&g_lock);
    NSDictionary *snapshot = @{ @"version": @1, @"pid": @(getpid()), @"started_at": @(g_started),
        @"updated_at": @(NSDate.date.timeIntervalSince1970), @"last_activity_at": @(g_last_activity),
        @"libraries": counts(g_libraries), @"pipelines": counts(g_pipelines), @"unique_sources": @(g_sources.count),
        @"translations": @(g_translations), @"translation_failed": @(g_translation_failed),
        @"translate_ms": @(g_translate_ns / 1000000.0), @"shader_waits": @(g_waits), @"shader_wait_ms": @(g_wait_ns / 1000000.0),
        @"cache": vx_shader_cache_snapshot() };
    pthread_mutex_unlock(&g_lock);
    return snapshot;
}
static void write_progress(int force) {
    NSDictionary *s = vx_shader_progress_snapshot();
    NSDictionary *lib = s[@"libraries"], *pso = s[@"pipelines"];
    // Idle snapshots still carry a heartbeat: an old file must never enable the trigger prompt.
    if (g_path) {
        NSData *data = [NSJSONSerialization dataWithJSONObject:s options:NSJSONWritingSortedKeys error:nil];
        NSError *error = nil;
        if (data && ![data writeToFile:g_path options:NSDataWritingAtomic error:&error]) {
            static int warned;
            if (!__atomic_exchange_n(&warned, 1, __ATOMIC_RELAXED)) vx_log("Direct3D: cannot write shader progress: %s", error.localizedDescription.UTF8String);
        }
    }
    static double last_log;
    static uint64_t last_events;
    uint64_t events = [lib[@"completed"] unsignedLongLongValue] + [pso[@"completed"] unsignedLongLongValue] +
                      [lib[@"requested"] unsignedLongLongValue] + [pso[@"requested"] unsignedLongLongValue];
    double now = [s[@"updated_at"] doubleValue];
    if ((force || now - last_log >= 1) && events != last_events) {
        vx_log("Direct3D compilation: libraries %llu/%llu (%llu unique, %llu failed), pipelines %llu/%llu (%llu failed), translation %.1f ms, library requests %.1f ms, pipeline requests %.1f ms, shader waits %llu / %.1f ms",
            [lib[@"completed"] unsignedLongLongValue], [lib[@"requested"] unsignedLongLongValue], [s[@"unique_sources"] unsignedLongLongValue], [lib[@"failed"] unsignedLongLongValue],
            [pso[@"completed"] unsignedLongLongValue], [pso[@"requested"] unsignedLongLongValue], [pso[@"failed"] unsignedLongLongValue],
            [s[@"translate_ms"] doubleValue], [lib[@"elapsed_ms"] doubleValue], [pso[@"elapsed_ms"] doubleValue],
            [s[@"shader_waits"] unsignedLongLongValue], [s[@"shader_wait_ms"] doubleValue]);
        NSDictionary *cache = s[@"cache"];
        vx_log("Direct3D cache: libraries %llu memory / %llu disk hits, %llu source compiles; GPU pipelines %llu archive hits / %llu misses, %llu archives saved, %llu cache errors",
            [cache[@"library_memory_hits"] unsignedLongLongValue], [cache[@"library_disk_hits"] unsignedLongLongValue], [cache[@"source_compiles"] unsignedLongLongValue],
            [cache[@"pipeline_hits"] unsignedLongLongValue], [cache[@"pipeline_misses"] unsignedLongLongValue], [cache[@"archives_saved"] unsignedLongLongValue], [cache[@"errors"] unsignedLongLongValue]);
        last_log = now; last_events = events;
    }
}
void vx_shader_progress_flush(void) {
    vx_shader_cache_flush();
    progress_init();
    dispatch_sync(g_queue, ^{ @autoreleasepool { write_progress(1); } });
}
void vx_shader_progress_exit(void) {
    // Windows ExitProcess uses _exit: it does not run the host's atexit handlers.
    if (__atomic_load_n(&g_initialized, __ATOMIC_ACQUIRE)) vx_shader_progress_flush();
}

void vx_shader_translation_finished(uint64_t ns, int success) {
    progress_init();
    pthread_mutex_lock(&g_lock);
    g_translations++; g_translate_ns += ns; g_translation_failed += !success;
    pthread_mutex_unlock(&g_lock);
}
void vx_shader_wait_finished(uint64_t ns) {
    progress_init();
    pthread_mutex_lock(&g_lock);
    g_waits++; g_wait_ns += ns;
    pthread_mutex_unlock(&g_lock);
}
static uint64_t begin(int library, NSString *source) {
    progress_init();
    NSData *digest = nil;
    if (source) {
        unsigned char hash[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256(source.UTF8String, (CC_LONG)[source lengthOfBytesUsingEncoding:NSUTF8StringEncoding], hash);
        digest = [NSData dataWithBytes:hash length:sizeof hash];
    }
    pthread_mutex_lock(&g_lock);
    (library ? &g_libraries : &g_pipelines)->requested++;
    if (digest) [g_sources addObject:digest];
    g_last_activity = NSDate.date.timeIntervalSince1970;
    pthread_mutex_unlock(&g_lock);
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}
static void finish(int library, uint64_t start, int success) {
    uint64_t ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
    pthread_mutex_lock(&g_lock);
    compile_counts *c = library ? &g_libraries : &g_pipelines;
    c->completed++; c->failed += !success; c->ns += ns;
    g_last_activity = NSDate.date.timeIntervalSince1970;
    pthread_mutex_unlock(&g_lock);
}
id<MTLLibrary> vx_metal_library(id<MTLDevice> device, NSString *source, NSError **error) {
    return vx_metal_library_entries(device, source, @[@"vx_main"], error);
}
id<MTLLibrary> vx_metal_library_entries(id<MTLDevice> device, NSString *source, NSArray<NSString *> *entries, NSError **error) {
    uint64_t start = begin(1, source);
    id<MTLLibrary> lib = vx_shader_cache_library(device, source, entries, error);
    finish(1, start, lib != nil);
    return lib;
}
void vx_metal_library_async(id<MTLDevice> device, NSString *source, MTLNewLibraryCompletionHandler done) {
    uint64_t start = begin(1, source);
    vx_shader_cache_library_async(device, source, @[@"vx_main"], ^(id<MTLLibrary> lib, NSError *error) {
        finish(1, start, lib != nil);
        done(lib, error);
    });
}
id<MTLRenderPipelineState> vx_metal_render_pipeline(id<MTLDevice> device, MTLRenderPipelineDescriptor *desc, NSError **error) {
    uint64_t start = begin(0, nil);
    id<MTLRenderPipelineState> pso = vx_shader_cache_render(device, desc, error);
    finish(0, start, pso != nil);
    return pso;
}
id<MTLComputePipelineState> vx_metal_compute_pipeline(id<MTLDevice> device, id<MTLFunction> function, NSError **error) {
    uint64_t start = begin(0, nil);
    id<MTLComputePipelineState> pso = vx_shader_cache_compute_function(device, function, error);
    finish(0, start, pso != nil);
    return pso;
}
id<MTLComputePipelineState> vx_metal_compute_descriptor(id<MTLDevice> device, MTLComputePipelineDescriptor *desc, NSError **error) {
    uint64_t start = begin(0, nil);
    id<MTLComputePipelineState> pso = vx_shader_cache_compute(device, desc, error);
    finish(0, start, pso != nil);
    return pso;
}
