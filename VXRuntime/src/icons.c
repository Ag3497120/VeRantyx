// Icons and cursors that carry their pictures (user32, shell32): CreateIconIndirect, CreateIconFromResource(Ex),
// LoadImage and LoadIcon from a module's resources or an .ico/.cur file, ExtractIconEx from an .exe, .dll or .ico on
// disk, CopyIcon, CopyImage, GetIconInfo, DrawIconEx, DestroyIcon, LookupIconIdFromDirectory(Ex).
//
// An icon keeps straight-alpha BGRA pixels, top-down. One loaded from an icon file or an icon group keeps the larger
// sizes of the same picture too, so that the notification area (tray.m) shows it sharp on a Retina display.
// The system's own icons (LoadIcon(NULL, IDI_*)) and cursors from resources stay the stand-in handles user32.c gave
// them before (they have no pixels here). Formats: Microsoft's "Icons" and "PE format" (.rsrc) documentation.
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wctype.h>
#include <ImageIO/ImageIO.h>
#include "ui.h"

#define ICON_BASE 0xC00000                      // below 16 MB, where the vCPU's low memory never is (vx.h)
#define MAX_ICONS 65536
enum { IMAGE_BITMAP = 0, IMAGE_ICON = 1, IMAGE_CURSOR = 2 };
enum { LR_COPYRETURNORG = 0x4, LR_COPYDELETEORG = 0x8, LR_LOADFROMFILE = 0x10, LR_DEFAULTSIZE = 0x40, LR_SHARED = 0x8000 };
enum { DI_MASK = 1, DI_IMAGE = 2, DI_DEFAULTSIZE = 8 };
enum { RT_CURSOR = 1, RT_ICON = 3, RT_GROUP_ICON = 14 };
enum { ERROR_INVALID_PARAMETER = 87, ERROR_FILE_NOT_FOUND = 2, ERROR_INVALID_CURSOR_HANDLE = 1402,
       ERROR_RESOURCE_DATA_NOT_FOUND = 1812, ERROR_RESOURCE_TYPE_NOT_FOUND = 1813, ERROR_RESOURCE_NAME_NOT_FOUND = 1814 };
#define SYSTEM_ICON_SIZE 32                     // SM_CXICON, SM_CYICON (user32.c's GetSystemMetrics)
#define SMALL_ICON_SIZE 16                      // SM_CXSMICON

typedef struct { int w, h; uint32_t *px; } image;
typedef struct {
    int is_icon, hot_x, hot_y, shared;
    image img;                                  // the picture the handle stands for
    image more[6]; int nmore;                   // larger sizes of it (from an icon file or group)
    const void *key; uintptr_t key_name; int key_w, key_h;   // LR_SHARED: what it was loaded from
} icon;

static icon *g_icons[MAX_ICONS];
static int g_icon_next, g_icon_top;             // top: past the highest slot ever used (find_shared looks below it)
static pthread_mutex_t g_icon_lock = PTHREAD_MUTEX_INITIALIZER;

static icon *get(HANDLE h) {                    // (g_icon_lock held)
    uintptr_t v = (uintptr_t)h - ICON_BASE;
    return (uintptr_t)h >= ICON_BASE && v % 4 == 0 && v / 4 < MAX_ICONS ? g_icons[v / 4] : NULL;
}
static int is_icon_handle(HANDLE h) { uintptr_t v = (uintptr_t)h - ICON_BASE; return (uintptr_t)h >= ICON_BASE && v / 4 < MAX_ICONS; }
static void free_icon(icon *ic) {
    if (!ic) return;
    free(ic->img.px);
    for (int i = 0; i < ic->nmore; i++) free(ic->more[i].px);
    free(ic);
}
static HANDLE new_icon(icon *ic) {
    if (!ic) return NULL;
    pthread_mutex_lock(&g_icon_lock);
    int i = g_icon_next;
    for (int k = 0; k < MAX_ICONS && g_icons[i]; k++) i = (i + 1) % MAX_ICONS;
    HANDLE h = NULL;
    if (!g_icons[i]) {
        g_icons[i] = ic; g_icon_next = (i + 1) % MAX_ICONS; h = (HANDLE)(uintptr_t)(ICON_BASE + i * 4);
        if (i >= g_icon_top) g_icon_top = i + 1;
    }
    pthread_mutex_unlock(&g_icon_lock);
    if (!h) { free_icon(ic); vx_set_last_error(8); }
    return h;
}
static HANDLE system_icon(uintptr_t id) { return (HANDLE)(0x20000 | (id & 0xffff)); }   // user32.c's stand-ins

// ---- pixels ---------------------------------------------------------------------------------------------------------------
static CGImageRef cg_image(const image *im) {
    CFDataRef data = CFDataCreate(NULL, (const UInt8 *)im->px, (CFIndex)im->w * im->h * 4);
    CGDataProviderRef dp = CGDataProviderCreateWithCFData(data);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGImageRef img = CGImageCreate((size_t)im->w, (size_t)im->h, 8, 32, (size_t)im->w * 4, cs, kCGImageAlphaFirst | kCGBitmapByteOrder32Little,
                                   dp, NULL, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs); CGDataProviderRelease(dp); CFRelease(data);
    return img;
}
// An image drawn at w x h (scaled smoothly); straight alpha again afterwards (bitmap contexts premultiply).
static int render(CGImageRef src, int w, int h, image *out) {
    uint32_t *px = calloc((size_t)w * h, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(px, (size_t)w, (size_t)h, 8, (size_t)w * 4, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (!c) { free(px); return 0; }
    CGContextSetInterpolationQuality(c, kCGInterpolationHigh);
    CGContextDrawImage(c, CGRectMake(0, 0, w, h), src);
    CGContextRelease(c);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t p = px[i], a = p >> 24;
        if (a && a < 255) {
            uint32_t b = (p & 0xff) * 255 / a, g = ((p >> 8) & 0xff) * 255 / a, r = ((p >> 16) & 0xff) * 255 / a;
            px[i] = a << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
        }
    }
    *out = (image){ w, h, px };
    return 1;
}
static image scaled(const image *im, int w, int h) {
    image out = { 0, 0, NULL };
    if (w == im->w && h == im->h) {
        out = *im;
        out.px = malloc((size_t)w * h * 4);
        memcpy(out.px, im->px, (size_t)w * h * 4);
        return out;
    }
    CGImageRef img = cg_image(im);
    if (img) { render(img, w, h, &out); CGImageRelease(img); }
    return out;
}

// One image of an icon (an RT_ICON resource or an .ico entry): a PNG, or a DIB whose height counts both the color
// rows and the AND mask's rows (bottom-up). A mask bit of 1 is transparent; a 32-bpp image with any alpha uses its alpha.
static int decode_image(const uint8_t *p, size_t n, image *out) {
    if (n >= 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8)) {
        CFDataRef data = CFDataCreate(NULL, p, (CFIndex)n);
        CGImageSourceRef src = CGImageSourceCreateWithData(data, NULL);
        CFRelease(data);
        CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
        if (src) CFRelease(src);
        if (!img) return 0;
        int ok = render(img, (int)CGImageGetWidth(img), (int)CGImageGetHeight(img), out);
        CGImageRelease(img);
        return ok;
    }
    if (n < 40) return 0;
    uint32_t hsize = *(const uint32_t *)p;
    int32_t w = *(const int32_t *)(p + 4), h2 = *(const int32_t *)(p + 8);
    uint16_t bpp = *(const uint16_t *)(p + 14);
    uint32_t compression = *(const uint32_t *)(p + 16), used = *(const uint32_t *)(p + 32);
    if (hsize < 40 || hsize > n || w <= 0 || w > 1024 || h2 <= 0 || h2 > 2048 || (compression != 0 && compression != 3)) return 0;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return 0;
    int h = h2 / 2;
    if (h <= 0) return 0;
    size_t colors = bpp <= 8 ? (used && used <= (1u << bpp) ? used : 1u << bpp) : 0;
    size_t masks = compression == 3 ? 12 : 0;
    size_t stride = (((size_t)w * bpp + 31) / 32) * 4, mstride = (((size_t)w + 31) / 32) * 4;
    size_t off = hsize + masks + colors * 4;
    int has_mask = off + stride * h + mstride * h <= n;               // (some writers leave the mask out of 32-bpp images)
    if (off + stride * h > n) return 0;
    const uint32_t *palette = (const uint32_t *)(p + hsize + masks);
    const uint8_t *bits = p + off, *mask = bits + stride * h;
    uint32_t *px = malloc((size_t)w * h * 4);
    int any_alpha = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = bits + stride * (size_t)(h - 1 - y);
        uint32_t *o = px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            uint32_t c;
            switch (bpp) {
            case 32: c = ((const uint32_t *)row)[x]; break;
            case 24: c = 0xff000000u | (uint32_t)row[x * 3 + 2] << 16 | (uint32_t)row[x * 3 + 1] << 8 | row[x * 3]; break;
            case 16: { uint16_t v = ((const uint16_t *)row)[x];
                       c = 0xff000000u | ((v >> 10) & 31) * 255 / 31 << 16 | ((v >> 5) & 31) * 255 / 31 << 8 | (v & 31) * 255 / 31; break; }
            default: {
                int per = 8 / bpp, idx = (row[x / per] >> ((per - 1 - x % per) * bpp)) & ((1 << bpp) - 1);
                c = ((size_t)idx < colors ? palette[idx] : 0) | 0xff000000u;
            }
            }
            if (bpp == 32 && (c >> 24)) any_alpha = 1;
            o[x] = c;
        }
    }
    if (!any_alpha)
        for (int y = 0; y < h; y++) {
            const uint8_t *mrow = has_mask ? mask + mstride * (size_t)(h - 1 - y) : NULL;
            for (int x = 0; x < w; x++) {
                int transparent = mrow && (mrow[x / 8] >> (7 - x % 8) & 1);
                px[(size_t)y * w + x] = (px[(size_t)y * w + x] & 0xffffffu) | (transparent ? 0 : 0xff000000u);
            }
        }
    *out = (image){ w, h, px };
    return 1;
}

// ---- icon directories: .ico/.cur files (entries with file offsets) and RT_GROUP_ICON resources (with RT_ICON ids) ----------
typedef struct { int w, h, bpp; uint32_t size, where; int hot_x, hot_y; } dir_entry;
static int read_directory(const uint8_t *p, size_t n, int group, dir_entry *out, int cap, int *type) {
    if (n < 6 || *(const uint16_t *)p != 0) return -1;
    int t = *(const uint16_t *)(p + 2), count = *(const uint16_t *)(p + 4), each = group ? 14 : 16;
    if ((t != 1 && t != 2) || (size_t)6 + (size_t)count * each > n) return -1;
    if (type) *type = t;
    int k = 0;
    for (int i = 0; i < count && k < cap; i++) {
        const uint8_t *e = p + 6 + i * each;
        dir_entry d = { e[0] ? e[0] : 256, e[1] ? e[1] : 256, *(const uint16_t *)(e + 6), *(const uint32_t *)(e + 8), 0, 0, 0 };
        if (group) d.where = *(const uint16_t *)(e + 12); else d.where = *(const uint32_t *)(e + 12);
        if (t == 2 && !group) { d.hot_x = *(const uint16_t *)(e + 4); d.hot_y = *(const uint16_t *)(e + 6); d.bpp = 0; }
        if (!d.bpp) { int colors = e[2]; d.bpp = colors == 2 ? 1 : colors == 16 ? 4 : colors ? 8 : 32; }
        out[k++] = d;
    }
    return k;
}
// The entry that suits cx x cy best: the same size, else the nearest larger (scaled down), else the nearest smaller;
// among those of one size the most colors.
static int best_entry(const dir_entry *e, int n, int cx, int cy) {
    int best = -1; long best_key = 0;
    for (int i = 0; i < n; i++) {
        long fit = e[i].w >= cx && e[i].h >= cy ? (e[i].w - cx) + (e[i].h - cy) : 100000 + (cx - e[i].w) + (cy - e[i].h);
        long key = fit * 64 - e[i].bpp;
        if (best < 0 || key < best_key) { best = i; best_key = key; }
    }
    return best;
}

// ---- resources of a PE image, mapped (a loaded module) or as a file ----------------------------------------------------------
typedef struct {
    const uint8_t *base; size_t size;           // the mapping or the file's bytes
    int file;                                   // RVAs go through the section table
    const uint8_t *sections; int nsec;
    const uint8_t *root; uint32_t root_size;    // the resource directory
} pe_view;
static const uint8_t *pe_at(const pe_view *v, uint32_t rva, uint32_t len) {
    uint64_t off = rva;
    if (v->file) {
        off = UINT64_MAX;
        for (int i = 0; i < v->nsec; i++) {
            const uint8_t *s = v->sections + 40 * i;
            uint32_t vsize = *(const uint32_t *)(s + 8), va = *(const uint32_t *)(s + 12), raw = *(const uint32_t *)(s + 16), rawoff = *(const uint32_t *)(s + 20);
            if (rva >= va && rva < va + (vsize > raw ? vsize : raw)) { if (rva - va + (uint64_t)len > raw) return NULL; off = (uint64_t)rawoff + rva - va; break; }
        }
    }
    return off + len <= v->size ? v->base + off : NULL;
}
static int pe_open(pe_view *v, const uint8_t *base, size_t size, int file) {
    memset(v, 0, sizeof *v);
    v->base = base; v->size = size; v->file = file;
    if (size < 0x40 || base[0] != 'M' || base[1] != 'Z') return 0;
    uint32_t pe = *(const uint32_t *)(base + 0x3c);
    if ((uint64_t)pe + 24 + 112 + 3 * 8 > size || memcmp(base + pe, "PE\0\0", 4)) return 0;
    uint16_t nsec = *(const uint16_t *)(base + pe + 6), opt_size = *(const uint16_t *)(base + pe + 20);
    const uint8_t *opt = base + pe + 24;
    int wide = *(const uint16_t *)opt == 0x20b;
    const uint8_t *dirs = opt + (wide ? 112 : 96);
    v->sections = opt + opt_size; v->nsec = nsec;
    if ((uint64_t)(v->sections - base) + 40ull * nsec > size) return 0;
    uint32_t rva = *(const uint32_t *)(dirs + 2 * 8), n = *(const uint32_t *)(dirs + 2 * 8 + 4);
    if (!rva || n < 16) return 0;
    v->root = pe_at(v, rva, n);
    v->root_size = n;
    return v->root != NULL;
}
static const uint8_t *rsrc_dir(const pe_view *v, uint32_t off) { return off + 16 <= v->root_size ? v->root + off : NULL; }
static int rsrc_count(const uint8_t *dir) { return *(const uint16_t *)(dir + 12) + *(const uint16_t *)(dir + 14); }
// A name or id as Windows takes them: an integer below 0x10000, "#123", or a string (compared without case).
static int rsrc_matches(const pe_view *v, uint32_t field, const WCHAR *want) {
    if ((uintptr_t)want < 0x10000) return !(field & 0x80000000u) && field == (uintptr_t)want;
    if (want[0] == '#') {
        uint32_t id = 0;
        for (const WCHAR *c = want + 1; *c >= '0' && *c <= '9'; c++) id = id * 10 + (uint32_t)(*c - '0');
        return !(field & 0x80000000u) && field == id;
    }
    if (!(field & 0x80000000u)) return 0;
    uint32_t at = field & 0x7fffffffu;
    if (at + 2 > v->root_size) return 0;
    const uint16_t *s = (const uint16_t *)(v->root + at);
    size_t len = s[0], wl = vx_wcslen(want);
    if (len != wl || at + 2 + 2 * len > v->root_size) return 0;
    for (size_t i = 0; i < len; i++) if (towupper(s[1 + i]) != towupper(want[i])) return 0;
    return 1;
}
// The entry of a directory: the one named `want`, or the index-th when want is (WCHAR *)-1.
static const uint32_t *rsrc_entry(const pe_view *v, const uint8_t *dir, const WCHAR *want, int index) {
    int n = rsrc_count(dir);
    if ((uint64_t)(dir - v->root) + 16 + 8ull * n > v->root_size) return NULL;
    const uint32_t *e = (const uint32_t *)(dir + 16);
    for (int i = 0; i < n; i++, e += 2)
        if (want == (const WCHAR *)(intptr_t)-1 ? i == index : rsrc_matches(v, e[0], want)) return e;
    return NULL;
}
static const uint8_t *subdir(const pe_view *v, const uint32_t *e) { return e && (e[1] & 0x80000000u) ? rsrc_dir(v, e[1] & 0x7fffffffu) : NULL; }
// The data of a resource (its first language), or NULL with the error Windows gives.
static const uint8_t *rsrc_data(const pe_view *v, const uint32_t *name_entry, uint32_t *len) {
    const uint8_t *langs = subdir(v, name_entry);
    const uint32_t *l = langs ? rsrc_entry(v, langs, (const WCHAR *)(intptr_t)-1, 0) : NULL;
    if (!l || (l[1] & 0x80000000u) || (l[1] & 0x7fffffffu) + 16 > v->root_size) { vx_set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND); return NULL; }
    const uint32_t *data = (const uint32_t *)(v->root + (l[1] & 0x7fffffffu));
    *len = data[1];
    const uint8_t *p = pe_at(v, data[0], data[1]);
    if (!p) vx_set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND);
    return p;
}
static const uint8_t *rsrc_find(const pe_view *v, uintptr_t type, const WCHAR *name, uint32_t *len) {
    const uint8_t *types = rsrc_dir(v, 0), *names = types ? subdir(v, rsrc_entry(v, types, (const WCHAR *)type, 0)) : NULL;
    if (!names) { vx_set_last_error(ERROR_RESOURCE_TYPE_NOT_FOUND); return NULL; }
    const uint32_t *e = rsrc_entry(v, names, name, 0);
    if (!e) { vx_set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND); return NULL; }
    return rsrc_data(v, e, len);
}

// A loaded module (NULL: the program) as a view of its mapped image.
static int module_view(HANDLE module, pe_view *v) {
    const uint8_t *base = module ? (const uint8_t *)module : vx_proc.image_base;
    const vx_image *im = vx_image_at((uint64_t)(uintptr_t)base);
    if (!im || im->base != base) { vx_set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND); return 0; }   // (a built-in DLL)
    if (!pe_open(v, base, im->size, 0)) { vx_set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND); return 0; }
    return 1;
}
// A file's bytes, mapped read-only (munmap when done).
static uint8_t *map_file(const WCHAR *path, size_t *size) {
    char *a = vx_utf8_dup(path), unix_path[4096];
    int ok = a && vx_unix_path(a, unix_path, sizeof unix_path);
    free(a);
    if (!ok) return NULL;
    int fd = open(unix_path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 6) { if (fd >= 0) close(fd); vx_set_last_error(ERROR_FILE_NOT_FOUND); return NULL; }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    *size = (size_t)st.st_size;
    return p;
}

// ---- making icons ---------------------------------------------------------------------------------------------------------------
// An icon of a directory's best entry for cx x cy (0: that entry's own size), with the larger sizes kept too.
// `entry_data` gives an entry's bytes (from the file, or the RT_ICON resource of its id).
typedef const uint8_t *(*entry_fn)(const void *ctx, const dir_entry *e, uint32_t *len);
static icon *icon_from_directory(const dir_entry *e, int n, int is_icon, int cx, int cy, entry_fn data, const void *ctx) {
    int b = best_entry(e, n, cx ? cx : SYSTEM_ICON_SIZE, cy ? cy : SYSTEM_ICON_SIZE);
    if (b < 0) return NULL;
    uint32_t len;
    const uint8_t *p = data(ctx, &e[b], &len);
    image im;
    if (!p || !decode_image(p, len, &im)) return NULL;
    icon *ic = calloc(1, sizeof *ic);
    ic->is_icon = is_icon;
    int w = cx ? cx : im.w, h = cy ? cy : im.h;
    if (w != im.w || h != im.h) { ic->img = scaled(&im, w, h); free(im.px); } else ic->img = im;
    if (!ic->img.px) { free(ic); return NULL; }
    ic->hot_x = is_icon ? ic->img.w / 2 : e[b].hot_x * ic->img.w / (im.w ? im.w : 1);
    ic->hot_y = is_icon ? ic->img.h / 2 : e[b].hot_y * ic->img.h / (im.h ? im.h : 1);
    // The larger sizes, best entry of each (up to four times the size: a Retina status item needs 2x).
    for (int s = 0; s < n && ic->nmore < 6; s++) {
        if (e[s].w <= ic->img.w || e[s].w > ic->img.w * 4 || best_entry(e, n, e[s].w, e[s].h) != s) continue;
        int dup = 0;
        for (int k = 0; k < ic->nmore; k++) dup |= ic->more[k].w == e[s].w;
        image m;
        if (!dup && (p = data(ctx, &e[s], &len)) && decode_image(p, len, &m)) ic->more[ic->nmore++] = m;
    }
    return ic;
}
typedef struct { const uint8_t *file; size_t size; } file_ctx;
static const uint8_t *file_entry(const void *ctx, const dir_entry *e, uint32_t *len) {
    const file_ctx *f = ctx;
    if ((uint64_t)e->where + e->size > f->size) return NULL;
    *len = e->size;
    return f->file + e->where;
}
static const uint8_t *resource_entry(const void *ctx, const dir_entry *e, uint32_t *len) {
    return rsrc_find(ctx, RT_ICON, (const WCHAR *)(uintptr_t)e->where, len);
}
static icon *icon_from_group(const pe_view *v, const uint8_t *group, uint32_t len, int cx, int cy) {
    dir_entry e[64];
    int t, n = read_directory(group, len, 1, e, 64, &t);
    if (n <= 0 || t != 1) { vx_set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND); return NULL; }
    return icon_from_directory(e, n, 1, cx, cy, resource_entry, v);
}
static icon *icon_from_ico(const uint8_t *p, size_t size, int want_icon, int cx, int cy) {
    dir_entry e[64];
    int t, n = read_directory(p, size, 0, e, 64, &t);
    if (n <= 0 || (t == 1) != want_icon) return NULL;
    file_ctx f = { p, size };
    return icon_from_directory(e, n, t == 1, cx, cy, file_entry, &f);
}

static int desired(int v, uint32_t flags) { return v ? v : flags & LR_DEFAULTSIZE ? SYSTEM_ICON_SIZE : 0; }

// LR_SHARED: the same resource loaded again is the same handle.
static HANDLE find_shared(const void *key, uintptr_t name, int w, int h) {
    HANDLE r = NULL;
    pthread_mutex_lock(&g_icon_lock);
    for (int i = 0; i < g_icon_top && !r; i++) {
        icon *ic = g_icons[i];
        if (ic && ic->shared && ic->key == key && ic->key_name == name && ic->key_w == w && ic->key_h == h) r = (HANDLE)(uintptr_t)(ICON_BASE + i * 4);
    }
    pthread_mutex_unlock(&g_icon_lock);
    return r;
}
static HANDLE keep(icon *ic, uint32_t flags, const void *key, uintptr_t name, int w, int h) {
    if (!ic) return NULL;
    if (flags & LR_SHARED) { ic->shared = 1; ic->key = key; ic->key_name = name; ic->key_w = w; ic->key_h = h; }
    return new_icon(ic);
}
// A name's identity for the shared cache: the id, or a hash of the string (case-blind).
static uintptr_t name_key(const WCHAR *name) {
    if ((uintptr_t)name < 0x10000) return (uintptr_t)name;
    uintptr_t k = 1469598103934665603ull;
    for (const WCHAR *c = name; *c; c++) k = (k ^ (uintptr_t)towupper(*c)) * 1099511628211ull;
    return k | 0x10000;
}

// ---- user32 --------------------------------------------------------------------------------------------------------------------
typedef struct { BOOL fIcon; DWORD xHotspot, yHotspot; HANDLE hbmMask, hbmColor; } ICONINFO;

static WINAPI HANDLE CreateIconIndirect(const ICONINFO *ii) {
    if (!ii || (!ii->hbmColor && !ii->hbmMask)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    int mw = 0, mh = 0, mbpp = 0, w, h, bpp;
    uint32_t *mask = ii->hbmMask ? vx_gdi_bitmap_bgra(ii->hbmMask, &mw, &mh, &mbpp) : NULL;
    uint32_t *px;
    if (ii->hbmColor) {
        px = vx_gdi_bitmap_bgra(ii->hbmColor, &w, &h, &bpp);
        if (!px) { free(mask); vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
        int any_alpha = 0;
        for (size_t i = 0; bpp == 32 && i < (size_t)w * h; i++) any_alpha |= (px[i] >> 24) != 0;
        if (!any_alpha)
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    int transparent = mask && x < mw && y < mh && (mask[(size_t)y * mw + x] & 0xffffff);
                    px[(size_t)y * w + x] = (px[(size_t)y * w + x] & 0xffffffu) | (transparent ? 0 : 0xff000000u);
                }
    } else {
        // Black and white: the AND mask above the XOR bits, in one bitmap twice as high.
        if (!mask || mh < 2) { free(mask); vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
        w = mw; h = mh / 2;
        px = malloc((size_t)w * h * 4);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int and_bit = (mask[(size_t)y * mw + x] & 0xffffff) != 0, xor_bit = (mask[(size_t)(y + h) * mw + x] & 0xffffff) != 0;
                px[(size_t)y * w + x] = and_bit ? 0 : 0xff000000u | (xor_bit ? 0xffffff : 0);   // (inverting pixels: left out)
            }
    }
    free(mask);
    icon *ic = calloc(1, sizeof *ic);
    ic->is_icon = ii->fIcon != 0;
    ic->img = (image){ w, h, px };
    ic->hot_x = ic->is_icon ? w / 2 : (int)ii->xHotspot;
    ic->hot_y = ic->is_icon ? h / 2 : (int)ii->yHotspot;
    return new_icon(ic);
}

static WINAPI HANDLE CreateIconFromResourceEx(const uint8_t *bits, DWORD size, BOOL is_icon, DWORD ver, int cx, int cy, DWORD flags) {
    if (!bits || ver < 0x20000 || ver > 0x30000 || size < (is_icon ? 40u : 44u)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    int w = cx ? cx : flags & LR_DEFAULTSIZE ? SYSTEM_ICON_SIZE : 0, h = cy ? cy : flags & LR_DEFAULTSIZE ? SYSTEM_ICON_SIZE : 0;
    HANDLE shared = flags & LR_SHARED ? find_shared(bits, size, w, h) : NULL;
    if (shared) return shared;
    image im;
    const uint8_t *p = is_icon ? bits : bits + 4;
    if (!decode_image(p, size - (DWORD)(p - bits), &im)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    icon *ic = calloc(1, sizeof *ic);
    ic->is_icon = is_icon != 0;
    if ((w && w != im.w) || (h && h != im.h)) { ic->img = scaled(&im, w ? w : im.w, h ? h : im.h); free(im.px); } else ic->img = im;
    if (!ic->img.px) { free(ic); vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    ic->hot_x = is_icon ? ic->img.w / 2 : *(const uint16_t *)bits * ic->img.w / im.w;
    ic->hot_y = is_icon ? ic->img.h / 2 : *(const uint16_t *)(bits + 2) * ic->img.h / im.h;
    return keep(ic, flags, bits, size, w, h);
}
// "Equivalent to CreateIconFromResourceEx(bits, size, icon, ver, 0, 0, LR_DEFAULTSIZE | LR_SHARED)" (its documentation).
static WINAPI HANDLE CreateIconFromResource(const uint8_t *bits, DWORD size, BOOL is_icon, DWORD ver) {
    return CreateIconFromResourceEx(bits, size, is_icon, ver, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
}

static WINAPI int LookupIconIdFromDirectoryEx(const uint8_t *dir, BOOL is_icon, int cx, int cy, uint32_t flags) {
    dir_entry e[64];
    int t, n = dir ? read_directory(dir, 6 + 14 * (size_t)*(const uint16_t *)(dir + 4), 1, e, 64, &t) : -1;
    if (n <= 0 || (t == 1) != (is_icon != 0)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    int b = best_entry(e, n, desired(cx, flags | LR_DEFAULTSIZE), desired(cy, flags | LR_DEFAULTSIZE));
    return b < 0 ? 0 : (int)e[b].where;
}
static WINAPI int LookupIconIdFromDirectory(const uint8_t *dir, BOOL is_icon) { return LookupIconIdFromDirectoryEx(dir, is_icon, 0, 0, LR_DEFAULTSIZE); }

// Bitmaps and cursors from resources keep user32.c's stand-in handles (nothing draws them yet).
static HANDLE stand_in(uintptr_t name, uint32_t type) { return (HANDLE)((0x30000 + ((uintptr_t)type << 16)) | (name & 0xffff)); }

static WINAPI HANDLE LoadImageW(HANDLE module, const WCHAR *name, uint32_t type, int cx, int cy, uint32_t flags) {
    if (type != IMAGE_ICON && !(type == IMAGE_CURSOR && (flags & LR_LOADFROMFILE))) return stand_in((uintptr_t)name, type);
    int w = desired(cx, flags), h = desired(cy, flags);
    if (flags & LR_LOADFROMFILE) {
        if ((uintptr_t)name < 0x10000) { vx_set_last_error(ERROR_FILE_NOT_FOUND); return NULL; }
        size_t size;
        uint8_t *p = map_file(name, &size);
        if (!p) return NULL;
        icon *ic = icon_from_ico(p, size, type == IMAGE_ICON, w, h);
        munmap(p, size);
        if (!ic) { vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }   // (not an icon file)
        return new_icon(ic);                    // (LR_SHARED does not apply to files)
    }
    if (!module) return system_icon((uintptr_t)name);
    uintptr_t key = name_key(name);
    HANDLE shared = flags & LR_SHARED ? find_shared(module, key, w, h) : NULL;
    if (shared) return shared;
    pe_view v;
    uint32_t len;
    const uint8_t *group;
    if (!module_view(module, &v)) return vx_image_at((uint64_t)(uintptr_t)module) ? NULL : system_icon((uintptr_t)name);   // (a built-in DLL: a stand-in)
    if (!(group = rsrc_find(&v, RT_GROUP_ICON, name, &len))) return NULL;
    return keep(icon_from_group(&v, group, len, w, h), flags, module, key, w, h);
}
static WCHAR *wide_name(const char *s) { return (uintptr_t)s < 0x10000 ? (WCHAR *)(uintptr_t)s : vx_utf16_dup(s); }
static void free_name(const char *s, WCHAR *w) { if ((uintptr_t)s >= 0x10000) free(w); }
static WINAPI HANDLE LoadImageA(HANDLE module, const char *name, uint32_t type, int cx, int cy, uint32_t flags) {
    WCHAR *n = wide_name(name);
    HANDLE r = LoadImageW(module, n, type, cx, cy, flags);
    free_name(name, n);
    return r;
}
// LoadIcon: the system's icons stay stand-ins; a module's at SM_CXICON, shared.
static WINAPI HANDLE LoadIconW(HANDLE module, const WCHAR *name) {
    return module ? LoadImageW(module, name, IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED) : system_icon((uintptr_t)name);
}
static WINAPI HANDLE LoadIconA(HANDLE module, const char *name) {
    WCHAR *n = wide_name(name);
    HANDLE r = LoadIconW(module, n);
    free_name(name, n);
    return r;
}

static icon *copy_icon(const icon *src, int w, int h) {
    icon *ic = calloc(1, sizeof *ic);
    ic->is_icon = src->is_icon;
    ic->img = scaled(&src->img, w ? w : src->img.w, h ? h : src->img.h);
    ic->hot_x = src->hot_x * ic->img.w / src->img.w; ic->hot_y = src->hot_y * ic->img.h / src->img.h;
    for (int i = 0; i < src->nmore; i++) if (src->more[i].w > ic->img.w) ic->more[ic->nmore++] = scaled(&src->more[i], src->more[i].w, src->more[i].h);
    if (!ic->img.px) { free_icon(ic); return NULL; }
    return ic;
}
static WINAPI HANDLE CopyIcon(HANDLE h) {
    pthread_mutex_lock(&g_icon_lock);
    icon *src = get(h), *ic = src ? copy_icon(src, 0, 0) : NULL;
    pthread_mutex_unlock(&g_icon_lock);
    if (!src) {
        if (is_icon_handle(h)) { vx_set_last_error(ERROR_INVALID_CURSOR_HANDLE); return NULL; }
        return h;                               // a stand-in: the same system icon
    }
    return ic ? new_icon(ic) : NULL;
}
static BOOL destroy(HANDLE h) {
    pthread_mutex_lock(&g_icon_lock);
    icon *ic = get(h);
    int known = ic != NULL;
    if (ic && !ic->shared) g_icons[((uintptr_t)h - ICON_BASE) / 4] = NULL; else ic = NULL;
    pthread_mutex_unlock(&g_icon_lock);
    free_icon(ic);
    if (!known && is_icon_handle(h)) { vx_set_last_error(ERROR_INVALID_CURSOR_HANDLE); return 0; }
    return 1;                                   // (shared icons stay; stand-ins have nothing to free)
}
static WINAPI BOOL DestroyIcon(HANDLE h) { return destroy(h); }
static WINAPI BOOL DestroyCursor(HANDLE h) { return destroy(h); }

static WINAPI HANDLE CopyImage(HANDLE h, uint32_t type, int cx, int cy, uint32_t flags) {
    if (type != IMAGE_ICON && type != IMAGE_CURSOR) return h;   // (bitmaps: the same handle, as before)
    pthread_mutex_lock(&g_icon_lock);
    icon *src = get(h), *ic = NULL;
    int same = src && (!cx || cx == src->img.w) && (!cy || cy == src->img.h);
    if (src && !(same && (flags & LR_COPYRETURNORG))) ic = copy_icon(src, desired(cx, flags), desired(cy, flags));
    pthread_mutex_unlock(&g_icon_lock);
    if (!src) return h;
    if (same && (flags & LR_COPYRETURNORG)) return h;
    HANDLE r = ic ? new_icon(ic) : NULL;
    if (r && (flags & LR_COPYDELETEORG)) destroy(h);
    return r;
}

// A copy of the picture of h; 0 if h is not an icon with pixels.
static int copy_image(HANDLE h, image *out) {
    pthread_mutex_lock(&g_icon_lock);
    icon *ic = get(h);
    if (ic) *out = scaled(&ic->img, ic->img.w, ic->img.h);
    pthread_mutex_unlock(&g_icon_lock);
    return ic && out->px;
}
static WINAPI BOOL GetIconInfo(HANDLE h, ICONINFO *info) {
    pthread_mutex_lock(&g_icon_lock);
    icon *ic = get(h);
    int is_icon = ic ? ic->is_icon : 0, hx = ic ? ic->hot_x : 0, hy = ic ? ic->hot_y : 0;
    pthread_mutex_unlock(&g_icon_lock);
    image im;
    if (!ic || !info || !copy_image(h, &im)) { vx_set_last_error(ERROR_INVALID_CURSOR_HANDLE); return 0; }
    size_t mstride = (((size_t)im.w + 31) / 32) * 4;
    uint8_t *mask = calloc(mstride, (size_t)im.h);
    for (int y = 0; y < im.h; y++)
        for (int x = 0; x < im.w; x++)
            if (!(im.px[(size_t)y * im.w + x] >> 24)) mask[mstride * y + x / 8] |= (uint8_t)(0x80 >> (x % 8));
    info->fIcon = is_icon; info->xHotspot = (DWORD)hx; info->yHotspot = (DWORD)hy;
    info->hbmColor = vx_gdi_bitmap_from_bgra(im.w, im.h, im.px);
    info->hbmMask = vx_gdi_bitmap_mono(im.w, im.h, mask);
    free(mask); free(im.px);
    return 1;
}

static WINAPI BOOL DrawIconEx(HANDLE dc, int x, int y, HANDLE h, int cx, int cy, uint32_t step, HANDLE brush, uint32_t flags) {
    (void)step;
    image im;
    if (!copy_image(h, &im)) {
        if (is_icon_handle(h)) { vx_set_last_error(ERROR_INVALID_CURSOR_HANDLE); return 0; }
        return 1;                               // a stand-in: nothing to draw (as before)
    }
    int w = cx ? cx : flags & DI_DEFAULTSIZE ? SYSTEM_ICON_SIZE : im.w, hgt = cy ? cy : flags & DI_DEFAULTSIZE ? SYSTEM_ICON_SIZE : im.h;
    if (brush) {                                // "flicker-free": drawn over the brush's color first
        static int (WINAPI *fill)(HANDLE, const RECT *, HANDLE);
        if (!fill) fill = (int (WINAPI *)(HANDLE, const RECT *, HANDLE))vx_find_export("user32.dll", "FillRect");
        RECT r = { x, y, x + w, y + hgt };
        if (fill) fill(dc, &r, brush);
    }
    if ((flags & 3) == DI_MASK)                 // the mask alone: black where the icon is
        for (size_t i = 0; i < (size_t)im.w * im.h; i++) im.px[i] = im.px[i] >> 24 ? 0xff000000u : 0;
    int ok = vx_gdi_draw_bgra(dc, x, y, w, hgt, im.px, im.w, im.h, (flags & 3) != DI_IMAGE);
    free(im.px);
    return ok;
}
static WINAPI BOOL DrawIcon(HANDLE dc, int x, int y, HANDLE h) { return DrawIconEx(dc, x, y, h, 0, 0, 0, NULL, 3 | DI_DEFAULTSIZE); }

// ---- shell32 -----------------------------------------------------------------------------------------------------------------------
// The icons of an .exe or .dll (its RT_GROUP_ICON resources, in directory order) or of an .ico file (one).
static WINAPI unsigned ExtractIconExW(const WCHAR *file, int index, HANDLE *large, HANDLE *small, unsigned n) {
    size_t size;
    uint8_t *p = file ? map_file(file, &size) : NULL;
    if (!p) return 0;
    unsigned done = 0;
    pe_view v;
    if (p[0] == 0 && p[1] == 0 && p[2] == 1 && p[3] == 0) {
        if (index == -1 && !large && !small) done = 1;
        else if (index == 0 && n) {
            if (large) large[0] = new_icon(icon_from_ico(p, size, 1, SYSTEM_ICON_SIZE, SYSTEM_ICON_SIZE));
            if (small) small[0] = new_icon(icon_from_ico(p, size, 1, SMALL_ICON_SIZE, SMALL_ICON_SIZE));
            done = (large && large[0]) || (small && small[0]);
        }
    } else if (pe_open(&v, p, size, 1)) {
        const uint8_t *types = rsrc_dir(&v, 0), *groups = types ? subdir(&v, rsrc_entry(&v, types, (const WCHAR *)RT_GROUP_ICON, 0)) : NULL;
        int count = groups ? rsrc_count(groups) : 0;
        if (index == -1 && !large && !small) done = (unsigned)count;
        else {
            int first = index;
            if (index < 0 && groups) {                                    // -id: begins with the group of that id
                first = -1;
                for (int i = 0; i < count && first < 0; i++) { const uint32_t *e = rsrc_entry(&v, groups, (const WCHAR *)(intptr_t)-1, i); if (e && e[0] == (uint32_t)-index) first = i; }
            }
            for (unsigned k = 0; first >= 0 && k < n && first + (int)k < count; k++) {
                uint32_t len;
                const uint8_t *g = rsrc_data(&v, rsrc_entry(&v, groups, (const WCHAR *)(intptr_t)-1, first + (int)k), &len);
                if (!g) break;
                HANDLE l = large ? new_icon(icon_from_group(&v, g, len, SYSTEM_ICON_SIZE, SYSTEM_ICON_SIZE)) : NULL;
                HANDLE s = small ? new_icon(icon_from_group(&v, g, len, SMALL_ICON_SIZE, SMALL_ICON_SIZE)) : NULL;
                if (large) large[k] = l;
                if (small) small[k] = s;
                if (!l && !s) break;
                done++;
            }
        }
    }
    munmap(p, size);
    return done;
}
static WINAPI unsigned ExtractIconExA(const char *file, int index, HANDLE *large, HANDLE *small, unsigned n) {
    WCHAR *w = file ? vx_utf16_dup(file) : NULL;
    unsigned r = ExtractIconExW(w, index, large, small, n);
    free(w);
    return r;
}

// ---- for tray.m --------------------------------------------------------------------------------------------------------------------
int vx_icon_images(HANDLE h, vx_icon_image *out, int cap) {
    int n = 0;
    pthread_mutex_lock(&g_icon_lock);
    icon *ic = get(h);
    if (ic && cap > 0) {
        image c = scaled(&ic->img, ic->img.w, ic->img.h);
        if (c.px) out[n++] = (vx_icon_image){ c.w, c.h, c.px };
        for (int i = 0; i < ic->nmore && n < cap; i++) {
            image m = scaled(&ic->more[i], ic->more[i].w, ic->more[i].h);
            if (m.px) out[n++] = (vx_icon_image){ m.w, m.h, m.px };
        }
    }
    pthread_mutex_unlock(&g_icon_lock);
    return n;
}
void vx_icon_images_free(vx_icon_image *v, int n) { for (int i = 0; i < n; i++) free(v[i].px); }

static const vx_export user32[] = {
    VX_EXPORT(CreateIconIndirect), VX_EXPORT(CreateIconFromResource), VX_EXPORT(CreateIconFromResourceEx),
    VX_EXPORT(LookupIconIdFromDirectory), VX_EXPORT(LookupIconIdFromDirectoryEx),
    VX_EXPORT(LoadImageA), VX_EXPORT(LoadImageW), VX_EXPORT(LoadIconA), VX_EXPORT(LoadIconW),
    VX_EXPORT(CopyIcon), VX_EXPORT(CopyImage), VX_EXPORT(DestroyIcon), VX_EXPORT(DestroyCursor),
    VX_EXPORT(GetIconInfo), VX_EXPORT(DrawIconEx), VX_EXPORT(DrawIcon),
};
static const vx_export shell32[] = { VX_EXPORT(ExtractIconExW), VX_EXPORT(ExtractIconExA) };
void vx_icons_init(void) {
    vx_register_module("user32.dll", user32, sizeof user32 / sizeof user32[0]);
    vx_register_module("shell32.dll", shell32, sizeof shell32 / sizeof shell32[0]);
}

#ifdef __aarch64__
#include "thunks/icons.c.inc"
#endif
