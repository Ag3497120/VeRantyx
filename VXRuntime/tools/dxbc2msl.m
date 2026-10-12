// Translates a DXBC file to Metal Shading Language and compiles it with Metal, to check the translator:
//   build/dxbc2msl shader.dxbc
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/dxbc.h"
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: dxbc2msl file.dxbc\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    static unsigned char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    dxbc_shader sh;
    if (dxbc_translate(buf, n, &sh)) { fprintf(stderr, "%s: %s\n", argv[1], sh.error); return 1; }
    printf("%s", sh.msl);
    NSError *err = nil;
    id<MTLLibrary> lib = [MTLCreateSystemDefaultDevice() newLibraryWithSource:@(sh.msl) options:nil error:&err];
    if (!lib) { fprintf(stderr, "%s: Metal: %s\n", argv[1], err.localizedDescription.UTF8String); return 1; }
    fprintf(stderr, "%s: translated and compiled\n", argv[1]);
    return 0;
}
