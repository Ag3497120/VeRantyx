// Translates DXIL containers to Metal Shading Language (src/dxil_msl.c) and compiles each with Metal:
//   build/dxil2msl [-p] file.dxil...      (-p: print the MSL)
//   build/dxil2msl -vs vs.dxil gs.dxil... (geometry shaders after the vertex shader they follow: the vertex kernel,
//                                          the geometry kernel and its pass-through function are compiled)
//   build/dxil2msl -vs vs.dxil -hs hs.dxil ds.dxil   (a domain shader after its hull and vertex shaders: the vertex and
//                                          hull kernels and the post-tessellation vertex function are compiled)
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/dxil_msl.h"

static unsigned char *slurp(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc(*size);
    fread(data, 1, *size, f);
    fclose(f);
    return data;
}

static int compile(id<MTLDevice> dev, const char *what, const char *source) {
    MTLCompileOptions *opt = [MTLCompileOptions new];
    opt.languageVersion = MTLLanguageVersion3_1;
    NSError *err = nil;
    if ([dev newLibraryWithSource:@(source) options:opt error:&err]) return 1;
    printf("%s: Metal: %s\n", what, err.localizedDescription.UTF8String);
    return 0;
}

int main(int argc, char **argv) {
    int print = 0, bad = 0, n = 0, compiled = 0, skipped = 0;
    const char *vs_path = NULL, *hs_path = NULL;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p")) { print = 1; continue; }
        if (!strcmp(argv[i], "-vs") && i + 1 < argc) { vs_path = argv[++i]; continue; }
        if (!strcmp(argv[i], "-hs") && i + 1 < argc) { hs_path = argv[++i]; continue; }
        if (vs_path && hs_path) {                                  // a domain shader after its hull and vertex shaders
            long vsize = 0, hsize = 0, dsize = 0;
            unsigned char *vdata = slurp(vs_path, &vsize), *hdata = slurp(hs_path, &hsize), *ddata = slurp(argv[i], &dsize);
            dxil_module vm, hm, dm;
            dxil_msl vk, hk, dk;
            n++;
            int ok = vdata && hdata && ddata && !dxil_read(vdata, vsize, &vm);
            if (ok && dxil_read(hdata, hsize, &hm)) { printf("%s: %s\n", hs_path, hm.error); dxil_free(&vm); ok = 0; }
            if (ok && dxil_read(ddata, dsize, &dm)) { printf("%s: %s\n", argv[i], dm.error); dxil_free(&vm); dxil_free(&hm); ok = 0; }
            if (ok) {
                if (dxil_to_msl_vertex_kernel(&vm, &vk)) { printf("%s: vertex kernel: %s\n", vs_path, vk.error); ok = 0; }
                else if (dxil_to_msl_hull(&hm, &vk, &hk)) { printf("%s: %s\n", hs_path, hk.error); ok = 0; dxil_msl_free(&vk); }
                else if (dxil_to_msl_domain(&dm, &hk, &dk)) { printf("%s: %s\n", argv[i], dk.error); ok = 0; dxil_msl_free(&vk); dxil_msl_free(&hk); }
                else {
                    if (print) printf("%s\n%s\n%s", vk.msl, hk.msl, dk.msl);
                    ok = compile(dev, vs_path, vk.msl) & compile(dev, hs_path, hk.msl) & compile(dev, argv[i], dk.msl);
                    if (ok) printf("%s: hull kernel (%u in, %u out control points, %u patch constants, domain %u), vertex kernel and domain function compiled\n",
                                   argv[i], hk.tess_in_cp, hk.tess_out_cp, hk.npatch, hk.tess_domain);
                    dxil_msl_free(&vk); dxil_msl_free(&hk); dxil_msl_free(&dk);
                }
                dxil_free(&vm); dxil_free(&hm); dxil_free(&dm);
            }
            if (ok) compiled++; else bad++;
            free(vdata); free(hdata); free(ddata);
            continue;
        }
        if (vs_path) {                                             // a geometry shader after its vertex shader
            long vsize = 0, gsize = 0;
            unsigned char *vdata = slurp(vs_path, &vsize), *gdata = slurp(argv[i], &gsize);
            dxil_module vm, gm;
            dxil_msl vk, gk;
            n++;
            if (!vdata || !gdata || dxil_read(vdata, vsize, &vm)) { printf("%s: cannot read %s\n", argv[i], vs_path); bad++; free(vdata); free(gdata); continue; }
            if (dxil_read(gdata, gsize, &gm)) { printf("%s: %s\n", argv[i], gm.error); bad++; dxil_free(&vm); free(vdata); free(gdata); continue; }
            int ok = !dxil_to_msl_vertex_kernel(&vm, &vk);
            if (!ok) printf("%s: vertex kernel: %s\n", vs_path, vk.error);
            else if (dxil_to_msl_geometry(&gm, &vk, &gk)) { printf("%s: %s\n", argv[i], gk.error); ok = 0; }
            else {
                if (print) printf("%s\n%s\n%s", vk.msl, gk.msl, gk.msl_passthrough);
                ok = compile(dev, vs_path, vk.msl) & compile(dev, argv[i], gk.msl) & compile(dev, argv[i], gk.msl_passthrough);
                if (ok) printf("%s: geometry kernel (%u in, %u triangles, %u outputs), vertex kernel and pass-through compiled\n",
                               argv[i], gk.gs_in_verts, gk.gs_max_tris, gk.io_slots);
                dxil_msl_free(&gk);
            }
            if (ok) compiled++; else bad++;
            dxil_msl_free(&vk);
            dxil_free(&vm); dxil_free(&gm);
            free(vdata); free(gdata);
            continue;
        }
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
