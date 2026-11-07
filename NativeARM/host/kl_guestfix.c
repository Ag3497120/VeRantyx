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

// A fix names its image and offset, the FNV-1a hash of the original words it
// expects there (the game's code itself is not reproduced here), and how to
// build the replacement: each entry is either an index into the original words
// (KEEP(i): the game's instruction, moved) or a new instruction of ours (OURS).
#define KEEP(i) (0x80000000u | (uint32_t)(i))
#define OURS(w) ((uint32_t)(w) & 0x7fffffffu)
typedef struct {
    const char *image;          // basename
    uint32_t offset;            // of the first word
    unsigned n;
    uint64_t expect_fnv1a;      // hash of the n original words
    uint32_t build[8];          // KEEP(i) or OURS(word); OURS words never set bit 31
    const char *what;
} fix_t;

static const fix_t g_fixes[] = {
    // Preserve the Deckard three-preset selection through the engine's own
    // archived-convar save/load path. Keep every existing flag and callback.
    { "libclient.so", 0xfab1cc, 1, 0xeec63887f56b000cull,
      { 0xd2841003u },
      "archive vr_perf_mode_override2 (preserve selected graphics preset)" },

    // libvideo: the video decoder's profiling-scope exit (a CVProfNode-style
    // ExitScope) reads node->recursion at +0x48 with no check. The NULL node
    // came from a wrong tgkill answer (fixed in kl_glibc.c: the profiler
    // registry freed live threads' objects); kept as a guard. The
    // prologue is rebuilt to return when the node is NULL: the frame-pointer
    // move (word 1) is dropped to make room (the body never reads x29), and
    // NULL jumps to the function's own epilogue at +0xcc
    // ("cbz x0, +0xc0" = 0xb4000600, our only new instruction).
    { "libvideo.so", 0x1180a0, 7, 0x4dd69d9ecfed2441ull,
      { KEEP(0), KEEP(2), KEEP(3), OURS(0x34000600) | 0x80000000u, KEEP(4), KEEP(5), KEEP(6) },
      "video decoder profiling scope exit tolerates a NULL node" },
};

static uint64_t fnv1a(const void *p, size_t n) {
    const uint8_t *b = p; uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

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
    // Where each guest library sits: lets a `sample` of a stalled game (the
    // launcher takes one when the frame rate stays low) be read as image+offset.
    fprintf(stderr, "  [image] %p %s\n", kl_base(img), base);
    for (size_t i = 0; i < sizeof g_fixes / sizeof g_fixes[0]; i++) {
        const fix_t *f = &g_fixes[i];
        if (strcmp(base, f->image)) continue;
        uint32_t *at = (uint32_t *)((uint8_t *)kl_base(img) + f->offset);
        if (fnv1a(at, f->n * 4) != f->expect_fnv1a) {
            fprintf(stderr, "  [guestfix] %s+%#x: instructions differ (other build?) — not applied: %s\n", f->image, f->offset, f->what);
            continue;
        }
        uint32_t words[8];
        for (unsigned k = 0; k < f->n; k++) {
            uint32_t b = f->build[k];
            // KEEP(i) has bit 31 and a small index; a new instruction is stored
            // with bit 31 forced on too, so tell them apart by the index range.
            words[k] = (b & 0x7fffffffu) < f->n ? at[b & 0x7fffffffu] : (b & 0x7fffffffu) | 0x80000000u;
        }
        if (write_code(at, words, f->n) == 0)
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
