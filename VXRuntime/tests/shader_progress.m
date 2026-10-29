#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdarg.h>
#include "../src/vx.h"
#include "../src/shader_progress.h"

vx_process vx_proc;
void vx_log(const char *format, ...) { (void)format; }

// Hold asynchronous completions to check a nonempty queue without relying on compiler timing.
@interface TestCompiler : NSObject
@property NSMutableArray *callbacks;
@end
@implementation TestCompiler
- (instancetype)init { if ((self = [super init])) _callbacks = [NSMutableArray new]; return self; }
- (id)newLibraryWithSource:(NSString *)source options:(id)options error:(NSError **)error {
    return [source isEqual:@"bad"] ? nil : [NSObject new];
}
- (void)newLibraryWithSource:(NSString *)source options:(id)options completionHandler:(MTLNewLibraryCompletionHandler)done {
    [_callbacks addObject:[done copy]];
}
- (id)newComputePipelineStateWithFunction:(id)function error:(NSError **)error { return function ? [NSObject new] : nil; }
- (id)newComputePipelineStateWithDescriptor:(id)desc options:(NSUInteger)options reflection:(id)reflection error:(NSError **)error { return [NSObject new]; }
- (id)newRenderPipelineStateWithDescriptor:(id)desc error:(NSError **)error { return [NSObject new]; }
@end

int main(int argc, char **argv) {
    @autoreleasepool {
        char *game[] = { argc > 1 ? argv[1] : "game.exe", NULL };
        vx_proc.argv = game;
        TestCompiler *compiler = [TestCompiler new];
        id<MTLDevice> device = (id)compiler;
        __block int finished = 0;
        for (int i = 0; i < 2; i++) vx_metal_library_async(device, @"same source", ^(id<MTLLibrary> lib, NSError *error) { finished++; });
        NSDictionary *s = vx_shader_progress_snapshot();
        assert([s[@"libraries"][@"pending"] intValue] == 2 && [s[@"unique_sources"] intValue] == 1);
        MTLNewLibraryCompletionHandler first = compiler.callbacks[0], second = compiler.callbacks[1];
        first((id)[NSObject new], nil);
        second(nil, [NSError errorWithDomain:@"test" code:1 userInfo:nil]);
        assert(finished == 2);
        NSError *error = nil;
        assert(!vx_metal_library(device, @"bad", &error));
        dispatch_apply(1000, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^(size_t i) {
            @autoreleasepool { assert(vx_metal_library(device, @"same source", NULL)); }
        });
        assert(vx_metal_compute_pipeline(device, (id)[NSObject new], &error));
        assert(!vx_metal_compute_pipeline(device, nil, &error));
        assert(vx_metal_compute_descriptor(device, [MTLComputePipelineDescriptor new], &error));
        assert(vx_metal_render_pipeline(device, [MTLRenderPipelineDescriptor new], &error));
        vx_shader_translation_finished(1000000, 1);
        vx_shader_translation_finished(2000000, 0);
        vx_shader_wait_finished(4000000);
        s = vx_shader_progress_snapshot();
        assert([s[@"libraries"][@"requested"] intValue] == 1003);
        assert([s[@"libraries"][@"completed"] intValue] == 1003);
        assert([s[@"libraries"][@"failed"] intValue] == 2);
        assert([s[@"libraries"][@"pending"] intValue] == 0 && [s[@"unique_sources"] intValue] == 2);
        assert([s[@"pipelines"][@"requested"] intValue] == 4 && [s[@"pipelines"][@"failed"] intValue] == 1);
        assert([s[@"pipelines"][@"pending"] intValue] == 0);
        assert([s[@"translations"] intValue] == 2 && [s[@"translation_failed"] intValue] == 1);
        assert([s[@"translate_ms"] doubleValue] == 3 && [s[@"shader_wait_ms"] doubleValue] == 4);
        vx_shader_progress_flush();
        puts("shader progress: async queue, duplicate sources, concurrent completions, failures, pipelines and waits -> ok");
    }
}
