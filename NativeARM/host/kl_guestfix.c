// Byte-checked repairs of guest code, applied the moment an image is
// registered (Klepton's kl_image_loaded_hook). Each fix names its image, its
// offset and the exact instructions it expects; anything else is left alone
// and reported, so a different game build is never patched blindly.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include "klepton.h"
#include "kl_guestfix.h"
#include "kl_fault.h"

typedef struct {
    const char *image;          // basename
    uint32_t offset;            // of the first word
    unsigned n;
    uint32_t expect[8], replace[8];
    const char *what;
} fix_t;

static const fix_t g_fixes[] = {
    // libvideo: the video decoder's profiling-scope exit (a CVProfNode-style
    // ExitScope) reads node->recursion at +0x48 with no check. The NULL node
    // came from a wrong tgkill answer (fixed in kl_glibc.c: the profiler
    // registry freed live threads' objects); kept as a guard. The
    // prologue is rebuilt to return when the node is NULL: the frame-pointer
    // move is dropped to make room (the body never reads x29), and NULL jumps
    // to the function's own epilogue at +0xcc (returns 0, "not finished").
    { "libvideo.so", 0x1180a0, 7,
      { 0xa9be7bfd, 0x910003fd, 0xf9000bf3, 0xaa0003f3, 0xb9404801, 0x7100003f, 0x5400040d },
      { 0xa9be7bfd, 0xf9000bf3, 0xaa0003f3, 0xb4000600, 0xb9404801, 0x7100003f, 0x5400040d },
      "video decoder profiling scope exit tolerates a NULL node" },
};

static int write_code(uint32_t *at, const uint32_t *words, unsigned n) {
    uintptr_t page = (uintptr_t)at & ~(uintptr_t)0x3fff;
    uintptr_t end = ((uintptr_t)(at + n) + 0x3fff) & ~(uintptr_t)0x3fff;
    if (mprotect((void *)page, end - page, PROT_READ | PROT_WRITE) != 0) return -1;
    memcpy(at, words, n * 4);
    mprotect((void *)page, end - page, PROT_READ | PROT_EXEC);
    sys_icache_invalidate(at, n * 4);
    return 0;
}

static void on_image(const char *path, kl_image *img) {
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    for (size_t i = 0; i < sizeof g_fixes / sizeof g_fixes[0]; i++) {
        const fix_t *f = &g_fixes[i];
        if (strcmp(base, f->image)) continue;
        uint32_t *at = (uint32_t *)((uint8_t *)kl_base(img) + f->offset);
        if (!memcmp(at, f->replace, f->n * 4)) continue;          // already applied
        if (memcmp(at, f->expect, f->n * 4)) {
            fprintf(stderr, "  [guestfix] %s+%#x: instructions differ (other build?) — not applied: %s\n", f->image, f->offset, f->what);
            continue;
        }
        if (write_code(at, f->replace, f->n) == 0)
            fprintf(stderr, "  [guestfix] %s+%#x: %s\n", f->image, f->offset, f->what);
        else
            fprintf(stderr, "  [guestfix] %s+%#x: could not write code\n", f->image, f->offset);
    }
}

// Diagnostic (KL_GUESTFIX_DIAG=1): libvideo's two thread-specific keys at a fault.
static void video_key_report(FILE *f) {
    kl_image *v = kl_find_image("libvideo.so");
    if (!v) return;
    uint8_t *b = kl_base(v);
    const uint32_t *thr = (const uint32_t *)(b + 0x64bec8), *prof = (const uint32_t *)(b + 0x650120);
    fprintf(f, "-- libvideo keys: thread key %u valid %u, profile key %u valid %u, profile flag %u --\n",
            thr[0], ((const uint8_t *)thr)[4], prof[0], ((const uint8_t *)prof)[4], *(const uint32_t *)(b + 0x650118));
}

void kl_guestfix_install(void) {
    kl_image_loaded_hook = on_image;
    const char *d = getenv("KL_GUESTFIX_DIAG");
    if (d && *d == '1') kl_fault_add_reporter(video_key_report);
}
