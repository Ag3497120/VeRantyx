// Prints a DXIL container's module as src/dxil.c reads it: tools/dxildump <file.dxil>
#include "../src/dxil.h"
#include <stdio.h>
#include <stdlib.h>

static void line(void *ctx, const char *s) { (void)ctx; puts(s); }

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: dxildump <file.dxil>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc(n);
    fread(data, 1, n, f);
    fclose(f);
    dxil_module m;
    if (dxil_read(data, n, &m)) { fprintf(stderr, "%s: %s\n", argv[1], m.error); return 1; }
    dxil_dump(&m, line, NULL);
    dxil_free(&m);
    free(data);
    return 0;
}
