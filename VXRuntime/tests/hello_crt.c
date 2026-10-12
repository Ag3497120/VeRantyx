// A normal MinGW program: C runtime startup, argv, printf, malloc, a static constructor.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int constructed;
__attribute__((constructor)) static void init(void) { constructed = 42; }
int main(int argc, char **argv) {
    char *buf = malloc(64);
    snprintf(buf, 64, "argc=%d argv[0]=%s", argc, argc > 0 ? argv[0] : "?");
    printf("hello from the C runtime: %s, constructor=%d, pi=%.3f, hex=%#x\n", buf, constructed, 3.14159, 255);
    free(buf);
    return constructed == 42 ? 0 : 1;
}
