// vkmsl: SPIR-V → MSL with VXRuntime's Vulkan translator (src/vk_msl.c), for looking at one shader or scanning many.
//   vkmsl [--compile] [--scan] [--stage vertex|fragment|compute] shader.spv
// The descriptor layout is made up from the shader's own bindings (each set's bindings packed in binding order;
// unsized arrays get 16 descriptors). --scan prints one line: "OK <stage>" or "FAIL <stage> <step> <reason>".
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "vk_msl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compile_msl(const char *msl, size_t length, char *error, size_t cap) {
    @autoreleasepool {
        static id<MTLDevice> device;
        if (!device) device = MTLCreateSystemDefaultDevice();
        NSString *source = [[NSString alloc] initWithBytes:msl length:length encoding:NSUTF8StringEncoding];
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion3_1;
        NSError *e = nil;
        if ([device newLibraryWithSource:source options:options error:&e]) return 1;
        snprintf(error, cap, "%s", e.localizedDescription.UTF8String ?: "?");
        return 0;
    }
}

static int binding_order(const void *a, const void *b) {
    const vkmsl_binding *x = a, *y = b;
    if (x->set != y->set) return x->set < y->set ? -1 : 1;
    return x->binding < y->binding ? -1 : x->binding > y->binding;
}

int main(int argc, char **argv) {
    int compile = 0, scan = 0;
    int stage = -1;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--compile")) compile = 1;
        else if (!strcmp(argv[i], "--scan")) scan = 1;
        else if (!strcmp(argv[i], "--stage") && i + 1 < argc) {
            const char *s = argv[++i];
            stage = !strcmp(s, "vertex") ? 0 : !strcmp(s, "fragment") ? 4 : !strcmp(s, "compute") ? 5 : -1;
        } else path = argv[i];
    }
    if (!path) { fprintf(stderr, "usage: vkmsl [--compile] [--scan] [--stage vertex|fragment|compute] shader.spv\n"); return 2; }
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot read %s\n", path); return 2; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *words = malloc((size_t)size + 4);
    if (!words || fread(words, 1, (size_t)size, f) != (size_t)size) { fclose(f); return 2; }
    fclose(f);
    size_t count = (size_t)size / 4;
    if (stage < 0) {
        // (the first entry point's stage)
        for (size_t pos = 5; pos < count;) {
            uint32_t op = words[pos] & 0xffff, n = words[pos] >> 16;
            if (!n) break;
            if (op == 15) { stage = (int)words[pos + 1]; break; }
            pos += n;
        }
    }
    static const char *names[] = { "vertex", "tesc", "tese", "geometry", "fragment", "compute" };
    const char *sname = stage >= 0 && stage < 6 ? names[stage] : "?";
    vkmsl_binding bindings[256];
    uint32_t n = 0;
    vkmsl_reflect(words, count, (uint32_t)stage, bindings, 256, &n);
    if (n > 256) n = 256;
    qsort(bindings, n, sizeof bindings[0], binding_order);
    uint32_t word = 0, set = ~0u, dynamic = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (bindings[i].set != set) { set = bindings[i].set; word = 0; }
        if (!bindings[i].count) bindings[i].count = 16;
        bindings[i].word = word;
        word += bindings[i].count * vkmsl_descriptor_words(bindings[i].type);
        (void)dynamic;
    }
    vkmsl_options options = { .stage = (uint32_t)stage, .bindings = bindings, .binding_count = n, .vary_slots = 16 };
    if (getenv("VKMSL_VERTEX_KERNEL")) options.flags |= VKMSL_FLAG_VERTEX_KERNEL;
    vkmsl_result result;
    int rc = vkmsl_translate(words, count, &options, &result);
    if (rc) {
        if (scan) printf("FAIL %s translate %s\n", sname, result.error);
        else fprintf(stderr, "translation failed (%d): %s\n", rc, result.error);
        return 3;
    }
    if (compile) {
        char error[4096];
        if (!compile_msl(result.msl, result.msl_size, error, sizeof error)) {
            if (scan) {
                // (the first error line)
                char *e = strstr(error, "error: ");
                char *nl = e ? strchr(e, '\n') : NULL;
                if (nl) *nl = 0;
                printf("FAIL %s metal %s\n", sname, e ? e + 7 : error);
            } else fprintf(stderr, "%s\n", error);
            if (!scan) fwrite(result.msl, 1, result.msl_size, stdout);
            return 4;
        }
    }
    if (scan) printf("OK %s\n", sname);
    else fwrite(result.msl, 1, result.msl_size, stdout);
    vkmsl_result_free(&result);
    free(words);
    return 0;
}
