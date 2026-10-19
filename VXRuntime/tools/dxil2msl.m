// Translates DXIL containers to Metal Shading Language (src/dxil_msl.c) and compiles each with Metal:
//   build/dxil2msl [-p] file.dxil...      (-p: print the MSL)
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/dxil_msl.h"

int main(int argc, char **argv) {
    int print = 0, bad = 0, n = 0, compiled = 0, skipped = 0;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p")) { print = 1; continue; }
        FILE *f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); bad++; continue; }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *data = malloc(size);
        fread(data, 1, size, f);
        fclose(f);
        n++;
        dxil_module m;
        dxil_msl out;
        if (dxil_read(data, size, &m)) { printf("%s: %s\n", argv[i], m.error); bad++; free(data); continue; }
        if (dxil_to_msl(&m, &out)) { printf("%s: %s\n", argv[i], out.error); bad++; dxil_free(&m); free(data); continue; }
        if (print) printf("%s", out.msl);
        MTLCompileOptions *opt = [MTLCompileOptions new];
        if (out.uses_thread_fence) {
            if (@available(macOS 15.0, *)) opt.languageVersion = MTLLanguageVersion3_2;
            else {
                printf("%s: translated; Metal compile skipped (shader fences require MSL 3.2)\n", argv[i]);
                skipped++;
                dxil_msl_free(&out);
                dxil_free(&m);
                free(data);
                continue;
            }
        } else opt.languageVersion = MTLLanguageVersion3_1;
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(out.msl) options:opt error:&err];
        if (!lib) { printf("%s: Metal: %s\n", argv[i], err.localizedDescription.UTF8String); bad++; }
        else {
            compiled++;
            printf("%s: translated (%s) and compiled; %u resource ranges%s\n", argv[i], out.structured ? "structured" : "state machine",
                   out.nslots, out.uses_heap ? ", the descriptor heap" : "");
        }
        dxil_msl_free(&out);
        dxil_free(&m);
        free(data);
    }
    printf("dxil2msl: %d/%d shaders translated; %d compiled, %d skipped (MSL 3.2 unavailable), %d failed -> %s\n",
           n - bad, n, compiled, skipped, bad, bad || !n ? "FAILED" : "ok");
    return bad || !n ? 1 : 0;
}
