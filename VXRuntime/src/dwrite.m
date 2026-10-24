// DirectWrite (DWrite.dll) on CoreText and CoreGraphics: what Chromium and Skia use on Windows to find fonts, map
// text to fonts and glyphs, measure glyphs and draw them.
//
// The system font collection is the Mac's: CoreText's installed fonts grouped by family name, the faces of a family
// told apart by the weight, width and slope their OS/2 tables give. A font face is a CTFont at the size of its em
// square (CoreText's numbers are then design units) and, when its file can be read, that file mapped: tables,
// metrics, advances and names come from the font's own tables (the OpenType specification), outlines from CoreText,
// and glyph runs are filled with CoreGraphics into alpha textures. Fonts a program brings (web fonts) come through
// its own font file loader, read once, and become CGFonts made from their bytes. Font fallback asks CoreText which
// installed font covers the text (CTFontCreateForStringWithLanguage). Text formats and layouts (what Steam's own
// VGUI measures and draws with) lay text out one glyph per character, without shaping; GDI interop's bitmap render
// target blends glyph runs into a DIB section.
//
// Not offered: text analyzers (IDWriteTextAnalyzer; Chromium shapes text with HarfBuzz), colour glyphs
// (TranslateColorGlyphRun answers DWRITE_E_NOCOLOR, so emoji in Apple Color Emoji, which has bitmaps only, draw
// nothing yet), hinting (CoreText does not hint: grid fitting is always off), font sets, and IDWriteFactory3 and
// later, IDWriteFontFace3 and later, IDWriteFont3 (QueryInterface answers E_NOINTERFACE). VXRT_TRACE_DWRITE=1 logs
// the calls (the first 20 of each method) and the interfaces asked for and not offered.
#import <CoreText/CoreText.h>
#import <CoreGraphics/CoreGraphics.h>
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "d3d_common.h"
static HRESULT dw_not_implemented(const char *what);
#define d3d_not_implemented dw_not_implemented
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "dwrite_tables.h"
#pragma clang diagnostic pop
#undef d3d_not_implemented

#define DWRITE_E_FILEFORMAT ((HRESULT)0x88985000)
#define DWRITE_E_NOFONT ((HRESULT)0x88985002)
#define DWRITE_E_FILENOTFOUND ((HRESULT)0x88985003)
#define DWRITE_E_ALREADYREGISTERED ((HRESULT)0x88985006)
#define DWRITE_E_NOCOLOR ((HRESULT)0x8898500C)
#define E_NOT_SUFFICIENT_BUFFER ((HRESULT)0x8007007A)    // HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)

enum { FACE_CFF = 0, FACE_TRUETYPE = 1, FACE_COLLECTION = 2, FACE_UNKNOWN = 6 };
enum { FILE_UNKNOWN = 0, FILE_CFF = 1, FILE_TRUETYPE = 2, FILE_COLLECTION = 3 };
enum { STYLE_NORMAL = 0, STYLE_OBLIQUE = 1, STYLE_ITALIC = 2 };
enum { SIM_BOLD = 1, SIM_OBLIQUE = 2 };
enum { MODE_DEFAULT = 0, MODE_ALIASED = 1, MODE_GDI_CLASSIC = 2, MODE_GDI_NATURAL = 3, MODE_NATURAL = 4, MODE_NATURAL_SYMMETRIC = 5, MODE_OUTLINE = 6 };
enum { MEASURE_NATURAL = 0, MEASURE_GDI_CLASSIC = 1, MEASURE_GDI_NATURAL = 2 };
enum { TEXTURE_ALIASED_1x1 = 0, TEXTURE_CLEARTYPE_3x1 = 1 };
enum { AA_CLEARTYPE = 0, AA_GRAYSCALE = 1 };
enum { GRID_FIT_DEFAULT = 0, GRID_FIT_DISABLED = 1 };

// The structures programs hand over (dwrite.h, dwrite_1.h): the same layouts.
typedef struct {                       // DWRITE_FONT_METRICS1; its first ten fields are DWRITE_FONT_METRICS
    uint16_t upem, ascent, descent; int16_t line_gap; uint16_t cap_height, x_height;
    int16_t underline_pos; uint16_t underline_thick; int16_t strike_pos; uint16_t strike_thick;
    int16_t box_left, box_top, box_right, box_bottom;
    int16_t sub_x, sub_y, sub_w, sub_h, sup_x, sup_y, sup_w, sup_h;
    int32_t has_typo_metrics;
} font_metrics;
#define FONT_METRICS0_SIZE 20
typedef struct { int32_t lsb; uint32_t advance; int32_t rsb, tsb; uint32_t advance_height; int32_t bsb, vorigin; } glyph_metrics;
typedef struct { float advance_offset, ascender_offset; } glyph_offset;
typedef struct { float m11, m12, m21, m22, dx, dy; } matrix;
typedef struct { void *face; float em; uint32_t count; const uint16_t *glyphs; const float *advances; const glyph_offset *offsets; int32_t sideways; uint32_t bidi; } glyph_run;
typedef struct { int16_t rise, run, offset; } caret_metrics;
typedef struct { uint32_t first, last; } unicode_range;
typedef struct { LONG height, width, escapement, orientation, weight; uint8_t italic, underline, strikeout, charset, out_precision, clip_precision, quality, pitch_family; WCHAR face[32]; } logfont_w;
typedef struct { float x, y; } point_2f;

static const GUID IID_IUnknown_ = { 0, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static pthread_mutex_t g_lock;                       // (recursive) collections, families, faces made on demand
static void lock(void) { pthread_mutex_lock(&g_lock); }
// Made once on demand, read without the lock: set last (release), read first (acquire).
#define READY(p) __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define PUBLISH(p, v) __atomic_store_n(&(p), (v), __ATOMIC_RELEASE)
static void unlock(void) { pthread_mutex_unlock(&g_lock); }

// ---- tracing ---------------------------------------------------------------------------------------------------------------
static int g_trace;
#define TR(...) do { static int n_; if (g_trace && n_ < 20) { n_++; vx_log("DirectWrite: " __VA_ARGS__); } } while (0)
static HRESULT dw_not_implemented(const char *what) {
    static char seen[256][80]; static int n;
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    int known = 0;
    for (int i = 0; i < n && !known; i++) known = !strcmp(seen[i], what);
    if (!known && n < 256) snprintf(seen[n++], sizeof seen[0], "%s", what);
    pthread_mutex_unlock(&m);
    if (!known) vx_log("DirectWrite: %s is not implemented yet", what);
    return E_NOTIMPL;
}
static const struct { const char *name; const GUID *iid; } g_iid_names[] = {
#define N(i) { #i, &IID_##i }
    N(IDWriteFactory), N(IDWriteFactory1), N(IDWriteFactory2), N(IDWriteFactory3), N(IDWriteFactory4), N(IDWriteFactory5),
    N(IDWriteFactory6), N(IDWriteFactory7), N(IDWriteFontFace), N(IDWriteFontFace1), N(IDWriteFontFace2), N(IDWriteFontFace3),
    N(IDWriteFontFace4), N(IDWriteFontFace5), N(IDWriteFont), N(IDWriteFont1), N(IDWriteFont2), N(IDWriteFont3),
    N(IDWriteFontCollection), N(IDWriteFontCollection1), N(IDWriteFontFamily), N(IDWriteFontFamily1), N(IDWriteFontList),
    N(IDWriteFontList1), N(IDWriteRenderingParams), N(IDWriteRenderingParams1), N(IDWriteRenderingParams2),
    N(IDWriteRenderingParams3), N(IDWriteFontFallback), N(IDWriteFontFallback1), N(IDWriteGdiInterop), N(IDWriteGdiInterop1),
    N(IDWriteFontFile), N(IDWriteFontFileLoader), N(IDWriteLocalFontFileLoader), N(IDWriteInMemoryFontFileLoader),
    N(IDWriteFontFileStream), N(IDWriteLocalizedStrings), N(IDWriteGlyphRunAnalysis),
#undef N
};
static void trace_iid(const char *who, const GUID *g) {
    if (!g_trace || !g) return;
    for (size_t i = 0; i < sizeof g_iid_names / sizeof g_iid_names[0]; i++)
        if (d3d_same_guid(g, g_iid_names[i].iid)) { vx_log("DirectWrite: %s: QueryInterface(%s) not offered", who, g_iid_names[i].name); return; }
    vx_log("DirectWrite: %s: QueryInterface({%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}) not offered", who, g->d1, g->d2, g->d3,
           g->d4[0], g->d4[1], g->d4[2], g->d4[3], g->d4[4], g->d4[5], g->d4[6], g->d4[7]);
}

// ---- COM objects -------------------------------------------------------------------------------------------------------------
// Every object starts with this. Fonts and families keep no count of their own: their collection's counts for them.
typedef struct dw_obj {
    void **vtbl;
    volatile LONG refs;
    struct dw_obj *owner;                 // fonts and families: their collection
    void (*destroy)(struct dw_obj *);    // NULL: lives as long as the process (shared factory, system collection ...)
    const GUID *const *iids;             // what QueryInterface answers besides IUnknown
    const char *what;                    // (traces)
} dw_obj;
static dw_obj *counted(void *p) { dw_obj *o = p; return o->owner ? o->owner : o; }
static WINAPI uint32_t dw_addref(void *this) { return (uint32_t)__sync_add_and_fetch(&counted(this)->refs, 1); }
static WINAPI uint32_t dw_release(void *this) {
    dw_obj *o = counted(this);
    LONG r = __sync_sub_and_fetch(&o->refs, 1);
    if (r == 0 && o->destroy) o->destroy(o);
    return (uint32_t)(r < 0 ? 0 : r);
}
static WINAPI HRESULT dw_query(void *this, const GUID *iid, void **out) {
    if (!out) return E_POINTER;
    dw_obj *o = this;
    int ok = iid && d3d_same_guid(iid, &IID_IUnknown_);
    for (const GUID *const *g = o->iids; iid && g && *g && !ok; g++) ok = d3d_same_guid(iid, *g);
    if (ok) { dw_addref(this); *out = this; return S_OK; }
    *out = NULL;
    trace_iid(o->what, iid);
    return E_NOINTERFACE;
}
static void *dw_new(size_t size, void **vtbl, const GUID *const *iids, const char *what, void (*destroy)(dw_obj *)) {
    dw_obj *o = calloc(1, size);
    o->vtbl = vtbl; o->refs = 1; o->iids = iids; o->what = what; o->destroy = destroy;
    return o;
}
// The program's COM objects (its loaders, streams, text sources, geometry sinks): methods by vtable slot.
#define SLOT(obj, i) ((*(void ***)(obj))[i])
static void com_addref(void *o) { ((uint32_t (WINAPI *)(void *))SLOT(o, 1))(o); }
static void com_release(void *o) { ((uint32_t (WINAPI *)(void *))SLOT(o, 2))(o); }

static void **g_factory_vtbl, **g_collection_vtbl, **g_family_vtbl, **g_font_vtbl, **g_list_vtbl, **g_face_vtbl, **g_strings_vtbl,
            **g_file_vtbl, **g_local_loader_vtbl, **g_stream_vtbl, **g_params_vtbl, **g_analysis_vtbl, **g_fallback_vtbl, **g_gdi_vtbl;
static void *g_memory_loader_vtbl[4];

// ---- OpenType files ------------------------------------------------------------------------------------------------------
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static int16_t bes16(const uint8_t *p) { return (int16_t)be16(p); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
#define TAG(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

// The faces in a font file (1, or a collection's count) and its file type; 0 faces if it is no OpenType file.
static uint32_t sfnt_faces(const uint8_t *d, size_t n, int *file_type) {
    *file_type = FILE_UNKNOWN;
    if (!d || n < 12) return 0;
    uint32_t v = be32(d);
    if (v == TAG('t', 't', 'c', 'f')) {
        uint32_t c = be32(d + 8);
        if (!c || 12 + 4ull * c > n) return 0;
        *file_type = FILE_COLLECTION;
        return c;
    }
    if (v == 0x00010000 || v == TAG('t', 'r', 'u', 'e')) { *file_type = FILE_TRUETYPE; return 1; }
    if (v == TAG('O', 'T', 'T', 'O')) { *file_type = FILE_CFF; return 1; }
    return 0;
}
// Where a face's table directory is, -1 if not there.
static long sfnt_dir(const uint8_t *d, size_t n, uint32_t index) {
    int type;
    uint32_t c = sfnt_faces(d, n, &type);
    if (index >= c) return -1;
    uint64_t off = type == FILE_COLLECTION ? be32(d + 12 + 4 * index) : 0;
    if (off + 12 > n || off + 12 + 16ull * be16(d + off + 4) > n) return -1;
    return (long)off;
}
static int sfnt_table(const uint8_t *d, size_t n, long dir, uint32_t tag, const uint8_t **out, uint32_t *len) {
    uint16_t count = be16(d + dir + 4);
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *r = d + dir + 12 + 16 * i;
        if (be32(r) != tag) continue;
        uint32_t off = be32(r + 8), l = be32(r + 12);
        if ((uint64_t)off + l > n) return 0;
        *out = d + off; *len = l;
        return 1;
    }
    return 0;
}
// A single-face font file from tables (a collection's face, or what CoreText hands out): the bytes, malloc'd.
typedef struct { uint32_t tag; const uint8_t *data; uint32_t len; } sfnt_part;
static uint8_t *sfnt_build(uint32_t version, const sfnt_part *t, int count, size_t *size) {
    size_t total = 12 + 16 * (size_t)count;
    for (int i = 0; i < count; i++) total += (t[i].len + 3) & ~3u;
    uint8_t *b = calloc(1, total), *p = b + 12 + 16 * count;
    int sel = 0;
    while ((2 << sel) <= count) sel++;
    uint16_t range = (uint16_t)(16 << sel);
    uint8_t head[12] = { version >> 24, version >> 16, version >> 8, version, count >> 8, count, range >> 8, range, 0, sel,
                         (uint8_t)((count * 16 - range) >> 8), (uint8_t)(count * 16 - range) };
    memcpy(b, head, 12);
    for (int i = 0; i < count; i++) {
        uint8_t *r = b + 12 + 16 * i;
        uint32_t sum = 0, off = (uint32_t)(p - b);
        memcpy(p, t[i].data, t[i].len);
        for (uint32_t k = 0; k < ((t[i].len + 3) & ~3u); k += 4) sum += be32(p + k);
        uint32_t fields[4] = { t[i].tag, sum, off, t[i].len };
        for (int f = 0; f < 4; f++) { r[4 * f] = fields[f] >> 24; r[4 * f + 1] = fields[f] >> 16; r[4 * f + 2] = fields[f] >> 8; r[4 * f + 3] = fields[f]; }
        p += (t[i].len + 3) & ~3u;
    }
    *size = total;
    return b;
}
static uint8_t *sfnt_extract(const uint8_t *d, size_t n, long dir, size_t *size) {
    uint16_t count = be16(d + dir + 4);
    sfnt_part *t = calloc(count ? count : 1, sizeof *t);
    int k = 0;
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *r = d + dir + 12 + 16 * i;
        uint32_t off = be32(r + 8), l = be32(r + 12);
        if ((uint64_t)off + l <= n) t[k++] = (sfnt_part){ be32(r), d + off, l };
    }
    uint8_t *b = sfnt_build(be32(d + dir), t, k, size);
    free(t);
    return b;
}

// The 'name' table: the strings of a name ID by language. Windows-platform records (UTF-16BE) with the locale of
// their LCID; Unicode-platform and Macintosh Roman records count as English when there is no Windows one.
typedef struct { char locale[24]; WCHAR *text; uint32_t len; } name_entry;
static int add_name(name_entry *e, int n, int cap, const char *locale, const WCHAR *text, uint32_t len) {
    for (int i = 0; i < n; i++) if (!strcmp(e[i].locale, locale)) return n;
    if (n >= cap || !len) return n;
    snprintf(e[n].locale, sizeof e[n].locale, "%s", locale);
    e[n].text = malloc(2 * (len + 1));
    memcpy(e[n].text, text, 2 * len);
    e[n].text[len] = 0; e[n].len = len;
    return n + 1;
}
static int name_strings(const uint8_t *t, uint32_t len, int id, name_entry *out, int cap) {
    if (!t || len < 6) return 0;
    uint16_t count = be16(t + 2), base = be16(t + 4);
    int n = 0;
    WCHAR buf[512];
    for (int pass = 0; pass < 2; pass++) {
        for (uint16_t i = 0; i < count && 6 + 12u * (i + 1) <= len; i++) {
            const uint8_t *r = t + 6 + 12 * i;
            uint16_t plat = be16(r), enc = be16(r + 2), lang = be16(r + 4), nid = be16(r + 6), l = be16(r + 8), off = be16(r + 10);
            if (nid != id || (uint32_t)base + off + l > len) continue;
            const uint8_t *s = t + base + off;
            if (pass == 0 && plat == 3 && (enc == 0 || enc == 1 || enc == 10)) {
                uint32_t m = l / 2 < 512 ? l / 2 : 512;
                for (uint32_t k = 0; k < m; k++) buf[k] = be16(s + 2 * k);
                const vx_locale *loc = lang ? vx_locale_by_lcid(lang) : NULL;
                if (!loc && lang != 0x409) continue;
                char name[24];
                snprintf(name, sizeof name, "%s", loc ? loc->name : "en-US");
                for (char *c = name; *c; c++) *c = (char)tolower(*c);
                n = add_name(out, n, cap, name, buf, m);
            } else if (pass == 1 && !n && plat == 0) {
                uint32_t m = l / 2 < 512 ? l / 2 : 512;
                for (uint32_t k = 0; k < m; k++) buf[k] = be16(s + 2 * k);
                n = add_name(out, n, cap, "en-us", buf, m);
            } else if (pass == 1 && !n && plat == 1 && enc == 0 && lang == 0) {
                CFStringRef cs = CFStringCreateWithBytes(NULL, s, l, kCFStringEncodingMacRoman, false);
                if (!cs) continue;
                CFIndex m = CFStringGetLength(cs);
                if (m > 512) m = 512;
                CFStringGetCharacters(cs, CFRangeMake(0, m), buf);
                CFRelease(cs);
                n = add_name(out, n, cap, "en-us", buf, (uint32_t)m);
            }
        }
    }
    return n;
}

// ---- font files ------------------------------------------------------------------------------------------------------
// Files are mapped once and stay mapped (fonts the system has; a program's local font files).
typedef struct mapping { char *path; const uint8_t *base; size_t size; uint64_t mtime; struct mapping *next; } mapping;
static mapping *g_mappings;
static uint64_t filetime_of(struct timespec t) { return ((uint64_t)t.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)t.tv_nsec / 100; }
static mapping *map_file(const char *path) {
    lock();
    mapping *m = g_mappings;
    while (m && strcmp(m->path, path)) m = m->next;
    if (!m) {
        int fd = open(path, O_RDONLY);
        struct stat st;
        if (fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0) {
            void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (p != MAP_FAILED) {
                m = calloc(1, sizeof *m);
                m->path = strdup(path); m->base = p; m->size = (size_t)st.st_size; m->mtime = filetime_of(st.st_mtimespec);
                m->next = g_mappings; g_mappings = m;
            }
        }
        if (fd >= 0) close(fd);
    }
    unlock();
    return m;
}

// IDWriteFontFile. A local file (our local loader, keyed by its Windows path), bytes of our own (fonts whose file
// cannot be read: rebuilt from CoreText's tables), or a file of the program's loader (read through it once).
enum { F_LOCAL, F_MEMORY, F_CUSTOM };
typedef struct dw_file {
    dw_obj o;
    int kind;
    void *loader;                        // F_CUSTOM: the program's IDWriteFontFileLoader (held)
    uint8_t *key; uint32_t key_size;
    mapping *map;                        // F_LOCAL (NULL until read)
    char *unix_path;                     // F_LOCAL
    CFDataRef bytes;                     // F_MEMORY, F_CUSTOM once read
} dw_file;
static const GUID *const g_file_iids[] = { &IID_IDWriteFontFile, NULL };
static void file_destroy(dw_obj *o) {
    dw_file *f = (dw_file *)o;
    if (f->loader) com_release(f->loader);
    if (f->bytes) CFRelease(f->bytes);
    free(f->key); free(f->unix_path); free(f);
}
static dw_obj g_local_loader;           // IDWriteLocalFontFileLoader
static dw_obj g_memory_loader;          // IDWriteFontFileLoader of our own bytes
static int is_ours(void *p, void **vtbl) { return p && *(void ***)p == vtbl; }

// A local file: the key is the file's last write time (FILETIME) and its Windows path.
static dw_file *local_file(const char *unix_path) {
    struct stat st;
    if (stat(unix_path, &st) || !S_ISREG(st.st_mode)) return NULL;
    char win[1024];
    vx_windows_path(unix_path, win, sizeof win);
    size_t wl = vx_utf8_to_utf16(win, strlen(win), NULL, 0);
    dw_file *f = dw_new(sizeof *f, g_file_vtbl, g_file_iids, "IDWriteFontFile", file_destroy);
    f->kind = F_LOCAL;
    f->key_size = (uint32_t)(8 + 2 * (wl + 1));
    f->key = calloc(1, f->key_size);
    uint64_t ft = filetime_of(st.st_mtimespec);
    memcpy(f->key, &ft, 8);
    vx_utf8_to_utf16(win, strlen(win), (WCHAR *)(f->key + 8), wl);
    f->unix_path = strdup(unix_path);
    return f;
}
static int key_path(const void *key, uint32_t size, char *out, size_t cap) {
    if (!key || size < 10 || (size - 8) % 2) return 0;
    const WCHAR *w = (const WCHAR *)((const uint8_t *)key + 8);
    size_t n = (size - 8) / 2;
    while (n && !w[n - 1]) n--;
    char win[1024];
    size_t l = vx_utf16_to_utf8(w, n, win, sizeof win - 1);
    if (l >= sizeof win - 1) return 0;
    win[l] = 0;
    return vx_unix_path(win, out, cap);
}
// The bytes of a file (0 if they cannot be had).
static int file_bytes(dw_file *f, const uint8_t **data, size_t *size) {
    if (f->kind == F_LOCAL) {
        if (!f->map) f->map = map_file(f->unix_path);
        if (!f->map) return 0;
        *data = f->map->base; *size = f->map->size;
        return 1;
    }
    lock();
    if (!f->bytes && f->kind == F_CUSTOM) {
        // IDWriteFontFileLoader::CreateStreamFromKey (3); IDWriteFontFileStream::ReadFileFragment (3),
        // ReleaseFileFragment (4), GetFileSize (5)
        void *stream = NULL;
        HRESULT hr = ((HRESULT (WINAPI *)(void *, const void *, uint32_t, void **))SLOT(f->loader, 3))(f->loader, f->key, f->key_size, &stream);
        uint64_t n = 0;
        if (hr >= 0 && stream && ((HRESULT (WINAPI *)(void *, uint64_t *))SLOT(stream, 5))(stream, &n) >= 0 && n && n < (1ull << 31)) {
            const void *p = NULL; void *ctx = NULL;
            if (((HRESULT (WINAPI *)(void *, const void **, uint64_t, uint64_t, void **))SLOT(stream, 3))(stream, &p, 0, n, &ctx) >= 0 && p) {
                f->bytes = CFDataCreate(NULL, VX_LOW(p), (CFIndex)n);
                ((void (WINAPI *)(void *, void *))SLOT(stream, 4))(stream, ctx);
            }
        }
        if (stream) com_release(stream);
        TR("read %llu bytes of a font file through the program's loader (%s)", (unsigned long long)n, f->bytes ? "ok" : "failed");
    }
    unlock();
    if (!f->bytes) return 0;
    *data = CFDataGetBytePtr(f->bytes); *size = (size_t)CFDataGetLength(f->bytes);
    return 1;
}
static CFDataRef file_cfdata(dw_file *f) {           // the bytes as CFData (retained), for CoreGraphics
    const uint8_t *d; size_t n;
    if (!file_bytes(f, &d, &n)) return NULL;
    if (f->bytes) return CFRetain(f->bytes);
    return CFDataCreateWithBytesNoCopy(NULL, d, (CFIndex)n, kCFAllocatorNull);   // (mapped for good)
}
static WINAPI HRESULT file_GetReferenceKey(void *this, const void **key, uint32_t *size) {
    dw_file *f = this;
    if (!key || !size) return E_INVALIDARG;
    *key = f->key; *size = f->key_size;
    return S_OK;
}
static WINAPI HRESULT file_GetLoader(void *this, void **loader) {
    dw_file *f = this;
    if (!loader) return E_INVALIDARG;
    *loader = f->kind == F_LOCAL ? (void *)&g_local_loader : f->kind == F_MEMORY ? (void *)&g_memory_loader : f->loader;
    if (f->kind == F_CUSTOM) com_addref(*loader); else dw_addref(*loader);
    return S_OK;
}
static int face_type_of(int file_type) { return file_type == FILE_CFF ? FACE_CFF : file_type == FILE_TRUETYPE ? FACE_TRUETYPE : file_type == FILE_COLLECTION ? FACE_COLLECTION : FACE_UNKNOWN; }
static WINAPI HRESULT file_Analyze(void *this, int32_t *supported, uint32_t *file_type, uint32_t *face_type, uint32_t *faces) {
    dw_file *f = this;
    const uint8_t *d = NULL; size_t n = 0;
    int type = FILE_UNKNOWN;
    uint32_t count = file_bytes(f, &d, &n) ? sfnt_faces(d, n, &type) : 0;
    TR("IDWriteFontFile::Analyze: %u faces, type %d", count, type);
    if (supported) *supported = count > 0;
    if (file_type) *file_type = (uint32_t)type;
    if (face_type) *face_type = (uint32_t)face_type_of(type);
    if (faces) *faces = count;
    if (!d) return DWRITE_E_FILENOTFOUND;
    return S_OK;
}

// IDWriteFontFileStream over bytes that stay (a mapping, or bytes a file holds: the stream holds the file).
typedef struct { dw_obj o; const uint8_t *data; size_t size; uint64_t mtime; dw_obj *holder; } dw_stream;
static const GUID *const g_stream_iids[] = { &IID_IDWriteFontFileStream, NULL };
static void stream_destroy(dw_obj *o) { dw_stream *s = (dw_stream *)o; if (s->holder) dw_release(s->holder); free(s); }
static dw_stream *stream_new(const uint8_t *data, size_t size, uint64_t mtime, dw_obj *holder) {
    dw_stream *s = dw_new(sizeof *s, g_stream_vtbl, g_stream_iids, "IDWriteFontFileStream", stream_destroy);
    s->data = data; s->size = size; s->mtime = mtime; s->holder = holder;
    if (holder) dw_addref(holder);
    return s;
}
static WINAPI HRESULT stream_ReadFileFragment(void *this, const void **start, uint64_t offset, uint64_t size, void **ctx) {
    dw_stream *s = this;
    if (!start || !ctx) return E_INVALIDARG;
    *ctx = NULL; *start = NULL;
    if (offset > s->size || size > s->size - offset) return E_FAIL;
    *start = s->data + offset;
    return S_OK;
}
static WINAPI void stream_ReleaseFileFragment(void *this, void *ctx) { (void)this; (void)ctx; }
static WINAPI HRESULT stream_GetFileSize(void *this, uint64_t *size) { if (!size) return E_INVALIDARG; *size = ((dw_stream *)this)->size; return S_OK; }
static WINAPI HRESULT stream_GetLastWriteTime(void *this, uint64_t *t) { if (!t) return E_INVALIDARG; *t = ((dw_stream *)this)->mtime; return S_OK; }

// IDWriteLocalFontFileLoader
static const GUID *const g_local_loader_iids[] = { &IID_IDWriteFontFileLoader, &IID_IDWriteLocalFontFileLoader, NULL };
static WINAPI HRESULT local_CreateStreamFromKey(void *this, const void *key, uint32_t size, void **out) {
    (void)this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    char path[1024];
    if (!key_path(key, size, path, sizeof path)) return DWRITE_E_FILENOTFOUND;
    mapping *m = map_file(path);
    if (!m) return DWRITE_E_FILENOTFOUND;
    TR("IDWriteLocalFontFileLoader::CreateStreamFromKey(%s)", path);
    *out = stream_new(m->base, m->size, m->mtime, NULL);
    return S_OK;
}
static WINAPI HRESULT local_GetFilePathLengthFromKey(void *this, const void *key, uint32_t size, uint32_t *len) {
    (void)this;
    if (!key || size < 10 || !len) return E_INVALIDARG;
    const WCHAR *w = (const WCHAR *)((const uint8_t *)key + 8);
    uint32_t n = (size - 8) / 2;
    while (n && !w[n - 1]) n--;
    *len = n;
    return S_OK;
}
static WINAPI HRESULT local_GetFilePathFromKey(void *this, const void *key, uint32_t size, WCHAR *path, uint32_t cap) {
    uint32_t n;
    HRESULT hr = local_GetFilePathLengthFromKey(this, key, size, &n);
    if (hr < 0) return hr;
    if (!path || cap < n + 1) return E_NOT_SUFFICIENT_BUFFER;
    memcpy(path, (const uint8_t *)key + 8, 2 * n);
    path[n] = 0;
    TR("IDWriteLocalFontFileLoader::GetFilePathFromKey");
    return S_OK;
}
static WINAPI HRESULT local_GetLastWriteTimeFromKey(void *this, const void *key, uint32_t size, uint64_t *t) {
    (void)this;
    if (!key || size < 8 || !t) return E_INVALIDARG;
    memcpy(t, key, 8);
    return S_OK;
}
// IDWriteFontFileLoader of our own bytes: the key is the file object's address.
static dw_file *g_memory_files[256];
static int g_memory_count;
static WINAPI HRESULT memory_CreateStreamFromKey(void *this, const void *key, uint32_t size, void **out) {
    (void)this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!key || size != sizeof(void *)) return E_INVALIDARG;
    dw_file *f; memcpy(&f, key, sizeof f);
    lock();
    int known = 0;
    for (int i = 0; i < g_memory_count && !known; i++) known = g_memory_files[i] == f;
    unlock();
    if (!known) return E_INVALIDARG;
    *out = stream_new(CFDataGetBytePtr(f->bytes), (size_t)CFDataGetLength(f->bytes), 0, &f->o);
    return S_OK;
}

// ---- collections, families, fonts, faces ------------------------------------------------------------------------------------
typedef struct dw_face dw_face;
typedef struct dw_font dw_font;
typedef struct dw_family dw_family;
typedef struct dw_collection dw_collection;
struct dw_face {
    dw_obj o;                            // IDWriteFontFace2
    CTFontRef ct;                        // at the size of the em square: CoreText's numbers are design units
    uint32_t upem, index;
    int type, sims;
    dw_file *file;                       // (held) NULL until asked for (system fonts) or when none can be had
    const uint8_t *data; size_t size; long dir;   // the file's bytes and this face's table directory (dir < 0: none, tables from CoreText)
    dw_font *font;                       // a collection's font this face is (GetFontFromFontFace), or NULL
    char *path;                          // system fonts: the file CoreText has the face from
    int ready; font_metrics m;
    const uint8_t *hmtx, *vmtx; uint32_t hmtx_len, vmtx_len; uint16_t nh, nv, glyphs; int has_vertical;
    CFDataRef tables[2];                 // hmtx, vmtx when CoreText copied them
    int symbol, fixed_pitch;
    uint8_t panose[10];
    char ps[128];
};
struct dw_font {
    dw_obj o;                            // IDWriteFont2 (counted by the collection)
    dw_family *family;
    CTFontDescriptorRef desc;            // system fonts (retained)
    dw_face *face;                       // (held) made on demand for system fonts
    char ps[128];
    int weight, stretch, style, sims, props;
    dw_font *base;                       // a simulated font: the real one
};
struct dw_family {
    dw_obj o;                            // IDWriteFontFamily (counted by the collection)
    dw_collection *coll;
    CFStringRef name;
    dw_font **fonts; uint32_t count, cap;
    dw_font *simulated[16]; int nsimulated;
    int sorted;
};
struct dw_collection {
    dw_obj o;                            // IDWriteFontCollection
    int system;
    dw_family **families; uint32_t count, visible, cap;   // past `visible`: families only fallback found (system)
    struct { uint32_t family; WCHAR *name; } *aliases; uint32_t naliases; int aliases_built;   // localized names
};
static const GUID *const g_collection_iids[] = { &IID_IDWriteFontCollection, NULL };
static const GUID *const g_family_iids[] = { &IID_IDWriteFontList, &IID_IDWriteFontFamily, NULL };
static const GUID *const g_font_iids[] = { &IID_IDWriteFont, &IID_IDWriteFont1, &IID_IDWriteFont2, NULL };
static const GUID *const g_face_iids[] = { &IID_IDWriteFontFace, &IID_IDWriteFontFace1, &IID_IDWriteFontFace2, NULL };
static const GUID *const g_list_iids[] = { &IID_IDWriteFontList, NULL };
static dw_collection *g_system;

// A table of a face: from its file's bytes, else a copy CoreText makes (*keep: to release after use).
static int face_table(dw_face *f, uint32_t tag, const uint8_t **out, uint32_t *len, CFDataRef *keep) {
    *keep = NULL;
    if (f->dir >= 0) return sfnt_table(f->data, f->size, f->dir, tag, out, len);
    CFDataRef d = CTFontCopyTable(f->ct, tag, kCTFontTableOptionNoOptions);
    if (!d) return 0;
    *out = CFDataGetBytePtr(d); *len = (uint32_t)CFDataGetLength(d); *keep = d;
    return 1;
}
#define TABLE(f, tag, p, l, k) (face_table((f), (tag), &(p), &(l), &(k)))
static void unkeep(CFDataRef k) { if (k) CFRelease(k); }

// Metrics (DWRITE_FONT_METRICS1) from head, hhea, OS/2 and post. Ascent and descent are OS/2's typographic values
// when the font asks for them (USE_TYPO_METRICS), else its Windows ones (the OS/2 specification's recommendation).
static void face_prepare(dw_face *f) {
    if (READY(f->ready)) return;
    lock();
    if (f->ready) { unlock(); return; }
    font_metrics *m = &f->m;
    memset(m, 0, sizeof *m);
    const uint8_t *t; uint32_t l; CFDataRef k = NULL;
    m->upem = (uint16_t)f->upem;
    int16_t hasc = (int16_t)CTFontGetAscent(f->ct), hdesc = -(int16_t)CTFontGetDescent(f->ct), hgap = (int16_t)CTFontGetLeading(f->ct);
    if (TABLE(f, TAG('h', 'h', 'e', 'a'), t, l, k) && l >= 36) {
        hasc = bes16(t + 4); hdesc = bes16(t + 6); hgap = bes16(t + 8);
        f->nh = be16(t + 34);
    }
    unkeep(k);
    if (TABLE(f, TAG('h', 'e', 'a', 'd'), t, l, k) && l >= 54) {
        m->box_left = bes16(t + 36); m->box_bottom = bes16(t + 38); m->box_right = bes16(t + 40); m->box_top = bes16(t + 42);
    }
    unkeep(k);
    m->ascent = (uint16_t)(hasc > 0 ? hasc : 0); m->descent = (uint16_t)(hdesc < 0 ? -hdesc : hdesc); m->line_gap = hgap;
    m->cap_height = (uint16_t)lround(CTFontGetCapHeight(f->ct)); m->x_height = (uint16_t)lround(CTFontGetXHeight(f->ct));
    m->strike_thick = (uint16_t)(f->upem / 20); m->strike_pos = (int16_t)(m->x_height / 2);
    if (TABLE(f, TAG('O', 'S', '/', '2'), t, l, k) && l >= 78) {
        uint16_t version = be16(t), fs = be16(t + 62);
        m->sub_w = bes16(t + 10); m->sub_h = bes16(t + 12); m->sub_x = bes16(t + 14); m->sub_y = (int16_t)-bes16(t + 16);
        m->sup_w = bes16(t + 18); m->sup_h = bes16(t + 20); m->sup_x = bes16(t + 22); m->sup_y = bes16(t + 24);
        m->strike_thick = be16(t + 26); m->strike_pos = bes16(t + 28);
        memcpy(f->panose, t + 32, 10);
        if (fs & 0x80) {                                           // USE_TYPO_METRICS
            m->ascent = (uint16_t)bes16(t + 68); m->descent = (uint16_t)-bes16(t + 70); m->line_gap = bes16(t + 72);
            m->has_typo_metrics = 1;
        } else {
            m->ascent = be16(t + 74); m->descent = be16(t + 76);     // (line gap: hhea's)
        }
        if (version >= 2 && l >= 90) { m->x_height = (uint16_t)bes16(t + 86); m->cap_height = (uint16_t)bes16(t + 88); }
    }
    unkeep(k);
    if (TABLE(f, TAG('p', 'o', 's', 't'), t, l, k) && l >= 16) {
        m->underline_pos = bes16(t + 8); m->underline_thick = (uint16_t)bes16(t + 10);
        f->fixed_pitch = be32(t + 12) != 0;
    } else {
        m->underline_pos = (int16_t)lround(CTFontGetUnderlinePosition(f->ct)); m->underline_thick = (uint16_t)lround(CTFontGetUnderlineThickness(f->ct));
    }
    unkeep(k);
    if (TABLE(f, TAG('c', 'm', 'a', 'p'), t, l, k) && l >= 4) {
        uint16_t n = be16(t + 2);
        for (uint16_t i = 0; i < n && 4 + 8u * (i + 1) <= l; i++) if (be16(t + 4 + 8 * i) == 3 && be16(t + 6 + 8 * i) == 0) f->symbol = 1;
    }
    unkeep(k);
    if (TABLE(f, TAG('v', 'h', 'e', 'a'), t, l, k) && l >= 36) { f->nv = be16(t + 34); f->has_vertical = 1; }
    unkeep(k);
    f->glyphs = (uint16_t)CTFontGetGlyphCount(f->ct);
    if (TABLE(f, TAG('h', 'm', 't', 'x'), t, l, k)) { f->hmtx = t; f->hmtx_len = l; f->tables[0] = k; }
    if (f->has_vertical && TABLE(f, TAG('v', 'm', 't', 'x'), t, l, k)) { f->vmtx = t; f->vmtx_len = l; f->tables[1] = k; }
    else f->has_vertical = 0;
    PUBLISH(f->ready, 1);
    unlock();
}
// Design advance from hmtx (CoreText's when the table is missing).
static uint32_t face_advance(dw_face *f, uint16_t g, int16_t *lsb) {
    face_prepare(f);
    if (f->hmtx && f->nh) {
        uint32_t i = g < f->nh ? g : f->nh - 1u;
        if (4u * i + 4 <= f->hmtx_len) {
            if (lsb) {
                if (g < f->nh) *lsb = bes16(f->hmtx + 4 * i + 2);
                else { uint32_t o = 4u * f->nh + 2u * (g - f->nh); *lsb = o + 2 <= f->hmtx_len ? bes16(f->hmtx + o) : 0; }
            }
            return be16(f->hmtx + 4 * i);
        }
    }
    CGGlyph cg = g; CGSize adv;
    CTFontGetAdvancesForGlyphs(f->ct, kCTFontOrientationHorizontal, &cg, &adv, 1);
    if (lsb) *lsb = 0;
    return (uint32_t)lround(adv.width);
}
static void face_glyph_metrics(dw_face *f, uint16_t g, glyph_metrics *out) {
    face_prepare(f);
    int16_t lsb;
    uint32_t adv = face_advance(f, g, &lsb);
    CGGlyph cg = g;
    CGRect r = CTFontGetBoundingRectsForGlyphs(f->ct, kCTFontOrientationHorizontal, &cg, NULL, 1);
    int empty = r.size.width <= 0 || r.size.height <= 0;
    double x0 = empty ? lsb : floor(r.origin.x), x1 = empty ? lsb : ceil(r.origin.x + r.size.width);
    double y0 = empty ? 0 : floor(r.origin.y), y1 = empty ? 0 : ceil(r.origin.y + r.size.height);
    if (f->sims & SIM_OBLIQUE && !empty) { x1 += ceil(y1 > 0 ? y1 * 0.33 : 0); x0 += floor(y0 < 0 ? y0 * 0.33 : 0); }
    out->advance = adv;
    out->lsb = (int32_t)x0;
    out->rsb = (int32_t)adv - (int32_t)x1;
    int32_t vorigin = f->m.ascent;
    uint32_t vadv = (uint32_t)(f->m.ascent + f->m.descent);
    if (f->has_vertical && f->nv) {
        uint32_t i = g < f->nv ? g : f->nv - 1u;
        if (4u * i + 4 <= f->vmtx_len) {
            vadv = be16(f->vmtx + 4 * i);
            int16_t tsb = g < f->nv ? bes16(f->vmtx + 4 * i + 2) : 0;
            if (!empty && g < f->nv) vorigin = (int32_t)y1 + tsb;
        }
    }
    out->advance_height = vadv;
    out->vorigin = vorigin;
    out->tsb = empty ? 0 : vorigin - (int32_t)y1;
    out->bsb = empty ? (int32_t)vadv : (int32_t)vadv - (vorigin - (int32_t)y0);
}
static uint16_t face_glyph(dw_face *f, uint32_t cp) {
    UniChar u[2]; CGGlyph g[2] = { 0, 0 };
    int n = 1;
    if (cp > 0x10FFFF) return 0;
    if (cp >= 0x10000) { u[0] = (UniChar)(0xD800 + ((cp - 0x10000) >> 10)); u[1] = (UniChar)(0xDC00 + (cp & 0x3FF)); n = 2; }
    else u[0] = (UniChar)cp;
    CTFontGetGlyphsForCharacters(f->ct, u, g, n);
    return g[0];
}

static dw_face *face_alloc(void) { return dw_new(sizeof(dw_face), g_face_vtbl, g_face_iids, "IDWriteFontFace", NULL); }
static void face_destroy(dw_obj *o) {
    dw_face *f = (dw_face *)o;
    if (f->ct) CFRelease(f->ct);
    for (int i = 0; i < 2; i++) if (f->tables[i]) CFRelease(f->tables[i]);
    if (f->file) dw_release(f->file);
    free(f->path);
    free(f);
}
static void name_ps(const uint8_t *d, size_t n, long dir, char *out, size_t cap) {
    const uint8_t *t; uint32_t l;
    name_entry e[4];
    out[0] = 0;
    if (!sfnt_table(d, n, dir, TAG('n', 'a', 'm', 'e'), &t, &l)) return;
    int c = name_strings(t, l, 6, e, 4);
    if (c) { size_t k = vx_utf16_to_utf8(e[0].text, e[0].len, out, cap - 1); out[k < cap ? k : cap - 1] = 0; }
    for (int i = 0; i < c; i++) free(e[i].text);
}
// A face of a font file (the factory's CreateFontFace, custom collections): a CGFont made from the face's bytes.
static HRESULT face_from_file(dw_file *file, uint32_t index, int sims, dw_face **out) {
    *out = NULL;
    const uint8_t *d; size_t n;
    if (!file_bytes(file, &d, &n)) return file->kind == F_LOCAL ? DWRITE_E_FILENOTFOUND : DWRITE_E_FILEFORMAT;
    int type;
    uint32_t faces = sfnt_faces(d, n, &type);
    long dir = sfnt_dir(d, n, index);
    if (!faces || dir < 0) return DWRITE_E_FILEFORMAT;
    CFDataRef data;
    if (type == FILE_COLLECTION) {
        size_t sz; uint8_t *one = sfnt_extract(d, n, dir, &sz);
        data = CFDataCreateWithBytesNoCopy(NULL, one, (CFIndex)sz, kCFAllocatorMalloc);
    } else data = file_cfdata(file);
    if (!data) return DWRITE_E_FILEFORMAT;
    CGDataProviderRef p = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    CGFontRef cg = CGFontCreateWithDataProvider(p);
    CGDataProviderRelease(p);
    if (!cg) return DWRITE_E_FILEFORMAT;
    dw_face *f = face_alloc();
    f->o.destroy = face_destroy;
    f->upem = CGFontGetUnitsPerEm(cg);
    if (!f->upem) f->upem = 1000;
    f->ct = CTFontCreateWithGraphicsFont(cg, f->upem, NULL, NULL);
    CGFontRelease(cg);
    f->index = index; f->sims = sims; f->type = face_type_of(type);
    f->file = file; dw_addref(file);
    f->data = d; f->size = n; f->dir = dir;
    name_ps(d, n, dir, f->ps, sizeof f->ps);
    if (!f->ps[0]) { CFStringRef s = CTFontCopyPostScriptName(f->ct); if (s) { CFStringGetCString(s, f->ps, sizeof f->ps, kCFStringEncodingUTF8); CFRelease(s); } }
    *out = f;
    return S_OK;
}
static void cf_utf8(CFStringRef s, char *out, size_t cap) { out[0] = 0; if (s) CFStringGetCString(s, out, (CFIndex)cap, kCFStringEncodingUTF8); }
// A system font's face: the CTFont from its descriptor, and its file mapped when it can be read.
static dw_face *face_from_descriptor(CTFontDescriptorRef desc, int sims) {
    const void *k[] = { kCTFontOpticalSizeAttribute }, *v[] = { CFSTR("none") };   // (sizes are not the em: no optical sizing)
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CTFontDescriptorRef d2 = CTFontDescriptorCreateCopyWithAttributes(desc, attrs);
    CFRelease(attrs);
    CTFontRef probe = CTFontCreateWithFontDescriptor(d2, 12.0, NULL);
    if (!probe) { CFRelease(d2); return NULL; }
    dw_face *f = face_alloc();
    f->o.destroy = face_destroy;
    f->upem = CTFontGetUnitsPerEm(probe);
    if (!f->upem) f->upem = 1000;
    CFRelease(probe);
    f->ct = CTFontCreateWithFontDescriptor(d2, f->upem, NULL);
    CFRelease(d2);
    f->sims = sims; f->dir = -1;
    CFStringRef ps = CTFontCopyPostScriptName(f->ct);
    cf_utf8(ps, f->ps, sizeof f->ps);
    if (ps) CFRelease(ps);
    CFURLRef url = CTFontDescriptorCopyAttribute(desc, kCTFontURLAttribute);
    char path[1024];
    if (url && CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, sizeof path)) {
        mapping *m = map_file(path);
        int type;
        uint32_t faces = m ? sfnt_faces(m->base, m->size, &type) : 0;
        for (uint32_t i = 0; i < faces; i++) {
            long dir = sfnt_dir(m->base, m->size, i);
            char name[128];
            if (dir < 0) continue;
            if (faces > 1) { name_ps(m->base, m->size, dir, name, sizeof name); if (strcmp(name, f->ps)) continue; }
            f->data = m->base; f->size = m->size; f->dir = dir; f->index = i; f->type = face_type_of(type);
            f->path = strdup(path);
            break;
        }
    }
    if (url) CFRelease(url);
    if (f->dir < 0) {
        CFDataRef cff = CTFontCopyTable(f->ct, TAG('C', 'F', 'F', ' '), kCTFontTableOptionNoOptions);
        f->type = cff ? FACE_CFF : FACE_TRUETYPE;
        if (cff) CFRelease(cff);
    }
    return f;
}
// The file of a system font's face: its own, or (no readable file) one rebuilt from CoreText's tables.
static dw_file *face_file(dw_face *f) {
    lock();
    if (!f->file && f->path) f->file = local_file(f->path);
    if (!f->file && g_memory_count < (int)(sizeof g_memory_files / sizeof g_memory_files[0])) {
        CFArrayRef tags = CTFontCopyAvailableTables(f->ct, kCTFontTableOptionNoOptions);
        CFIndex n = tags ? CFArrayGetCount(tags) : 0;
        sfnt_part *parts = calloc(n ? (size_t)n : 1, sizeof *parts);
        CFDataRef *keep = calloc(n ? (size_t)n : 1, sizeof *keep);
        int k = 0;
        for (CFIndex i = 0; i < n; i++) {
            uint32_t tag = (uint32_t)(uintptr_t)CFArrayGetValueAtIndex(tags, i);
            CFDataRef d = CTFontCopyTable(f->ct, tag, kCTFontTableOptionNoOptions);
            if (d) { keep[k] = d; parts[k] = (sfnt_part){ tag, CFDataGetBytePtr(d), (uint32_t)CFDataGetLength(d) }; k++; }
        }
        size_t size;
        uint8_t *bytes = sfnt_build(f->type == FACE_CFF ? TAG('O', 'T', 'T', 'O') : 0x00010000, parts, k, &size);
        for (int i = 0; i < k; i++) CFRelease(keep[i]);
        free(keep); free(parts);
        if (tags) CFRelease(tags);
        dw_file *file = dw_new(sizeof *file, g_file_vtbl, g_file_iids, "IDWriteFontFile", NULL);
        file->kind = F_MEMORY;
        file->bytes = CFDataCreateWithBytesNoCopy(NULL, bytes, (CFIndex)size, kCFAllocatorMalloc);
        file->key_size = sizeof(void *);
        file->key = malloc(sizeof(void *));
        memcpy(file->key, &file, sizeof(void *));
        g_memory_files[g_memory_count++] = file;
        f->file = file;
        f->index = 0;
    }
    unlock();
    return f->file;
}

// Font properties (weight, stretch, style) from OS/2, else CoreText's traits.
static int weight_from_trait(double w) {
    static const double edge[] = { -0.7, -0.5, -0.23, 0.1, 0.25, 0.35, 0.5, 0.6 };
    int i = 0;
    while (i < 8 && w > edge[i]) i++;
    return 100 * (i + 1);
}
static dw_face *font_face(dw_font *ft);
static void font_props(dw_font *ft) {
    if (READY(ft->props)) return;
    dw_font *real = ft->base ? ft->base : ft;
    dw_face *f = font_face(real);
    int weight = 400, stretch = 5, style = STYLE_NORMAL, have = 0;
    const uint8_t *t; uint32_t l; CFDataRef k = NULL;
    if (f && TABLE(f, TAG('O', 'S', '/', '2'), t, l, k) && l >= 64) {
        weight = be16(t + 4); stretch = be16(t + 6);
        uint16_t fs = be16(t + 62);
        if (weight > 0 && weight < 10) weight *= 100;                  // (some old fonts give 1..9)
        if (weight < 1 || weight > 999) weight = 400;
        if (stretch < 1 || stretch > 9) stretch = 5;
        style = fs & 1 ? STYLE_ITALIC : (be16(t) >= 4 && fs & 0x200) ? STYLE_OBLIQUE : STYLE_NORMAL;
        have = 1;
    }
    if (f) unkeep(k);
    if (!have && f) {
        CFDictionaryRef traits = CTFontCopyTraits(f->ct);
        CFNumberRef w = traits ? CFDictionaryGetValue(traits, kCTFontWeightTrait) : NULL, wd = traits ? CFDictionaryGetValue(traits, kCTFontWidthTrait) : NULL;
        double v;
        if (w && CFNumberGetValue(w, kCFNumberDoubleType, &v)) weight = weight_from_trait(v);
        if (wd && CFNumberGetValue(wd, kCFNumberDoubleType, &v)) stretch = (int)lround(5 + v * 4);
        if (CTFontGetSymbolicTraits(f->ct) & kCTFontTraitItalic) style = STYLE_ITALIC;
        if (traits) CFRelease(traits);
    }
    if (ft->sims & SIM_BOLD && weight < 700) weight = 700;
    if (ft->sims & SIM_OBLIQUE) style = STYLE_OBLIQUE;
    ft->weight = weight; ft->stretch = stretch; ft->style = style;
    PUBLISH(ft->props, 1);
}
// The face of a font, made the first time it is asked for.
static dw_face *font_face(dw_font *ft) {
    dw_face *made = READY(ft->face);
    if (made) return made;
    lock();
    if (!ft->face) {
        dw_face *f = NULL;
        if (ft->desc) f = face_from_descriptor(ft->desc, ft->sims);
        else if (ft->base) {
            dw_face *b = font_face(ft->base);
            if (b && b->file) face_from_file(b->file, b->index, ft->sims, &f);
            else if (b && ft->base->desc) f = face_from_descriptor(ft->base->desc, ft->sims);
        }
        if (f) f->font = ft;
        PUBLISH(ft->face, f);
    }
    unlock();
    return ft->face;
}
static dw_font *font_new(dw_family *fam, CTFontDescriptorRef desc, dw_face *face) {
    dw_font *ft = dw_new(sizeof *ft, g_font_vtbl, g_font_iids, "IDWriteFont", NULL);
    ft->o.owner = &fam->coll->o;
    ft->family = fam;
    if (desc) { ft->desc = CFRetain(desc); CFStringRef ps = CTFontDescriptorCopyAttribute(desc, kCTFontNameAttribute); cf_utf8(ps, ft->ps, sizeof ft->ps); if (ps) CFRelease(ps); }
    if (face) { ft->face = face; face->font = ft; snprintf(ft->ps, sizeof ft->ps, "%s", face->ps); }
    if (fam->count == fam->cap) { fam->cap = fam->cap ? fam->cap * 2 : 4; fam->fonts = realloc(fam->fonts, fam->cap * sizeof *fam->fonts); }
    fam->fonts[fam->count++] = ft;
    return ft;
}
static dw_family *family_new(dw_collection *c, CFStringRef name, int visible) {
    if (c->count == c->cap) return NULL;
    dw_family *fam = dw_new(sizeof *fam, g_family_vtbl, g_family_iids, "IDWriteFontFamily", NULL);
    fam->o.owner = &c->o;
    fam->coll = c;
    fam->name = CFRetain(name);
    c->families[c->count++] = fam;
    if (visible) c->visible = c->count;
    return fam;
}
static void collection_destroy(dw_obj *o) {
    dw_collection *c = (dw_collection *)o;
    lock();
    for (uint32_t i = 0; i < c->count; i++) {
        dw_family *fam = c->families[i];
        for (uint32_t j = 0; j < fam->count + (uint32_t)fam->nsimulated; j++) {
            dw_font *ft = j < fam->count ? fam->fonts[j] : fam->simulated[j - fam->count];
            if (ft->face) { ft->face->font = NULL; dw_release(ft->face); }
            if (ft->desc) CFRelease(ft->desc);
            free(ft);
        }
        CFRelease(fam->name);
        free(fam->fonts); free(fam);
    }
    for (uint32_t i = 0; i < c->naliases; i++) free(c->aliases[i].name);
    unlock();
    free(c->aliases); free(c->families); free(c);
}
static dw_collection *collection_new(int system, uint32_t cap) {
    dw_collection *c = dw_new(sizeof *c, g_collection_vtbl, g_collection_iids, "IDWriteFontCollection", system ? NULL : collection_destroy);
    c->system = system;
    c->cap = cap;
    c->families = calloc(cap ? cap : 1, sizeof *c->families);
    return c;
}
static CFComparisonResult compare_families(const void *a, const void *b) {
    return CFStringCompare((*(dw_family *const *)a)->name, (*(dw_family *const *)b)->name, kCFCompareCaseInsensitive);
}
static int sort_families(const void *a, const void *b) { return (int)compare_families(a, b); }
static dw_family *family_named(dw_collection *c, CFStringRef name, uint32_t upto) {
    for (uint32_t i = 0; i < upto; i++)
        if (CFStringCompare(c->families[i]->name, name, kCFCompareCaseInsensitive) == kCFCompareEqualTo) return c->families[i];
    return NULL;
}
// The system collection: CoreText's installed fonts by family (a PostScript name once: CoreText lists some fonts in
// two places).
static void system_collection_build(void) {
    CTFontCollectionRef all = CTFontCollectionCreateFromAvailableFonts(NULL);
    CFArrayRef descs = all ? CTFontCollectionCreateMatchingFontDescriptors(all) : NULL;
    CFIndex n = descs ? CFArrayGetCount(descs) : 0;
    dw_collection *c = collection_new(1, (uint32_t)n + 512);
    CFMutableDictionaryRef by_family = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, NULL);
    CFMutableSetRef seen = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
    for (CFIndex i = 0; i < n; i++) {
        CTFontDescriptorRef d = CFArrayGetValueAtIndex(descs, i);
        CFStringRef fam = CTFontDescriptorCopyAttribute(d, kCTFontFamilyNameAttribute), ps = CTFontDescriptorCopyAttribute(d, kCTFontNameAttribute);
        if (fam && ps && !CFSetContainsValue(seen, ps) && CFStringGetLength(fam) && CFStringGetCharacterAtIndex(fam, 0) != '.') {
            CFSetAddValue(seen, ps);
            dw_family *f = (dw_family *)CFDictionaryGetValue(by_family, fam);
            if (!f && (f = family_new(c, fam, 1))) CFDictionarySetValue(by_family, fam, f);
            if (f) font_new(f, d, NULL);
        }
        if (fam) CFRelease(fam);
        if (ps) CFRelease(ps);
    }
    qsort(c->families, c->count, sizeof *c->families, sort_families);
    c->visible = c->count;
    CFRelease(seen); CFRelease(by_family);
    if (descs) CFRelease(descs);
    if (all) CFRelease(all);
    if (g_trace) vx_log("DirectWrite: system font collection: %u families, %ld faces listed by CoreText", c->count, (long)n);
    g_system = c;
}
static dw_collection *system_collection(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, system_collection_build);
    return g_system;
}

// The order of a family's fonts: by stretch, style and weight.
static int compare_fonts(const void *a, const void *b) {
    dw_font *x = *(dw_font *const *)a, *y = *(dw_font *const *)b;
    if (x->stretch != y->stretch) return x->stretch - y->stretch;
    if (x->style != y->style) return x->style - y->style;
    return x->weight - y->weight;
}
static void family_sort(dw_family *fam) {
    if (READY(fam->sorted)) return;
    lock();
    if (!fam->sorted) {
        for (uint32_t i = 0; i < fam->count; i++) font_props(fam->fonts[i]);
        qsort(fam->fonts, fam->count, sizeof *fam->fonts, compare_fonts);
        PUBLISH(fam->sorted, 1);
    }
    unlock();
}
// Matching (the CSS Fonts font matching algorithm): stretch first, then style, then weight.
static int stretch_rank(int want, int s) {
    if (want < 1 || want > 9) want = 5;
    if (want <= 5) return s <= want ? want - s : 100 + s - want;
    return s >= want ? s - want : 100 + want - s;
}
static int style_rank(int want, int s) {         // the same style, then italic for oblique (and back), upright last
    if (s == want) return 0;
    if (want == STYLE_NORMAL) return s == STYLE_OBLIQUE ? 1 : 2;
    return s == STYLE_NORMAL ? 2 : 1;
}
static int weight_rank(int want, int w) {
    if (want >= 400 && want <= 500) {
        if (w >= want && w <= 500) return w - want;
        if (w < want) return 1000 + want - w;
        return 2000 + w - 500;
    }
    if (want < 400) return w <= want ? want - w : 1000 + w - want;
    return w >= want ? w - want : 1000 + want - w;
}
static long match_rank(dw_font *ft, int weight, int stretch, int style) {
    font_props(ft);
    return (long)stretch_rank(stretch, ft->stretch) * 10000000L + (long)style_rank(style, ft->style) * 100000L + weight_rank(weight, ft->weight);
}
static dw_font *family_match(dw_family *fam, int weight, int stretch, int style) {
    family_sort(fam);
    dw_font *best = NULL; long br = 0;
    for (uint32_t i = 0; i < fam->count; i++) {
        long r = match_rank(fam->fonts[i], weight, stretch, style);
        if (!best || r < br) { best = fam->fonts[i]; br = r; }
    }
    if (!best) return NULL;
    // Simulations: a bold for a weight of 600 or more from a face of 500 or less, an oblique for a slant from an upright one.
    int sims = (weight >= 600 && best->weight <= 500 ? SIM_BOLD : 0) | (style != STYLE_NORMAL && best->style == STYLE_NORMAL ? SIM_OBLIQUE : 0);
    if (!sims) return best;
    lock();
    dw_font *sim = NULL;
    for (int i = 0; i < fam->nsimulated && !sim; i++) if (fam->simulated[i]->base == best && fam->simulated[i]->sims == sims) sim = fam->simulated[i];
    if (!sim && fam->nsimulated < 16) {
        sim = dw_new(sizeof *sim, g_font_vtbl, g_font_iids, "IDWriteFont", NULL);
        sim->o.owner = &fam->coll->o;
        sim->family = fam; sim->base = best; sim->sims = sims;
        snprintf(sim->ps, sizeof sim->ps, "%s", best->ps);
        fam->simulated[fam->nsimulated++] = sim;
    }
    unlock();
    return sim ? sim : best;
}

// ---- IDWriteLocalizedStrings --------------------------------------------------------------------------------------------
typedef struct { dw_obj o; int n; name_entry e[16]; } dw_strings;
static const GUID *const g_strings_iids[] = { &IID_IDWriteLocalizedStrings, NULL };
static void strings_destroy(dw_obj *o) { dw_strings *s = (dw_strings *)o; for (int i = 0; i < s->n; i++) free(s->e[i].text); free(s); }
static dw_strings *strings_new(void) { return dw_new(sizeof(dw_strings), g_strings_vtbl, g_strings_iids, "IDWriteLocalizedStrings", strings_destroy); }
static void strings_set_english(dw_strings *s, CFStringRef text) {      // the "en-us" entry, first
    if (!text) return;
    CFIndex n = CFStringGetLength(text);
    WCHAR buf[512];
    if (n > 511) n = 511;
    CFStringGetCharacters(text, CFRangeMake(0, n), buf);
    for (int i = 0; i < s->n; i++) if (!strcmp(s->e[i].locale, "en-us")) { free(s->e[i].text); memmove(s->e + i, s->e + i + 1, (size_t)(s->n - i - 1) * sizeof s->e[0]); s->n--; break; }
    if (s->n == 16) { free(s->e[15].text); s->n--; }
    memmove(s->e + 1, s->e, (size_t)s->n * sizeof s->e[0]);
    s->n++;
    memset(&s->e[0], 0, sizeof s->e[0]);
    add_name(s->e, 0, 1, "en-us", buf, (uint32_t)n);
}
// A name of a face in every language its name table has (the first ID found), English forced to `english` when given.
static dw_strings *face_names(dw_face *f, const int *ids, CFStringRef english) {
    dw_strings *s = strings_new();
    const uint8_t *t; uint32_t l; CFDataRef k = NULL;
    if (f && TABLE(f, TAG('n', 'a', 'm', 'e'), t, l, k))
        for (const int *id = ids; *id && !s->n; id++) s->n = name_strings(t, l, *id, s->e, 16);
    if (f) unkeep(k);
    if (english) strings_set_english(s, english);
    return s;
}
static WINAPI uint32_t strings_GetCount(void *this) { return (uint32_t)((dw_strings *)this)->n; }
static int wcsieq_ascii(const WCHAR *w, const char *a) {
    for (; *w && *a; w++, a++) if (tolower(*w < 128 ? *w : 0) != tolower((unsigned char)*a)) return 0;
    return !*w && !*a;
}
static WINAPI HRESULT strings_FindLocaleName(void *this, const WCHAR *locale, uint32_t *index, int32_t *exists) {
    dw_strings *s = this;
    if (!locale || !index || !exists) return E_INVALIDARG;
    *index = UINT32_MAX; *exists = 0;
    for (int i = 0; i < s->n; i++) if (wcsieq_ascii(locale, s->e[i].locale)) { *index = (uint32_t)i; *exists = 1; break; }
    return S_OK;
}
static WINAPI HRESULT strings_GetLocaleNameLength(void *this, uint32_t i, uint32_t *len) {
    dw_strings *s = this;
    if (!len) return E_INVALIDARG;
    if (i >= (uint32_t)s->n) { *len = 0; return E_INVALIDARG; }
    *len = (uint32_t)strlen(s->e[i].locale);
    return S_OK;
}
static WINAPI HRESULT strings_GetLocaleName(void *this, uint32_t i, WCHAR *out, uint32_t cap) {
    dw_strings *s = this;
    if (!out || i >= (uint32_t)s->n) { if (out && cap) out[0] = 0; return E_INVALIDARG; }
    size_t n = strlen(s->e[i].locale);
    if (cap < n + 1) { if (cap) out[0] = 0; return E_NOT_SUFFICIENT_BUFFER; }
    for (size_t k = 0; k <= n; k++) out[k] = (WCHAR)s->e[i].locale[k];
    return S_OK;
}
static WINAPI HRESULT strings_GetStringLength(void *this, uint32_t i, uint32_t *len) {
    dw_strings *s = this;
    if (!len) return E_INVALIDARG;
    if (i >= (uint32_t)s->n) { *len = 0; return E_INVALIDARG; }
    *len = s->e[i].len;
    return S_OK;
}
static WINAPI HRESULT strings_GetString(void *this, uint32_t i, WCHAR *out, uint32_t cap) {
    dw_strings *s = this;
    if (!out || i >= (uint32_t)s->n) { if (out && cap) out[0] = 0; return E_INVALIDARG; }
    if (cap < s->e[i].len + 1) { if (cap) out[0] = 0; return E_NOT_SUFFICIENT_BUFFER; }
    memcpy(out, s->e[i].text, 2 * (s->e[i].len + 1));
    return S_OK;
}

// ---- IDWriteFontCollection -------------------------------------------------------------------------------------------------
static WINAPI uint32_t coll_GetFontFamilyCount(void *this) { TR("IDWriteFontCollection::GetFontFamilyCount"); return ((dw_collection *)this)->visible; }
static WINAPI HRESULT coll_GetFontFamily(void *this, uint32_t i, void **out) {
    dw_collection *c = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (i >= c->visible) return E_INVALIDARG;
    dw_addref(c->families[i]);
    *out = c->families[i];
    return S_OK;
}
// Localized family names (the name tables' typographic or legacy family names), for names CoreText's do not match.
static void collection_aliases(dw_collection *c) {
    if (READY(c->aliases_built)) return;
    lock();
    if (!c->aliases_built) {
        static const int ids[] = { 16, 1, 21, 0 };
        for (uint32_t i = 0; i < c->visible; i++) {
            dw_family *fam = c->families[i];
            if (!fam->count) continue;
            dw_face *f = font_face(fam->fonts[0]);
            if (!f) continue;
            const uint8_t *t; uint32_t l; CFDataRef k = NULL;
            if (TABLE(f, TAG('n', 'a', 'm', 'e'), t, l, k)) {
                for (const int *id = ids; *id; id++) {
                    name_entry e[16];
                    int n = name_strings(t, l, *id, e, 16);
                    for (int j = 0; j < n; j++) {
                        c->aliases = realloc(c->aliases, (c->naliases + 1) * sizeof *c->aliases);
                        c->aliases[c->naliases].family = i;
                        c->aliases[c->naliases++].name = e[j].text;
                    }
                }
            }
            unkeep(k);
        }
        PUBLISH(c->aliases_built, 1);
    }
    unlock();
}
static int find_family(dw_collection *c, const WCHAR *name, uint32_t *index) {
    size_t n = vx_wcslen(name);
    CFStringRef s = CFStringCreateWithCharacters(NULL, name, (CFIndex)n);
    for (uint32_t i = 0; i < c->visible; i++)
        if (CFStringCompare(c->families[i]->name, s, kCFCompareCaseInsensitive) == kCFCompareEqualTo) { *index = i; CFRelease(s); return 1; }
    collection_aliases(c);
    int found = 0;
    for (uint32_t i = 0; i < c->naliases && !found; i++) {
        CFStringRef a = CFStringCreateWithCharactersNoCopy(NULL, c->aliases[i].name, (CFIndex)vx_wcslen(c->aliases[i].name), kCFAllocatorNull);
        if (CFStringCompare(a, s, kCFCompareCaseInsensitive) == kCFCompareEqualTo) { *index = c->aliases[i].family; found = 1; }
        CFRelease(a);
    }
    CFRelease(s);
    return found;
}
static WINAPI HRESULT coll_FindFamilyName(void *this, const WCHAR *name, uint32_t *index, int32_t *exists) {
    dw_collection *c = this;
    if (!name || !index || !exists) return E_INVALIDARG;
    *index = UINT32_MAX;
    *exists = find_family(c, name, index);
    if (g_trace) { char *a = vx_utf8_dup(name); TR("IDWriteFontCollection::FindFamilyName(%s) -> %s", a, *exists ? "found" : "not found"); free(a); }
    return S_OK;
}
static dw_font *collection_font_of_face(dw_collection *c, dw_face *f) {
    if (f->font && f->font->family->coll == c) return f->font;
    for (uint32_t i = 0; i < c->count; i++) {
        dw_family *fam = c->families[i];
        for (uint32_t j = 0; j < fam->count; j++) {
            dw_font *ft = fam->fonts[j];
            if (ft->face == f || (f->ps[0] && !strcmp(ft->ps, f->ps))) {
                if (!f->sims) return ft;
                for (int s = 0; s < fam->nsimulated; s++) if (fam->simulated[s]->base == ft && fam->simulated[s]->sims == f->sims) return fam->simulated[s];
                return ft;
            }
        }
    }
    return NULL;
}
static WINAPI HRESULT coll_GetFontFromFontFace(void *this, void *face, void **out) {
    dw_collection *c = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!is_ours(face, g_face_vtbl)) return E_INVALIDARG;
    lock();
    dw_font *ft = collection_font_of_face(c, face);
    unlock();
    TR("IDWriteFontCollection::GetFontFromFontFace(%s) -> %s", ((dw_face *)face)->ps, ft ? "found" : "not found");
    if (!ft) return DWRITE_E_NOFONT;
    dw_addref(ft);
    *out = ft;
    return S_OK;
}

// ---- IDWriteFontFamily, IDWriteFontList ------------------------------------------------------------------------------------
static WINAPI HRESULT fam_GetFontCollection(void *this, void **out) {
    if (!out) return E_INVALIDARG;
    dw_collection *c = ((dw_family *)this)->coll;
    dw_addref(c);
    *out = c;
    return S_OK;
}
static WINAPI uint32_t fam_GetFontCount(void *this) { return ((dw_family *)this)->count; }
static WINAPI HRESULT fam_GetFont(void *this, uint32_t i, void **out) {
    dw_family *fam = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (i >= fam->count) return E_INVALIDARG;
    family_sort(fam);
    dw_addref(fam->fonts[i]);
    *out = fam->fonts[i];
    return S_OK;
}
static WINAPI HRESULT fam_GetFamilyNames(void *this, void **out) {
    dw_family *fam = this;
    if (!out) return E_INVALIDARG;
    static const int ids[] = { 16, 1, 0 };
    *out = face_names(fam->count ? font_face(fam->fonts[0]) : NULL, ids, fam->name);
    return S_OK;
}
static WINAPI HRESULT fam_GetFirstMatchingFont(void *this, uint32_t weight, uint32_t stretch, uint32_t style, void **out) {
    dw_family *fam = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    dw_font *ft = family_match(fam, (int)weight, (int)stretch, (int)style);
    if (g_trace) { char n[128]; cf_utf8(fam->name, n, sizeof n); TR("IDWriteFontFamily::GetFirstMatchingFont(%s, %u, %u, %u) -> %s sims %d", n, weight, stretch, style, ft ? ft->ps : "none", ft ? ft->sims : 0); }
    if (!ft) return DWRITE_E_NOFONT;
    dw_addref(ft);
    *out = ft;
    return S_OK;
}
typedef struct { dw_obj o; dw_collection *coll; dw_font **fonts; uint32_t count; } dw_list;
static void list_destroy(dw_obj *o) { dw_list *l = (dw_list *)o; dw_release(l->coll); free(l->fonts); free(l); }
static int g_sort_weight, g_sort_stretch, g_sort_style;   // (under the lock)
static int compare_match(const void *a, const void *b) {
    long x = match_rank(*(dw_font *const *)a, g_sort_weight, g_sort_stretch, g_sort_style), y = match_rank(*(dw_font *const *)b, g_sort_weight, g_sort_stretch, g_sort_style);
    return x < y ? -1 : x > y;
}
static WINAPI HRESULT fam_GetMatchingFonts(void *this, uint32_t weight, uint32_t stretch, uint32_t style, void **out) {
    dw_family *fam = this;
    if (!out) return E_INVALIDARG;
    family_sort(fam);
    dw_list *l = dw_new(sizeof *l, g_list_vtbl, g_list_iids, "IDWriteFontList", list_destroy);
    l->coll = fam->coll; dw_addref(fam->coll);
    l->fonts = calloc(fam->count ? fam->count : 1, sizeof *l->fonts);
    memcpy(l->fonts, fam->fonts, fam->count * sizeof *l->fonts);
    l->count = fam->count;
    lock();
    g_sort_weight = (int)weight; g_sort_stretch = (int)stretch; g_sort_style = (int)style;
    qsort(l->fonts, l->count, sizeof *l->fonts, compare_match);
    unlock();
    *out = l;
    return S_OK;
}
static WINAPI HRESULT list_GetFontCollection(void *this, void **out) {
    if (!out) return E_INVALIDARG;
    dw_collection *c = ((dw_list *)this)->coll;
    dw_addref(c);
    *out = c;
    return S_OK;
}
static WINAPI uint32_t list_GetFontCount(void *this) { return ((dw_list *)this)->count; }
static WINAPI HRESULT list_GetFont(void *this, uint32_t i, void **out) {
    dw_list *l = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (i >= l->count) return E_INVALIDARG;
    dw_addref(l->fonts[i]);
    *out = l->fonts[i];
    return S_OK;
}

// ---- IDWriteFont ---------------------------------------------------------------------------------------------------------------
static WINAPI HRESULT font_GetFontFamily(void *this, void **out) {
    if (!out) return E_INVALIDARG;
    dw_family *fam = ((dw_font *)this)->family;
    dw_addref(fam);
    *out = fam;
    return S_OK;
}
static WINAPI uint32_t font_GetWeight(void *this) { font_props(this); return (uint32_t)((dw_font *)this)->weight; }
static WINAPI uint32_t font_GetStretch(void *this) { font_props(this); return (uint32_t)((dw_font *)this)->stretch; }
static WINAPI uint32_t font_GetStyle(void *this) { font_props(this); return (uint32_t)((dw_font *)this)->style; }
static WINAPI int32_t font_IsSymbolFont(void *this) { dw_face *f = font_face(this); if (!f) return 0; face_prepare(f); return f->symbol; }
static WINAPI HRESULT font_GetFaceNames(void *this, void **out) {
    dw_font *ft = this;
    if (!out) return E_INVALIDARG;
    dw_font *real = ft->base ? ft->base : ft;
    dw_face *f = font_face(real);
    static const int ids[] = { 17, 2, 0 };
    CFStringRef style = real->desc ? CTFontDescriptorCopyAttribute(real->desc, kCTFontStyleNameAttribute) : (f ? CTFontCopyName(f->ct, kCTFontStyleNameKey) : NULL);
    if (ft->sims) {                         // "Bold", "Oblique", "Bold Oblique" (a Regular base gives its place up)
        CFMutableStringRef s = CFStringCreateMutable(NULL, 0);
        if (style && CFStringCompare(style, CFSTR("Regular"), kCFCompareCaseInsensitive) && !(ft->sims & SIM_BOLD)) CFStringAppend(s, style);
        if (ft->sims & SIM_BOLD) CFStringAppend(s, CFSTR("Bold"));
        if (ft->sims & SIM_OBLIQUE) CFStringAppend(s, CFStringGetLength(s) ? CFSTR(" Oblique") : CFSTR("Oblique"));
        dw_strings *st = strings_new();
        strings_set_english(st, s);
        CFRelease(s);
        if (style) CFRelease(style);
        *out = st;
        return S_OK;
    }
    *out = face_names(f, ids, style);
    if (style) CFRelease(style);
    return S_OK;
}
static WINAPI HRESULT font_GetInformationalStrings(void *this, uint32_t id, void **out, int32_t *exists) {
    dw_font *ft = this;
    if (!out || !exists) return E_INVALIDARG;
    *out = NULL; *exists = 0;
    // DWRITE_INFORMATIONAL_STRING_ID -> the 'name' table's name ID (-1: none; the script tags of 20 and 21 are not offered)
    static const int ids[22] = { -1, 0, 5, 7, 8, 9, 12, 10, 11, 13, 14, 1, 2, 16, 17, 19, 4, 6, 20, 21, -1, -1 };
    if (id >= 22 || ids[id] < 0) return S_OK;
    dw_face *f = font_face(ft->base ? ft->base : ft);
    if (!f) return S_OK;
    dw_strings *s = strings_new();
    const uint8_t *t; uint32_t l; CFDataRef k = NULL;
    if (TABLE(f, TAG('n', 'a', 'm', 'e'), t, l, k)) s->n = name_strings(t, l, ids[id], s->e, 16);
    unkeep(k);
    if (!s->n) { dw_release(s); return S_OK; }
    *out = s; *exists = 1;
    return S_OK;
}
static WINAPI uint32_t font_GetSimulations(void *this) { return (uint32_t)((dw_font *)this)->sims; }
static WINAPI void font_GetMetrics(void *this, font_metrics *m) {
    dw_face *f = font_face(this);
    if (!m) return;
    if (!f) { memset(m, 0, FONT_METRICS0_SIZE); return; }
    face_prepare(f);
    memcpy(m, &f->m, FONT_METRICS0_SIZE);
}
static WINAPI void font_GetMetrics1(void *this, font_metrics *m) {
    dw_face *f = font_face(this);
    if (!m) return;
    if (!f) { memset(m, 0, sizeof *m); return; }
    face_prepare(f);
    *m = f->m;
}
static WINAPI HRESULT font_HasCharacter(void *this, uint32_t cp, int32_t *exists) {
    if (!exists) return E_INVALIDARG;
    dw_face *f = font_face(this);
    *exists = f && face_glyph(f, cp) != 0;
    return S_OK;
}
static WINAPI HRESULT font_CreateFontFace(void *this, void **out) {
    dw_font *ft = this;
    if (!out) return E_INVALIDARG;
    dw_face *f = font_face(ft);
    TR("IDWriteFont::CreateFontFace(%s) -> %s", ft->ps, f ? "ok" : "failed");
    *out = NULL;
    if (!f) return DWRITE_E_FILEFORMAT;
    dw_addref(f);
    *out = f;
    return S_OK;
}
static WINAPI void font_GetPanose(void *this, uint8_t *p) {
    dw_face *f = font_face(this);
    if (!p) return;
    memset(p, 0, 10);
    if (f) { face_prepare(f); memcpy(p, f->panose, 10); }
}
static HRESULT unicode_ranges(dw_face *f, uint32_t max, unicode_range *out, uint32_t *count);
static WINAPI HRESULT font_GetUnicodeRanges(void *this, uint32_t max, unicode_range *out, uint32_t *count) {
    dw_face *f = font_face(this);
    if (!count) return E_INVALIDARG;
    if (!f) { *count = 0; return S_OK; }
    return unicode_ranges(f, max, out, count);
}
static WINAPI int32_t font_IsMonospacedFont(void *this) { dw_face *f = font_face(this); if (!f) return 0; face_prepare(f); return f->fixed_pitch; }
static WINAPI int32_t font_IsColorFont(void *this) { (void)this; return 0; }

// ---- IDWriteFontFace ---------------------------------------------------------------------------------------------------------
static WINAPI uint32_t face_GetType(void *this) { return (uint32_t)((dw_face *)this)->type; }
static WINAPI HRESULT face_GetFiles(void *this, uint32_t *count, void **files) {
    dw_face *f = this;
    if (!count) return E_INVALIDARG;
    dw_file *file = face_file(f);
    TR("IDWriteFontFace::GetFiles(%s) -> %s", f->ps, file ? (file->kind == F_LOCAL ? file->unix_path : "bytes of our own") : "none");
    if (!files) { *count = file ? 1 : 0; return S_OK; }
    if (*count < 1) return E_INVALIDARG;
    if (!file) return E_FAIL;
    dw_addref(file);
    files[0] = file;
    *count = 1;
    return S_OK;
}
static WINAPI uint32_t face_GetIndex(void *this) { dw_face *f = this; face_file(f); return f->index; }
static WINAPI uint32_t face_GetSimulations(void *this) { return (uint32_t)((dw_face *)this)->sims; }
static WINAPI int32_t face_IsSymbolFont(void *this) { face_prepare(this); return ((dw_face *)this)->symbol; }
static WINAPI void face_GetMetrics(void *this, font_metrics *m) { face_prepare(this); if (m) memcpy(m, &((dw_face *)this)->m, FONT_METRICS0_SIZE); }
static WINAPI void face_GetMetrics1(void *this, font_metrics *m) { face_prepare(this); if (m) *m = ((dw_face *)this)->m; }
static WINAPI uint16_t face_GetGlyphCount(void *this) { face_prepare(this); return ((dw_face *)this)->glyphs; }
static WINAPI HRESULT face_GetDesignGlyphMetrics(void *this, const uint16_t *glyphs, uint32_t count, glyph_metrics *out, int32_t sideways) {
    dw_face *f = this;
    (void)sideways;
    if (!glyphs || !out) return E_INVALIDARG;
    TR("IDWriteFontFace::GetDesignGlyphMetrics(%s, %u glyphs)", f->ps, count);
    for (uint32_t i = 0; i < count; i++) face_glyph_metrics(f, glyphs[i], &out[i]);
    return S_OK;
}
static WINAPI HRESULT face_GetGlyphIndices(void *this, const uint32_t *cps, uint32_t count, uint16_t *out) {
    dw_face *f = this;
    if (!out) return E_INVALIDARG;
    if (!cps) { memset(out, 0, 2 * (size_t)count); return E_INVALIDARG; }
    TR("IDWriteFontFace::GetGlyphIndices(%s, %u code points, first U+%04X)", f->ps, count, count ? cps[0] : 0);
    for (uint32_t i = 0; i < count; i++) out[i] = face_glyph(f, cps[i]);
    return S_OK;
}
// Tags come as DWRITE_MAKE_OPENTYPE_TAG makes them: the first letter in the low byte.
static WINAPI HRESULT face_TryGetFontTable(void *this, uint32_t tag, const void **data, uint32_t *size, void **ctx, int32_t *exists) {
    dw_face *f = this;
    if (!data || !size || !ctx || !exists) return E_INVALIDARG;
    uint32_t t = __builtin_bswap32(tag);
    const uint8_t *p; uint32_t l; CFDataRef k;
    *exists = TABLE(f, t, p, l, k);
    TR("IDWriteFontFace::TryGetFontTable(%s, '%c%c%c%c') -> %s", f->ps, t >> 24, (t >> 16) & 255, (t >> 8) & 255, t & 255, *exists ? "found" : "none");
    if (!*exists) { *data = NULL; *size = 0; *ctx = NULL; return S_OK; }
    *data = p; *size = l; *ctx = (void *)k;      // (CoreText's copy, released by ReleaseFontTable; NULL for the file's own bytes)
    return S_OK;
}
static WINAPI void face_ReleaseFontTable(void *this, void *ctx) { (void)this; if (ctx) CFRelease((CFDataRef)ctx); }

// Glyph run positions (DIPs, y down): where each glyph's origin is.
static float run_advance(dw_face *f, const glyph_run *r, uint32_t i) {
    return r->advances ? r->advances[i] : (float)face_advance(f, r->glyphs[i], NULL) * r->em / (float)f->upem;
}
static void glyph_origin(dw_face *f, const glyph_run *r, int rtl, uint32_t i, float *pen, float *x, float *y) {
    float adv = run_advance(f, r, i);
    const glyph_offset *o = r->offsets ? &r->offsets[i] : NULL;
    if (rtl) { *pen -= adv; *x = *pen - (o ? o->advance_offset : 0); }
    else { *x = *pen + (o ? o->advance_offset : 0); *pen += adv; }
    *y = o ? -o->ascender_offset : 0;
}
#define OBLIQUE_SKEW 0.33f                   // the slant of a simulated oblique (about 18 degrees)

// GetGlyphRunOutline: the glyphs' outlines into the program's ID2D1SimplifiedGeometrySink (SetFillMode 3,
// BeginFigure 5, AddLines 6, AddBeziers 7, EndFigure 8); quadratic curves become cubic ones.
typedef struct { void *sink; point_2f start, last; int open; point_2f lines[64]; int nlines; float bez[64 * 6]; int nbez; } outline_state;
static void sink_flush(outline_state *s) {
    if (s->nlines) ((void (WINAPI *)(void *, const point_2f *, uint32_t))SLOT(s->sink, 6))(s->sink, s->lines, (uint32_t)s->nlines);
    if (s->nbez) ((void (WINAPI *)(void *, const float *, uint32_t))SLOT(s->sink, 7))(s->sink, s->bez, (uint32_t)s->nbez);
    s->nlines = s->nbez = 0;
}
static void sink_end(outline_state *s) {
    if (!s->open) return;
    sink_flush(s);
    ((void (WINAPI *)(void *, uint32_t))SLOT(s->sink, 8))(s->sink, 1);              // D2D1_FIGURE_END_CLOSED
    s->open = 0;
}
static void sink_begin(outline_state *s, point_2f p) {
    sink_end(s);
    uint64_t packed; memcpy(&packed, &p, 8);                                         // D2D1_POINT_2F by value: one register
    ((void (WINAPI *)(void *, uint64_t, uint32_t))SLOT(s->sink, 5))(s->sink, packed, 0);   // D2D1_FIGURE_BEGIN_FILLED
    s->open = 1; s->start = s->last = p;
}
static void sink_line(outline_state *s, point_2f p) {
    if (s->nbez) sink_flush(s);
    if (s->nlines == 64) sink_flush(s);
    s->lines[s->nlines++] = p; s->last = p;
}
static void sink_cubic(outline_state *s, point_2f a, point_2f b, point_2f c) {
    if (s->nlines) sink_flush(s);
    if (s->nbez == 64) sink_flush(s);
    float *q = s->bez + 6 * s->nbez++;
    q[0] = a.x; q[1] = a.y; q[2] = b.x; q[3] = b.y; q[4] = c.x; q[5] = c.y;
    s->last = c;
}
static void outline_element(void *info, const CGPathElement *e) {
    outline_state *s = info;
    point_2f p[3];
    int n = e->type == kCGPathElementAddQuadCurveToPoint ? 2 : e->type == kCGPathElementAddCurveToPoint ? 3 : e->type == kCGPathElementCloseSubpath ? 0 : 1;
    for (int i = 0; i < n; i++) p[i] = (point_2f){ (float)e->points[i].x, (float)e->points[i].y };
    switch (e->type) {
    case kCGPathElementMoveToPoint: sink_begin(s, p[0]); break;
    case kCGPathElementAddLineToPoint: if (!s->open) sink_begin(s, s->last); sink_line(s, p[0]); break;
    case kCGPathElementAddQuadCurveToPoint: {
        if (!s->open) sink_begin(s, s->last);
        point_2f a = s->last;
        point_2f c1 = { a.x + 2.0f / 3 * (p[0].x - a.x), a.y + 2.0f / 3 * (p[0].y - a.y) }, c2 = { p[1].x + 2.0f / 3 * (p[0].x - p[1].x), p[1].y + 2.0f / 3 * (p[0].y - p[1].y) };
        sink_cubic(s, c1, c2, p[1]);
        break;
    }
    case kCGPathElementAddCurveToPoint: if (!s->open) sink_begin(s, s->last); sink_cubic(s, p[0], p[1], p[2]); break;
    case kCGPathElementCloseSubpath: sink_end(s); s->last = s->start; break;   // (FIGURE_END_CLOSED draws the closing line)
    }
}
// A glyph's outline at (x, y) in DIPs (y down), scaled to the em size, then by `m` (NULL: none).
static CGPathRef glyph_path(dw_face *f, uint16_t g, float em, float x, float y, const CGAffineTransform *m) {
    float s = em / (float)f->upem, k = f->sims & SIM_OBLIQUE ? OBLIQUE_SKEW : 0;
    CGAffineTransform t = CGAffineTransformMake(s, 0, s * k, -s, x, y);
    if (m) t = CGAffineTransformConcat(t, *m);
    return CTFontCreatePathForGlyph(f->ct, g, &t);
}
static WINAPI HRESULT face_GetGlyphRunOutline(void *this, float em, const uint16_t *glyphs, const float *advances, const glyph_offset *offsets,
                                              uint32_t count, int32_t sideways, int32_t rtl, void *sink) {
    dw_face *f = this;
    (void)sideways;
    if (!sink) return E_INVALIDARG;
    if (!count) return S_OK;
    if (!glyphs) return E_INVALIDARG;
    TR("IDWriteFontFace::GetGlyphRunOutline(%s, %.1f, %u glyphs)", f->ps, em, count);
    glyph_run r = { f, em, count, glyphs, advances, offsets, 0, 0 };
    outline_state *s = calloc(1, sizeof *s);
    s->sink = sink;
    ((void (WINAPI *)(void *, uint32_t))SLOT(sink, 3))(sink, 1);                    // D2D1_FILL_MODE_WINDING
    float pen = 0;
    for (uint32_t i = 0; i < count; i++) {
        float x, y;
        glyph_origin(f, &r, rtl, i, &pen, &x, &y);
        CGPathRef p = glyph_path(f, glyphs[i], em, x, y, NULL);
        if (!p) continue;
        CGPathApply(p, s, outline_element);
        sink_end(s);
        CGPathRelease(p);
    }
    free(s);
    return S_OK;
}
// Rendering modes: no hinting here, so the natural modes (outlines from 100 pixels per em up).
static int recommended_mode(float pixels, int measuring, void *params);
static WINAPI HRESULT face_GetRecommendedRenderingMode(void *this, float em, float ppd, uint32_t measuring, void *params, uint32_t *mode) {
    (void)this;
    if (!mode) return E_INVALIDARG;
    *mode = (uint32_t)recommended_mode(em * ppd, (int)measuring, params);
    return S_OK;
}
static float matrix_scale(const matrix *m) { return m ? sqrtf(fabsf(m->m11 * m->m22 - m->m12 * m->m21)) : 1.0f; }
static WINAPI HRESULT face_GetRecommendedRenderingMode1(void *this, float em, float dpi_x, float dpi_y, const matrix *m, int32_t sideways, uint32_t threshold, uint32_t measuring, uint32_t *mode) {
    (void)this; (void)sideways; (void)threshold;
    if (!mode) return E_INVALIDARG;
    *mode = (uint32_t)recommended_mode(em * sqrtf(dpi_x * dpi_y) / 96.0f * matrix_scale(m), (int)measuring, NULL);
    return S_OK;
}
static WINAPI HRESULT face_GetRecommendedRenderingMode2(void *this, float em, float dpi_x, float dpi_y, const matrix *m, int32_t sideways, uint32_t threshold, uint32_t measuring, void *params, uint32_t *mode, uint32_t *grid) {
    (void)this; (void)sideways; (void)threshold;
    if (!mode || !grid) return E_INVALIDARG;
    *mode = (uint32_t)recommended_mode(em * sqrtf(dpi_x * dpi_y) / 96.0f * matrix_scale(m), (int)measuring, params);
    *grid = GRID_FIT_DISABLED;
    return S_OK;
}
// GDI-compatible metrics: the design metrics rounded to whole pixels at that size, given back in design units.
static int32_t gdi_round(int32_t v, float pixels, uint32_t upem) { return pixels > 0 ? (int32_t)lroundf(roundf(v * pixels / (float)upem) * (float)upem / pixels) : v; }
static void gdi_metrics(dw_face *f, float em, float ppd, const matrix *m, font_metrics *out) {
    face_prepare(f);
    float px = em * ppd * (m ? fabsf(m->m22) : 1.0f);
    *out = f->m;
    out->ascent = (uint16_t)gdi_round(f->m.ascent, px, f->upem); out->descent = (uint16_t)gdi_round(f->m.descent, px, f->upem);
    out->line_gap = (int16_t)gdi_round(f->m.line_gap, px, f->upem); out->cap_height = (uint16_t)gdi_round(f->m.cap_height, px, f->upem);
    out->x_height = (uint16_t)gdi_round(f->m.x_height, px, f->upem);
}
static WINAPI HRESULT face_GetGdiCompatibleMetrics(void *this, float em, float ppd, const matrix *m, font_metrics *out) {
    if (!out || em <= 0) return E_INVALIDARG;
    font_metrics t; gdi_metrics(this, em, ppd, m, &t);
    memcpy(out, &t, FONT_METRICS0_SIZE);
    return S_OK;
}
static WINAPI HRESULT face_GetGdiCompatibleMetrics1(void *this, float em, float ppd, const matrix *m, font_metrics *out) {
    if (!out || em <= 0) return E_INVALIDARG;
    gdi_metrics(this, em, ppd, m, out);
    return S_OK;
}
static WINAPI HRESULT face_GetGdiCompatibleGlyphMetrics(void *this, float em, float ppd, const matrix *m, int32_t natural, const uint16_t *glyphs,
                                                        uint32_t count, glyph_metrics *out, int32_t sideways) {
    dw_face *f = this;
    (void)natural; (void)sideways;
    if (!glyphs || !out || em <= 0) return E_INVALIDARG;
    float px = em * ppd * (m ? fabsf(m->m11) : 1.0f);
    for (uint32_t i = 0; i < count; i++) {
        face_glyph_metrics(f, glyphs[i], &out[i]);
        int32_t adv = gdi_round((int32_t)out[i].advance, px, f->upem);
        out[i].rsb += adv - (int32_t)out[i].advance;
        out[i].advance = (uint32_t)adv;
    }
    return S_OK;
}
static WINAPI void face_GetCaretMetrics(void *this, caret_metrics *c) {
    dw_face *f = this;
    if (!c) return;
    c->rise = 1; c->run = 0; c->offset = 0;
    const uint8_t *t; uint32_t l; CFDataRef k = NULL;
    if (TABLE(f, TAG('h', 'h', 'e', 'a'), t, l, k) && l >= 24) { c->rise = bes16(t + 18); c->run = bes16(t + 20); c->offset = bes16(t + 22); }
    unkeep(k);
}
// Unicode ranges: from CoreText's character set (the font's cmap). Its bitmap: 8192 bytes for the BMP, then for
// each other plane present its number and 8192 bytes.
static HRESULT unicode_ranges(dw_face *f, uint32_t max, unicode_range *out, uint32_t *count) {
    CFCharacterSetRef cs = CTFontCopyCharacterSet(f->ct);
    uint32_t n = 0;
    if (cs) {
        CFDataRef bm = CFCharacterSetCreateBitmapRepresentation(NULL, cs);
        const uint8_t *b = CFDataGetBytePtr(bm);
        CFIndex len = CFDataGetLength(bm);
        for (CFIndex at = 0; at + 8192 <= len; at += at ? 8193 : 8192) {
            uint32_t plane = 0;
            const uint8_t *bits = b;
            if (at) { if (at + 8193 > len) break; plane = b[at]; bits = b + at + 1; }
            int in = 0; uint32_t start = 0;
            for (uint32_t c = 0; c <= 0x10000; c++) {
                int has = c < 0x10000 && (bits[c >> 3] >> (c & 7)) & 1;
                if (has && !in) { start = c; in = 1; }
                else if (!has && in) { if (out && n < max) out[n] = (unicode_range){ plane << 16 | start, plane << 16 | (c - 1) }; n++; in = 0; }
            }
        }
        CFRelease(bm);
        CFRelease(cs);
    }
    *count = n;
    return n > max ? E_NOT_SUFFICIENT_BUFFER : S_OK;
}
static WINAPI HRESULT face_GetUnicodeRanges(void *this, uint32_t max, unicode_range *out, uint32_t *count) {
    if (!count) return E_INVALIDARG;
    return unicode_ranges(this, max, out, count);
}
static WINAPI int32_t face_IsMonospacedFont(void *this) { face_prepare(this); return ((dw_face *)this)->fixed_pitch; }
static WINAPI HRESULT face_GetDesignGlyphAdvances(void *this, uint32_t count, const uint16_t *glyphs, int32_t *adv, int32_t sideways) {
    dw_face *f = this;
    if (!glyphs || !adv) return E_INVALIDARG;
    for (uint32_t i = 0; i < count; i++) {
        if (sideways) { glyph_metrics gm; face_glyph_metrics(f, glyphs[i], &gm); adv[i] = (int32_t)gm.advance_height; }
        else adv[i] = (int32_t)face_advance(f, glyphs[i], NULL);
    }
    return S_OK;
}
static WINAPI HRESULT face_GetGdiCompatibleGlyphAdvances(void *this, float em, float ppd, const matrix *m, int32_t natural, int32_t sideways,
                                                         uint32_t count, const uint16_t *glyphs, int32_t *adv) {
    dw_face *f = this;
    (void)natural;
    if (!glyphs || !adv || em <= 0) return E_INVALIDARG;
    face_GetDesignGlyphAdvances(this, count, glyphs, adv, sideways);
    float px = em * ppd * (m ? fabsf(m->m11) : 1.0f);
    for (uint32_t i = 0; i < count; i++) adv[i] = gdi_round(adv[i], px, f->upem);
    return S_OK;
}
// Kerning: none offered (HasKerningPairs is FALSE; programs that kern shape with the font's own GPOS).
static WINAPI HRESULT face_GetKerningPairAdjustments(void *this, uint32_t count, const uint16_t *glyphs, int32_t *adj) {
    (void)this;
    if (!glyphs || !adj) return E_INVALIDARG;
    memset(adj, 0, 4 * (size_t)count);
    return S_OK;
}
static WINAPI int32_t face_HasKerningPairs(void *this) { (void)this; return 0; }
static WINAPI HRESULT face_GetVerticalGlyphVariants(void *this, uint32_t count, const uint16_t *nominal, uint16_t *vertical) {
    (void)this;
    if (!nominal || !vertical) return E_INVALIDARG;
    memcpy(vertical, nominal, 2 * (size_t)count);
    return S_OK;
}
static WINAPI int32_t face_HasVerticalGlyphVariants(void *this) { (void)this; return 0; }
static WINAPI int32_t face_IsColorFont(void *this) { (void)this; return 0; }
static WINAPI uint32_t face_GetColorPaletteCount(void *this) { (void)this; return 0; }
static WINAPI uint32_t face_GetPaletteEntryCount(void *this) { (void)this; return 0; }
static WINAPI HRESULT face_GetPaletteEntries(void *this, uint32_t palette, uint32_t first, uint32_t count, void *entries) {
    (void)this; (void)palette; (void)first; (void)count; (void)entries;
    return DWRITE_E_NOCOLOR;
}

// ---- IDWriteRenderingParams ------------------------------------------------------------------------------------------------
typedef struct { dw_obj o; float gamma, contrast, gray_contrast, cleartype; uint32_t geometry, mode, grid; } dw_params;
static const GUID *const g_params_iids[] = { &IID_IDWriteRenderingParams, &IID_IDWriteRenderingParams1, &IID_IDWriteRenderingParams2, NULL };
static void params_destroy(dw_obj *o) { free(o); }
static dw_params *params_new(float gamma, float contrast, float gray, float cleartype, uint32_t geometry, uint32_t mode, uint32_t grid) {
    dw_params *p = dw_new(sizeof *p, g_params_vtbl, g_params_iids, "IDWriteRenderingParams", params_destroy);
    p->gamma = gamma; p->contrast = contrast; p->gray_contrast = gray; p->cleartype = cleartype; p->geometry = geometry; p->mode = mode; p->grid = grid;
    return p;
}
static WINAPI float params_GetGamma(void *this) { return ((dw_params *)this)->gamma; }
static WINAPI float params_GetEnhancedContrast(void *this) { return ((dw_params *)this)->contrast; }
static WINAPI float params_GetClearTypeLevel(void *this) { return ((dw_params *)this)->cleartype; }
static WINAPI uint32_t params_GetPixelGeometry(void *this) { return ((dw_params *)this)->geometry; }
static WINAPI uint32_t params_GetRenderingMode(void *this) { return ((dw_params *)this)->mode; }
static WINAPI float params_GetGrayscaleEnhancedContrast(void *this) { return ((dw_params *)this)->gray_contrast; }
static WINAPI uint32_t params_GetGridFitMode(void *this) { return ((dw_params *)this)->grid; }
static int recommended_mode(float pixels, int measuring, void *params) {
    if (is_ours(params, g_params_vtbl) && ((dw_params *)params)->mode != MODE_DEFAULT) return (int)((dw_params *)params)->mode;
    if (pixels >= 100) return MODE_OUTLINE;
    return measuring == MEASURE_GDI_CLASSIC ? MODE_GDI_CLASSIC : measuring == MEASURE_GDI_NATURAL ? MODE_GDI_NATURAL : MODE_NATURAL_SYMMETRIC;
}

// ---- IDWriteGlyphRunAnalysis: the run filled into a coverage bitmap once, served as alpha textures -------------------------
// Aliased and grayscale runs: one coverage value per pixel. ClearType runs (antialiased, not grayscale): three per
// pixel, the run filled at three times the horizontal resolution (DWRITE_TEXTURE_CLEARTYPE_3x1: three bytes per pixel
// across, one down), each subpixel then averaged with its two neighbours so that a stem on a subpixel boundary does
// not light a single colour (our choice of filter; DirectWrite's own is not documented).
typedef struct { dw_obj o; int aliased, grayscale, sub; RECT bounds; uint8_t *cov; int x0, y0, w, h; float gray_contrast; } dw_analysis;
static const GUID *const g_analysis_iids[] = { &IID_IDWriteGlyphRunAnalysis, NULL };
static void analysis_destroy(dw_obj *o) { dw_analysis *a = (dw_analysis *)o; free(a->cov); free(a); }
static HRESULT analysis_new(const glyph_run *run, float ppd, const matrix *m, uint32_t mode, uint32_t aa, float ox, float oy, void **out) {
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!run) return E_INVALIDARG;
    glyph_run r = *run;                          // (its pointers as this runtime sees them: vx.h)
    r.face = VX_LOW(r.face); r.glyphs = VX_LOW(r.glyphs); r.advances = VX_LOW(r.advances); r.offsets = VX_LOW(r.offsets);
    run = &r;
    if (!is_ours(run->face, g_face_vtbl) || (run->count && !run->glyphs)) return E_INVALIDARG;
    if (mode == MODE_DEFAULT || mode == MODE_OUTLINE || mode > MODE_NATURAL_SYMMETRIC) return E_INVALIDARG;
    dw_face *f = run->face;
    matrix id = { 1, 0, 0, 1, 0, 0 };
    if (!m) m = &id;
    // DIPs (y down) to device pixels: scaled by pixels per DIP, then the transform.
    CGAffineTransform dev = CGAffineTransformMake(ppd * m->m11, ppd * m->m12, ppd * m->m21, ppd * m->m22, m->dx, m->dy);
    CGMutablePathRef all = CGPathCreateMutable();
    float pen = ox;
    int rtl = run->bidi & 1;
    for (uint32_t i = 0; i < run->count; i++) {
        float x, y;
        glyph_origin(f, run, rtl, i, &pen, &x, &y);
        CGPathRef p = glyph_path(f, run->glyphs[i], run->em, x, y + oy, &dev);
        if (p) { CGPathAddPath(all, NULL, p); CGPathRelease(p); }
    }
    dw_analysis *a = dw_new(sizeof *a, g_analysis_vtbl, g_analysis_iids, "IDWriteGlyphRunAnalysis", analysis_destroy);
    a->aliased = mode == MODE_ALIASED;
    a->grayscale = aa == AA_GRAYSCALE;
    a->sub = a->aliased || a->grayscale ? 1 : 3;
    int sub = a->sub;
    float bold = f->sims & SIM_BOLD ? run->em * ppd * matrix_scale(m) / 32.0f : 0;
    CGRect box = CGPathGetPathBoundingBox(all);
    if (!CGRectIsEmpty(box) && !CGRectIsNull(box)) {
        int x0 = (int)floor(box.origin.x - bold) - 1, y0 = (int)floor(box.origin.y - bold) - 1;
        int x1 = (int)ceil(box.origin.x + box.size.width + bold) + 1, y1 = (int)ceil(box.origin.y + box.size.height + bold) + 1;
        if ((int64_t)(x1 - x0) * (y1 - y0) * sub > (64 << 20)) { CGPathRelease(all); dw_release(a); return E_OUTOFMEMORY; }
        int w = x1 - x0, h = y1 - y0, ws = w * sub;      // (ws: samples across)
        CGContextRef ctx = CGBitmapContextCreate(NULL, (size_t)ws, (size_t)h, 8, 0, NULL, (CGBitmapInfo)kCGImageAlphaOnly);
        if (ctx) {
            CGContextSetShouldAntialias(ctx, !a->aliased);
            CGContextSetAllowsAntialiasing(ctx, !a->aliased);
            // device (x, y down) to the bitmap (its first row is the top one; `sub` samples per pixel across)
            CGContextTranslateCTM(ctx, 0, h);
            CGContextScaleCTM(ctx, sub, -1);
            CGContextTranslateCTM(ctx, -x0, -y0);
            CGContextAddPath(ctx, all);
            CGContextSetGrayFillColor(ctx, 0, 1);
            if (bold) { CGContextSetGrayStrokeColor(ctx, 0, 1); CGContextSetLineWidth(ctx, bold); CGContextSetLineJoin(ctx, kCGLineJoinRound); CGContextDrawPath(ctx, kCGPathFillStroke); }
            else CGContextFillPath(ctx);
            const uint8_t *px = CGBitmapContextGetData(ctx);
            size_t stride = CGBitmapContextGetBytesPerRow(ctx);
            uint8_t *filtered = NULL;
            if (sub > 1) {                    // each subpixel with its neighbours, (1 1 1) / 3 (the margin keeps the spread inside)
                filtered = malloc((size_t)ws * h);
                for (int y = 0; y < h; y++) {
                    const uint8_t *s = px + (size_t)y * stride;
                    uint8_t *d = filtered + (size_t)y * ws;
                    for (int x = 0; x < ws; x++) d[x] = (uint8_t)(((x > 0 ? s[x - 1] : 0) + s[x] + (x + 1 < ws ? s[x + 1] : 0) + 1) / 3);
                }
                px = filtered; stride = (size_t)ws;
            }
            int bx0 = w, by0 = h, bx1 = -1, by1 = -1;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < ws; x++)
                    if (px[(size_t)y * stride + x]) { int p = x / sub; if (p < bx0) bx0 = p; if (p > bx1) bx1 = p; if (y < by0) by0 = y; if (y > by1) by1 = y; }
            if (bx1 >= 0) {
                a->x0 = x0 + bx0; a->y0 = y0 + by0; a->w = bx1 - bx0 + 1; a->h = by1 - by0 + 1;
                a->cov = malloc((size_t)a->w * sub * a->h);
                for (int y = 0; y < a->h; y++) memcpy(a->cov + (size_t)y * a->w * sub, px + (size_t)(by0 + y) * stride + (size_t)bx0 * sub, (size_t)a->w * sub);
                a->bounds = (RECT){ a->x0, a->y0, a->x0 + a->w, a->y0 + a->h };
            }
            free(filtered);
            CGContextRelease(ctx);
        }
    }
    CGPathRelease(all);
    TR("CreateGlyphRunAnalysis(%s, %.1f, %u glyphs, mode %u, aa %u) -> bounds %d,%d %dx%d", f->ps, run->em, run->count, mode, aa, a->x0, a->y0, a->w, a->h);
    *out = a;
    return S_OK;
}
// Which textures hold the run: an aliased run the bi-level one, an antialiased run the 3x1 one; a grayscale run
// both (its one coverage value per pixel in each, three times over in the 3x1 one).
static int analysis_serves(dw_analysis *a, uint32_t type) {
    if (type == TEXTURE_ALIASED_1x1) return a->aliased || a->grayscale;
    if (type == TEXTURE_CLEARTYPE_3x1) return !a->aliased;
    return 0;
}
static WINAPI HRESULT analysis_GetAlphaTextureBounds(void *this, uint32_t type, RECT *r) {
    dw_analysis *a = this;
    if (!r) return E_INVALIDARG;
    if (type > TEXTURE_CLEARTYPE_3x1) { memset(r, 0, sizeof *r); return E_INVALIDARG; }
    if (analysis_serves(a, type) && a->cov) *r = a->bounds; else memset(r, 0, sizeof *r);
    TR("IDWriteGlyphRunAnalysis::GetAlphaTextureBounds(type %u) -> %d,%d,%d,%d", type, r->left, r->top, r->right, r->bottom);
    return S_OK;
}
static WINAPI HRESULT analysis_CreateAlphaTexture(void *this, uint32_t type, const RECT *r, uint8_t *out, uint32_t size) {
    dw_analysis *a = this;
    if (!r || !out || type > TEXTURE_CLEARTYPE_3x1 || r->right < r->left || r->bottom < r->top) return E_INVALIDARG;
    uint32_t bpp = type == TEXTURE_CLEARTYPE_3x1 ? 3 : 1;
    uint64_t w = (uint64_t)(r->right - r->left), h = (uint64_t)(r->bottom - r->top), need = w * h * bpp;
    if (size < need) return E_NOT_SUFFICIENT_BUFFER;
    memset(out, 0, need);
    TR("IDWriteGlyphRunAnalysis::CreateAlphaTexture(type %u, %d,%d,%d,%d)", type, r->left, r->top, r->right, r->bottom);
    if (!analysis_serves(a, type) || !a->cov) return S_OK;
    for (LONG y = r->top; y < r->bottom; y++) {
        int sy = y - a->y0;
        if (sy < 0 || sy >= a->h) continue;
        for (LONG x = r->left; x < r->right; x++) {
            int sx = x - a->x0;
            if (sx < 0 || sx >= a->w) continue;
            const uint8_t *v = a->cov + ((size_t)sy * a->w + sx) * a->sub;
            uint8_t *o = out + ((size_t)(y - r->top) * w + (size_t)(x - r->left)) * bpp;
            for (uint32_t k = 0; k < bpp; k++) o[k] = v[a->sub > 1 ? k : 0];
        }
    }
    return S_OK;
}
static WINAPI HRESULT analysis_GetAlphaBlendParams(void *this, void *params, float *gamma, float *contrast, float *cleartype) {
    dw_analysis *a = this;
    if (!gamma || !contrast || !cleartype) return E_INVALIDARG;
    *gamma = 1.8f; *contrast = 0.5f; *cleartype = 0.0f;
    if (is_ours(params, g_params_vtbl)) {      // (rendering parameters are always ours: programs get them from the factory)
        dw_params *p = params;
        *gamma = p->gamma; *contrast = a->grayscale ? p->gray_contrast : p->contrast; *cleartype = p->cleartype;
    }
    if (a->grayscale) *cleartype = 0.0f;
    return S_OK;
}

// ---- IDWriteFontFallback -----------------------------------------------------------------------------------------------------
static dw_obj g_fallback;
static int continues_cluster(uint32_t cp) {          // marks that go with the character before them
    static CFCharacterSetRef nonbase;
    if (!nonbase) nonbase = CFCharacterSetGetPredefined(kCFCharacterSetNonBase);
    return cp == 0x200C || cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF) || (cp >= 0x1F3FB && cp <= 0x1F3FF) ||
           (cp >= 0xE0020 && cp <= 0xE007F) || CFCharacterSetIsLongCharacterMember(nonbase, cp);
}
static uint32_t next_cp(const WCHAR *t, uint32_t n, uint32_t i, uint32_t *units) {
    *units = 1;
    if (t[i] >= 0xD800 && t[i] < 0xDC00 && i + 1 < n && t[i + 1] >= 0xDC00 && t[i + 1] < 0xE000) { *units = 2; return 0x10000 + ((t[i] - 0xD800u) << 10) + (t[i + 1] - 0xDC00u); }
    return t[i];
}
// How much of the text a face covers from its start (UTF-16 units; clusters stay together), stopping where `stop`
// (if any) covers the text again.
static uint32_t covered_run(dw_face *f, dw_face *stop, const WCHAR *t, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        uint32_t u, cp = next_cp(t, n, i, &u);
        if (i && continues_cluster(cp)) { i += u; continue; }
        if (!face_glyph(f, cp)) break;
        if (i && stop && face_glyph(stop, cp)) break;
        i += u;
    }
    return i;
}
// The font of the system collection that is CoreText's font (by PostScript name), a family of its own added when
// the collection does not list it.
static dw_font *system_font_for(CTFontRef ct, int weight, int stretch, int style) {
    dw_collection *c = system_collection();
    char ps[128];
    CFStringRef s = CTFontCopyPostScriptName(ct);
    cf_utf8(s, ps, sizeof ps);
    if (s) CFRelease(s);
    CFStringRef famname = CTFontCopyFamilyName(ct);
    dw_font *found = NULL;
    lock();
    dw_family *fam = famname ? family_named(c, famname, c->count) : NULL;
    if (fam) {
        dw_font *exact = NULL;
        for (uint32_t i = 0; i < fam->count && !exact; i++) if (!strcmp(fam->fonts[i]->ps, ps)) exact = fam->fonts[i];
        found = exact;
        // the face CoreText chose is for the base font's weight and slope already; ask the family only if it differs
        if (exact) { font_props(exact); if (exact->style != style && style != STYLE_NORMAL) found = NULL; }
        if (!found) found = family_match(fam, weight, stretch, style);
    } else if (famname) {
        CTFontDescriptorRef d = CTFontCopyFontDescriptor(ct);
        dw_family *nf = family_new(c, famname, 0);
        if (nf && d) found = font_new(nf, d, NULL);
        if (d) CFRelease(d);
    }
    unlock();
    if (famname) CFRelease(famname);
    return found;
}
// The installed font CoreText takes for the start of the text after `base` (NULL: none has its first character).
static dw_font *fallback_font(dw_face *base, const WCHAR *text, uint32_t n, const WCHAR *locale, int weight, int stretch, int style) {
    uint32_t u, cp0 = next_cp(text, n, 0, &u);
    CTFontRef start = base ? CFRetain(base->ct) : CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 12.0, NULL);
    CFStringRef str = CFStringCreateWithCharactersNoCopy(NULL, text, (CFIndex)n, kCFAllocatorNull);
    CFStringRef lang = locale && locale[0] ? CFStringCreateWithCharacters(NULL, locale, (CFIndex)vx_wcslen(locale)) : NULL;
    CTFontRef fb = start ? CTFontCreateForStringWithLanguage(start, str, CFRangeMake(0, (CFIndex)n), lang) : NULL;
    char ps[128];
    CFStringRef psn = fb ? CTFontCopyPostScriptName(fb) : NULL;
    cf_utf8(psn, ps, sizeof ps);
    if (psn) CFRelease(psn);
    dw_font *ft = fb && strcmp(ps, "LastResort") ? system_font_for(fb, weight, stretch, style) : NULL;
    dw_face *ff = ft ? font_face(ft) : NULL;
    if (!ff || !face_glyph(ff, cp0)) ft = NULL;
    if (fb) CFRelease(fb);
    if (lang) CFRelease(lang);
    CFRelease(str);
    if (start) CFRelease(start);
    return ft;
}
static WINAPI HRESULT fallback_MapCharacters(void *this, void *source, uint32_t pos, uint32_t length, void *base_coll, const WCHAR *base_family,
                                             uint32_t weight, uint32_t style, uint32_t stretch, uint32_t *mapped, void **font, float *scale) {
    (void)this;
    if (!source || !mapped || !font || !scale) return E_INVALIDARG;
    *mapped = 0; *font = NULL; *scale = 1.0f;
    if (!length) return S_OK;
    // IDWriteTextAnalysisSource::GetTextAtPosition (3), GetLocaleName (6)
    WCHAR *text = malloc(2 * (size_t)length);
    uint32_t got = 0;
    while (got < length) {
        const WCHAR *t = NULL; uint32_t n = 0;
        if (((HRESULT (WINAPI *)(void *, uint32_t, const WCHAR **, uint32_t *))SLOT(source, 3))(source, pos + got, &t, &n) < 0 || !t || !n) break;
        t = VX_LOW(t);
        if (n > length - got) n = length - got;
        memcpy(text + got, t, 2 * (size_t)n);
        got += n;
    }
    if (!got) { free(text); return S_OK; }
    const WCHAR *locale = NULL; uint32_t loclen = 0;
    ((HRESULT (WINAPI *)(void *, uint32_t, uint32_t *, const WCHAR **))SLOT(source, 6))(source, pos, &loclen, &locale);
    locale = VX_LOW(locale);
    // the base font: the family asked for, in the collection given (ours) or the system's
    dw_collection *coll = is_ours(base_coll, g_collection_vtbl) ? base_coll : system_collection();
    dw_font *base = NULL;
    uint32_t idx;
    if (base_family && base_family[0] && find_family(coll, base_family, &idx)) base = family_match(coll->families[idx], (int)weight, (int)stretch, (int)style);
    dw_face *bf = base ? font_face(base) : NULL;
    uint32_t u, cp0 = next_cp(text, got, 0, &u);
    dw_font *result = NULL;
    uint32_t run = 0;
    if (bf && face_glyph(bf, cp0)) { result = base; run = covered_run(bf, NULL, text, got); }
    else if ((result = fallback_font(bf, text, got, locale, (int)weight, (int)stretch, (int)style))) run = covered_run(font_face(result), bf, text, got);
    if (!result) {                        // no font has it: the characters up to one some font has, with no font
        run = u;
        while (run < got) {
            uint32_t k, cp = next_cp(text, got, run, &k);
            if (!continues_cluster(cp)) break;
            run += k;
        }
    }
    if (!run) run = u;
    *mapped = run;
    if (result) { dw_addref(result); *font = result; }
    TR("IDWriteFontFallback::MapCharacters(U+%04X.., %u) -> %s for %u", cp0, got, result ? result->ps : "no font", run);
    free(text);
    return S_OK;
}

// ---- IDWriteGdiInterop ---------------------------------------------------------------------------------------------------------
static dw_obj g_gdi;
static WINAPI HRESULT gdi_CreateFontFromLOGFONT(void *this, const logfont_w *lf, void **out) {
    (void)this;
    if (!lf || !out) return E_INVALIDARG;
    *out = NULL;
    dw_collection *c = system_collection();
    uint32_t idx;
    WCHAR name[33];
    memcpy(name, lf->face, sizeof lf->face);
    name[32] = 0;
    if (!find_family(c, name, &idx)) return DWRITE_E_NOFONT;
    int weight = lf->weight ? (int)lf->weight : 400;
    dw_font *ft = family_match(c->families[idx], weight, 5, lf->italic ? STYLE_ITALIC : STYLE_NORMAL);
    if (!ft) return DWRITE_E_NOFONT;
    dw_addref(ft);
    *out = ft;
    return S_OK;
}
static WINAPI HRESULT gdi_CreateBitmapRenderTarget(void *this, HANDLE hdc, uint32_t w, uint32_t h, void **out);
static WINAPI HRESULT gdi_CreateFontFaceFromHdc(void *this, void *hdc, void **out) { (void)this; (void)hdc; if (out) *out = NULL; return dw_not_implemented("IDWriteGdiInterop::CreateFontFaceFromHdc"); }
static void font_logfont(dw_font *ft, logfont_w *lf) {
    memset(lf, 0, sizeof *lf);
    font_props(ft);
    lf->weight = ft->weight;
    lf->italic = ft->style != STYLE_NORMAL;
    lf->charset = 1;                                          // DEFAULT_CHARSET
    CFIndex n = CFStringGetLength(ft->family->name);
    if (n > 31) n = 31;
    CFStringGetCharacters(ft->family->name, CFRangeMake(0, n), lf->face);
}
static WINAPI HRESULT gdi_ConvertFontToLOGFONT(void *this, void *font, logfont_w *lf, int32_t *system) {
    (void)this;
    if (!lf || !system) return E_INVALIDARG;
    if (!is_ours(font, g_font_vtbl)) { memset(lf, 0, sizeof *lf); *system = 0; return E_INVALIDARG; }
    font_logfont(font, lf);
    *system = ((dw_font *)font)->family->coll->system;
    return S_OK;
}
static WINAPI HRESULT gdi_ConvertFontFaceToLOGFONT(void *this, void *face, logfont_w *lf) {
    (void)this;
    if (!lf) return E_INVALIDARG;
    memset(lf, 0, sizeof *lf);
    if (!is_ours(face, g_face_vtbl)) return E_INVALIDARG;
    dw_face *f = face;
    lock();
    dw_font *ft = f->font ? f->font : collection_font_of_face(system_collection(), f);
    unlock();
    if (!ft) return DWRITE_E_NOFONT;
    font_logfont(ft, lf);
    return S_OK;
}

// ---- IDWriteTextFormat, IDWriteTextLayout: text laid out without shaping ---------------------------------------------------
// What Steam's own UI (VGUI2's Win32Font in steam.exe) measures and draws with. A layout gives each character (code
// point) one glyph of the first font that has it (the format's family, else the font fallback finds), advances by
// the glyph's advance (whole pixels in GDI-compatible layouts), breaks lines after spaces, hyphens and CJK
// characters (anywhere in a word too long for a line, unless wrapping is by whole words), and aligns the lines.
// Not done: shaping (ligatures, kerning, marks placed on their base), bidirectional text (right-to-left runs are laid
// out left to right), trimming signs, inline objects, vertical text.
typedef struct {
    WCHAR family[128], locale[86];
    dw_collection *coll;                 // (held)
    uint32_t weight, style, stretch;
    float size;
    int underline, strike;
    void *effect, *typography;           // the program's IUnknown, our typography (held)
} text_attrs;
typedef struct dw_format {
    dw_obj o;                            // IDWriteTextFormat; an IDWriteTextLayout begins with one
    text_attrs a;                        // (a layout's attributes where none were set on a range)
    uint32_t align, palign, wrap, rdir, fdir;
    float tab;
    uint32_t trim[3]; void *trim_sign;   // DWRITE_TRIMMING, the program's IDWriteInlineObject (held)
    uint32_t spacing_method; float spacing, baseline;
} dw_format;
typedef struct { uint32_t pos, units; uint16_t glyph; uint16_t attr; dw_face *face; float advance; uint8_t space, newline, brk, tab, hidden, soft_hyphen; } text_cluster;
typedef struct { uint32_t first, count; float x, y, width, trailing, height, baseline; uint32_t units, trailing_units, newline_units; } text_line;
typedef struct {
    dw_format f;
    WCHAR *text; uint32_t len;
    float max_w, max_h;
    int gdi, gdi_natural; float ppd; matrix m;      // GDI-compatible layouts: advances in whole pixels at this scale
    text_attrs *attrs; uint32_t nattrs; uint16_t *attr_of;   // the attributes of each UTF-16 unit (index into attrs)
    dw_face **primary;                   // each attribute set's own face (NULL: its family is not there)
    text_cluster *cl; uint32_t ncl;
    text_line *lines; uint32_t nlines;
    int dirty;
    float left, top, width, width_ws, height;
} dw_layout;
static const GUID *const g_format_iids[] = { &IID_IDWriteTextFormat, NULL };
static const GUID *const g_layout_iids[] = { &IID_IDWriteTextFormat, &IID_IDWriteTextLayout, NULL };
static void **g_format_vtbl, **g_layout_vtbl, **g_typography_vtbl, **g_target_vtbl;
static int is_format(void *p) { return is_ours(p, g_format_vtbl) || is_ours(p, g_layout_vtbl); }

static void attrs_hold(text_attrs *a) {
    if (a->coll) dw_addref(a->coll);
    if (a->effect) com_addref(a->effect);
    if (a->typography) dw_addref(a->typography);
}
static void attrs_drop(text_attrs *a) {
    if (a->coll) dw_release(a->coll);
    if (a->effect) com_release(a->effect);
    if (a->typography) dw_release(a->typography);
}
static void format_destroy(dw_obj *o) {
    dw_format *f = (dw_format *)o;
    attrs_drop(&f->a);
    if (f->trim_sign) com_release(f->trim_sign);
    if (f->o.vtbl == g_layout_vtbl) {
        dw_layout *l = (dw_layout *)f;
        for (uint32_t i = 0; i < l->nattrs; i++) attrs_drop(&l->attrs[i]);
        free(l->attrs); free(l->attr_of); free(l->primary); free(l->text); free(l->cl); free(l->lines);
    }
    free(f);
}
static size_t wcs_copy(WCHAR *dst, size_t cap, const WCHAR *src) {
    size_t n = src ? vx_wcslen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, 2 * n);
    dst[n] = 0;
    return n;
}
static HRESULT wcs_out(const WCHAR *s, WCHAR *out, uint32_t cap) {
    size_t n = vx_wcslen(s);
    if (!out) return E_INVALIDARG;
    if (cap < n + 1) { if (cap) out[0] = 0; return E_NOT_SUFFICIENT_BUFFER; }
    memcpy(out, s, 2 * (n + 1));
    return S_OK;
}

// IDWriteTextFormat (its methods serve layouts too: a layout begins with a format)
#define FORMAT(this) ((dw_format *)(this))
static void layout_dirty(void *this) { if (FORMAT(this)->o.vtbl == g_layout_vtbl) ((dw_layout *)this)->dirty = 1; }
static WINAPI HRESULT fmt_SetTextAlignment(void *this, uint32_t v) { if (v > 3) return E_INVALIDARG; FORMAT(this)->align = v; layout_dirty(this); return S_OK; }
static WINAPI HRESULT fmt_SetParagraphAlignment(void *this, uint32_t v) { if (v > 2) return E_INVALIDARG; FORMAT(this)->palign = v; layout_dirty(this); return S_OK; }
static WINAPI HRESULT fmt_SetWordWrapping(void *this, uint32_t v) { if (v > 4) return E_INVALIDARG; FORMAT(this)->wrap = v; layout_dirty(this); return S_OK; }
static WINAPI HRESULT fmt_SetReadingDirection(void *this, uint32_t v) { if (v > 3) return E_INVALIDARG; FORMAT(this)->rdir = v; layout_dirty(this); return S_OK; }
static WINAPI HRESULT fmt_SetFlowDirection(void *this, uint32_t v) { if (v > 3) return E_INVALIDARG; FORMAT(this)->fdir = v; layout_dirty(this); return S_OK; }
static WINAPI HRESULT fmt_SetIncrementalTabStop(void *this, float v) { if (!(v > 0)) return E_INVALIDARG; FORMAT(this)->tab = v; layout_dirty(this); return S_OK; }
static WINAPI HRESULT fmt_SetTrimming(void *this, const uint32_t *trim, void *sign) {
    dw_format *f = this;
    if (!trim) return E_INVALIDARG;
    if (sign) com_addref(sign);
    if (f->trim_sign) com_release(f->trim_sign);
    memcpy(f->trim, trim, sizeof f->trim);
    f->trim_sign = sign;
    return S_OK;
}
static WINAPI HRESULT fmt_SetLineSpacing(void *this, uint32_t method, float spacing, float baseline) {
    dw_format *f = this;
    if (method > 2 || spacing < 0) return E_INVALIDARG;
    f->spacing_method = method; f->spacing = spacing; f->baseline = baseline;
    layout_dirty(this);
    return S_OK;
}
static WINAPI uint32_t fmt_GetTextAlignment(void *this) { return FORMAT(this)->align; }
static WINAPI uint32_t fmt_GetParagraphAlignment(void *this) { return FORMAT(this)->palign; }
static WINAPI uint32_t fmt_GetWordWrapping(void *this) { return FORMAT(this)->wrap; }
static WINAPI uint32_t fmt_GetReadingDirection(void *this) { return FORMAT(this)->rdir; }
static WINAPI uint32_t fmt_GetFlowDirection(void *this) { return FORMAT(this)->fdir; }
static WINAPI float fmt_GetIncrementalTabStop(void *this) { return FORMAT(this)->tab; }
static WINAPI HRESULT fmt_GetTrimming(void *this, uint32_t *trim, void **sign) {
    dw_format *f = this;
    if (!trim || !sign) return E_INVALIDARG;
    memcpy(trim, f->trim, sizeof f->trim);
    *sign = f->trim_sign;
    if (*sign) com_addref(*sign);
    return S_OK;
}
static WINAPI HRESULT fmt_GetLineSpacing(void *this, uint32_t *method, float *spacing, float *baseline) {
    dw_format *f = this;
    if (!method || !spacing || !baseline) return E_INVALIDARG;
    *method = f->spacing_method; *spacing = f->spacing; *baseline = f->baseline;
    return S_OK;
}
static WINAPI HRESULT fmt_GetFontCollection(void *this, void **out) {
    if (!out) return E_INVALIDARG;
    dw_collection *c = FORMAT(this)->a.coll;
    dw_addref(c);
    *out = c;
    return S_OK;
}
static WINAPI uint32_t fmt_GetFontFamilyNameLength(void *this) { return (uint32_t)vx_wcslen(FORMAT(this)->a.family); }
static WINAPI HRESULT fmt_GetFontFamilyName(void *this, WCHAR *out, uint32_t cap) { return wcs_out(FORMAT(this)->a.family, out, cap); }
static WINAPI uint32_t fmt_GetFontWeight(void *this) { return FORMAT(this)->a.weight; }
static WINAPI uint32_t fmt_GetFontStyle(void *this) { return FORMAT(this)->a.style; }
static WINAPI uint32_t fmt_GetFontStretch(void *this) { return FORMAT(this)->a.stretch; }
static WINAPI float fmt_GetFontSize(void *this) { return FORMAT(this)->a.size; }
static WINAPI uint32_t fmt_GetLocaleNameLength(void *this) { return (uint32_t)vx_wcslen(FORMAT(this)->a.locale); }
static WINAPI HRESULT fmt_GetLocaleName(void *this, WCHAR *out, uint32_t cap) { return wcs_out(FORMAT(this)->a.locale, out, cap); }

// ---- the layout itself ---------------------------------------------------------------------------------------------------
static int is_newline(uint32_t cp) { return cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D || cp == 0x85 || cp == 0x2028 || cp == 0x2029; }
static int is_space(uint32_t cp) { return cp == 0x20 || cp == 0x09 || cp == 0x3000 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A && cp != 0x2007) || cp == 0x205F; }
static int is_cjk(uint32_t cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x3FFFF);
}
static int is_hidden(uint32_t cp) {                  // control and format characters: no glyph, no advance
    return (cp < 0x20 && cp != 0x09) || (cp >= 0x7F && cp < 0xA0) || cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0x200E ||
           cp == 0x200F || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2064) || cp == 0xFEFF;
}
static const WCHAR g_letter_a[] = { 'A', 0 };
static dw_face *any_face(void) {                     // the face fallback gives a Latin letter: where nothing else is
    dw_font *ft = fallback_font(NULL, g_letter_a, 1, NULL, 400, 5, 0);
    return ft ? font_face(ft) : NULL;
}
static dw_face *attrs_face(text_attrs *a) {
    uint32_t idx;
    WCHAR name[128];
    wcs_copy(name, 128, a->family);
    if (!name[0] || !find_family(a->coll, name, &idx)) return NULL;
    dw_font *ft = family_match(a->coll->families[idx], (int)a->weight, (int)a->stretch, (int)a->style);
    return ft ? font_face(ft) : NULL;
}
static float px_round(dw_layout *l, float v) {       // GDI-compatible layouts: whole pixels
    if (!l->gdi) return v;
    float s = l->ppd * (l->m.m22 != 0 ? fabsf(l->m.m22) : 1.0f);
    return s > 0 ? roundf(v * s) / s : v;
}
static void layout_build(dw_layout *l) {
    if (!l->dirty) return;
    free(l->cl); free(l->lines);
    l->cl = calloc(l->len + 1, sizeof *l->cl); l->ncl = 0;
    if (!l->primary) l->primary = calloc(l->nattrs ? l->nattrs : 1, sizeof *l->primary);
    for (uint32_t a = 0; a < l->nattrs; a++) if (!l->primary[a]) l->primary[a] = attrs_face(&l->attrs[a]);
    dw_face *last_face = NULL, *last_fallback = NULL;
    for (uint32_t i = 0; i < l->len;) {
        uint32_t units, cp = next_cp(l->text, l->len, i, &units);
        if (cp == 0x0D && i + 1 < l->len && l->text[i + 1] == 0x0A) units = 2;
        text_cluster *c = &l->cl[l->ncl++];
        c->pos = i; c->units = units; c->attr = l->attr_of[i];
        text_attrs *a = &l->attrs[c->attr];
        dw_face *f = l->primary[c->attr];
        c->newline = is_newline(cp); c->space = is_space(cp); c->tab = cp == 0x09; c->soft_hyphen = cp == 0xAD;
        c->hidden = c->newline || is_hidden(cp) || c->soft_hyphen;
        if (!c->hidden) {
            uint32_t look = c->tab ? 0x20 : cp;
            uint16_t g = f ? face_glyph(f, look) : 0;
            if (!g && last_face && continues_cluster(cp) && face_glyph(last_face, cp)) { f = last_face; g = face_glyph(f, cp); }
            if (!g && last_fallback && (g = face_glyph(last_fallback, look))) f = last_fallback;   // (the font the text before fell back to)
            if (!g) {                                        // another font has it (fallback)
                dw_font *fb = fallback_font(f, l->text + i, l->len - i, a->locale, (int)a->weight, (int)a->stretch, (int)a->style);
                dw_face *ff = fb ? font_face(fb) : NULL;
                if (ff) { f = ff; g = face_glyph(ff, look); last_fallback = ff; }
            }
            if (!f) f = any_face();                          // no font has it: the notdef of a system font
            c->glyph = g;
            c->face = f;
            if (f) c->advance = px_round(l, (float)face_advance(f, g, NULL) * a->size / (float)f->upem);
        } else c->face = f ? f : last_face;
        if (c->face) last_face = c->face;
        if (c->space || cp == '-' || cp == 0x2010 || c->soft_hyphen) c->brk = 1;
        if (is_cjk(cp)) { c->brk = 1; if (l->ncl > 1) l->cl[l->ncl - 2].brk = 1; }
        i += units;
    }
    // lines
    l->lines = calloc(l->ncl + 2, sizeof *l->lines); l->nlines = 0;
    int wrap = l->f.wrap != 1 && l->max_w > 0 && l->max_w < 1e30f;      // (1: NO_WRAP)
    uint32_t start = 0;
    for (;;) {
        float x = 0;
        int64_t last_break = -1;
        uint32_t i = start, end = start;      // [start, end): the line's clusters
        int newline = 0;
        while (i < l->ncl) {
            text_cluster *c = &l->cl[i];
            if (c->newline) { end = i + 1; newline = 1; break; }
            if (c->tab) {
                float tab = l->f.tab > 0 ? l->f.tab : 4 * l->attrs[c->attr].size;
                c->advance = tab - fmodf(x, tab);
            }
            if (wrap && i > start && !c->space && x + c->advance > l->max_w + 0.001f) {
                if (last_break >= (int64_t)start) { end = (uint32_t)last_break + 1; break; }
                if (l->f.wrap != 3) { end = i; break; }                  // (3: WHOLE_WORD keeps a long word whole)
            }
            x += c->advance;
            if (c->brk) last_break = i;
            end = ++i;
        }
        while (!newline && end < l->ncl && l->cl[end].space) end++;       // trailing spaces stay on the line
        if (!newline && end < l->ncl && l->cl[end].newline) { end++; newline = 1; }
        text_line *ln = &l->lines[l->nlines++];
        ln->first = start; ln->count = end - start;
        float w = 0, trailing = 0, asc = 0, below = 0;
        uint32_t trailing_units = 0, units = 0, newline_units = 0;
        dw_face *where = NULL;                // (an empty line: the font where it is)
        float where_size = l->f.a.size;
        for (uint32_t k = start; k < end; k++) {
            text_cluster *c = &l->cl[k];
            units += c->units;
            if (c->face) { where = c->face; where_size = l->attrs[c->attr].size; }
            if (c->newline) { newline_units += c->units; continue; }
            if (c->space) { trailing += c->advance; trailing_units += c->units; }
            else { w += trailing + c->advance; trailing = 0; trailing_units = 0; }
            if (c->face && !c->hidden) {          // the line's height: its tallest font
                face_prepare(c->face);
                float s = l->attrs[c->attr].size / (float)c->face->upem;
                if (c->face->m.ascent * s > asc) asc = c->face->m.ascent * s;
                if ((c->face->m.descent + c->face->m.line_gap) * s > below) below = (c->face->m.descent + c->face->m.line_gap) * s;
            }
        }
        if (!asc && !below) {
            if (!where && start > 0 && start - 1 < l->ncl) { where = l->cl[start - 1].face; where_size = l->attrs[l->cl[start - 1].attr].size; }
            if (!where) where = l->nattrs && l->primary[0] ? l->primary[0] : any_face();
            if (where) {
                face_prepare(where);
                float s = where_size / (float)where->upem;
                asc = where->m.ascent * s; below = (where->m.descent + where->m.line_gap) * s;
            }
        }
        ln->width = w; ln->trailing = trailing;
        ln->baseline = px_round(l, asc); ln->height = px_round(l, asc + below);
        if (l->f.spacing_method == 1) { ln->height = l->f.spacing; ln->baseline = l->f.baseline; }                 // UNIFORM
        else if (l->f.spacing_method == 2) { ln->height *= l->f.spacing; ln->baseline *= l->f.baseline; }          // PROPORTIONAL
        ln->units = units; ln->trailing_units = trailing_units; ln->newline_units = newline_units;
        start = end;
        if (start < l->ncl) continue;
        if (newline) {                        // text ending in a newline: an empty last line
            text_line *e = &l->lines[l->nlines++];
            *e = (text_line){ l->ncl, 0, 0, 0, 0, 0, ln->height, ln->baseline, 0, 0, 0 };
        }
        break;
    }
    float total = 0, maxw = 0, maxw_ws = 0, left = 1e30f;
    for (uint32_t k = 0; k < l->nlines; k++) {
        text_line *ln = &l->lines[k];
        float room = l->max_w - ln->width;
        ln->x = l->f.align == 1 ? room : l->f.align == 2 ? room / 2 : 0;          // TRAILING, CENTER (JUSTIFIED: leading)
        if (l->f.rdir == 1) ln->x = l->f.align == 0 ? room : l->f.align == 1 ? 0 : ln->x;   // right-to-left paragraphs lead from the right
        ln->x = px_round(l, ln->x);
        ln->y = total;
        total += ln->height;
        if (ln->width > maxw) maxw = ln->width;
        if (ln->width + ln->trailing > maxw_ws) maxw_ws = ln->width + ln->trailing;
        if (ln->x < left) left = ln->x;
    }
    float top = l->f.palign == 1 ? l->max_h - total : l->f.palign == 2 ? (l->max_h - total) / 2 : 0;   // FAR, CENTER
    top = px_round(l, top);
    for (uint32_t k = 0; k < l->nlines; k++) l->lines[k].y += top;
    l->left = l->nlines ? left : 0; l->top = top; l->width = maxw; l->width_ws = maxw_ws; l->height = total;
    l->dirty = 0;
}
static void layout_lock_build(dw_layout *l) { lock(); layout_build(l); unlock(); }

// IDWriteTextRenderer (the program's): IsPixelSnappingDisabled 3, GetCurrentTransform 4, GetPixelsPerDip 5,
// DrawGlyphRun 6, DrawUnderline 7, DrawStrikethrough 8. Its drawing methods take the baseline origin as two FLOATs
// (XMM2, XMM3): in the ARM64 build those go into the program's XMM registers before the call.
#ifdef __aarch64__
#include "arm64.h"
#endif
static HRESULT call_with_origin(void *obj, int slot, void *ctx, float x, float y, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
#if defined(__aarch64__)
    x86_ctx *c = vx_x86_ctx();
    memset(c->xmm[2], 0, 16); memcpy(c->xmm[2], &x, 4);
    memset(c->xmm[3], 0, 16); memcpy(c->xmm[3], &y, 4);
    uint64_t args[8] = { (uint64_t)(uintptr_t)obj, (uint64_t)(uintptr_t)ctx, 0, 0, a4, a5, a6, a7 };
    return (HRESULT)vx_x86_call((uint64_t)(uintptr_t)SLOT(obj, slot), 8, args);
#elif !defined(VX_ABI_SCAN)
    return ((HRESULT (WINAPI *)(void *, void *, float, float, uint64_t, uint64_t, uint64_t, uint64_t))SLOT(obj, slot))(obj, ctx, x, y, a4, a5, a6, a7);
#else
    (void)obj; (void)slot; (void)ctx; (void)x; (void)y; (void)a4; (void)a5; (void)a6; (void)a7;
    return E_NOTIMPL;                                  // (tools/vx_abi_scan.py: the two builds above call it)
#endif
}
typedef struct { const WCHAR *locale, *string; uint32_t length; const uint16_t *cluster_map; uint32_t position; } run_description;
typedef struct { float width, thickness, offset, run_height; uint32_t rdir, fdir; const WCHAR *locale; uint32_t measuring; } line_decoration;
static WINAPI HRESULT layout_Draw(void *this, void *ctx, void *renderer, float ox, float oy) {
    dw_layout *l = this;
    if (!renderer) return E_INVALIDARG;
    layout_lock_build(l);
    TR("IDWriteTextLayout::Draw(%u characters, %u lines) at %.1f,%.1f", l->len, l->nlines, ox, oy);
    int32_t snap_off = 0; float ppd = 1; matrix m = { 1, 0, 0, 1, 0, 0 };
    ((HRESULT (WINAPI *)(void *, void *, int32_t *))SLOT(renderer, 3))(renderer, ctx, &snap_off);
    ((HRESULT (WINAPI *)(void *, void *, float *))SLOT(renderer, 5))(renderer, ctx, &ppd);
    ((HRESULT (WINAPI *)(void *, void *, matrix *))SLOT(renderer, 4))(renderer, ctx, &m);
    uint32_t measuring = l->gdi ? (l->gdi_natural ? MEASURE_GDI_NATURAL : MEASURE_GDI_CLASSIC) : MEASURE_NATURAL;
    uint16_t *glyphs = malloc(2 * (size_t)(l->ncl + 1)), *map = malloc(2 * (size_t)(l->len + 1));
    float *advances = malloc(4 * (size_t)(l->ncl + 1));
    HRESULT hr = S_OK;
    for (uint32_t li = 0; li < l->nlines && hr >= 0; li++) {
        text_line *ln = &l->lines[li];
        float x = ox + ln->x, y = oy + ln->y + ln->baseline;
        if (!snap_off) {                                      // the baseline on a whole device pixel
            float s = ppd * (m.m22 != 0 ? m.m22 : 1);
            if (s > 0) y = (roundf((y * s) + m.dy) - m.dy) / s;
        }
        for (uint32_t k = ln->first; k < ln->first + ln->count && hr >= 0;) {
            text_cluster *c = &l->cl[k];
            if (c->hidden || !c->face) { x += c->advance; k++; continue; }
            uint32_t n = 0, first = k;
            float run_w = 0;
            while (k < ln->first + ln->count && !l->cl[k].hidden && l->cl[k].face == c->face && l->cl[k].attr == c->attr) {
                for (uint32_t u = 0; u < l->cl[k].units; u++) map[l->cl[k].pos - c->pos + u] = (uint16_t)n;
                glyphs[n] = l->cl[k].glyph; advances[n] = l->cl[k].advance; run_w += advances[n]; n++; k++;
            }
            text_attrs *a = &l->attrs[c->attr];
            uint32_t units = l->cl[k - 1].pos + l->cl[k - 1].units - c->pos;
            glyph_run run = { c->face, a->size, n, glyphs, advances, NULL, 0, 0 };
            run_description desc = { a->locale, l->text + c->pos, units, map, c->pos };
            hr = call_with_origin(renderer, 6, ctx, x, y, measuring, (uint64_t)(uintptr_t)&run, (uint64_t)(uintptr_t)&desc, (uint64_t)(uintptr_t)a->effect);
            face_prepare(c->face);
            float s = a->size / (float)c->face->upem;
            if (hr >= 0 && a->underline) {
                line_decoration u = { run_w, c->face->m.underline_thick * s, -c->face->m.underline_pos * s, ln->height, l->f.rdir, l->f.fdir, a->locale, measuring };
                hr = call_with_origin(renderer, 7, ctx, x, y, (uint64_t)(uintptr_t)&u, (uint64_t)(uintptr_t)a->effect, 0, 0);
            }
            if (hr >= 0 && a->strike) {          // DWRITE_STRIKETHROUGH: line_decoration without runHeight
                struct { float width, thickness, offset; uint32_t rdir, fdir; const WCHAR *locale; uint32_t measuring; } st =
                    { run_w, c->face->m.strike_thick * s, -c->face->m.strike_pos * s, l->f.rdir, l->f.fdir, a->locale, measuring };
                hr = call_with_origin(renderer, 8, ctx, x, y, (uint64_t)(uintptr_t)&st, (uint64_t)(uintptr_t)a->effect, 0, 0);
            }
            x += run_w;
            (void)first;
        }
    }
    free(glyphs); free(map); free(advances);
    return hr;
}
typedef struct { float left, top, width, width_ws, height, layout_w, layout_h; uint32_t depth, lines; } text_metrics;
static WINAPI HRESULT layout_GetMetrics(void *this, text_metrics *out) {
    dw_layout *l = this;
    if (!out) return E_INVALIDARG;
    layout_lock_build(l);
    *out = (text_metrics){ l->left, l->top, l->width, l->width_ws, l->height, l->max_w, l->max_h, 1, l->nlines };
    TR("IDWriteTextLayout::GetMetrics(%u characters) -> %.1f x %.1f, %u lines", l->len, l->width, l->height, l->nlines);
    return S_OK;
}
typedef struct { uint32_t length, trailing, newline; float height, baseline; int32_t trimmed; } line_metrics;
static WINAPI HRESULT layout_GetLineMetrics(void *this, line_metrics *out, uint32_t max, uint32_t *count) {
    dw_layout *l = this;
    if (!count) return E_INVALIDARG;
    layout_lock_build(l);
    *count = l->nlines;
    if (!out || max < l->nlines) return E_NOT_SUFFICIENT_BUFFER;
    for (uint32_t i = 0; i < l->nlines; i++)
        out[i] = (line_metrics){ l->lines[i].units, l->lines[i].trailing_units, l->lines[i].newline_units, l->lines[i].height, l->lines[i].baseline, 0 };
    return S_OK;
}
typedef struct { float left, top, right, bottom; } overhangs;
static WINAPI HRESULT layout_GetOverhangMetrics(void *this, overhangs *out) {
    dw_layout *l = this;
    if (!out) return E_INVALIDARG;
    layout_lock_build(l);
    // the ink: the glyphs' black boxes, by line
    float ink_l = 1e30f, ink_t = 1e30f, ink_r = -1e30f, ink_b = -1e30f;
    for (uint32_t li = 0; li < l->nlines; li++) {
        text_line *ln = &l->lines[li];
        float x = ln->x;
        for (uint32_t k = ln->first; k < ln->first + ln->count; k++) {
            text_cluster *c = &l->cl[k];
            if (!c->hidden && c->face && !c->space) {
                glyph_metrics gm;
                face_glyph_metrics(c->face, c->glyph, &gm);
                float s = l->attrs[c->attr].size / (float)c->face->upem, base = ln->y + ln->baseline;
                float gl = x + gm.lsb * s, gr = x + ((float)gm.advance - gm.rsb) * s;
                float gt = base - (gm.vorigin - gm.tsb) * s, gb = base + ((float)gm.advance_height - gm.vorigin - gm.bsb) * s;
                if (gl < ink_l) ink_l = gl; if (gr > ink_r) ink_r = gr; if (gt < ink_t) ink_t = gt; if (gb > ink_b) ink_b = gb;
            }
            x += c->advance;
        }
    }
    if (ink_r < ink_l) { ink_l = ink_r = l->left; ink_t = ink_b = l->top; }
    *out = (overhangs){ -ink_l, -ink_t, ink_r - l->max_w, ink_b - l->max_h };
    return S_OK;
}
typedef struct { float width; uint16_t length, flags; } cluster_metrics;     // flags: canWrapLineAfter 1, isWhitespace 2, isNewline 4, isSoftHyphen 8
static WINAPI HRESULT layout_GetClusterMetrics(void *this, cluster_metrics *out, uint32_t max, uint32_t *count) {
    dw_layout *l = this;
    if (!count) return E_INVALIDARG;
    layout_lock_build(l);
    *count = l->ncl;
    if (!out || max < l->ncl) return E_NOT_SUFFICIENT_BUFFER;
    for (uint32_t i = 0; i < l->ncl; i++) {
        text_cluster *c = &l->cl[i];
        out[i] = (cluster_metrics){ c->advance, (uint16_t)c->units, (uint16_t)((c->brk || c->newline || i + 1 == l->ncl) | c->space << 1 | c->newline << 2 | c->soft_hyphen << 3) };
    }
    return S_OK;
}
static WINAPI HRESULT layout_DetermineMinWidth(void *this, float *out) {
    dw_layout *l = this;
    if (!out) return E_INVALIDARG;
    layout_lock_build(l);
    float best = 0, word = 0;
    for (uint32_t i = 0; i < l->ncl; i++) {
        text_cluster *c = &l->cl[i];
        if (c->newline || c->space) { word = 0; continue; }
        word += c->advance;
        if (word > best) best = word;
        if (c->brk) word = 0;
    }
    *out = best;
    return S_OK;
}
typedef struct { uint32_t position, length; float left, top, width, height; uint32_t bidi; int32_t is_text, trimmed; } hit_metrics;
static void cluster_hit(dw_layout *l, uint32_t li, uint32_t k, float x, hit_metrics *h) {
    text_line *ln = &l->lines[li];
    text_cluster *c = k < l->ncl ? &l->cl[k] : NULL;
    *h = (hit_metrics){ c ? c->pos : l->len, c ? c->units : 0, x, ln->y, c ? c->advance : 0, ln->height, 0, c && !c->newline, 0 };
}
static float cluster_x(dw_layout *l, uint32_t li, uint32_t k) {
    text_line *ln = &l->lines[li];
    float x = ln->x;
    for (uint32_t i = ln->first; i < k && i < ln->first + ln->count; i++) x += l->cl[i].advance;
    return x;
}
static int line_of_cluster(dw_layout *l, uint32_t k) {
    for (uint32_t li = 0; li < l->nlines; li++) if (k < l->lines[li].first + l->lines[li].count || li + 1 == l->nlines) return (int)li;
    return 0;
}
static uint32_t cluster_at(dw_layout *l, uint32_t pos) { uint32_t k = 0; while (k < l->ncl && l->cl[k].pos + l->cl[k].units <= pos) k++; return k; }
static WINAPI HRESULT layout_HitTestPoint(void *this, float px, float py, int32_t *trailing, int32_t *inside, hit_metrics *h) {
    dw_layout *l = this;
    if (!trailing || !inside || !h) return E_INVALIDARG;
    layout_lock_build(l);
    uint32_t li = 0;
    while (li + 1 < l->nlines && py >= l->lines[li].y + l->lines[li].height) li++;
    text_line *ln = &l->lines[li];
    *inside = py >= l->top && py < l->top + l->height && px >= ln->x && px < ln->x + ln->width;
    float x = ln->x;
    uint32_t k = ln->first, last = ln->first + ln->count;
    while (last > ln->first && l->cl[last - 1].newline) last--;
    *trailing = 0;
    if (px < x || last == ln->first) { cluster_hit(l, li, last == ln->first ? ln->first : k, x, h); return S_OK; }
    for (; k < last; k++) {
        float a = l->cl[k].advance;
        if (px < x + a) { *trailing = px >= x + a / 2; cluster_hit(l, li, k, x, h); return S_OK; }
        x += a;
    }
    *trailing = 1;
    cluster_hit(l, li, last - 1, x - l->cl[last - 1].advance, h);
    return S_OK;
}
static WINAPI HRESULT layout_HitTestTextPosition(void *this, uint32_t pos, int32_t trailing, float *px, float *py, hit_metrics *h) {
    dw_layout *l = this;
    if (!px || !py || !h) return E_INVALIDARG;
    layout_lock_build(l);
    uint32_t k = cluster_at(l, pos);
    int li = line_of_cluster(l, k);
    float x = cluster_x(l, (uint32_t)li, k);
    cluster_hit(l, (uint32_t)li, k, x, h);
    *px = x + (trailing && k < l->ncl ? l->cl[k].advance : 0);
    *py = l->lines[li].y;
    return S_OK;
}
static WINAPI HRESULT layout_HitTestTextRange(void *this, uint32_t pos, uint32_t len, float ox, float oy, hit_metrics *out, uint32_t max, uint32_t *count) {
    dw_layout *l = this;
    if (!count) return E_INVALIDARG;
    layout_lock_build(l);
    uint32_t n = 0, end = pos + len < pos ? UINT32_MAX : pos + len;
    for (uint32_t li = 0; li < l->nlines; li++) {
        text_line *ln = &l->lines[li];
        float x = ln->x, x0 = 0, x1 = 0;
        uint32_t p0 = UINT32_MAX, p1 = 0;
        for (uint32_t k = ln->first; k < ln->first + ln->count; k++) {
            text_cluster *c = &l->cl[k];
            if (c->pos + c->units > pos && c->pos < end) {
                if (p0 == UINT32_MAX) { p0 = c->pos; x0 = x; }
                p1 = c->pos + c->units; x1 = x + c->advance;
            }
            x += c->advance;
        }
        if (p0 == UINT32_MAX) continue;
        if (out && n < max) out[n] = (hit_metrics){ p0, p1 - p0, ox + x0, oy + ln->y, x1 - x0, ln->height, 0, 1, 0 };
        n++;
    }
    *count = n;
    return out && n <= max ? S_OK : E_NOT_SUFFICIENT_BUFFER;
}

// Attributes over ranges: every UTF-16 unit has an attribute set; setting one on a range gives the units there sets
// with that attribute changed (shared between units with equal sets).
typedef struct { uint32_t start, length; } text_range;
enum { A_COLL, A_FAMILY, A_WEIGHT, A_STYLE, A_STRETCH, A_SIZE, A_UNDERLINE, A_STRIKE, A_EFFECT, A_TYPOGRAPHY, A_LOCALE };
static int attrs_equal(const text_attrs *a, const text_attrs *b) {
    return a->coll == b->coll && !memcmp(a->family, b->family, sizeof a->family) && !memcmp(a->locale, b->locale, sizeof a->locale) && a->weight == b->weight &&
           a->style == b->style && a->stretch == b->stretch && a->size == b->size && a->underline == b->underline && a->strike == b->strike &&
           a->effect == b->effect && a->typography == b->typography;
}
static void attrs_change(text_attrs *a, int what, const void *v) {
    switch (what) {
    case A_COLL: { dw_collection *c = *(dw_collection *const *)v; dw_addref(c); dw_release(a->coll); a->coll = c; break; }
    case A_FAMILY: memset(a->family, 0, sizeof a->family); wcs_copy(a->family, 128, v); break;
    case A_LOCALE: memset(a->locale, 0, sizeof a->locale); wcs_copy(a->locale, 86, v); break;
    case A_WEIGHT: a->weight = *(const uint32_t *)v; break;
    case A_STYLE: a->style = *(const uint32_t *)v; break;
    case A_STRETCH: a->stretch = *(const uint32_t *)v; break;
    case A_SIZE: a->size = *(const float *)v; break;
    case A_UNDERLINE: a->underline = *(const int32_t *)v != 0; break;
    case A_STRIKE: a->strike = *(const int32_t *)v != 0; break;
    case A_EFFECT: { void *e = *(void *const *)v; if (e) com_addref(e); if (a->effect) com_release(a->effect); a->effect = e; break; }
    case A_TYPOGRAPHY: { void *t = *(void *const *)v; if (t) dw_addref(t); if (a->typography) dw_release(a->typography); a->typography = t; break; }
    }
}
static uint16_t attrs_index(dw_layout *l, const text_attrs *a) {
    for (uint32_t i = 0; i < l->nattrs; i++) if (attrs_equal(&l->attrs[i], a)) return (uint16_t)i;
    l->attrs = realloc(l->attrs, (l->nattrs + 1) * sizeof *l->attrs);
    l->primary = realloc(l->primary, (l->nattrs + 1) * sizeof *l->primary);
    l->attrs[l->nattrs] = *a;
    attrs_hold(&l->attrs[l->nattrs]);
    l->primary[l->nattrs] = NULL;
    return (uint16_t)l->nattrs++;
}
static HRESULT layout_set(void *this, int what, const void *v, text_range r) {
    dw_layout *l = this;
    if (r.start >= l->len || !r.length) return S_OK;
    uint32_t end = r.length > l->len - r.start ? l->len : r.start + r.length;
    lock();
    if (l->nattrs >= 60000) { unlock(); return E_OUTOFMEMORY; }
    for (uint32_t i = r.start; i < end; i++) {
        text_attrs a = l->attrs[l->attr_of[i]];
        // (a copy holding no references of its own: attrs_change swaps references in it, so take them first)
        attrs_hold(&a);
        attrs_change(&a, what, v);
        l->attr_of[i] = attrs_index(l, &a);
        attrs_drop(&a);
    }
    l->dirty = 1;
    unlock();
    return S_OK;
}
static WINAPI HRESULT layout_SetMaxWidth(void *this, float w) { if (w < 0) return E_INVALIDARG; ((dw_layout *)this)->max_w = w; layout_dirty(this); return S_OK; }
static WINAPI HRESULT layout_SetMaxHeight(void *this, float h) { if (h < 0) return E_INVALIDARG; ((dw_layout *)this)->max_h = h; layout_dirty(this); return S_OK; }
static WINAPI HRESULT layout_SetFontCollection(void *this, void *c, text_range r) {
    dw_collection *coll = c ? c : system_collection();
    if (!is_ours(coll, g_collection_vtbl)) return E_INVALIDARG;
    return layout_set(this, A_COLL, &coll, r);
}
static WINAPI HRESULT layout_SetFontFamilyName(void *this, const WCHAR *name, text_range r) { if (!name) return E_INVALIDARG; return layout_set(this, A_FAMILY, name, r); }
static WINAPI HRESULT layout_SetFontWeight(void *this, uint32_t v, text_range r) { if (v < 1 || v > 999) return E_INVALIDARG; return layout_set(this, A_WEIGHT, &v, r); }
static WINAPI HRESULT layout_SetFontStyle(void *this, uint32_t v, text_range r) { if (v > 2) return E_INVALIDARG; return layout_set(this, A_STYLE, &v, r); }
static WINAPI HRESULT layout_SetFontStretch(void *this, uint32_t v, text_range r) { if (v < 1 || v > 9) return E_INVALIDARG; return layout_set(this, A_STRETCH, &v, r); }
static WINAPI HRESULT layout_SetFontSize(void *this, float v, text_range r) { if (!(v > 0)) return E_INVALIDARG; return layout_set(this, A_SIZE, &v, r); }
static WINAPI HRESULT layout_SetUnderline(void *this, int32_t v, text_range r) { return layout_set(this, A_UNDERLINE, &v, r); }
static WINAPI HRESULT layout_SetStrikethrough(void *this, int32_t v, text_range r) { return layout_set(this, A_STRIKE, &v, r); }
static WINAPI HRESULT layout_SetDrawingEffect(void *this, void *e, text_range r) { return layout_set(this, A_EFFECT, &e, r); }
static WINAPI HRESULT layout_SetInlineObject(void *this, void *o, text_range r) { (void)this; (void)o; (void)r; return dw_not_implemented("IDWriteTextLayout::SetInlineObject"); }
static WINAPI HRESULT layout_SetTypography(void *this, void *t, text_range r) {
    if (t && !is_ours(t, g_typography_vtbl)) return E_INVALIDARG;
    return layout_set(this, A_TYPOGRAPHY, &t, r);
}
static WINAPI HRESULT layout_SetLocaleName(void *this, const WCHAR *name, text_range r) { if (!name) return E_INVALIDARG; return layout_set(this, A_LOCALE, name, r); }
static WINAPI float layout_GetMaxWidth(void *this) { return ((dw_layout *)this)->max_w; }
static WINAPI float layout_GetMaxHeight(void *this) { return ((dw_layout *)this)->max_h; }
// The attributes at a position, and the range of positions that share them.
static text_attrs *attrs_at(dw_layout *l, uint32_t pos, text_range *r) {
    if (pos >= l->len) { if (r) *r = (text_range){ l->len, UINT32_MAX - l->len }; return &l->f.a; }
    uint16_t a = l->attr_of[pos];
    uint32_t s = pos, e = pos + 1;
    while (s > 0 && l->attr_of[s - 1] == a) s--;
    while (e < l->len && l->attr_of[e] == a) e++;
    if (r) *r = (text_range){ s, e - s };
    return &l->attrs[a];
}
static WINAPI HRESULT layout_GetFontCollectionAt(void *this, uint32_t pos, void **out, text_range *r) {
    if (!out) return E_INVALIDARG;
    text_attrs *a = attrs_at(this, pos, r);
    dw_addref(a->coll); *out = a->coll;
    return S_OK;
}
static WINAPI HRESULT layout_GetFontFamilyNameLengthAt(void *this, uint32_t pos, uint32_t *len, text_range *r) { if (!len) return E_INVALIDARG; *len = (uint32_t)vx_wcslen(attrs_at(this, pos, r)->family); return S_OK; }
static WINAPI HRESULT layout_GetFontFamilyNameAt(void *this, uint32_t pos, WCHAR *out, uint32_t cap, text_range *r) { return wcs_out(attrs_at(this, pos, r)->family, out, cap); }
static WINAPI HRESULT layout_GetFontWeightAt(void *this, uint32_t pos, uint32_t *v, text_range *r) { if (!v) return E_INVALIDARG; *v = attrs_at(this, pos, r)->weight; return S_OK; }
static WINAPI HRESULT layout_GetFontStyleAt(void *this, uint32_t pos, uint32_t *v, text_range *r) { if (!v) return E_INVALIDARG; *v = attrs_at(this, pos, r)->style; return S_OK; }
static WINAPI HRESULT layout_GetFontStretchAt(void *this, uint32_t pos, uint32_t *v, text_range *r) { if (!v) return E_INVALIDARG; *v = attrs_at(this, pos, r)->stretch; return S_OK; }
static WINAPI HRESULT layout_GetFontSizeAt(void *this, uint32_t pos, float *v, text_range *r) { if (!v) return E_INVALIDARG; *v = attrs_at(this, pos, r)->size; return S_OK; }
static WINAPI HRESULT layout_GetUnderline(void *this, uint32_t pos, int32_t *v, text_range *r) { if (!v) return E_INVALIDARG; *v = attrs_at(this, pos, r)->underline; return S_OK; }
static WINAPI HRESULT layout_GetStrikethrough(void *this, uint32_t pos, int32_t *v, text_range *r) { if (!v) return E_INVALIDARG; *v = attrs_at(this, pos, r)->strike; return S_OK; }
static WINAPI HRESULT layout_GetDrawingEffect(void *this, uint32_t pos, void **v, text_range *r) {
    if (!v) return E_INVALIDARG;
    *v = attrs_at(this, pos, r)->effect;
    if (*v) com_addref(*v);
    return S_OK;
}
static WINAPI HRESULT layout_GetInlineObject(void *this, uint32_t pos, void **v, text_range *r) { if (!v) return E_INVALIDARG; attrs_at(this, pos, r); *v = NULL; return S_OK; }
static WINAPI HRESULT layout_GetTypography(void *this, uint32_t pos, void **v, text_range *r) {
    if (!v) return E_INVALIDARG;
    *v = attrs_at(this, pos, r)->typography;
    if (*v) dw_addref(*v);
    return S_OK;
}
static WINAPI HRESULT layout_GetLocaleNameLengthAt(void *this, uint32_t pos, uint32_t *len, text_range *r) { if (!len) return E_INVALIDARG; *len = (uint32_t)vx_wcslen(attrs_at(this, pos, r)->locale); return S_OK; }
static WINAPI HRESULT layout_GetLocaleNameAt(void *this, uint32_t pos, WCHAR *out, uint32_t cap, text_range *r) { return wcs_out(attrs_at(this, pos, r)->locale, out, cap); }

static HRESULT format_new(const WCHAR *family, void *coll, uint32_t weight, uint32_t style, uint32_t stretch, float size, const WCHAR *locale, void **out) {
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!family || !locale || !(size > 0) || weight < 1 || weight > 999 || style > 2 || stretch < 1 || stretch > 9) return E_INVALIDARG;
    if (coll && !is_ours(coll, g_collection_vtbl)) return E_INVALIDARG;
    dw_format *f = dw_new(sizeof *f, g_format_vtbl, g_format_iids, "IDWriteTextFormat", format_destroy);
    wcs_copy(f->a.family, 128, family); wcs_copy(f->a.locale, 86, locale);
    f->a.coll = coll ? coll : system_collection();
    dw_addref(f->a.coll);
    f->a.weight = weight; f->a.style = style; f->a.stretch = stretch; f->a.size = size;
    f->tab = 4 * size;
    f->wrap = 0;                                              // DWRITE_WORD_WRAPPING_WRAP
    if (g_trace) { char *n = vx_utf8_dup(family); TR("IDWriteFactory::CreateTextFormat(%s, %u, %u, %u, %.1f)", n, weight, style, stretch, size); free(n); }
    *out = f;
    return S_OK;
}
static HRESULT layout_new(const WCHAR *text, uint32_t len, void *format, float w, float h, int gdi, float ppd, const matrix *m, int natural, void **out) {
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!is_format(format) || (len && !text) || w < 0 || h < 0 || len >= 1u << 28) return E_INVALIDARG;
    dw_format *src = format;
    dw_layout *l = dw_new(sizeof *l, g_layout_vtbl, g_layout_iids, "IDWriteTextLayout", format_destroy);
    // the format's settings (a layout as the format: its own defaults)
    memcpy((char *)&l->f + sizeof(dw_obj), (const char *)src + sizeof(dw_obj), sizeof(dw_format) - sizeof(dw_obj));
    attrs_hold(&l->f.a);
    if (l->f.trim_sign) com_addref(l->f.trim_sign);
    l->text = malloc(2 * (size_t)(len + 1));
    if (len) memcpy(l->text, text, 2 * (size_t)len);
    l->text[len] = 0; l->len = len;
    l->max_w = w; l->max_h = h;
    l->gdi = gdi; l->gdi_natural = natural; l->ppd = ppd > 0 ? ppd : 1;
    l->m = m ? *m : (matrix){ 1, 0, 0, 1, 0, 0 };
    l->attr_of = calloc(len + 1, sizeof *l->attr_of);
    attrs_index(l, &l->f.a);
    l->dirty = 1;
    if (g_trace) { char *s = vx_utf8_dup(l->text); TR("IDWriteFactory::Create%sTextLayout(\"%.40s\", %.0f x %.0f)", gdi ? "GdiCompatible" : "", s, w, h); free(s); }
    *out = l;
    return S_OK;
}

// ---- IDWriteTypography: font features kept (the layout does not shape, so they change nothing yet) --------------------------
typedef struct { dw_obj o; uint32_t (*features)[2]; uint32_t count; } dw_typography;
static const GUID *const g_typography_iids[] = { &IID_IDWriteTypography, NULL };
static void typography_destroy(dw_obj *o) { free(((dw_typography *)o)->features); free(o); }
typedef struct { uint32_t tag, parameter; } font_feature;
static WINAPI HRESULT typo_AddFontFeature(void *this, font_feature f) {
    dw_typography *t = this;
    t->features = realloc(t->features, (t->count + 1) * sizeof *t->features);
    t->features[t->count][0] = f.tag; t->features[t->count][1] = f.parameter; t->count++;
    return S_OK;
}
static WINAPI uint32_t typo_GetFontFeatureCount(void *this) { return ((dw_typography *)this)->count; }
static WINAPI HRESULT typo_GetFontFeature(void *this, uint32_t i, font_feature *out) {
    dw_typography *t = this;
    if (!out || i >= t->count) return E_INVALIDARG;
    out->tag = t->features[i][0]; out->parameter = t->features[i][1];
    return S_OK;
}

// ---- IDWriteBitmapRenderTarget: a memory DC with a 32-bit DIB section that glyph runs are blended into ----------------------
typedef struct { dw_obj o; HANDLE dc, bitmap, first; uint32_t *bits; int w, h; float ppd; matrix m; } dw_target;
static const GUID *const g_target_iids[] = { &IID_IDWriteBitmapRenderTarget, NULL };
static void *gdi_fn(const char *name) { return vx_find_export("gdi32.dll", name); }
static int target_bitmap(dw_target *t, int w, int h) {
    // BITMAPINFOHEADER: top-down (negative height), 32 bits per pixel, BI_RGB
    struct { DWORD size; LONG width, height; uint16_t planes, bpp; DWORD compression, image_size; LONG xppm, yppm; DWORD used, important; } bi =
        { 40, w > 0 ? w : 1, -(h > 0 ? h : 1), 1, 32, 0, 0, 0, 0, 0, 0 };
    void *bits = NULL;
    HANDLE b = ((HANDLE (WINAPI *)(HANDLE, const void *, uint32_t, void **, HANDLE, DWORD))gdi_fn("CreateDIBSection"))(t->dc, &bi, 0, &bits, NULL, 0);
    if (!b || !bits) return 0;
    HANDLE old = ((HANDLE (WINAPI *)(HANDLE, HANDLE))gdi_fn("SelectObject"))(t->dc, b);
    if (!t->first) t->first = old;
    if (t->bitmap) ((BOOL (WINAPI *)(HANDLE))gdi_fn("DeleteObject"))(t->bitmap);
    t->bitmap = b; t->bits = bits; t->w = w; t->h = h;
    return 1;
}
static void target_destroy(dw_obj *o) {
    dw_target *t = (dw_target *)o;
    if (t->first) ((HANDLE (WINAPI *)(HANDLE, HANDLE))gdi_fn("SelectObject"))(t->dc, t->first);
    if (t->bitmap) ((BOOL (WINAPI *)(HANDLE))gdi_fn("DeleteObject"))(t->bitmap);
    if (t->dc) ((BOOL (WINAPI *)(HANDLE))gdi_fn("DeleteDC"))(t->dc);
    free(t);
}
static WINAPI HRESULT target_DrawGlyphRun(void *this, float x, float y, uint32_t measuring, const glyph_run *run, void *params, uint32_t color, RECT *box) {
    dw_target *t = this;
    if (box) memset(box, 0, sizeof *box);
    if (!run) return E_INVALIDARG;
    uint32_t mode = is_ours(params, g_params_vtbl) && ((dw_params *)params)->mode == MODE_ALIASED ? MODE_ALIASED : MODE_NATURAL;
    void *an = NULL;
    (void)measuring;
    HRESULT hr = analysis_new(run, t->ppd, &t->m, mode, AA_GRAYSCALE, x, y, &an);
    if (hr < 0) return hr;
    dw_analysis *a = an;
    if (a->cov) {
        uint8_t cr = color & 255, cg = (color >> 8) & 255, cb = (color >> 16) & 255;
        for (int yy = 0; yy < a->h; yy++) {
            int dy = a->y0 + yy;
            if (dy < 0 || dy >= t->h) continue;
            for (int xx = 0; xx < a->w; xx++) {
                int dx = a->x0 + xx;
                uint32_t v = a->cov[(size_t)yy * a->w + xx];
                if (!v || dx < 0 || dx >= t->w) continue;
                uint8_t *p = (uint8_t *)&t->bits[(size_t)dy * t->w + dx];        // B G R x
                p[0] = (uint8_t)(p[0] + ((int)cb - p[0]) * (int)v / 255);
                p[1] = (uint8_t)(p[1] + ((int)cg - p[1]) * (int)v / 255);
                p[2] = (uint8_t)(p[2] + ((int)cr - p[2]) * (int)v / 255);
            }
        }
        if (box) *box = a->bounds;
    }
    TR("IDWriteBitmapRenderTarget::DrawGlyphRun(%u glyphs at %.1f,%.1f, colour %06x) -> %dx%d pixels", run->count, x, y, color, a->w, a->h);
    dw_release(a);
    return S_OK;
}
static WINAPI HANDLE target_GetMemoryDC(void *this) { return ((dw_target *)this)->dc; }
static WINAPI float target_GetPixelsPerDip(void *this) { return ((dw_target *)this)->ppd; }
static WINAPI HRESULT target_SetPixelsPerDip(void *this, float v) { if (!(v > 0)) return E_INVALIDARG; ((dw_target *)this)->ppd = v; return S_OK; }
static WINAPI HRESULT target_GetCurrentTransform(void *this, matrix *m) { if (!m) return E_INVALIDARG; *m = ((dw_target *)this)->m; return S_OK; }
static WINAPI HRESULT target_SetCurrentTransform(void *this, const matrix *m) { ((dw_target *)this)->m = m ? *m : (matrix){ 1, 0, 0, 1, 0, 0 }; return S_OK; }
typedef struct { LONG cx, cy; } size_l;
static WINAPI HRESULT target_GetSize(void *this, size_l *s) { if (!s) return E_INVALIDARG; s->cx = ((dw_target *)this)->w; s->cy = ((dw_target *)this)->h; return S_OK; }
static WINAPI HRESULT target_Resize(void *this, uint32_t w, uint32_t h) {
    dw_target *t = this;
    if ((int)w == t->w && (int)h == t->h) return S_OK;
    return target_bitmap(t, (int)w, (int)h) ? S_OK : E_OUTOFMEMORY;
}
static WINAPI HRESULT gdi_CreateBitmapRenderTarget(void *this, HANDLE hdc, uint32_t w, uint32_t h, void **out) {
    (void)this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    dw_target *t = dw_new(sizeof *t, g_target_vtbl, g_target_iids, "IDWriteBitmapRenderTarget", target_destroy);
    t->ppd = 1; t->m = (matrix){ 1, 0, 0, 1, 0, 0 };
    t->dc = ((HANDLE (WINAPI *)(HANDLE))gdi_fn("CreateCompatibleDC"))(hdc);
    if (!t->dc || !target_bitmap(t, (int)w, (int)h)) { target_destroy(&t->o); return E_OUTOFMEMORY; }
    TR("IDWriteGdiInterop::CreateBitmapRenderTarget(%u x %u)", w, h);
    *out = t;
    return S_OK;
}

// ---- IDWriteFactory2 ----------------------------------------------------------------------------------------------------------
typedef struct { dw_obj o; void *file_loaders[64]; int nfile; void *coll_loaders[64]; int ncoll; } dw_factory;
static const GUID *const g_factory_iids[] = { &IID_IDWriteFactory, &IID_IDWriteFactory1, &IID_IDWriteFactory2, NULL };
static dw_factory g_shared;
static void factory_destroy(dw_obj *o) {
    dw_factory *f = (dw_factory *)o;
    for (int i = 0; i < f->nfile; i++) com_release(f->file_loaders[i]);
    for (int i = 0; i < f->ncoll; i++) com_release(f->coll_loaders[i]);
    free(f);
}
static int loader_index(void **list, int n, void *l) { for (int i = 0; i < n; i++) if (list[i] == l) return i; return -1; }
static HRESULT loader_register(void **list, int *n, void *l) {
    if (!l) return E_INVALIDARG;
    lock();
    HRESULT hr = loader_index(list, *n, l) >= 0 ? DWRITE_E_ALREADYREGISTERED : *n >= 64 ? E_OUTOFMEMORY : S_OK;
    if (hr == S_OK) { com_addref(l); list[(*n)++] = l; }
    unlock();
    return hr;
}
static HRESULT loader_unregister(void **list, int *n, void *l) {
    if (!l) return E_INVALIDARG;
    lock();
    int i = loader_index(list, *n, l);
    if (i >= 0) { list[i] = list[--(*n)]; }
    unlock();
    if (i < 0) return E_INVALIDARG;
    com_release(l);
    return S_OK;
}
static WINAPI HRESULT factory_GetSystemFontCollection(void *this, void **out, int32_t check) {
    (void)this; (void)check;
    if (!out) return E_INVALIDARG;
    dw_collection *c = system_collection();
    TR("IDWriteFactory::GetSystemFontCollection -> %u families", c->visible);
    dw_addref(c);
    *out = c;
    return S_OK;
}
static void collection_add_face(dw_collection *c, dw_face *f) {
    CFStringRef name = CTFontCopyFamilyName(f->ct);
    if (!name) name = CFStringCreateWithCString(NULL, f->ps[0] ? f->ps : "Unknown", kCFStringEncodingUTF8);
    dw_family *fam = family_named(c, name, c->count);
    if (!fam && c->count == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 8;
        c->families = realloc(c->families, c->cap * sizeof *c->families);
    }
    if (!fam) fam = family_new(c, name, 1);
    CFRelease(name);
    if (fam) font_new(fam, NULL, f); else dw_release(f);
}
static WINAPI HRESULT factory_CreateCustomFontCollection(void *this, void *loader, const void *key, uint32_t key_size, void **out) {
    dw_factory *fac = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    lock();
    int known = loader && loader_index(fac->coll_loaders, fac->ncoll, loader) >= 0;
    unlock();
    if (!known) return E_INVALIDARG;
    // IDWriteFontCollectionLoader::CreateEnumeratorFromKey (3); IDWriteFontFileEnumerator::MoveNext (3), GetCurrentFontFile (4)
    void *en = NULL;
    HRESULT hr = ((HRESULT (WINAPI *)(void *, void *, const void *, uint32_t, void **))SLOT(loader, 3))(loader, this, key, key_size, &en);
    if (hr < 0) return hr;
    if (!en) return E_FAIL;
    dw_collection *c = collection_new(0, 8);
    int files = 0;
    for (;;) {
        int32_t more = 0;
        if (((HRESULT (WINAPI *)(void *, int32_t *))SLOT(en, 3))(en, &more) < 0 || !more) break;
        void *file = NULL;
        if (((HRESULT (WINAPI *)(void *, void **))SLOT(en, 4))(en, &file) < 0 || !file) continue;
        file = VX_LOW(file);
        if (is_ours(file, g_file_vtbl)) {
            files++;
            const uint8_t *d; size_t n;
            int type;
            uint32_t faces = file_bytes(file, &d, &n) ? sfnt_faces(d, n, &type) : 0;
            for (uint32_t i = 0; i < faces; i++) {
                dw_face *f;
                if (face_from_file(file, i, 0, &f) >= 0) { lock(); collection_add_face(c, f); unlock(); }
            }
        }
        com_release(file);
    }
    com_release(en);
    TR("IDWriteFactory::CreateCustomFontCollection: %d files, %u families", files, c->count);
    *out = c;
    return S_OK;
}
static WINAPI HRESULT factory_RegisterFontCollectionLoader(void *this, void *l) { dw_factory *f = this; return loader_register(f->coll_loaders, &f->ncoll, l); }
static WINAPI HRESULT factory_UnregisterFontCollectionLoader(void *this, void *l) { dw_factory *f = this; return loader_unregister(f->coll_loaders, &f->ncoll, l); }
static WINAPI HRESULT factory_RegisterFontFileLoader(void *this, void *l) {
    dw_factory *f = this;
    TR("IDWriteFactory::RegisterFontFileLoader");
    if (l == (void *)&g_local_loader || l == (void *)&g_memory_loader) return DWRITE_E_ALREADYREGISTERED;
    return loader_register(f->file_loaders, &f->nfile, l);
}
static WINAPI HRESULT factory_UnregisterFontFileLoader(void *this, void *l) { dw_factory *f = this; return loader_unregister(f->file_loaders, &f->nfile, l); }
static WINAPI HRESULT factory_CreateFontFileReference(void *this, const WCHAR *path, const void *writetime, void **out) {
    (void)this; (void)writetime;
    if (!path || !out) return E_INVALIDARG;
    *out = NULL;
    char *win = vx_utf8_dup(path), unix_path[1024];
    int ok = vx_unix_path(win, unix_path, sizeof unix_path);
    TR("IDWriteFactory::CreateFontFileReference(%s)", win);
    free(win);
    dw_file *f = ok ? local_file(unix_path) : NULL;
    if (!f) return DWRITE_E_FILENOTFOUND;
    *out = f;
    return S_OK;
}
static WINAPI HRESULT factory_CreateCustomFontFileReference(void *this, const void *key, uint32_t key_size, void *loader, void **out) {
    dw_factory *fac = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!loader || !key) return E_INVALIDARG;
    if (loader == (void *)&g_local_loader) {             // the local loader's own key: a local file
        char path[1024];
        dw_file *f = key_path(key, key_size, path, sizeof path) ? local_file(path) : NULL;
        if (!f) return DWRITE_E_FILENOTFOUND;
        *out = f;
        return S_OK;
    }
    if (loader == (void *)&g_memory_loader) {
        dw_file *mf; void *s = NULL;
        if (key_size != sizeof mf || memory_CreateStreamFromKey(loader, key, key_size, &s) < 0) return E_INVALIDARG;
        dw_release(s);
        memcpy(&mf, key, sizeof mf);
        dw_addref(mf);
        *out = mf;
        return S_OK;
    }
    lock();
    int known = loader_index(fac->file_loaders, fac->nfile, loader) >= 0;
    unlock();
    if (!known) return E_INVALIDARG;
    dw_file *f = dw_new(sizeof *f, g_file_vtbl, g_file_iids, "IDWriteFontFile", file_destroy);
    f->kind = F_CUSTOM;
    f->loader = loader; com_addref(loader);
    f->key = malloc(key_size ? key_size : 1); memcpy(f->key, key, key_size); f->key_size = key_size;
    TR("IDWriteFactory::CreateCustomFontFileReference(%u-byte key)", key_size);
    *out = f;
    return S_OK;
}
// Faces of local files are made once per file and face; others each time.
static struct { dw_face *face; int sims; } g_local_faces[512];
static int g_nlocal_faces;
static WINAPI HRESULT factory_CreateFontFace(void *this, uint32_t type, uint32_t nfiles, void *const *files, uint32_t index, uint32_t sims, void **out) {
    (void)this; (void)type;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!nfiles || !files || !is_ours(files[0], g_file_vtbl) || sims > 3) return E_INVALIDARG;
    dw_file *file = files[0];
    if (file->kind == F_LOCAL) {
        lock();
        for (int i = 0; i < g_nlocal_faces; i++) {
            dw_face *f = g_local_faces[i].face;
            if (f->index == index && g_local_faces[i].sims == (int)sims && !strcmp(f->file->unix_path, file->unix_path)) {
                dw_addref(f); *out = f; unlock();
                return S_OK;
            }
        }
        unlock();
    }
    dw_face *f;
    HRESULT hr = face_from_file(file, index, (int)sims, &f);
    TR("IDWriteFactory::CreateFontFace(face %u of a %s file) -> %s (%08x)", index, file->kind == F_LOCAL ? "local" : file->kind == F_CUSTOM ? "program's" : "memory", hr >= 0 ? f->ps : "-", (unsigned)hr);
    if (hr < 0) return hr;
    if (file->kind == F_LOCAL) {
        lock();
        if (g_nlocal_faces < 512) { dw_addref(f); g_local_faces[g_nlocal_faces].face = f; g_local_faces[g_nlocal_faces++].sims = (int)sims; }
        unlock();
    }
    *out = f;
    return S_OK;
}
// Rendering parameters for this Mac: grayscale (ClearType level 0: a Mac's display has no subpixel order to use),
// DirectWrite's usual gamma and contrast.
static WINAPI HRESULT factory_CreateRenderingParams(void *this, void **out) {
    (void)this;
    if (!out) return E_INVALIDARG;
    *out = params_new(1.8f, 0.5f, 1.0f, 0.0f, 0 /* FLAT */, MODE_DEFAULT, GRID_FIT_DEFAULT);
    return S_OK;
}
static WINAPI HRESULT factory_CreateMonitorRenderingParams(void *this, void *monitor, void **out) { (void)monitor; return factory_CreateRenderingParams(this, out); }
static WINAPI HRESULT factory_CreateCustomRenderingParams(void *this, float gamma, float contrast, float cleartype, uint32_t geometry, uint32_t mode, void **out) {
    (void)this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (gamma <= 0 || contrast < 0 || cleartype < 0 || cleartype > 1 || geometry > 2 || mode > MODE_OUTLINE) return E_INVALIDARG;
    *out = params_new(gamma, contrast, 1.0f, cleartype, geometry, mode, GRID_FIT_DEFAULT);
    return S_OK;
}
static WINAPI HRESULT factory_CreateCustomRenderingParams1(void *this, float gamma, float contrast, float gray, float cleartype, uint32_t geometry, uint32_t mode, void **out) {
    HRESULT hr = factory_CreateCustomRenderingParams(this, gamma, contrast, cleartype, geometry, mode, out);
    if (hr >= 0) { if (gray < 0) { dw_release(*out); *out = NULL; return E_INVALIDARG; } ((dw_params *)*out)->gray_contrast = gray; }
    return hr;
}
static WINAPI HRESULT factory_CreateCustomRenderingParams2(void *this, float gamma, float contrast, float gray, float cleartype, uint32_t geometry, uint32_t mode, uint32_t grid, void **out) {
    HRESULT hr = factory_CreateCustomRenderingParams1(this, gamma, contrast, gray, cleartype, geometry, mode, out);
    if (hr >= 0) ((dw_params *)*out)->grid = grid;
    return hr;
}
static WINAPI HRESULT factory_GetGdiInterop(void *this, void **out) { (void)this; if (!out) return E_INVALIDARG; dw_addref(&g_gdi); *out = &g_gdi; return S_OK; }
static WINAPI HRESULT factory_CreateGlyphRunAnalysis(void *this, const glyph_run *run, float ppd, const matrix *m, uint32_t mode, uint32_t measuring,
                                                     float x, float y, void **out) {
    (void)this; (void)measuring;
    return analysis_new(run, ppd, m, mode, AA_CLEARTYPE, x, y, out);
}
static WINAPI HRESULT factory_CreateGlyphRunAnalysis2(void *this, const glyph_run *run, const matrix *m, uint32_t mode, uint32_t measuring, uint32_t grid,
                                                      uint32_t aa, float x, float y, void **out) {
    (void)this; (void)measuring; (void)grid;
    return analysis_new(run, 1.0f, m, mode, aa, x, y, out);
}
static WINAPI HRESULT factory_GetEudcFontCollection(void *this, void **out, int32_t check) {    // none on a Mac: an empty collection
    (void)this; (void)check;
    if (!out) return E_INVALIDARG;
    *out = collection_new(0, 1);
    return S_OK;
}
static WINAPI HRESULT factory_GetSystemFontFallback(void *this, void **out) {
    (void)this;
    if (!out) return E_INVALIDARG;
    TR("IDWriteFactory2::GetSystemFontFallback");
    dw_addref(&g_fallback);
    *out = &g_fallback;
    return S_OK;
}
static WINAPI HRESULT factory_TranslateColorGlyphRun(void *this, float x, float y, const glyph_run *run, const void *desc, uint32_t measuring,
                                                     const matrix *m, uint32_t palette, void **layers) {
    (void)this; (void)x; (void)y; (void)run; (void)desc; (void)measuring; (void)m; (void)palette;
    if (layers) *layers = NULL;
    return DWRITE_E_NOCOLOR;
}
// What is not offered says so and hands back no object.
#define NOT_OFFERED(name, out_type, ...) \
    static WINAPI HRESULT factory_##name(void *this, __VA_ARGS__, out_type out) { (void)this; if (out) *out = NULL; return dw_not_implemented("IDWriteFactory::" #name); }
static WINAPI HRESULT factory_CreateTextFormat(void *this, const WCHAR *family, void *coll, uint32_t weight, uint32_t style, uint32_t stretch, float size,
                                               const WCHAR *locale, void **out) {
    (void)this;
    return format_new(family, coll, weight, style, stretch, size, locale, out);
}
static WINAPI HRESULT factory_CreateTextLayout(void *this, const WCHAR *s, uint32_t n, void *format, float w, float h, void **out) {
    (void)this;
    return layout_new(s, n, format, w, h, 0, 1, NULL, 0, out);
}
static WINAPI HRESULT factory_CreateGdiCompatibleTextLayout(void *this, const WCHAR *s, uint32_t n, void *format, float w, float h, float ppd,
                                                            const matrix *m, int32_t natural, void **out) {
    (void)this;
    return layout_new(s, n, format, w, h, 1, ppd, m, natural, out);
}
NOT_OFFERED(CreateEllipsisTrimmingSign, void **, void *format)
NOT_OFFERED(CreateNumberSubstitution, void **, uint32_t method, const WCHAR *locale, int32_t ignore)
static WINAPI HRESULT factory_CreateTypography(void *this, void **out) {
    (void)this;
    if (!out) return E_INVALIDARG;
    *out = dw_new(sizeof(dw_typography), g_typography_vtbl, g_typography_iids, "IDWriteTypography", typography_destroy);
    return S_OK;
}
static WINAPI HRESULT factory_CreateTextAnalyzer(void *this, void **out) { (void)this; if (out) *out = NULL; return dw_not_implemented("IDWriteFactory::CreateTextAnalyzer"); }
static WINAPI HRESULT factory_CreateFontFallbackBuilder(void *this, void **out) { (void)this; if (out) *out = NULL; return dw_not_implemented("IDWriteFactory2::CreateFontFallbackBuilder"); }

// DWriteCreateFactory: the shared factory (one per process) or an isolated one; IDWriteFactory up to IDWriteFactory2.
static WINAPI HRESULT DWriteCreateFactory(uint32_t type, const GUID *iid, void **out) {
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (type > 1 || !iid) return E_INVALIDARG;
    int ok = d3d_same_guid(iid, &IID_IUnknown_);
    for (const GUID *const *g = g_factory_iids; *g && !ok; g++) ok = d3d_same_guid(iid, *g);
    TR("DWriteCreateFactory(%s) -> %s", type ? "isolated" : "shared", ok ? "IDWriteFactory2" : "interface not offered");
    if (!ok) { trace_iid("DWriteCreateFactory", iid); return E_NOINTERFACE; }
    if (type == 0) { dw_addref(&g_shared); *out = &g_shared; return S_OK; }
    *out = dw_new(sizeof(dw_factory), g_factory_vtbl, g_factory_iids, "IDWriteFactory", factory_destroy);
    return S_OK;
}

// ---- tables -------------------------------------------------------------------------------------------------------------------
#define M(name, fn) { name, (void *)fn }
#define IUNKNOWN M("QueryInterface", dw_query), M("AddRef", dw_addref), M("Release", dw_release)
static const d3d_method m_factory[] = {
    IUNKNOWN, M("GetSystemFontCollection", factory_GetSystemFontCollection), M("CreateCustomFontCollection", factory_CreateCustomFontCollection),
    M("RegisterFontCollectionLoader", factory_RegisterFontCollectionLoader), M("UnregisterFontCollectionLoader", factory_UnregisterFontCollectionLoader),
    M("CreateFontFileReference", factory_CreateFontFileReference), M("CreateCustomFontFileReference", factory_CreateCustomFontFileReference),
    M("CreateFontFace", factory_CreateFontFace), M("CreateRenderingParams", factory_CreateRenderingParams),
    M("CreateMonitorRenderingParams", factory_CreateMonitorRenderingParams), M("CreateCustomRenderingParams", factory_CreateCustomRenderingParams),
    M("RegisterFontFileLoader", factory_RegisterFontFileLoader), M("UnregisterFontFileLoader", factory_UnregisterFontFileLoader),
    M("CreateTextFormat", factory_CreateTextFormat), M("CreateTypography", factory_CreateTypography), M("GetGdiInterop", factory_GetGdiInterop),
    M("CreateTextLayout", factory_CreateTextLayout), M("CreateGdiCompatibleTextLayout", factory_CreateGdiCompatibleTextLayout),
    M("CreateEllipsisTrimmingSign", factory_CreateEllipsisTrimmingSign), M("CreateTextAnalyzer", factory_CreateTextAnalyzer),
    M("CreateNumberSubstitution", factory_CreateNumberSubstitution), M("CreateGlyphRunAnalysis", factory_CreateGlyphRunAnalysis),
    M("GetEudcFontCollection", factory_GetEudcFontCollection), M("IDWriteFactory1_CreateCustomRenderingParams", factory_CreateCustomRenderingParams1),
    M("GetSystemFontFallback", factory_GetSystemFontFallback), M("CreateFontFallbackBuilder", factory_CreateFontFallbackBuilder),
    M("TranslateColorGlyphRun", factory_TranslateColorGlyphRun), M("IDWriteFactory2_CreateCustomRenderingParams", factory_CreateCustomRenderingParams2),
    M("IDWriteFactory2_CreateGlyphRunAnalysis", factory_CreateGlyphRunAnalysis2), { 0 } };
static const d3d_method m_collection[] = {
    IUNKNOWN, M("GetFontFamilyCount", coll_GetFontFamilyCount), M("GetFontFamily", coll_GetFontFamily), M("FindFamilyName", coll_FindFamilyName),
    M("GetFontFromFontFace", coll_GetFontFromFontFace), { 0 } };
static const d3d_method m_family[] = {
    IUNKNOWN, M("GetFontCollection", fam_GetFontCollection), M("GetFontCount", fam_GetFontCount), M("GetFont", fam_GetFont),
    M("GetFamilyNames", fam_GetFamilyNames), M("GetFirstMatchingFont", fam_GetFirstMatchingFont), M("GetMatchingFonts", fam_GetMatchingFonts), { 0 } };
static const d3d_method m_list[] = { IUNKNOWN, M("GetFontCollection", list_GetFontCollection), M("GetFontCount", list_GetFontCount), M("GetFont", list_GetFont), { 0 } };
static const d3d_method m_font[] = {
    IUNKNOWN, M("GetFontFamily", font_GetFontFamily), M("GetWeight", font_GetWeight), M("GetStretch", font_GetStretch), M("GetStyle", font_GetStyle),
    M("IsSymbolFont", font_IsSymbolFont), M("GetFaceNames", font_GetFaceNames), M("GetInformationalStrings", font_GetInformationalStrings),
    M("GetSimulations", font_GetSimulations), M("GetMetrics", font_GetMetrics), M("HasCharacter", font_HasCharacter), M("CreateFontFace", font_CreateFontFace),
    M("IDWriteFont1_GetMetrics", font_GetMetrics1), M("GetPanose", font_GetPanose), M("GetUnicodeRanges", font_GetUnicodeRanges),
    M("IsMonospacedFont", font_IsMonospacedFont), M("IsColorFont", font_IsColorFont), { 0 } };
static const d3d_method m_face[] = {
    IUNKNOWN, M("GetType", face_GetType), M("GetFiles", face_GetFiles), M("GetIndex", face_GetIndex), M("GetSimulations", face_GetSimulations),
    M("IsSymbolFont", face_IsSymbolFont), M("GetMetrics", face_GetMetrics), M("GetGlyphCount", face_GetGlyphCount),
    M("GetDesignGlyphMetrics", face_GetDesignGlyphMetrics), M("GetGlyphIndices", face_GetGlyphIndices), M("TryGetFontTable", face_TryGetFontTable),
    M("ReleaseFontTable", face_ReleaseFontTable), M("GetGlyphRunOutline", face_GetGlyphRunOutline),
    M("GetRecommendedRenderingMode", face_GetRecommendedRenderingMode), M("GetGdiCompatibleMetrics", face_GetGdiCompatibleMetrics),
    M("GetGdiCompatibleGlyphMetrics", face_GetGdiCompatibleGlyphMetrics), M("IDWriteFontFace1_GetMetrics", face_GetMetrics1),
    M("IDWriteFontFace1_GetGdiCompatibleMetrics", face_GetGdiCompatibleMetrics1), M("GetCaretMetrics", face_GetCaretMetrics),
    M("GetUnicodeRanges", face_GetUnicodeRanges), M("IsMonospacedFont", face_IsMonospacedFont), M("GetDesignGlyphAdvances", face_GetDesignGlyphAdvances),
    M("GetGdiCompatibleGlyphAdvances", face_GetGdiCompatibleGlyphAdvances), M("GetKerningPairAdjustments", face_GetKerningPairAdjustments),
    M("HasKerningPairs", face_HasKerningPairs), M("IDWriteFontFace1_GetRecommendedRenderingMode", face_GetRecommendedRenderingMode1),
    M("GetVerticalGlyphVariants", face_GetVerticalGlyphVariants), M("HasVerticalGlyphVariants", face_HasVerticalGlyphVariants),
    M("IsColorFont", face_IsColorFont), M("GetColorPaletteCount", face_GetColorPaletteCount), M("GetPaletteEntryCount", face_GetPaletteEntryCount),
    M("GetPaletteEntries", face_GetPaletteEntries), M("IDWriteFontFace2_GetRecommendedRenderingMode", face_GetRecommendedRenderingMode2), { 0 } };
static const d3d_method m_strings[] = {
    IUNKNOWN, M("GetCount", strings_GetCount), M("FindLocaleName", strings_FindLocaleName), M("GetLocaleNameLength", strings_GetLocaleNameLength),
    M("GetLocaleName", strings_GetLocaleName), M("GetStringLength", strings_GetStringLength), M("GetString", strings_GetString), { 0 } };
static const d3d_method m_file[] = { IUNKNOWN, M("GetReferenceKey", file_GetReferenceKey), M("GetLoader", file_GetLoader), M("Analyze", file_Analyze), { 0 } };
static const d3d_method m_local_loader[] = {
    IUNKNOWN, M("CreateStreamFromKey", local_CreateStreamFromKey), M("GetFilePathLengthFromKey", local_GetFilePathLengthFromKey),
    M("GetFilePathFromKey", local_GetFilePathFromKey), M("GetLastWriteTimeFromKey", local_GetLastWriteTimeFromKey), { 0 } };
static const d3d_method m_stream[] = {
    IUNKNOWN, M("ReadFileFragment", stream_ReadFileFragment), M("ReleaseFileFragment", stream_ReleaseFileFragment), M("GetFileSize", stream_GetFileSize),
    M("GetLastWriteTime", stream_GetLastWriteTime), { 0 } };
static const d3d_method m_params[] = {
    IUNKNOWN, M("GetGamma", params_GetGamma), M("GetEnhancedContrast", params_GetEnhancedContrast), M("GetClearTypeLevel", params_GetClearTypeLevel),
    M("GetPixelGeometry", params_GetPixelGeometry), M("GetRenderingMode", params_GetRenderingMode),
    M("GetGrayscaleEnhancedContrast", params_GetGrayscaleEnhancedContrast), M("GetGridFitMode", params_GetGridFitMode), { 0 } };
static const d3d_method m_analysis[] = {
    IUNKNOWN, M("GetAlphaTextureBounds", analysis_GetAlphaTextureBounds), M("CreateAlphaTexture", analysis_CreateAlphaTexture),
    M("GetAlphaBlendParams", analysis_GetAlphaBlendParams), { 0 } };
static const d3d_method m_fallback[] = { IUNKNOWN, M("MapCharacters", fallback_MapCharacters), { 0 } };
static const d3d_method m_gdi[] = {
    IUNKNOWN, M("CreateFontFromLOGFONT", gdi_CreateFontFromLOGFONT), M("ConvertFontToLOGFONT", gdi_ConvertFontToLOGFONT),
    M("ConvertFontFaceToLOGFONT", gdi_ConvertFontFaceToLOGFONT), M("CreateFontFaceFromHdc", gdi_CreateFontFaceFromHdc),
    M("CreateBitmapRenderTarget", gdi_CreateBitmapRenderTarget), { 0 } };
#define FORMAT_METHODS \
    M("SetTextAlignment", fmt_SetTextAlignment), M("SetParagraphAlignment", fmt_SetParagraphAlignment), M("SetWordWrapping", fmt_SetWordWrapping), \
    M("SetReadingDirection", fmt_SetReadingDirection), M("SetFlowDirection", fmt_SetFlowDirection), M("SetIncrementalTabStop", fmt_SetIncrementalTabStop), \
    M("SetTrimming", fmt_SetTrimming), M("SetLineSpacing", fmt_SetLineSpacing), M("GetTextAlignment", fmt_GetTextAlignment), \
    M("GetParagraphAlignment", fmt_GetParagraphAlignment), M("GetWordWrapping", fmt_GetWordWrapping), M("GetReadingDirection", fmt_GetReadingDirection), \
    M("GetFlowDirection", fmt_GetFlowDirection), M("GetIncrementalTabStop", fmt_GetIncrementalTabStop), M("GetTrimming", fmt_GetTrimming), \
    M("GetLineSpacing", fmt_GetLineSpacing), M("GetFontCollection", fmt_GetFontCollection), M("GetFontFamilyNameLength", fmt_GetFontFamilyNameLength), \
    M("GetFontFamilyName", fmt_GetFontFamilyName), M("GetFontWeight", fmt_GetFontWeight), M("GetFontStyle", fmt_GetFontStyle), \
    M("GetFontStretch", fmt_GetFontStretch), M("GetFontSize", fmt_GetFontSize), M("GetLocaleNameLength", fmt_GetLocaleNameLength), \
    M("GetLocaleName", fmt_GetLocaleName)
static const d3d_method m_format[] = { IUNKNOWN, FORMAT_METHODS, { 0 } };
static const d3d_method m_layout[] = {
    IUNKNOWN, FORMAT_METHODS, M("SetMaxWidth", layout_SetMaxWidth), M("SetMaxHeight", layout_SetMaxHeight), M("SetFontCollection", layout_SetFontCollection),
    M("SetFontFamilyName", layout_SetFontFamilyName), M("SetFontWeight", layout_SetFontWeight), M("SetFontStyle", layout_SetFontStyle),
    M("SetFontStretch", layout_SetFontStretch), M("SetFontSize", layout_SetFontSize), M("SetUnderline", layout_SetUnderline),
    M("SetStrikethrough", layout_SetStrikethrough), M("SetDrawingEffect", layout_SetDrawingEffect), M("SetInlineObject", layout_SetInlineObject),
    M("SetTypography", layout_SetTypography), M("SetLocaleName", layout_SetLocaleName), M("GetMaxWidth", layout_GetMaxWidth), M("GetMaxHeight", layout_GetMaxHeight),
    M("IDWriteTextLayout_GetFontCollection", layout_GetFontCollectionAt), M("IDWriteTextLayout_GetFontFamilyNameLength", layout_GetFontFamilyNameLengthAt),
    M("IDWriteTextLayout_GetFontFamilyName", layout_GetFontFamilyNameAt), M("IDWriteTextLayout_GetFontWeight", layout_GetFontWeightAt),
    M("IDWriteTextLayout_GetFontStyle", layout_GetFontStyleAt), M("IDWriteTextLayout_GetFontStretch", layout_GetFontStretchAt),
    M("IDWriteTextLayout_GetFontSize", layout_GetFontSizeAt), M("GetUnderline", layout_GetUnderline), M("GetStrikethrough", layout_GetStrikethrough),
    M("GetDrawingEffect", layout_GetDrawingEffect), M("GetInlineObject", layout_GetInlineObject), M("GetTypography", layout_GetTypography),
    M("IDWriteTextLayout_GetLocaleNameLength", layout_GetLocaleNameLengthAt), M("IDWriteTextLayout_GetLocaleName", layout_GetLocaleNameAt),
    M("Draw", layout_Draw), M("GetLineMetrics", layout_GetLineMetrics), M("GetMetrics", layout_GetMetrics), M("GetOverhangMetrics", layout_GetOverhangMetrics),
    M("GetClusterMetrics", layout_GetClusterMetrics), M("DetermineMinWidth", layout_DetermineMinWidth), M("HitTestPoint", layout_HitTestPoint),
    M("HitTestTextPosition", layout_HitTestTextPosition), M("HitTestTextRange", layout_HitTestTextRange), { 0 } };
static const d3d_method m_typography[] = {
    IUNKNOWN, M("AddFontFeature", typo_AddFontFeature), M("GetFontFeatureCount", typo_GetFontFeatureCount), M("GetFontFeature", typo_GetFontFeature), { 0 } };
static const d3d_method m_target[] = {
    IUNKNOWN, M("DrawGlyphRun", target_DrawGlyphRun), M("GetMemoryDC", target_GetMemoryDC), M("GetPixelsPerDip", target_GetPixelsPerDip),
    M("SetPixelsPerDip", target_SetPixelsPerDip), M("GetCurrentTransform", target_GetCurrentTransform), M("SetCurrentTransform", target_SetCurrentTransform),
    M("GetSize", target_GetSize), M("Resize", target_Resize), { 0 } };
static const GUID *const g_fallback_iids[] = { &IID_IDWriteFontFallback, NULL };
static const GUID *const g_gdi_iids[] = { &IID_IDWriteGdiInterop, NULL };
static const GUID *const g_memory_loader_iids[] = { &IID_IDWriteFontFileLoader, NULL };

static const vx_export dwrite_exports[] = { VX_EXPORT(DWriteCreateFactory) };

void vx_dwrite_init(void) {
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_lock, &ma);
    const char *t = getenv("VXRT_TRACE_DWRITE");
    g_trace = t && *t == '1';
    g_factory_vtbl = D3D_VTABLE(IDWriteFactory2, m_factory);
    g_collection_vtbl = D3D_VTABLE(IDWriteFontCollection, m_collection);
    g_family_vtbl = D3D_VTABLE(IDWriteFontFamily, m_family);
    g_list_vtbl = D3D_VTABLE(IDWriteFontList, m_list);
    g_font_vtbl = D3D_VTABLE(IDWriteFont2, m_font);
    g_face_vtbl = D3D_VTABLE(IDWriteFontFace2, m_face);
    g_strings_vtbl = D3D_VTABLE(IDWriteLocalizedStrings, m_strings);
    g_file_vtbl = D3D_VTABLE(IDWriteFontFile, m_file);
    g_local_loader_vtbl = D3D_VTABLE(IDWriteLocalFontFileLoader, m_local_loader);
    g_stream_vtbl = D3D_VTABLE(IDWriteFontFileStream, m_stream);
    g_params_vtbl = D3D_VTABLE(IDWriteRenderingParams2, m_params);
    g_analysis_vtbl = D3D_VTABLE(IDWriteGlyphRunAnalysis, m_analysis);
    g_fallback_vtbl = D3D_VTABLE(IDWriteFontFallback, m_fallback);
    g_gdi_vtbl = D3D_VTABLE(IDWriteGdiInterop, m_gdi);
    g_format_vtbl = D3D_VTABLE(IDWriteTextFormat, m_format);
    g_layout_vtbl = D3D_VTABLE(IDWriteTextLayout, m_layout);
    g_typography_vtbl = D3D_VTABLE(IDWriteTypography, m_typography);
    g_target_vtbl = D3D_VTABLE(IDWriteBitmapRenderTarget, m_target);
    g_memory_loader_vtbl[0] = (void *)dw_query; g_memory_loader_vtbl[1] = (void *)dw_addref; g_memory_loader_vtbl[2] = (void *)dw_release;
    g_memory_loader_vtbl[3] = (void *)memory_CreateStreamFromKey;
    g_shared = (dw_factory){ { g_factory_vtbl, 1, NULL, NULL, g_factory_iids, "IDWriteFactory" }, { 0 }, 0, { 0 }, 0 };
    g_local_loader = (dw_obj){ g_local_loader_vtbl, 1, NULL, NULL, g_local_loader_iids, "IDWriteLocalFontFileLoader" };
    g_memory_loader = (dw_obj){ g_memory_loader_vtbl, 1, NULL, NULL, g_memory_loader_iids, "IDWriteFontFileLoader" };
    g_fallback = (dw_obj){ g_fallback_vtbl, 1, NULL, NULL, g_fallback_iids, "IDWriteFontFallback" };
    g_gdi = (dw_obj){ g_gdi_vtbl, 1, NULL, NULL, g_gdi_iids, "IDWriteGdiInterop" };
    vx_register_module("dwrite.dll", dwrite_exports, sizeof dwrite_exports / sizeof dwrite_exports[0]);
}
#ifdef __aarch64__
#include "thunks/dwrite.m.inc"
#endif
