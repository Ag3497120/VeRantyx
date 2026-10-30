#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <CommonCrypto/CommonDigest.h>
#include <pthread.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "shader_cache.h"

// Each pipeline gets an immutable, content-addressed archive. Atomic replacement allows several Windows
// processes to share the cache without losing each other's entries or holding locks across compiler calls.
@interface VXShaderInfo : NSObject
@property NSString *root, *key;
@property NSArray<NSString *> *entries;
@end
@implementation VXShaderInfo
@end

@interface VXShaderRequest : NSObject
@property VXShaderInfo *info;
@property dispatch_group_t group;
@property id<MTLLibrary> library;
@property NSError *error;
@property NSString *memoryKey;
@end
@implementation VXShaderRequest
@end

static char g_info_key, g_root_key;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static NSCache *g_libraries;
static NSMutableDictionary *g_pending;
static dispatch_queue_t g_writes;
static uint64_t g_memory_hits, g_disk_hits, g_source_compiles, g_pipeline_hits, g_pipeline_misses, g_saved, g_errors;

static void initialize(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        g_libraries = [NSCache new];
        g_libraries.countLimit = 256;
        g_pending = [NSMutableDictionary new];
        g_writes = dispatch_queue_create("vxrt.shader-cache-writes", DISPATCH_QUEUE_SERIAL);
    });
}
static void count(uint64_t *counter) { __atomic_fetch_add(counter, 1, __ATOMIC_RELAXED); }
static uint64_t value(uint64_t *counter) { return __atomic_load_n(counter, __ATOMIC_RELAXED); }
static NSString *hash_data(NSData *data) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
    char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
    for (size_t i = 0; i < sizeof digest; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return @(hex);
}
static NSString *hash_object(id object) {
    return hash_data([NSJSONSerialization dataWithJSONObject:object options:NSJSONWritingSortedKeys error:nil]);
}

// Bound disk use at startup. A dangling library index is just a miss; Metal's own cache is never touched.
static void prune(NSString *root) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSURL *url = [NSURL fileURLWithPath:root];
    NSArray<NSURL *> *files = [fm contentsOfDirectoryAtURL:url includingPropertiesForKeys:@[NSURLFileSizeKey, NSURLContentModificationDateKey] options:NSDirectoryEnumerationSkipsHiddenFiles error:nil];
    NSMutableArray *archives = [NSMutableArray new];
    uint64_t total = 0;
    for (NSURL *file in files) if ([file.pathExtension isEqual:@"metallib"]) {
        NSDictionary *r = [file resourceValuesForKeys:@[NSURLFileSizeKey, NSURLContentModificationDateKey] error:nil];
        uint64_t size = [r[NSURLFileSizeKey] unsignedLongLongValue];
        total += size;
        [archives addObject:@{ @"url": file, @"size": @(size), @"date": r[NSURLContentModificationDateKey] ?: NSDate.distantPast }];
    }
    [archives sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) { return [a[@"date"] compare:b[@"date"]]; }];
    for (NSDictionary *a in archives) {
        if (total <= 1024ull * 1024 * 1024) break;
        if ([fm removeItemAtURL:a[@"url"] error:nil]) total -= [a[@"size"] unsignedLongLongValue];
    }
}

static NSString *cache_root(id<MTLDevice> device) {
    const char *enabled = getenv("VXRT_SHADER_CACHE");
    if (enabled && !strcmp(enabled, "0")) return nil;
    if (![device respondsToSelector:@selector(newBinaryArchiveWithDescriptor:error:)]) return nil;
    initialize();
    @synchronized (device) {
        id saved = objc_getAssociatedObject(device, &g_root_key);
        if (saved) return saved == NSNull.null ? nil : saved;
        const char *override = getenv("VXRT_SHADER_CACHE_DIR");
        NSString *base = override && *override ? @(override) : [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Caches/VeRantyx/shaders"];
        char build[128] = {0}; size_t size = sizeof build;
        if (sysctlbyname("kern.osversion", build, &size, NULL, 0)) snprintf(build, sizeof build, "%s", NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String);
        NSString *scope = hash_object(@[@"vxrt-metal-v1", @(build), @(device.registryID), device.name]);
        NSString *root = [[base stringByAppendingPathComponent:@"v1"] stringByAppendingPathComponent:scope];
        if (![NSFileManager.defaultManager createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil]) {
            count(&g_errors); root = nil;
        }
        objc_setAssociatedObject(device, &g_root_key, root ?: NSNull.null, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        if (root) dispatch_async(g_writes, ^{ @autoreleasepool { prune(root); } });
        return root;
    }
}
static NSString *symbol(VXShaderInfo *info, NSString *name) {
    return [NSString stringWithFormat:@"vxc_%@_%@", info.key, name];
}
static MTLCompileOptions *compile_options(VXShaderInfo *info) {
    MTLCompileOptions *options = [MTLCompileOptions new];
    NSMutableDictionary *macros = [NSMutableDictionary new];
    // Archives merge IR from several libraries. Unique entry names keep two unrelated vx_main functions
    // distinguishable when the archived IR is loaded as an MTLLibrary on the next launch.
    for (NSString *name in info.entries) macros[name] = symbol(info, name);
    options.preprocessorMacros = macros;
    return options;
}
static NSString *index_path(VXShaderInfo *info) {
    return [info.root stringByAppendingPathComponent:[info.key stringByAppendingString:@".index"]];
}
static id<MTLLibrary> load_library(id<MTLDevice> device, VXShaderInfo *info) {
    NSString *file = [NSString stringWithContentsOfFile:index_path(info) encoding:NSUTF8StringEncoding error:nil];
    if (!file) return nil;
    NSCharacterSet *hex = [NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"];
    if (file.length != 73 || ![file hasSuffix:@".metallib"] || [[file substringToIndex:64] rangeOfCharacterFromSet:hex.invertedSet].location != NSNotFound) {
        count(&g_errors); return nil;
    }
    id<MTLLibrary> lib = [device newLibraryWithURL:[NSURL fileURLWithPath:[info.root stringByAppendingPathComponent:file]] error:nil];
    if (!lib) { count(&g_errors); return nil; }
    for (NSString *name in info.entries) if (![lib.functionNames containsObject:symbol(info, name)]) { count(&g_errors); return nil; }
    count(&g_disk_hits);
    return lib;
}
static VXShaderRequest *request(id<MTLDevice> device, NSString *source, NSArray<NSString *> *entries, BOOL *owner) {
    NSString *root = entries.count ? cache_root(device) : nil;
    if (!root) return nil;
    NSString *key = hash_object(@[source, entries]);
    NSString *memory_key = [NSString stringWithFormat:@"%@/%@/%p", root, key, device];
    pthread_mutex_lock(&g_lock);
    VXShaderRequest *r = [g_libraries objectForKey:memory_key] ?: g_pending[memory_key];
    *owner = r == nil;
    if (!r) {
        r = [VXShaderRequest new];
        r.memoryKey = memory_key;
        r.info = [VXShaderInfo new]; r.info.root = root; r.info.key = key; r.info.entries = [entries copy];
        r.group = dispatch_group_create(); dispatch_group_enter(r.group);
        g_pending[memory_key] = r;
    } else count(&g_memory_hits);
    pthread_mutex_unlock(&g_lock);
    return r;
}
static void complete(VXShaderRequest *r, id<MTLLibrary> lib, NSError *error) {
    if (lib) objc_setAssociatedObject(lib, &g_info_key, r.info, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    pthread_mutex_lock(&g_lock);
    r.library = lib; r.error = error;
    if (lib) [g_libraries setObject:r forKey:r.memoryKey];
    [g_pending removeObjectForKey:r.memoryKey];
    dispatch_group_leave(r.group);
    pthread_mutex_unlock(&g_lock);
}
id<MTLLibrary> vx_shader_cache_library(id<MTLDevice> device, NSString *source, NSArray<NSString *> *entries, NSError **error) {
    BOOL owner;
    VXShaderRequest *r = request(device, source, entries, &owner);
    if (!r) { count(&g_source_compiles); return [device newLibraryWithSource:source options:nil error:error]; }
    if (owner) {
        NSError *e = nil;
        id<MTLLibrary> lib = load_library(device, r.info);
        if (!lib) { count(&g_source_compiles); lib = [device newLibraryWithSource:source options:compile_options(r.info) error:&e]; }
        complete(r, lib, e);
    } else dispatch_group_wait(r.group, DISPATCH_TIME_FOREVER);
    if (error) *error = r.error;
    return r.library;
}
void vx_shader_cache_library_async(id<MTLDevice> device, NSString *source, NSArray<NSString *> *entries, MTLNewLibraryCompletionHandler done) {
    BOOL owner;
    VXShaderRequest *r = request(device, source, entries, &owner);
    if (!r) {
        count(&g_source_compiles);
        [device newLibraryWithSource:source options:nil completionHandler:done];
        return;
    }
    dispatch_group_notify(r.group, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ done(r.library, r.error); });
    if (owner) dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool {
        id<MTLLibrary> lib = load_library(device, r.info);
        if (lib) complete(r, lib, nil);
        else {
            count(&g_source_compiles);
            [device newLibraryWithSource:source options:compile_options(r.info) completionHandler:^(id<MTLLibrary> result, NSError *e) { complete(r, result, e); }];
        }
    } });
}
id<MTLFunction> vx_metal_function(id<MTLLibrary> library, NSString *name) {
    VXShaderInfo *info = objc_getAssociatedObject(library, &g_info_key);
    id<MTLFunction> function = [library newFunctionWithName:info ? symbol(info, name) : name];
    if (function && info) objc_setAssociatedObject(function, &g_info_key, info, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return function;
}

// Only scalar descriptor state and the source-derived function names enter keys. Metal additionally checks the
// exact native descriptor with FailOnBinaryArchiveMiss, so an omitted future field cannot select wrong GPU code.
static NSArray *vertex_layout(MTLVertexDescriptor *v) {
    if (!v) return @[];
    NSMutableArray *a = [NSMutableArray new];
    for (int i = 0; i < 31; i++) {
        [a addObject:@[@(v.attributes[i].format), @(v.attributes[i].offset), @(v.attributes[i].bufferIndex),
                      @(v.layouts[i].stride), @(v.layouts[i].stepFunction), @(v.layouts[i].stepRate)]];
    }
    return a;
}
static NSArray *stage_layout(MTLStageInputOutputDescriptor *s) {
    if (!s) return @[];
    NSMutableArray *a = [NSMutableArray arrayWithObject:@[@(s.indexType), @(s.indexBufferIndex)]];
    for (int i = 0; i < 31; i++) {
        [a addObject:@[@(s.attributes[i].format), @(s.attributes[i].offset), @(s.attributes[i].bufferIndex),
                      @(s.layouts[i].stride), @(s.layouts[i].stepFunction), @(s.layouts[i].stepRate)]];
    }
    return a;
}
static NSString *render_key(MTLRenderPipelineDescriptor *d) {
    NSMutableArray *a = [NSMutableArray arrayWithArray:@[@"render", d.vertexFunction.name ?: @"", d.fragmentFunction.name ?: @"",
        @(d.rasterSampleCount), @(d.alphaToCoverageEnabled), @(d.alphaToOneEnabled), @(d.rasterizationEnabled),
        @(d.depthAttachmentPixelFormat), @(d.stencilAttachmentPixelFormat), @(d.inputPrimitiveTopology),
        @(d.supportIndirectCommandBuffers), vertex_layout(d.vertexDescriptor)]];
    for (int i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
        [a addObject:@[@(c.pixelFormat), @(c.blendingEnabled), @(c.sourceRGBBlendFactor), @(c.destinationRGBBlendFactor),
                      @(c.rgbBlendOperation), @(c.sourceAlphaBlendFactor), @(c.destinationAlphaBlendFactor), @(c.alphaBlendOperation), @(c.writeMask)]];
    }
    for (int i = 0; i < 31; i++) [a addObject:@[@(d.vertexBuffers[i].mutability), @(d.fragmentBuffers[i].mutability)]];
    return hash_object(a);
}
static NSString *compute_key(MTLComputePipelineDescriptor *d) {
    NSMutableArray *a = [NSMutableArray arrayWithArray:@[@"compute", d.computeFunction.name ?: @"", @(d.threadGroupSizeIsMultipleOfThreadExecutionWidth),
        @(d.maxTotalThreadsPerThreadgroup), @(d.supportIndirectCommandBuffers), stage_layout(d.stageInputDescriptor)]];
    for (int i = 0; i < 31; i++) [a addObject:@(d.buffers[i].mutability)];
    return hash_object(a);
}
static id<MTLBinaryArchive> open_archive(id<MTLDevice> device, NSString *path) {
    MTLBinaryArchiveDescriptor *d = [MTLBinaryArchiveDescriptor new];
    if (path) d.url = [NSURL fileURLWithPath:path];
    id<MTLBinaryArchive> archive = [device newBinaryArchiveWithDescriptor:d error:nil];
    if (!archive) count(&g_errors);
    return archive;
}
static void publish_indexes(NSString *path, NSArray<VXShaderInfo *> *infos) {
    for (VXShaderInfo *info in infos) {
        NSString *file = index_path(info);
        if ([[NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:nil] isEqual:path.lastPathComponent]) continue;
        if (![path.lastPathComponent writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:nil]) count(&g_errors);
    }
}
static void save_archive(id<MTLBinaryArchive> archive, NSString *path, NSArray<VXShaderInfo *> *infos) {
    dispatch_async(g_writes, ^{ @autoreleasepool {
        NSString *temp = [path stringByAppendingFormat:@".%@.tmp", NSUUID.UUID.UUIDString];
        if ([archive serializeToURL:[NSURL fileURLWithPath:temp] error:nil] && !rename(temp.fileSystemRepresentation, path.fileSystemRepresentation)) {
            count(&g_saved);
            publish_indexes(path, infos);
        } else count(&g_errors);
        [NSFileManager.defaultManager removeItemAtPath:temp error:nil];
    } });
}
id<MTLRenderPipelineState> vx_shader_cache_render(id<MTLDevice> device, MTLRenderPipelineDescriptor *desc, NSError **error) {
    VXShaderInfo *vs = objc_getAssociatedObject(desc.vertexFunction, &g_info_key), *ps = objc_getAssociatedObject(desc.fragmentFunction, &g_info_key);
    if (!vs || (desc.fragmentFunction && !ps) || desc.binaryArchives.count) return [device newRenderPipelineStateWithDescriptor:desc error:error];
    MTLRenderPipelineDescriptor *d = [desc copy];
    NSString *path = [vs.root stringByAppendingPathComponent:[render_key(d) stringByAppendingString:@".metallib"]];
    NSArray *infos = ps ? @[vs, ps] : @[vs];
    id<MTLBinaryArchive> archive = [NSFileManager.defaultManager fileExistsAtPath:path] ? open_archive(device, path) : nil;
    if (archive) {
        d.binaryArchives = @[archive];
        id<MTLRenderPipelineState> hit = [device newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
        if (hit) {
            count(&g_pipeline_hits); if (error) *error = nil;
            dispatch_async(g_writes, ^{ @autoreleasepool { publish_indexes(path, infos); } });
            return hit;
        }
    }
    count(&g_pipeline_misses);
    d.binaryArchives = nil;
    archive = open_archive(device, nil);
    BOOL added = archive && [archive addRenderPipelineFunctionsWithDescriptor:d error:nil];
    if (added) d.binaryArchives = @[archive];
    id<MTLRenderPipelineState> result = [device newRenderPipelineStateWithDescriptor:d error:error];
    if (result && added) save_archive(archive, path, infos);
    return result;
}
id<MTLComputePipelineState> vx_shader_cache_compute_function(id<MTLDevice> device, id<MTLFunction> function, NSError **error) {
    if (!objc_getAssociatedObject(function, &g_info_key)) return [device newComputePipelineStateWithFunction:function error:error];
    MTLComputePipelineDescriptor *desc = [MTLComputePipelineDescriptor new];
    desc.computeFunction = function;
    return vx_shader_cache_compute(device, desc, error);
}
id<MTLComputePipelineState> vx_shader_cache_compute(id<MTLDevice> device, MTLComputePipelineDescriptor *desc, NSError **error) {
    VXShaderInfo *info = objc_getAssociatedObject(desc.computeFunction, &g_info_key);
    if (!info || desc.binaryArchives.count) return [device newComputePipelineStateWithDescriptor:desc options:0 reflection:nil error:error];
    MTLComputePipelineDescriptor *d = [desc copy];
    NSString *path = [info.root stringByAppendingPathComponent:[compute_key(d) stringByAppendingString:@".metallib"]];
    id<MTLBinaryArchive> archive = [NSFileManager.defaultManager fileExistsAtPath:path] ? open_archive(device, path) : nil;
    if (archive) {
        d.binaryArchives = @[archive];
        id<MTLComputePipelineState> hit = [device newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
        if (hit) {
            count(&g_pipeline_hits); if (error) *error = nil;
            dispatch_async(g_writes, ^{ @autoreleasepool { publish_indexes(path, @[info]); } });
            return hit;
        }
    }
    count(&g_pipeline_misses);
    d.binaryArchives = nil;
    archive = open_archive(device, nil);
    BOOL added = archive && [archive addComputePipelineFunctionsWithDescriptor:d error:nil];
    if (added) d.binaryArchives = @[archive];
    id<MTLComputePipelineState> result = [device newComputePipelineStateWithDescriptor:d options:0 reflection:nil error:error];
    if (result && added) save_archive(archive, path, @[info]);
    return result;
}
NSDictionary *vx_shader_cache_snapshot(void) {
    return @{ @"library_memory_hits": @(value(&g_memory_hits)), @"library_disk_hits": @(value(&g_disk_hits)),
              @"source_compiles": @(value(&g_source_compiles)), @"pipeline_hits": @(value(&g_pipeline_hits)),
              @"pipeline_misses": @(value(&g_pipeline_misses)), @"archives_saved": @(value(&g_saved)), @"errors": @(value(&g_errors)) };
}
void vx_shader_cache_flush(void) {
    initialize();
    dispatch_sync(g_writes, ^{});
}
