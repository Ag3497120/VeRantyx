#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "gpu_ir_msl.h"
#include "spirv_ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_file(const char *path, void **bytes, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 0; }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return 0; }
    void *data = length ? malloc((size_t)length) : NULL;
    if (length && !data) { fclose(file); return 0; }
    size_t actual = length ? fread(data, 1, (size_t)length, file) : 0;
    int ok = actual == (size_t)length && !ferror(file);
    fclose(file);
    if (!ok) { free(data); return 0; }
    *bytes = data;
    *size = (size_t)length;
    return 1;
}

static int compile_msl(const char *msl, size_t length) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { fprintf(stderr, "Metal device is unavailable\n"); return 0; }
        NSString *source = [[NSString alloc] initWithBytes:msl length:length encoding:NSUTF8StringEncoding];
        if (!source) { fprintf(stderr, "emitted MSL is not UTF-8\n"); return 0; }
        NSError *error = nil;
        if (![device newLibraryWithSource:source options:nil error:&error]) {
            fprintf(stderr, "Metal compilation failed: %s\n", error.localizedDescription.UTF8String ?: "unknown error");
            return 0;
        }
        return 1;
    }
}

int main(int argc, char **argv) {
    int compile = 0;
    const char *path = NULL, *entry_name = "main";
    int entry_set = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--compile")) compile = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\nusage: spirv2msl [--compile] shader.spv [compute-entry]\n", argv[i]);
            return 2;
        } else if (!path) path = argv[i];
        else if (!entry_set) { entry_name = argv[i]; entry_set = 1; }
        else {
            fprintf(stderr, "usage: spirv2msl [--compile] shader.spv [compute-entry]\n");
            return 2;
        }
    }
    if (!path) {
        fprintf(stderr, "usage: spirv2msl [--compile] shader.spv [compute-entry]\n");
        return 2;
    }

    void *bytes = NULL;
    size_t size = 0;
    if (!read_file(path, &bytes, &size)) {
        fprintf(stderr, "cannot read SPIR-V module: %s\n", path);
        return 2;
    }
    spirv_module module = { 0 };
    int rc = spirv_read(bytes, size, &module);
    free(bytes);
    if (rc != SPIRV_OK) {
        fprintf(stderr, "SPIR-V reader rejected %s (%d): %s\n", path, rc, module.error);
        spirv_free(&module);
        return 3;
    }
    const spirv_entry_point *entry = spirv_find_entry_point(&module, entry_name,
        SPIRV_EXECUTION_MODEL_GL_COMPUTE);
    if (!entry) {
        fprintf(stderr, "compute entry point not found: %s\n", entry_name);
        spirv_free(&module);
        return 4;
    }
    gpu_ir_shader *unresolved_shader = NULL, *shader = NULL;
    rc = spirv_build_gpu_ir_shader(&module, entry, &unresolved_shader);
    uint32_t local_size[3] = { 0, 0, 0 };
    if (rc == GPU_IR_OK) {
        if (entry->local_size_kind == SPIRV_LOCAL_SIZE_LITERAL) {
            memcpy(local_size, entry->local_size, sizeof local_size);
        } else if (entry->local_size_kind == SPIRV_LOCAL_SIZE_IDS && entry->has_local_size_default) {
            for (uint32_t axis = 0; axis < 3; axis++) {
                if (!entry->local_size_default[axis] || entry->local_size_default[axis] > UINT32_MAX) {
                    rc = GPU_IR_UNSUPPORTED;
                    break;
                }
                local_size[axis] = (uint32_t)entry->local_size_default[axis];
            }
        } else {
            rc = GPU_IR_UNSUPPORTED;
        }
    }
    if (rc == GPU_IR_OK)
        rc = gpu_ir_shader_specialize(unresolved_shader, NULL, 0, local_size, &shader);
    gpu_ir_shader_release(unresolved_shader);
    if (rc != GPU_IR_OK || !shader) {
        fprintf(stderr, "SPIR-V to common IR lowering failed (%d)\n", rc);
        spirv_free(&module);
        return 5;
    }
    char *msl = NULL;
    size_t msl_size = 0;
    rc = gpu_ir_msl_emit_compute(shader, &msl, &msl_size);
    if (rc != GPU_IR_OK) {
        fprintf(stderr, "common IR to MSL emission failed (%d)\n", rc);
        gpu_ir_shader_release(shader);
        spirv_free(&module);
        return 6;
    }
    if (compile && !compile_msl(msl, msl_size)) {
        free(msl);
        gpu_ir_shader_release(shader);
        spirv_free(&module);
        return 7;
    }
    int output_ok = fwrite(msl, 1, msl_size, stdout) == msl_size && !ferror(stdout);
    if (!output_ok) fprintf(stderr, "could not write MSL output\n");
    free(msl);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return output_ok ? 0 : 2;
}
