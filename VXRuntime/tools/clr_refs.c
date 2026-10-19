// Lists the framework members a managed program (and the managed DLLs beside it) calls: every MemberRef whose type
// comes from an assembly that is not among the files given, as "assembly  Namespace.Type::member". Used to see what
// the CLR host (src/dotnet_runtime.c) must provide before running a program, instead of finding out one crash at a time.
//   build/clr_refs EDLaunch.exe LauncherModel.dll ...   (all the program's own assemblies: their names are "ours")
#include "dotnet_metadata.h"
#include "pe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return b;
}

static int ours(char **names, int n, const char *assembly) {
    for (int i = 0; i < n; i++) if (!strcasecmp(names[i], assembly)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    char *names[256];
    int n = 0;
    for (int i = 1; i < argc && n < 256; i++) {            // the assembly name of a file is its file name without the extension
        const char *base = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        names[n] = strdup(base);
        char *dot = strrchr(names[n], '.');
        if (dot) *dot = 0;
        n++;
    }
    for (int i = 1; i < argc; i++) {
        size_t size;
        uint8_t *image = slurp(argv[i], &size);
        vx_pe_info pe;
        vx_dotnet_metadata md;
        if (!image || !vx_pe_inspect(image, size, &pe) || !vx_dotnet_metadata_open(image, size, &pe, &md)) {
            fprintf(stderr, "%s: not a managed image\n", argv[i]);
            continue;
        }
        for (uint32_t row = 1; row <= md.row_count[0x0A]; row++) {
            vx_dotnet_member_ref ref;
            if (!vx_dotnet_member_ref_get(&md, 0x0A000000u | row, &ref)) continue;
            if ((ref.parent_token >> 24) != 0x01) continue;           // a TypeRef (not a type spec or method def)
            uint32_t assembly_ref;
            const char *assembly = "?";
            if (vx_dotnet_type_assembly_ref(&md, ref.parent_token, &assembly_ref))
                vx_dotnet_assembly_ref_name(&md, assembly_ref, &assembly);
            if (ours(names, n, assembly)) continue;
            printf("%s\t%s.%s::%s\n", assembly, ref.parent_namespace ? ref.parent_namespace : "", ref.parent_name, ref.name);
        }
        free(image);
    }
    return 0;
}
