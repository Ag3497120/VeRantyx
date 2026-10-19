#include "vx.h"
#include "pe.h"
#include "dotnet_metadata.h"
#include "dotnet_exec.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *path;
    uint8_t *image;
    size_t image_size;
    vx_pe_info pe;
    vx_dotnet_metadata metadata;
} managed_image;

static managed_image *g_pending_image;

static WINAPI int cor_exe_main(void) {
    managed_image *program = g_pending_image;
    if (!program) {
        vx_log("mscoree.dll!_CorExeMain called without a managed entry image");
        return 193;
    }
    vx_dotnet_method method;
    if (!vx_dotnet_method_get(&program->metadata, program->metadata.entry_point_token, &method)) {
        vx_log("%s: CLR entry token %08x is not a MethodDef", program->path,
               program->metadata.entry_point_token);
        return 193;
    }
    int64_t result = 0;
    char error[256];
    char assembly_directory[4096];
    const char *separator = strrchr(program->path, '/');
    size_t directory_length = separator ? (size_t)(separator - program->path) : 1;
    if (!separator) {
        assembly_directory[0] = '.';
        assembly_directory[1] = 0;
    } else if (directory_length < sizeof assembly_directory) {
        memcpy(assembly_directory, program->path, directory_length);
        assembly_directory[directory_length] = 0;
    } else {
        assembly_directory[0] = 0;
    }
    vx_dotnet_set_export_resolver(vx_find_export);
    int executed = vx_dotnet_execute_entry_from_file(&program->metadata, method.token,
                                                     assembly_directory[0] ? assembly_directory : NULL,
                                                     program->path, &result, error, sizeof error);
    vx_dotnet_set_export_resolver(NULL);
    if (!executed) {
        vx_log("%s: CLR %s.%s: %s", program->path,
               method.declaring_type_name ? method.declaring_type_name : "?", method.name, error);
        return 193;
    }
    vx_log("%s: CLR entry method %s.%s returned %lld", program->path,
           method.declaring_type_name ? method.declaring_type_name : "?", method.name, (long long)result);
    return (int)result;
}

static const vx_export g_mscoree_exports[] = {
    { "_CorExeMain", (void *)cor_exe_main },
    { "CorExeMain", (void *)cor_exe_main }
};

void vx_dotnet_init(void) {
    vx_register_module("mscoree.dll", g_mscoree_exports,
                       sizeof g_mscoree_exports / sizeof g_mscoree_exports[0]);
}

// Returns -1 for a non-managed image so the native PE loader can handle it.
// A CLI image runs only through the built-in mscoree entry point above.
int vx_dotnet_run_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return -1; }
    long end = ftell(f);
    if (end <= 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return -1; }
    uint8_t *bytes = malloc((size_t)end);
    if (!bytes || fread(bytes, 1, (size_t)end, f) != (size_t)end) {
        free(bytes); fclose(f); return -1;
    }
    fclose(f);

    managed_image program = { .path = path, .image = bytes, .image_size = (size_t)end };
    if (!vx_pe_inspect(bytes, program.image_size, &program.pe) || !program.pe.has_cli) {
        free(bytes);
        return -1;
    }
    if (!vx_dotnet_metadata_open(bytes, program.image_size, &program.pe, &program.metadata)) {
        vx_log("%s: invalid or unsupported ECMA-335 metadata", path);
        free(bytes);
        return 193;
    }
    if (program.pe.clr_flags & 0x10) {
        vx_log("%s: CLR image uses a native entry point; native mixed-mode execution is not implemented", path);
        free(bytes);
        return 193;
    }
    if (!program.metadata.entry_point_token) {
        vx_log("%s: CLR image has no entry point", path);
        free(bytes);
        return 193;
    }
    if (g_pending_image) {
        vx_log("%s: nested CLR entry startup is not implemented", path);
        free(bytes);
        return 193;
    }

    g_pending_image = &program;
    int (WINAPI *host)(void) = (int (WINAPI *)(void))vx_find_export("mscoree.dll", "_CorExeMain");
    int result = host ? host() : 193;
    if (!host) vx_log("mscoree.dll!_CorExeMain is not registered");
    g_pending_image = NULL;
    free(bytes);
    return result;
}
