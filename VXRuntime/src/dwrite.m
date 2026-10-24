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
// The newest versions of the object model are offered (IDWriteFactory7, IDWriteFontFace5, IDWriteFont3,
// IDWriteFontCollection3, font sets and their builders, font face references and font resources, the in-memory font
// file loader): every font is local, no font has variation axes (a variable font is its default instance), kerning
// pairs come from the legacy 'kern' table and vertical glyph variants from GSUB's 'vert' feature.
//
// Not offered: text analyzers (IDWriteTextAnalyzer; Chromium shapes text with HarfBuzz), colour glyphs
// (TranslateColorGlyphRun answers DWRITE_E_NOCOLOR, and a face reports outline glyph images only, so emoji in Apple
// Color Emoji, which has bitmaps only, draw nothing yet), hinting (CoreText does not hint: grid fitting is always
// off), remote fonts (the download queue stays empty), WOFF2, and the text format and layout versions after the first
// (IDWriteFactory6::CreateTextFormat). VXRT_TRACE_DWRITE=1 logs the calls (the first 20 of each method) and the
// interfaces asked for and not offered.
#import <CoreText/CoreText.h>
#import <CoreGraphics/CoreGraphics.h>
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
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
enum { MODE_DEFAULT = 0, MODE_ALIASED = 1, MODE_GDI_CLASSIC = 2, MODE_GDI_NATURAL = 3, MODE_NATURAL = 4, MODE_NATURAL_SYMMETRIC = 5, MODE_OUTLINE = 6,
       MODE_NATURAL_SYMMETRIC_DOWNSAMPLED = 7 };                // (DWRITE_RENDERING_MODE1 only)
// (DWRITE_RENDERING_MODE has no NATURAL_SYMMETRIC_DOWNSAMPLED: without hinting it is NATURAL_SYMMETRIC)
static uint32_t mode0(uint32_t mode) { return mode == MODE_NATURAL_SYMMETRIC_DOWNSAMPLED ? MODE_NATURAL_SYMMETRIC : mode; }
enum { LOCALITY_LOCAL = 2 };
enum { FAMILY_MODEL_TYPOGRAPHIC = 0, FAMILY_MODEL_WSS = 1 };
enum { SOURCE_UNKNOWN = 0, SOURCE_PER_MACHINE = 1, SOURCE_PER_USER = 2 };
enum { IMAGE_TRUETYPE = 1, IMAGE_CFF = 2 };                   // DWRITE_GLYPH_IMAGE_FORMATS
enum { CONTAINER_UNKNOWN = 0, CONTAINER_WOFF = 1, CONTAINER_WOFF2 = 2 };
// DWRITE_FONT_PROPERTY_ID
enum { PROP_NONE = 0, PROP_WSS_FAMILY = 1, PROP_TYPO_FAMILY = 2, PROP_WSS_FACE = 3, PROP_FULL_NAME = 4, PROP_WIN32_FAMILY = 5,
       PROP_POSTSCRIPT = 6, PROP_DESIGN_SCRIPTS = 7, PROP_SUPPORTED_SCRIPTS = 8, PROP_SEMANTIC_TAG = 9, PROP_WEIGHT = 10,
       PROP_STRETCH = 11, PROP_STYLE = 12, PROP_TYPO_FACE = 13, PROP_COUNT = 14 };
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
typedef struct { uint32_t tag; float value; } axis_value;                     // DWRITE_FONT_AXIS_VALUE
typedef struct { uint32_t tag; float min, max; } axis_range;                  // DWRITE_FONT_AXIS_RANGE
typedef struct { uint32_t id; const WCHAR *value, *locale; } font_property;  // DWRITE_FONT_PROPERTY
typedef struct { DWORD usb[4], csb[2]; } font_signature;                      // FONTSIGNATURE
#define AXIS_TAG(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)   // DWRITE_MAKE_FONT_AXIS_TAG

static const GUID IID_IUnknown_ = { 0, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
// (recursive) collections, families, faces made on demand. The program's code (its loaders and streams, any Release
// that may be its last) never runs under it: that code may wait for threads of the program's that are in DirectWrite.
static pthread_mutex_t g_lock;
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
    N(IDWriteFontFileStream), N(IDWriteLocalizedStrings), N(IDWriteGlyphRunAnalysis), N(IDWriteFontCollection2),
    N(IDWriteFontCollection3), N(IDWriteFontFamily2), N(IDWriteFontList2), N(IDWriteFontSet), N(IDWriteFontSet1),
    N(IDWriteFontSet2), N(IDWriteFontSet3), N(IDWriteFontSet4), N(IDWriteFontFaceReference), N(IDWriteFontFaceReference1),
    N(IDWriteFontResource), N(IDWriteFontSetBuilder), N(IDWriteFontSetBuilder1), N(IDWriteFontSetBuilder2), N(IDWriteStringList),
    N(IDWriteFontDownloadQueue), N(IDWriteFontFallbackBuilder), N(IDWriteFontFace6), N(IDWriteTextAnalyzer), N(IDWriteTextFormat1),
    N(IDWriteTextFormat3), N(IDWriteTextLayout1), N(IDWriteTextLayout2), N(IDWriteBitmapRenderTarget1),
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
    if (!o) return NULL;
    o->vtbl = vtbl; o->refs = 1; o->iids = iids; o->what = what; o->destroy = destroy;
    return o;
}
// The program's COM objects (its loaders, streams, text sources, geometry sinks): methods by vtable slot.
#define SLOT(obj, i) ((*(void ***)(obj))[i])
static void com_addref(void *o) { ((uint32_t (WINAPI *)(void *))SLOT(o, 1))(o); }
static void com_release(void *o) { ((uint32_t (WINAPI *)(void *))SLOT(o, 2))(o); }

static void **g_factory_vtbl, **g_collection_vtbl, **g_family_vtbl, **g_font_vtbl, **g_list_vtbl, **g_face_vtbl, **g_strings_vtbl,
            **g_file_vtbl, **g_local_loader_vtbl, **g_stream_vtbl, **g_params_vtbl, **g_analysis_vtbl, **g_fallback_vtbl, **g_gdi_vtbl,
            **g_ref_vtbl, **g_resource_vtbl, **g_set_vtbl, **g_builder_vtbl, **g_strlist_vtbl, **g_inmem_vtbl, **g_queue_vtbl,
            **g_fbbuilder_vtbl;
static void *g_memory_loader_vtbl[4], *g_numsub_vtbl[3];

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
static uint64_t align4(uint64_t v) { return (v + 3) & ~3ull; }
// The offset table of an sfnt (OpenType spec): its version, the table count and the binary search fields.
static void sfnt_header(uint8_t *b, uint32_t version, uint16_t count) {
    int sel = 0;
    while ((2u << sel) <= count) sel++;
    uint32_t range = count ? 16u << sel : 0, shift = 16u * count - range;
    uint8_t head[12] = { version >> 24, version >> 16, version >> 8, version, count >> 8, count, range >> 8, range, 0, sel,
                         (uint8_t)(shift >> 8), (uint8_t)shift };
    memcpy(b, head, 12);
}
// Table record i for a table whose bytes are already at off (zero padded to 4): tag, checksum, offset, length.
static void sfnt_record(uint8_t *b, int i, uint32_t tag, uint32_t off, uint32_t len) {
    uint8_t *r = b + 12 + 16 * i;
    uint32_t sum = 0;
    for (uint64_t k = 0; k < align4(len); k += 4) sum += be32(b + off + k);
    uint32_t fields[4] = { tag, sum, off, len };
    for (int f = 0; f < 4; f++) { r[4 * f] = fields[f] >> 24; r[4 * f + 1] = fields[f] >> 16; r[4 * f + 2] = fields[f] >> 8; r[4 * f + 3] = fields[f]; }
}
// NULL when the memory is not there or the file could not say where its tables are (offsets are 32-bit).
static uint8_t *sfnt_build(uint32_t version, const sfnt_part *t, int count, size_t *size) {
    if (count < 0 || count > 0xFFFF) return NULL;
    uint64_t total = 12 + 16ull * count;
    for (int i = 0; i < count; i++) total += align4(t[i].len);
    if (total > UINT32_MAX) return NULL;
    uint8_t *b = calloc(1, (size_t)total);
    if (!b) return NULL;
    sfnt_header(b, version, (uint16_t)count);
    uint64_t at = 12 + 16ull * count;
    for (int i = 0; i < count; i++) {
        if (t[i].len) memcpy(b + at, t[i].data, t[i].len);
        sfnt_record(b, i, t[i].tag, (uint32_t)at, t[i].len);
        at += align4(t[i].len);
    }
    *size = (size_t)total;
    return b;
}
// One face of a collection as a font file of its own. A face's tables are separate parts of the file, so its records
// adding up to more bytes than the file has means records sharing data (a font that would grow without bound when
// rebuilt): not a font.
static HRESULT sfnt_extract(const uint8_t *d, size_t n, long dir, uint8_t **out, size_t *size) {
    *out = NULL; *size = 0;
    uint16_t count = be16(d + dir + 4);
    sfnt_part *t = calloc(count ? count : 1, sizeof *t);
    if (!t) return E_OUTOFMEMORY;
    int k = 0;
    uint64_t sum = 0;
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *r = d + dir + 12 + 16 * i;
        uint32_t off = be32(r + 8), l = be32(r + 12);
        if ((uint64_t)off + l <= n) { t[k++] = (sfnt_part){ be32(r), d + off, l }; sum += l; }
    }
    HRESULT hr = DWRITE_E_FILEFORMAT;
    if (sum <= n) {
        *out = sfnt_build(be32(d + dir), t, k, size);
        hr = *out ? S_OK : E_OUTOFMEMORY;
    }
    free(t);
    return hr;
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
// cannot be read: rebuilt from CoreText's tables), a file of the program's loader (read through it once), or bytes
// the program gave an in-memory loader (IDWriteInMemoryFontFileLoader, keyed by their number there).
enum { F_LOCAL, F_MEMORY, F_CUSTOM, F_INMEM };
typedef struct dw_file {
    dw_obj o;
    int kind;
    void *loader;                        // F_CUSTOM: the program's IDWriteFontFileLoader; F_INMEM: our in-memory loader (held)
    uint8_t *key; uint32_t key_size;
    mapping *map;                        // F_LOCAL (NULL until read)
    char *unix_path;                     // F_LOCAL
    CFDataRef bytes;                     // F_MEMORY, F_INMEM, F_CUSTOM once read
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
// A file of the program's loader, read through it whole (NULL if it cannot be). Called without the lock: the
// loader and its streams are the program's code, which may wait for threads of its own that are in DirectWrite.
static CFDataRef custom_read(dw_file *f) {
    // IDWriteFontFileLoader::CreateStreamFromKey (3); IDWriteFontFileStream::ReadFileFragment (3),
    // ReleaseFileFragment (4), GetFileSize (5)
    void *stream = NULL;
    CFDataRef bytes = NULL;
    HRESULT hr = ((HRESULT (WINAPI *)(void *, const void *, uint32_t, void **))SLOT(f->loader, 3))(f->loader, f->key, f->key_size, &stream);
    uint64_t n = 0;
    if (hr >= 0 && stream && ((HRESULT (WINAPI *)(void *, uint64_t *))SLOT(stream, 5))(stream, &n) >= 0 && n && n < (1ull << 31)) {
        const void *p = NULL; void *ctx = NULL;
        if (((HRESULT (WINAPI *)(void *, const void **, uint64_t, uint64_t, void **))SLOT(stream, 3))(stream, &p, 0, n, &ctx) >= 0 && p) {
            bytes = CFDataCreate(NULL, VX_LOW(p), (CFIndex)n);
            ((void (WINAPI *)(void *, void *))SLOT(stream, 4))(stream, ctx);
        }
    }
    if (stream) com_release(stream);
    TR("read %llu bytes of a font file through the program's loader (%s)", (unsigned long long)n, bytes ? "ok" : "failed");
    return bytes;
}
// The bytes of a file (0 if they cannot be had). A local file is mapped, a program's file read once: two threads
// that read it at the same time each read it, and the bytes of the first to finish are kept.
static int file_bytes(dw_file *f, const uint8_t **data, size_t *size) {
    if (f->kind == F_LOCAL) {
        mapping *m = READY(f->map);
        if (!m && (m = map_file(f->unix_path))) PUBLISH(f->map, m);
        if (!m) return 0;
        *data = m->base; *size = m->size;
        return 1;
    }
    CFDataRef b = READY(f->bytes);
    if (!b && f->kind == F_CUSTOM && (b = custom_read(f))) {
        CFDataRef none = NULL;
        if (!__atomic_compare_exchange_n(&f->bytes, &none, b, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { CFRelease(b); b = none; }
    }
    if (!b) return 0;
    *data = CFDataGetBytePtr(b); *size = (size_t)CFDataGetLength(b);
    return 1;
}
static CFDataRef file_cfdata(dw_file *f) {           // the bytes as CFData (retained), for CoreGraphics
    const uint8_t *d; size_t n;
    if (!file_bytes(f, &d, &n)) return NULL;
    CFDataRef b = READY(f->bytes);
    if (b) return CFRetain(b);
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
    if (f->kind == F_CUSTOM) com_addref(*loader); else dw_addref(*loader);     // (F_INMEM: ours)
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

// IDWriteFontFileStream over bytes that stay (a mapping, or bytes a file holds: the stream holds the file), or bytes
// of its own (an unpacked WOFF file: owned, freed with it).
typedef struct { dw_obj o; const uint8_t *data; size_t size; uint64_t mtime; dw_obj *holder; void *owned; } dw_stream;
static const GUID *const g_stream_iids[] = { &IID_IDWriteFontFileStream, NULL };
static void stream_destroy(dw_obj *o) { dw_stream *s = (dw_stream *)o; if (s->holder) dw_release(s->holder); free(s->owned); free(s); }
static dw_stream *stream_new(const uint8_t *data, size_t size, uint64_t mtime, dw_obj *holder) {
    dw_stream *s = dw_new(sizeof *s, g_stream_vtbl, g_stream_iids, "IDWriteFontFileStream", stream_destroy);
    if (!s) return NULL;
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
    return *out ? S_OK : E_OUTOFMEMORY;
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
    return *out ? S_OK : E_OUTOFMEMORY;
}

// IDWriteInMemoryFontFileLoader (IDWriteFactory5): bytes the program hands over, copied, or used where they are while
// the loader holds their owner (an IUnknown of the program's, released when the loader goes). The key of a file is its
// number in the loader.
typedef struct { CFDataRef bytes; void *owner; } inmem_entry;
typedef struct { dw_obj o; inmem_entry *e; uint32_t n, cap; } dw_inmem;
static const GUID *const g_inmem_iids[] = { &IID_IDWriteFontFileLoader, &IID_IDWriteInMemoryFontFileLoader, NULL };
static void inmem_destroy(dw_obj *o) {
    dw_inmem *l = (dw_inmem *)o;
    for (uint32_t i = 0; i < l->n; i++) { CFRelease(l->e[i].bytes); if (l->e[i].owner) com_release(l->e[i].owner); }
    free(l->e); free(l);
}
static dw_file *inmem_file(dw_inmem *l, uint32_t index) {      // NULL: no such file
    lock();
    CFDataRef b = index < l->n ? CFRetain(l->e[index].bytes) : NULL;
    unlock();
    if (!b) return NULL;
    dw_file *f = dw_new(sizeof *f, g_file_vtbl, g_file_iids, "IDWriteFontFile", file_destroy);
    f->kind = F_INMEM;
    f->loader = l; dw_addref(l);
    f->key_size = 4; f->key = malloc(4); memcpy(f->key, &index, 4);
    f->bytes = b;
    return f;
}
static WINAPI HRESULT inmem_CreateStreamFromKey(void *this, const void *key, uint32_t size, void **out) {
    dw_inmem *l = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    uint32_t index;
    if (!key || size != 4) return E_INVALIDARG;
    memcpy(&index, key, 4);
    lock();
    CFDataRef b = index < l->n ? l->e[index].bytes : NULL;      // (the loader keeps them: the stream holds it)
    unlock();
    if (!b) return E_INVALIDARG;
    *out = stream_new(CFDataGetBytePtr(b), (size_t)CFDataGetLength(b), 0, &l->o);
    return *out ? S_OK : E_OUTOFMEMORY;
}
static WINAPI HRESULT inmem_CreateInMemoryFontFileReference(void *this, void *factory, const void *data, uint32_t size, void *owner, void **out) {
    dw_inmem *l = this;
    (void)factory;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!data || !size) return E_INVALIDARG;
    CFDataRef b = owner ? CFDataCreateWithBytesNoCopy(NULL, data, (CFIndex)size, kCFAllocatorNull) : CFDataCreate(NULL, data, (CFIndex)size);
    if (!b) return E_OUTOFMEMORY;
    if (owner) com_addref(owner);
    lock();
    if (l->n == l->cap) { l->cap = l->cap ? 2 * l->cap : 8; l->e = realloc(l->e, l->cap * sizeof *l->e); }
    uint32_t index = l->n++;
    l->e[index] = (inmem_entry){ b, owner };
    unlock();
    TR("IDWriteInMemoryFontFileLoader::CreateInMemoryFontFileReference(%u bytes, %s) -> file %u", size, owner ? "kept where they are" : "copied", index);
    *out = inmem_file(l, index);
    return S_OK;
}
static WINAPI uint32_t inmem_GetFileCount(void *this) { dw_inmem *l = this; lock(); uint32_t n = l->n; unlock(); return n; }

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
    dw_obj o;                            // IDWriteFontCollection3
    int system, model;                   // model: the DWRITE_FONT_FAMILY_MODEL it was asked for (the grouping is the same)
    dw_family **families; uint32_t count, visible, cap;   // past `visible`: families only fallback found (system)
    struct { uint32_t family; WCHAR *name; } *aliases; uint32_t naliases; int aliases_built;   // localized names
};
static const GUID *const g_collection_iids[] = { &IID_IDWriteFontCollection, &IID_IDWriteFontCollection1, &IID_IDWriteFontCollection2,
                                                 &IID_IDWriteFontCollection3, NULL };
static const GUID *const g_family_iids[] = { &IID_IDWriteFontList, &IID_IDWriteFontFamily, &IID_IDWriteFontFamily1, &IID_IDWriteFontFamily2, NULL };
static const GUID *const g_font_iids[] = { &IID_IDWriteFont, &IID_IDWriteFont1, &IID_IDWriteFont2, &IID_IDWriteFont3, NULL };
static const GUID *const g_face_iids[] = { &IID_IDWriteFontFace, &IID_IDWriteFontFace1, &IID_IDWriteFontFace2, &IID_IDWriteFontFace3,
                                           &IID_IDWriteFontFace4, &IID_IDWriteFontFace5, NULL };
static const GUID *const g_list_iids[] = { &IID_IDWriteFontList, &IID_IDWriteFontList1, &IID_IDWriteFontList2, NULL };
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
        size_t sz; uint8_t *one;
        HRESULT hr = sfnt_extract(d, n, dir, &one, &sz);
        if (hr < 0) return hr;
        data = CFDataCreateWithBytesNoCopy(NULL, one, (CFIndex)sz, kCFAllocatorMalloc);
        if (!data) { free(one); return E_OUTOFMEMORY; }
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
        if (!parts || !keep) n = 0;
        for (CFIndex i = 0; i < n; i++) {
            uint32_t tag = (uint32_t)(uintptr_t)CFArrayGetValueAtIndex(tags, i);
            CFDataRef d = CTFontCopyTable(f->ct, tag, kCTFontTableOptionNoOptions);
            if (d) { keep[k] = d; parts[k] = (sfnt_part){ tag, CFDataGetBytePtr(d), (uint32_t)CFDataGetLength(d) }; k++; }
        }
        size_t size = 0;
        uint8_t *bytes = parts && keep ? sfnt_build(f->type == FACE_CFF ? TAG('O', 'T', 'T', 'O') : 0x00010000, parts, k, &size) : NULL;
        for (int i = 0; i < k; i++) CFRelease(keep[i]);
        free(keep); free(parts);
        if (tags) CFRelease(tags);
        // (no memory for the tables: the face has no file for now)
        CFDataRef data = bytes ? CFDataCreateWithBytesNoCopy(NULL, bytes, (CFIndex)size, kCFAllocatorMalloc) : NULL;
        if (bytes && !data) free(bytes);
        dw_file *file = data ? dw_new(sizeof *file, g_file_vtbl, g_file_iids, "IDWriteFontFile", NULL) : NULL;
        void *key = file ? malloc(sizeof(void *)) : NULL;
        if (key) {
            file->kind = F_MEMORY;
            file->bytes = data;
            file->key_size = sizeof(void *);
            file->key = key;
            memcpy(file->key, &file, sizeof(void *));
            g_memory_files[g_memory_count++] = file;
            f->file = file;
            f->index = 0;
        } else {
            free(file);
            if (data) CFRelease(data);
        }
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
static void face_wss(dw_face *f, int sims, int *weight_out, int *stretch_out, int *style_out);
static void font_props(dw_font *ft) {
    if (READY(ft->props)) return;
    dw_font *real = ft->base ? ft->base : ft;
    face_wss(font_face(real), ft->sims, &ft->weight, &ft->stretch, &ft->style);
    PUBLISH(ft->props, 1);
}
// The weight, stretch and style of a face (NULL: the defaults), with its simulations.
static void face_wss(dw_face *f, int sims, int *weight_out, int *stretch_out, int *style_out) {
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
    if (sims & SIM_BOLD && weight < 700) weight = 700;
    if (sims & SIM_OBLIQUE) style = STYLE_OBLIQUE;
    *weight_out = weight; *stretch_out = stretch; *style_out = style;
}
// The face of a font, made the first time it is asked for. It is made outside the lock (a simulated font of a
// program's file makes its face from that file); when two threads make it at once, the first one kept is the face.
static dw_face *font_face(dw_font *ft) {
    dw_face *made = READY(ft->face);
    if (made) return made;
    dw_face *f = NULL;
    if (ft->desc) f = face_from_descriptor(ft->desc, ft->sims);
    else if (ft->base) {
        dw_face *b = font_face(ft->base);
        dw_file *file = NULL;
        uint32_t index = 0;
        if (b) { lock(); if ((file = b->file)) { dw_addref(file); index = b->index; } unlock(); }
        if (file) { face_from_file(file, index, ft->sims, &f); dw_release(file); }
        else if (b && ft->base->desc) f = face_from_descriptor(ft->base->desc, ft->sims);
    }
    lock();
    made = ft->face;
    if (!made && f) { f->font = ft; PUBLISH(ft->face, f); made = f; f = NULL; }
    unlock();
    if (f) dw_release(f);                     // another thread's was kept
    return made;
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
// The faces are let go after the lock: the last reference to a program's font file calls its loader's Release.
static void collection_destroy(dw_obj *o) {
    dw_collection *c = (dw_collection *)o;
    dw_face **faces = NULL;
    uint32_t nfaces = 0, cap = 0;
    lock();
    for (uint32_t i = 0; i < c->count; i++) {
        dw_family *fam = c->families[i];
        for (uint32_t j = 0; j < fam->count + (uint32_t)fam->nsimulated; j++) {
            dw_font *ft = j < fam->count ? fam->fonts[j] : fam->simulated[j - fam->count];
            if (ft->face) {
                ft->face->font = NULL;
                if (nfaces == cap) { cap = cap ? cap * 2 : 16; faces = realloc(faces, cap * sizeof *faces); }
                faces[nfaces++] = ft->face;
            }
            if (ft->desc) CFRelease(ft->desc);
            free(ft);
        }
        CFRelease(fam->name);
        free(fam->fonts); free(fam);
    }
    for (uint32_t i = 0; i < c->naliases; i++) free(c->aliases[i].name);
    unlock();
    for (uint32_t i = 0; i < nfaces; i++) dw_release(faces[i]);
    free(faces); free(c->aliases); free(c->families); free(c);
}
static dw_collection *collection_new(int system, uint32_t cap) {
    dw_collection *c = dw_new(sizeof *c, g_collection_vtbl, g_collection_iids, "IDWriteFontCollection", system ? NULL : collection_destroy);
    c->system = system;
    c->model = FAMILY_MODEL_WSS;
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
static long wss_rank(int weight, int stretch, int style, int fw, int fs, int fst) {
    return (long)stretch_rank(stretch, fs) * 10000000L + (long)style_rank(style, fst) * 100000L + weight_rank(weight, fw);
}
static long match_rank(dw_font *ft, int weight, int stretch, int style) {
    font_props(ft);
    return wss_rank(weight, stretch, style, ft->weight, ft->stretch, ft->style);
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
static int wcs_ieq(const WCHAR *a, const WCHAR *b) {      // equal but for case (Unicode)
    CFStringRef x = CFStringCreateWithCharactersNoCopy(NULL, a, (CFIndex)vx_wcslen(a), kCFAllocatorNull);
    CFStringRef y = CFStringCreateWithCharactersNoCopy(NULL, b, (CFIndex)vx_wcslen(b), kCFAllocatorNull);
    int eq = CFStringCompare(x, y, kCFCompareCaseInsensitive) == kCFCompareEqualTo;
    CFRelease(x); CFRelease(y);
    return eq;
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
// IDWriteFontCollection1..3
static void *system_font_set(void);
static HANDLE expiration_event(void);
static HRESULT set_of_fonts(dw_font *const *fonts, uint32_t n, void **out);
static WINAPI HRESULT coll_GetFontSet(void *this, void **out) {
    dw_collection *c = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (c->system) { void *s = system_font_set(); dw_addref(s); *out = s; return S_OK; }
    for (uint32_t i = 0; i < c->visible; i++) family_sort(c->families[i]);
    uint32_t n = 0;
    for (uint32_t i = 0; i < c->visible; i++) n += c->families[i]->count;
    dw_font **fonts = calloc(n ? n : 1, sizeof *fonts);
    n = 0;
    for (uint32_t i = 0; i < c->visible; i++) for (uint32_t j = 0; j < c->families[i]->count; j++) fonts[n++] = c->families[i]->fonts[j];
    HRESULT hr = set_of_fonts(fonts, n, out);
    free(fonts);
    return hr;
}
static WINAPI uint32_t coll_GetFontFamilyModel(void *this) { return (uint32_t)((dw_collection *)this)->model; }
static WINAPI HANDLE coll_GetExpirationEvent(void *this) { return ((dw_collection *)this)->system ? expiration_event() : NULL; }

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
static dw_list *list_new(dw_collection *c, dw_font *const *fonts, uint32_t n) {
    dw_list *l = dw_new(sizeof *l, g_list_vtbl, g_list_iids, "IDWriteFontList", list_destroy);
    l->coll = c; dw_addref(c);
    l->fonts = calloc(n ? n : 1, sizeof *l->fonts);
    if (n) memcpy(l->fonts, fonts, n * sizeof *l->fonts);
    l->count = n;
    return l;
}
static WINAPI HRESULT fam_GetMatchingFonts(void *this, uint32_t weight, uint32_t stretch, uint32_t style, void **out) {
    dw_family *fam = this;
    if (!out) return E_INVALIDARG;
    family_sort(fam);
    for (uint32_t i = 0; i < fam->count; i++) font_props(fam->fonts[i]);      // (compare_match runs under the lock)
    lock();
    dw_list *l = list_new(fam->coll, fam->fonts, fam->count);
    g_sort_weight = (int)weight; g_sort_stretch = (int)stretch; g_sort_style = (int)style;
    qsort(l->fonts, l->count, sizeof *l->fonts, compare_match);
    unlock();
    *out = l;
    return S_OK;
}
// Weight, stretch and style for font axis values (the OpenType registered axes: 'wght' is the weight, 'wdth' the
// width in percent of normal (the OS/2 width classes: 50, 62.5, 75, 87.5, 100, 112.5, 125, 150, 200), 'ital' 1 for
// italic, a 'slnt' other than 0 oblique).
static void wss_from_axes(const axis_value *v, uint32_t n, int *weight, int *stretch, int *style) {
    static const float widths[9] = { 50, 62.5f, 75, 87.5f, 100, 112.5f, 125, 150, 200 };
    *weight = 400; *stretch = 5; *style = STYLE_NORMAL;
    int slanted = 0;
    for (uint32_t i = 0; v && i < n; i++) {
        if (v[i].tag == AXIS_TAG('w', 'g', 'h', 't')) { float w = v[i].value; *weight = w < 1 ? 1 : w > 999 ? 999 : (int)lroundf(w); }
        else if (v[i].tag == AXIS_TAG('w', 'd', 't', 'h')) {
            int best = 0;
            for (int k = 1; k < 9; k++) if (fabsf(widths[k] - v[i].value) < fabsf(widths[best] - v[i].value)) best = k;
            *stretch = best + 1;
        } else if (v[i].tag == AXIS_TAG('i', 't', 'a', 'l') && v[i].value >= 0.5f) *style = STYLE_ITALIC;
        else if (v[i].tag == AXIS_TAG('s', 'l', 'n', 't') && v[i].value != 0) slanted = 1;
    }
    if (slanted && *style == STYLE_NORMAL) *style = STYLE_OBLIQUE;
}
static HRESULT set_of_fonts(dw_font *const *fonts, uint32_t n, void **out);
static WINAPI HRESULT font_GetFontFaceReference(void *this, void **out);
// IDWriteFontFamily1, IDWriteFontFamily2 (every font is local)
static WINAPI uint32_t fam_GetFontLocality(void *this, uint32_t i) { (void)this; (void)i; return LOCALITY_LOCAL; }
static WINAPI HRESULT fam_GetFontFaceReference(void *this, uint32_t i, void **out) {
    dw_font *ft = NULL;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    HRESULT hr = fam_GetFont(this, i, (void **)&ft);
    if (hr < 0) return hr;
    hr = font_GetFontFaceReference(ft, out);
    dw_release(ft);
    return hr;
}
static WINAPI HRESULT fam_GetMatchingFonts2(void *this, const axis_value *v, uint32_t n, void **out) {
    if (n && !v) return E_INVALIDARG;
    int w, s, st;
    wss_from_axes(v, n, &w, &s, &st);
    return fam_GetMatchingFonts(this, (uint32_t)w, (uint32_t)s, (uint32_t)st, out);
}
static WINAPI HRESULT fam_GetFontSet(void *this, void **out) {
    dw_family *fam = this;
    if (!out) return E_INVALIDARG;
    family_sort(fam);
    return set_of_fonts(fam->fonts, fam->count, out);
}
// IDWriteFontCollection2::GetMatchingFonts: a family's fonts by how well they match the axis values
static WINAPI HRESULT coll_GetMatchingFonts(void *this, const WCHAR *name, const axis_value *v, uint32_t n, void **out) {
    dw_collection *c = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!name || (n && !v)) return E_INVALIDARG;
    uint32_t idx;
    if (find_family(c, name, &idx)) return fam_GetMatchingFonts2(c->families[idx], v, n, out);
    *out = list_new(c, NULL, 0);                          // (no such family: no fonts)
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
// IDWriteFontList1, IDWriteFontList2
static WINAPI uint32_t list_GetFontLocality(void *this, uint32_t i) { (void)this; (void)i; return LOCALITY_LOCAL; }
static WINAPI HRESULT list_GetFontFaceReference(void *this, uint32_t i, void **out) {
    dw_list *l = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (i >= l->count) return E_INVALIDARG;
    return font_GetFontFaceReference(l->fonts[i], out);
}
static WINAPI HRESULT list_GetFontSet(void *this, void **out) { dw_list *l = this; if (!out) return E_INVALIDARG; return set_of_fonts(l->fonts, l->count, out); }

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
// A face's style names (its name table's typographic or legacy subfamily names, English forced to `style`); a face
// with simulations is named by them: "Bold", "Oblique", "Bold Oblique" (a Regular base gives its place up).
static dw_strings *style_names(dw_face *f, CFStringRef style, int sims) {
    static const int ids[] = { 17, 2, 0 };
    if (!sims) return face_names(f, ids, style);
    CFMutableStringRef s = CFStringCreateMutable(NULL, 0);
    if (style && CFStringCompare(style, CFSTR("Regular"), kCFCompareCaseInsensitive) && !(sims & SIM_BOLD)) CFStringAppend(s, style);
    if (sims & SIM_BOLD) CFStringAppend(s, CFSTR("Bold"));
    if (sims & SIM_OBLIQUE) CFStringAppend(s, CFStringGetLength(s) ? CFSTR(" Oblique") : CFSTR("Oblique"));
    dw_strings *st = strings_new();
    strings_set_english(st, s);
    CFRelease(s);
    return st;
}
static WINAPI HRESULT font_GetFaceNames(void *this, void **out) {
    dw_font *ft = this;
    if (!out) return E_INVALIDARG;
    dw_font *real = ft->base ? ft->base : ft;
    dw_face *f = font_face(real);
    CFStringRef style = real->desc ? CTFontDescriptorCopyAttribute(real->desc, kCTFontStyleNameAttribute) : (f ? CTFontCopyName(f->ct, kCTFontStyleNameKey) : NULL);
    *out = style_names(f, style, ft->sims);
    if (style) CFRelease(style);
    return S_OK;
}
// DWRITE_INFORMATIONAL_STRING_ID -> the 'name' table's name ID (-1: none; the script tags of 20 and 21 are not offered)
static HRESULT info_strings(dw_face *f, uint32_t id, void **out, int32_t *exists) {
    if (!out || !exists) return E_INVALIDARG;
    *out = NULL; *exists = 0;
    static const int ids[22] = { -1, 0, 5, 7, 8, 9, 12, 10, 11, 13, 14, 1, 2, 16, 17, 19, 4, 6, 20, 21, -1, -1 };
    if (id >= 22 || ids[id] < 0 || !f) return S_OK;
    dw_strings *s = strings_new();
    const uint8_t *t; uint32_t l; CFDataRef k = NULL;
    if (TABLE(f, TAG('n', 'a', 'm', 'e'), t, l, k)) s->n = name_strings(t, l, ids[id], s->e, 16);
    unkeep(k);
    if (!s->n) { dw_release(s); return S_OK; }
    *out = s; *exists = 1;
    return S_OK;
}
static WINAPI HRESULT font_GetInformationalStrings(void *this, uint32_t id, void **out, int32_t *exists) {
    dw_font *ft = this;
    return info_strings(out && exists ? font_face(ft->base ? ft->base : ft) : NULL, id, out, exists);
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
// IDWriteFont3
static int faces_equal(dw_face *a, dw_face *b);
static HRESULT face_reference(dw_face *f, dw_font *font, void **out);
static WINAPI int32_t font_Equals(void *this, void *other) {
    if (other == this) return 1;
    if (!is_ours(other, g_font_vtbl)) return 0;
    dw_face *a = font_face(this), *b = font_face(other);
    return a && b && faces_equal(a, b);
}
static WINAPI HRESULT font_GetFontFaceReference(void *this, void **out) {
    if (!out) return E_INVALIDARG;
    *out = NULL;
    dw_face *f = font_face(this);
    return f ? face_reference(f, this, out) : DWRITE_E_FILEFORMAT;
}
static WINAPI int32_t font_HasCharacter3(void *this, uint32_t cp) { dw_face *f = font_face(this); return f && face_glyph(f, cp) != 0; }
static WINAPI uint32_t font_GetLocality(void *this) { (void)this; return LOCALITY_LOCAL; }       // (every font is local)

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
    *mode = mode0((uint32_t)recommended_mode(em * ppd, (int)measuring, params));
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
    *mode = mode0((uint32_t)recommended_mode(em * sqrtf(dpi_x * dpi_y) / 96.0f * matrix_scale(m), (int)measuring, params));
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
// Kerning pairs: the legacy 'kern' table's (GPOS kerning is the shaper's), in either version of it: OpenType's (u16
// version 0, u16 table count; subtable: u16 version, u16 length, u16 coverage with the format in its high byte) or
// Apple's (u32 0x00010000, u32 count; subtable: u32 length, u16 coverage with the format in its low byte, u16 tuple
// index). Format 0 subtables of horizontal kerning (not minimum values, not cross-stream, not variations) count:
// u16 nPairs, three u16 of search hints, then pairs of u16 left, u16 right, FWORD value sorted by left << 16 | right.
// Each subtable's value adds to the ones before (an OpenType subtable with the override bit replaces them).
// count_only: the number of pairs there are; else the adjustment for (left, right).
static int32_t kern_table(const uint8_t *t, uint32_t len, int count_only, uint16_t left, uint16_t right) {
    if (!t || len < 8) return 0;
    int apple = be32(t) == 0x00010000;
    if (!apple && be16(t)) return 0;
    uint32_t ntables = apple ? be32(t + 4) : be16(t + 2), off = apple ? 8 : 4;
    int32_t value = 0, pairs = 0;
    for (uint32_t i = 0; i < ntables && off + 8 <= len; i++) {
        uint32_t sublen, body; uint16_t coverage; int usable, override = 0;
        if (apple) {
            sublen = be32(t + off); coverage = be16(t + off + 4); body = off + 8;
            usable = (coverage & 0xFF) == 0 && !(coverage & 0xE000);
        } else {
            sublen = be16(t + off + 2); coverage = be16(t + off + 4); body = off + 6;
            usable = (coverage >> 8) == 0 && (coverage & 7) == 1;
            override = (coverage & 8) != 0;
            if (ntables == 1) sublen = len - off;           // (one subtable: its 16-bit length may have overflowed)
        }
        if (sublen < 8 || (uint64_t)off + sublen > len) sublen = len - off;
        if (usable && body + 8 <= off + sublen) {
            uint32_t n = be16(t + body), room = (off + sublen - body - 8) / 6;
            if (n > room) n = room;
            const uint8_t *p = t + body + 8;
            if (count_only) pairs += (int32_t)n;
            else {
                uint32_t want = (uint32_t)left << 16 | right, lo = 0, hi = n;
                while (lo < hi) {
                    uint32_t mid = (lo + hi) / 2, k = be32(p + 6 * mid);
                    if (k == want) { int16_t v = bes16(p + 6 * mid + 4); value = override ? v : value + v; break; }
                    if (k < want) lo = mid + 1; else hi = mid;
                }
            }
        }
        off += sublen;
    }
    return count_only ? pairs : value;
}
// The adjustment of each glyph's advance for the glyph after it (design units); the last one's is 0.
static WINAPI HRESULT face_GetKerningPairAdjustments(void *this, uint32_t count, const uint16_t *glyphs, int32_t *adj) {
    dw_face *f = this;
    if (!glyphs || !adj) return E_INVALIDARG;
    memset(adj, 0, 4 * (size_t)count);
    const uint8_t *t; uint32_t l; CFDataRef k = NULL;
    if (count > 1 && TABLE(f, TAG('k', 'e', 'r', 'n'), t, l, k))
        for (uint32_t i = 0; i + 1 < count; i++) adj[i] = kern_table(t, l, 0, glyphs[i], glyphs[i + 1]);
    unkeep(k);
    return S_OK;
}
static WINAPI int32_t face_HasKerningPairs(void *this) {
    const uint8_t *t; uint32_t l; CFDataRef k = NULL;
    int32_t n = TABLE((dw_face *)this, TAG('k', 'e', 'r', 'n'), t, l, k) ? kern_table(t, l, 1, 0, 0) : 0;
    unkeep(k);
    return n > 0;
}
// Vertical glyph variants: GSUB's 'vert' feature (any script and language), its single substitutions (lookup type 1,
// or 7 extending to 1), applied in lookup order. GSUB: u16 major, minor version, Offset16 script list, feature list,
// lookup list. Feature list: u16 count, records of Tag, Offset16 feature; feature: Offset16 params, u16 lookup count,
// u16 lookup indices. Lookup list: u16 count, Offset16 lookups; lookup: u16 type, flags, subtable count, Offset16
// subtables. Coverage: format 1 u16 count, u16 glyphs (sorted); format 2 u16 count, ranges of u16 start, end, index.
static int coverage_index(const uint8_t *t, uint32_t len, uint32_t at, uint16_t g) {
    if (at + 4 > len) return -1;
    uint16_t format = be16(t + at), n = be16(t + at + 2);
    if (format == 1) {
        uint32_t lo = 0, hi = n;
        if (at + 4 + 2ull * n > len) return -1;
        while (lo < hi) { uint32_t mid = (lo + hi) / 2; uint16_t v = be16(t + at + 4 + 2 * mid); if (v == g) return (int)mid; if (v < g) lo = mid + 1; else hi = mid; }
    } else if (format == 2) {
        if (at + 4 + 6ull * n > len) return -1;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *r = t + at + 4 + 6 * i;
            if (g >= be16(r) && g <= be16(r + 2)) return be16(r + 4) + (g - be16(r));
        }
    }
    return -1;
}
// The single substitution subtables of the 'vert' lookups (their offsets in GSUB), up to `cap`.
static int vert_subtables(const uint8_t *t, uint32_t len, uint32_t *out, int cap) {
    if (!t || len < 10 || be16(t) != 1) return 0;
    uint32_t features = be16(t + 6), lookups = be16(t + 8);
    if (features + 2 > len || lookups + 2 > len) return 0;
    uint16_t nfeat = be16(t + features), nlook = be16(t + lookups);
    uint8_t used[8192] = { 0 };                      // (lookup indices taken, 65536 bits)
    int n = 0;
    for (uint32_t i = 0; i < nfeat && features + 2 + 6 * (i + 1) <= len; i++) {
        const uint8_t *r = t + features + 2 + 6 * i;
        if (be32(r) != TAG('v', 'e', 'r', 't')) continue;
        uint32_t ft = features + be16(r + 4);
        if (ft + 4 > len) continue;
        uint16_t nidx = be16(t + ft + 2);
        for (uint32_t j = 0; j < nidx && ft + 4 + 2 * (j + 1) <= len; j++) used[be16(t + ft + 4 + 2 * j) >> 3] |= (uint8_t)(1 << (be16(t + ft + 4 + 2 * j) & 7));
    }
    for (uint32_t li = 0; li < nlook && lookups + 2 + 2 * (li + 1) <= len; li++) {
        if (!(used[li >> 3] >> (li & 7) & 1)) continue;
        uint32_t lk = lookups + be16(t + lookups + 2 + 2 * li);
        if (lk + 6 > len) continue;
        uint16_t type = be16(t + lk), nsub = be16(t + lk + 4);
        for (uint32_t s = 0; s < nsub && lk + 6 + 2 * (s + 1) <= len && n < cap; s++) {
            uint32_t st = lk + be16(t + lk + 6 + 2 * s);
            if (type == 7) {                             // extension: u16 format, u16 type, Offset32 from here
                if (st + 8 > len || be16(t + st + 2) != 1 || (uint64_t)st + be32(t + st + 4) + 6 > len) continue;
                st += be32(t + st + 4);
            } else if (type != 1) continue;
            if (st + 6 <= len) out[n++] = st;
        }
    }
    return n;
}
static uint16_t vert_glyph(const uint8_t *t, uint32_t len, const uint32_t *subs, int n, uint16_t g) {
    for (int i = 0; i < n; i++) {
        uint32_t st = subs[i];
        int c = coverage_index(t, len, st + be16(t + st + 2), g);
        if (c < 0) continue;
        if (be16(t + st) == 1) g = (uint16_t)(g + bes16(t + st + 4));
        else if (be16(t + st) == 2 && (uint32_t)c < be16(t + st + 4) && st + 6 + 2 * (uint32_t)(c + 1) <= len) g = be16(t + st + 6 + 2 * c);
    }
    return g;
}
static WINAPI HRESULT face_GetVerticalGlyphVariants(void *this, uint32_t count, const uint16_t *nominal, uint16_t *vertical) {
    if (!nominal || !vertical) return E_INVALIDARG;
    memmove(vertical, nominal, 2 * (size_t)count);
    const uint8_t *t; uint32_t l; CFDataRef k = NULL;
    if (count && TABLE((dw_face *)this, TAG('G', 'S', 'U', 'B'), t, l, k)) {
        uint32_t subs[64];
        int n = vert_subtables(t, l, subs, 64);
        for (uint32_t i = 0; i < count && n; i++) vertical[i] = vert_glyph(t, l, subs, n, vertical[i]);
    }
    unkeep(k);
    return S_OK;
}
static WINAPI int32_t face_HasVerticalGlyphVariants(void *this) {
    const uint8_t *t; uint32_t l; CFDataRef k = NULL;
    uint32_t subs[1];
    int n = TABLE((dw_face *)this, TAG('G', 'S', 'U', 'B'), t, l, k) ? vert_subtables(t, l, subs, 1) : 0;
    unkeep(k);
    return n > 0;
}
static WINAPI int32_t face_IsColorFont(void *this) { (void)this; return 0; }
static WINAPI uint32_t face_GetColorPaletteCount(void *this) { (void)this; return 0; }
static WINAPI uint32_t face_GetPaletteEntryCount(void *this) { (void)this; return 0; }
static WINAPI HRESULT face_GetPaletteEntries(void *this, uint32_t palette, uint32_t first, uint32_t count, void *entries) {
    (void)this; (void)palette; (void)first; (void)count; (void)entries;
    return DWRITE_E_NOCOLOR;
}

// ---- faces of files, their identity; IDWriteFontFaceReference, IDWriteFontResource -----------------------------------------
// A face of a file: faces of local files are made once per file, face and simulations, others each time.
static struct { dw_face *face; int sims; } g_local_faces[512];
static int g_nlocal_faces;
static HRESULT face_for_file(dw_file *file, uint32_t index, int sims, dw_face **out) {
    *out = NULL;
    if (file->kind == F_LOCAL) {
        lock();
        for (int i = 0; i < g_nlocal_faces; i++) {
            dw_face *f = g_local_faces[i].face;
            if (f->index == index && g_local_faces[i].sims == sims && !strcmp(f->file->unix_path, file->unix_path)) {
                dw_addref(f); *out = f; unlock();
                return S_OK;
            }
        }
        unlock();
    }
    dw_face *f;
    HRESULT hr = face_from_file(file, index, sims, &f);      // (a program's file is read without the lock)
    if (hr < 0) return hr;
    if (file->kind == F_LOCAL) {
        lock();
        if (g_nlocal_faces < 512) { dw_addref(f); g_local_faces[g_nlocal_faces].face = f; g_local_faces[g_nlocal_faces++].sims = sims; }
        unlock();
    }
    *out = f;
    return S_OK;
}
// Two references to one file: the same object, the same local path, or the same loader and key.
static int file_same(dw_file *a, dw_file *b) {
    if (a == b) return 1;
    if (!a || !b || a->kind != b->kind) return 0;
    if (a->kind == F_LOCAL) return !strcmp(a->unix_path, b->unix_path);
    return a->kind != F_MEMORY && a->loader == b->loader && a->key_size == b->key_size && !memcmp(a->key, b->key, a->key_size);
}
static int faces_equal(dw_face *a, dw_face *b) {
    if (a == b) return 1;
    dw_file *fa = face_file(a), *fb = face_file(b);
    return fa && fb && file_same(fa, fb) && a->index == b->index && a->sims == b->sims;
}

// IDWriteFontFaceReference1: a face of a file (and the face it was made from, when it was: the same face again).
typedef struct { dw_obj o; dw_file *file; uint32_t index; int sims; dw_face *face; } dw_ref;
static const GUID *const g_ref_iids[] = { &IID_IDWriteFontFaceReference, &IID_IDWriteFontFaceReference1, NULL };
static void ref_destroy(dw_obj *o) { dw_ref *r = (dw_ref *)o; dw_release(r->file); if (r->face) dw_release(r->face); free(r); }
static dw_ref *ref_new(dw_file *file, uint32_t index, int sims, dw_face *face) {
    dw_ref *r = dw_new(sizeof *r, g_ref_vtbl, g_ref_iids, "IDWriteFontFaceReference", ref_destroy);
    r->file = file; dw_addref(file);
    r->index = index; r->sims = sims;
    if (face && face->sims == sims) { r->face = face; dw_addref(face); }
    return r;
}
static HRESULT face_reference(dw_face *f, dw_font *font, void **out) {
    (void)font;
    dw_file *file = face_file(f);
    if (!file) return E_FAIL;
    *out = ref_new(file, f->index, f->sims, f);
    return S_OK;
}
static WINAPI HRESULT ref_CreateFontFaceWithSimulations(void *this, uint32_t sims, void **out) {
    dw_ref *r = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (sims > 3) return E_INVALIDARG;
    if (r->face && (int)sims == r->sims) { dw_addref(r->face); *out = r->face; return S_OK; }
    dw_face *f;
    HRESULT hr = face_for_file(r->file, r->index, (int)sims, &f);
    if (hr >= 0) *out = f;
    return hr;
}
static WINAPI HRESULT ref_CreateFontFace(void *this, void **out) { return ref_CreateFontFaceWithSimulations(this, (uint32_t)((dw_ref *)this)->sims, out); }
static WINAPI int32_t ref_Equals(void *this, void *other) {
    dw_ref *a = this, *b = other;
    if (a == b) return 1;
    return is_ours(other, g_ref_vtbl) && file_same(a->file, b->file) && a->index == b->index && a->sims == b->sims;
}
static WINAPI uint32_t ref_GetFontFaceIndex(void *this) { return ((dw_ref *)this)->index; }
static WINAPI uint32_t ref_GetSimulations(void *this) { return (uint32_t)((dw_ref *)this)->sims; }
static WINAPI HRESULT ref_GetFontFile(void *this, void **out) {
    if (!out) return E_INVALIDARG;
    dw_file *f = ((dw_ref *)this)->file;
    dw_addref(f);
    *out = f;
    return S_OK;
}
static WINAPI uint64_t ref_GetFileSize(void *this) { const uint8_t *d; size_t n; return file_bytes(((dw_ref *)this)->file, &d, &n) ? (uint64_t)n : 0; }
static WINAPI HRESULT ref_GetFileTime(void *this, uint64_t *t) {          // (FILETIME)
    dw_file *f = ((dw_ref *)this)->file;
    if (!t) return E_INVALIDARG;
    *t = 0;
    if (f->kind == F_LOCAL) { memcpy(t, f->key, 8); return S_OK; }
    if (f->kind != F_CUSTOM) return S_OK;
    // the program's loader: CreateStreamFromKey (3), the stream's GetLastWriteTime (6); without the lock
    void *stream = NULL;
    HRESULT hr = ((HRESULT (WINAPI *)(void *, const void *, uint32_t, void **))SLOT(f->loader, 3))(f->loader, f->key, f->key_size, &stream);
    if (hr >= 0 && stream) hr = ((HRESULT (WINAPI *)(void *, uint64_t *))SLOT(stream, 6))(stream, t);
    if (stream) com_release(stream);
    return hr;
}
static WINAPI uint32_t ref_GetLocality(void *this) { (void)this; return LOCALITY_LOCAL; }
// (every font is local: nothing to download, the request is met)
static WINAPI HRESULT ref_EnqueueFontDownloadRequest(void *this) { (void)this; return S_OK; }
static WINAPI HRESULT ref_EnqueueCharacterDownloadRequest(void *this, const WCHAR *chars, uint32_t n) { (void)this; return n && !chars ? E_INVALIDARG : S_OK; }
static WINAPI HRESULT ref_EnqueueGlyphDownloadRequest(void *this, const uint16_t *glyphs, uint32_t n) { (void)this; return n && !glyphs ? E_INVALIDARG : S_OK; }
static WINAPI HRESULT ref_EnqueueFileFragmentDownloadRequest(void *this, uint64_t offset, uint64_t size) { (void)this; (void)offset; (void)size; return S_OK; }
// No font has variation axes.
static WINAPI uint32_t no_axes_count(void *this) { (void)this; return 0; }
static WINAPI HRESULT no_axes_values(void *this, void *values, uint32_t n) { (void)this; (void)values; (void)n; return S_OK; }

// IDWriteFontResource: a face of a file, without simulations or axis values chosen yet.
typedef struct { dw_obj o; dw_file *file; uint32_t index; } dw_resource;
static const GUID *const g_resource_iids[] = { &IID_IDWriteFontResource, NULL };
static void resource_destroy(dw_obj *o) { dw_release(((dw_resource *)o)->file); free(o); }
static HRESULT resource_new(dw_file *file, uint32_t index, void **out) {
    const uint8_t *d; size_t n; int type;
    if (!file_bytes(file, &d, &n)) return file->kind == F_LOCAL ? DWRITE_E_FILENOTFOUND : DWRITE_E_FILEFORMAT;
    if (index >= sfnt_faces(d, n, &type)) return DWRITE_E_FILEFORMAT;
    dw_resource *r = dw_new(sizeof *r, g_resource_vtbl, g_resource_iids, "IDWriteFontResource", resource_destroy);
    r->file = file; dw_addref(file); r->index = index;
    *out = r;
    return S_OK;
}
static WINAPI HRESULT res_GetFontFile(void *this, void **out) {
    if (!out) return E_INVALIDARG;
    dw_file *f = ((dw_resource *)this)->file;
    dw_addref(f);
    *out = f;
    return S_OK;
}
static WINAPI uint32_t res_GetFontFaceIndex(void *this) { return ((dw_resource *)this)->index; }
static WINAPI uint32_t res_GetFontAxisAttributes(void *this, uint32_t axis) { (void)this; (void)axis; return 0; }   // (NONE: no such axis)
static WINAPI HRESULT res_GetAxisNames(void *this, uint32_t axis, void **out) { (void)this; (void)axis; if (out) *out = NULL; return E_INVALIDARG; }
static WINAPI uint32_t res_GetAxisValueNameCount(void *this, uint32_t axis) { (void)this; (void)axis; return 0; }
static WINAPI HRESULT res_GetAxisValueNames(void *this, uint32_t axis, uint32_t value, axis_range *range, void **out) {
    (void)this; (void)axis; (void)value;
    if (range) memset(range, 0, sizeof *range);
    if (out) *out = NULL;
    return E_INVALIDARG;
}
static WINAPI int32_t res_HasVariations(void *this) { (void)this; return 0; }
// (axis values for axes the font does not have change nothing)
static WINAPI HRESULT res_CreateFontFace(void *this, uint32_t sims, const axis_value *v, uint32_t n, void **out) {
    dw_resource *r = this;
    (void)v; (void)n;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (sims > 3) return E_INVALIDARG;
    dw_face *f;
    HRESULT hr = face_for_file(r->file, r->index, (int)sims, &f);
    if (hr >= 0) *out = f;
    return hr;
}
static WINAPI HRESULT res_CreateFontFaceReference(void *this, uint32_t sims, const axis_value *v, uint32_t n, void **out) {
    dw_resource *r = this;
    (void)v; (void)n;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (sims > 3) return E_INVALIDARG;
    *out = ref_new(r->file, r->index, (int)sims, NULL);
    return S_OK;
}

// ---- IDWriteFontFace3, IDWriteFontFace4, IDWriteFontFace5 ---------------------------------------------------------------------
static WINAPI HRESULT fam_GetFamilyNames(void *this, void **out);
static WINAPI HRESULT face_GetFontFaceReference(void *this, void **out) {
    if (!out) return E_INVALIDARG;
    *out = NULL;
    return face_reference(this, NULL, out);
}
static WINAPI void face_GetPanose(void *this, uint8_t *p) { dw_face *f = this; if (!p) return; face_prepare(f); memcpy(p, f->panose, 10); }
static WINAPI uint32_t face_GetWeight(void *this) { dw_face *f = this; int w, s, st; face_wss(f, f->sims, &w, &s, &st); return (uint32_t)w; }
static WINAPI uint32_t face_GetStretch(void *this) { dw_face *f = this; int w, s, st; face_wss(f, f->sims, &w, &s, &st); return (uint32_t)s; }
static WINAPI uint32_t face_GetStyle(void *this) { dw_face *f = this; int w, s, st; face_wss(f, f->sims, &w, &s, &st); return (uint32_t)st; }
// A collection's face: its family's names; any other: the name table's, English as CoreText names the family.
static WINAPI HRESULT face_GetFamilyNames(void *this, void **out) {
    dw_face *f = this;
    if (!out) return E_INVALIDARG;
    lock();
    dw_family *fam = f->font ? f->font->family : NULL;
    if (fam) dw_addref(fam);
    unlock();
    if (fam) { HRESULT hr = fam_GetFamilyNames(fam, out); dw_release(fam); return hr; }
    static const int ids[] = { 16, 1, 0 };
    CFStringRef name = CTFontCopyFamilyName(f->ct);
    *out = face_names(f, ids, name);
    if (name) CFRelease(name);
    return S_OK;
}
static WINAPI HRESULT face_GetFaceNames(void *this, void **out) {
    dw_face *f = this;
    if (!out) return E_INVALIDARG;
    CFStringRef style = CTFontCopyName(f->ct, kCTFontStyleNameKey);
    *out = style_names(f, style, f->sims);
    if (style) CFRelease(style);
    return S_OK;
}
static WINAPI HRESULT face_GetInformationalStrings(void *this, uint32_t id, void **out, int32_t *exists) { return info_strings(this, id, out, exists); }
static WINAPI int32_t face_HasCharacter(void *this, uint32_t cp) { return face_glyph(this, cp) != 0; }
static WINAPI HRESULT face_GetRecommendedRenderingMode3(void *this, float em, float dpi_x, float dpi_y, const matrix *m, int32_t sideways, uint32_t threshold,
                                                        uint32_t measuring, void *params, uint32_t *mode, uint32_t *grid) {
    (void)this; (void)sideways; (void)threshold;
    if (!mode || !grid) return E_INVALIDARG;
    *mode = (uint32_t)recommended_mode(em * sqrtf(dpi_x * dpi_y) / 96.0f * matrix_scale(m), (int)measuring, params);
    *grid = GRID_FIT_DISABLED;
    return S_OK;
}
// (every font is local)
static WINAPI int32_t face_IsCharacterLocal(void *this, uint32_t cp) { (void)this; (void)cp; return 1; }
static WINAPI int32_t face_IsGlyphLocal(void *this, uint16_t g) { (void)this; (void)g; return 1; }
static WINAPI HRESULT face_AreCharactersLocal(void *this, const WCHAR *chars, uint32_t n, int32_t enqueue, int32_t *local) {
    (void)this; (void)enqueue;
    if (!local || (n && !chars)) return E_INVALIDARG;
    *local = 1;
    return S_OK;
}
static WINAPI HRESULT face_AreGlyphsLocal(void *this, const uint16_t *glyphs, uint32_t n, int32_t enqueue, int32_t *local) {
    (void)this; (void)enqueue;
    if (!local || (n && !glyphs)) return E_INVALIDARG;
    *local = 1;
    return S_OK;
}
// Glyph images: the outlines a face has (TrueType 'glyf', CFF 'CFF ' or 'CFF2'); bitmaps (sbix, CBDT), SVG and COLR
// are not offered.
static uint32_t face_image_formats(dw_face *f) {
    uint32_t formats = 0;
    const uint8_t *t; uint32_t l; CFDataRef k = NULL;
    if (TABLE(f, TAG('g', 'l', 'y', 'f'), t, l, k)) formats |= IMAGE_TRUETYPE;
    unkeep(k); k = NULL;
    if (TABLE(f, TAG('C', 'F', 'F', ' '), t, l, k)) formats |= IMAGE_CFF;
    unkeep(k); k = NULL;
    if (TABLE(f, TAG('C', 'F', 'F', '2'), t, l, k)) formats |= IMAGE_CFF;
    unkeep(k);
    return formats;
}
static WINAPI HRESULT face_GetGlyphImageFormats_(void *this, uint16_t g, uint32_t first, uint32_t last, uint32_t *formats) {
    dw_face *f = this;
    (void)first; (void)last;
    if (!formats) return E_INVALIDARG;
    face_prepare(f);
    *formats = g < f->glyphs ? face_image_formats(f) : 0;
    return g < f->glyphs ? S_OK : E_INVALIDARG;
}
static WINAPI uint32_t face_GetGlyphImageFormats(void *this) { return face_image_formats(this); }
static WINAPI HRESULT face_GetGlyphImageData(void *this, uint16_t g, uint32_t ppem, uint32_t format, void *data, void **ctx) {
    (void)this; (void)g; (void)ppem;
    if (!data || !ctx) return E_INVALIDARG;
    memset(data, 0, 64);                                   // DWRITE_GLYPH_IMAGE_DATA
    *ctx = NULL;
    if (format == IMAGE_TRUETYPE || format == IMAGE_CFF) return E_INVALIDARG;     // (outlines: GetGlyphRunOutline)
    return dw_not_implemented("IDWriteFontFace4::GetGlyphImageData (bitmap, SVG and COLR glyph images)");
}
static WINAPI void face_ReleaseGlyphImageData(void *this, void *ctx) { (void)this; (void)ctx; }
static WINAPI int32_t face_HasVariations(void *this) { (void)this; return 0; }
static WINAPI HRESULT face_GetFontResource(void *this, void **out) {
    dw_face *f = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    dw_file *file = face_file(f);
    return file ? resource_new(file, f->index, out) : E_FAIL;
}
static WINAPI int32_t face_Equals(void *this, void *other) { return other == this || (is_ours(other, g_face_vtbl) && faces_equal(this, other)); }

// ---- IDWriteRenderingParams ------------------------------------------------------------------------------------------------
typedef struct { dw_obj o; float gamma, contrast, gray_contrast, cleartype; uint32_t geometry, mode, grid; } dw_params;
static const GUID *const g_params_iids[] = { &IID_IDWriteRenderingParams, &IID_IDWriteRenderingParams1, &IID_IDWriteRenderingParams2,
                                             &IID_IDWriteRenderingParams3, NULL };
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
static WINAPI uint32_t params_GetRenderingMode(void *this) { return mode0(((dw_params *)this)->mode); }
static WINAPI uint32_t params_GetRenderingMode1(void *this) { return ((dw_params *)this)->mode; }
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
// The text of a range from the program's IDWriteTextAnalysisSource (GetTextAtPosition (3), GetLocaleName (6)):
// malloc'd, its length in *got (0: none); its locale (the program's string) in *locale.
static WCHAR *source_text(void *source, uint32_t pos, uint32_t length, uint32_t *got, const WCHAR **locale) {
    WCHAR *text = malloc(2 * (size_t)(length ? length : 1));
    *got = 0; *locale = NULL;
    while (*got < length) {
        const WCHAR *t = NULL; uint32_t n = 0;
        if (((HRESULT (WINAPI *)(void *, uint32_t, const WCHAR **, uint32_t *))SLOT(source, 3))(source, pos + *got, &t, &n) < 0 || !t || !n) break;
        t = VX_LOW(t);
        if (n > length - *got) n = length - *got;
        memcpy(text + *got, t, 2 * (size_t)n);
        *got += n;
    }
    if (*got) {
        uint32_t loclen = 0;
        ((HRESULT (WINAPI *)(void *, uint32_t, uint32_t *, const WCHAR **))SLOT(source, 6))(source, pos, &loclen, locale);
        *locale = VX_LOW(*locale);
    }
    return text;
}
// The base font of a fallback: the family asked for, in the collection given (ours) or the system's.
static dw_face *fallback_base(void *base_coll, const WCHAR *base_family, int weight, int stretch, int style, dw_font **font) {
    dw_collection *coll = is_ours(base_coll, g_collection_vtbl) ? base_coll : system_collection();
    uint32_t idx;
    *font = base_family && base_family[0] && find_family(coll, base_family, &idx) ? family_match(coll->families[idx], weight, stretch, style) : NULL;
    return *font ? font_face(*font) : NULL;
}
// What a text with no font for its start maps: up to a character some font may have (marks go with their base).
static uint32_t unmapped_run(const WCHAR *text, uint32_t got) {
    uint32_t run, u;
    next_cp(text, got, 0, &run);
    while (run < got) {
        uint32_t cp = next_cp(text, got, run, &u);
        if (!continues_cluster(cp)) break;
        run += u;
    }
    return run;
}
// The system fallback: the base font while it has the characters, else the installed font CoreText takes for them.
static void system_map(const WCHAR *text, uint32_t got, const WCHAR *locale, void *base_coll, const WCHAR *base_family, int weight, int style, int stretch,
                       uint32_t *mapped, dw_font **font) {
    dw_font *base;
    dw_face *bf = fallback_base(base_coll, base_family, weight, stretch, style, &base);
    uint32_t u, cp0 = next_cp(text, got, 0, &u);
    dw_font *result = NULL;
    uint32_t run = 0;
    if (bf && face_glyph(bf, cp0)) { result = base; run = covered_run(bf, NULL, text, got); }
    else if ((result = fallback_font(bf, text, got, locale, weight, stretch, style))) run = covered_run(font_face(result), bf, text, got);
    if (!result) run = unmapped_run(text, got);          // no font has it: with no font
    *mapped = run ? run : u;
    *font = result;
}
static HRESULT fallback_map(dw_obj *fb, void *source, uint32_t pos, uint32_t length, void *base_coll, const WCHAR *base_family, int weight, int style, int stretch,
                            uint32_t *mapped, dw_font **font, float *scale);
static WINAPI HRESULT fallback_MapCharacters(void *this, void *source, uint32_t pos, uint32_t length, void *base_coll, const WCHAR *base_family,
                                             uint32_t weight, uint32_t style, uint32_t stretch, uint32_t *mapped, void **font, float *scale) {
    if (!source || !mapped || !font || !scale) return E_INVALIDARG;
    *mapped = 0; *font = NULL; *scale = 1.0f;
    dw_font *ft = NULL;
    HRESULT hr = fallback_map(this, source, pos, length, base_coll, base_family, (int)weight, (int)style, (int)stretch, mapped, &ft, scale);
    if (ft) { dw_addref(ft); *font = ft; }
    return hr;
}
// IDWriteFontFallback1: weight, stretch and style from the axis values; the font's face
static WINAPI HRESULT fallback_MapCharacters1(void *this, void *source, uint32_t pos, uint32_t length, void *base_coll, const WCHAR *base_family,
                                              const axis_value *v, uint32_t n, uint32_t *mapped, float *scale, void **face) {
    if (!source || !mapped || !face || !scale || (n && !v)) return E_INVALIDARG;
    *mapped = 0; *face = NULL; *scale = 1.0f;
    int w, st, sty;
    wss_from_axes(v, n, &w, &st, &sty);
    dw_font *ft = NULL;
    HRESULT hr = fallback_map(this, source, pos, length, base_coll, base_family, w, sty, st, mapped, &ft, scale);
    dw_face *f = ft ? font_face(ft) : NULL;
    if (f) { dw_addref(f); *face = f; }
    return hr;
}

// ---- IDWriteFontFallbackBuilder: fallbacks of the program's own -----------------------------------------------------------
// A custom fallback: mappings in the order added, each from Unicode ranges to target families (in a collection, the
// system's when none), for a locale and a base family when it names them (any otherwise), with a scale; then, when the
// system fallback's mappings were added (AddMappings), the system fallback. As with the system fallback, the base font
// maps the characters it has first.
typedef struct { unicode_range *ranges; uint32_t nranges; WCHAR **families; uint32_t nfamilies; dw_collection *coll; WCHAR *locale, *base; float scale; } fb_mapping;
typedef struct { dw_obj o; fb_mapping *m; uint32_t n, cap; int system; } dw_fallback;      // (a builder, or the fallback it made)
static const GUID *const g_fallback_iids[] = { &IID_IDWriteFontFallback, &IID_IDWriteFontFallback1, NULL };
static const GUID *const g_fbbuilder_iids[] = { &IID_IDWriteFontFallbackBuilder, NULL };
static WCHAR *wcs_dup(const WCHAR *s) {
    size_t n = s ? vx_wcslen(s) : 0;
    WCHAR *d = malloc(2 * (n + 1));
    if (n) memcpy(d, s, 2 * n);
    d[n] = 0;
    return d;
}
static void mapping_add(dw_fallback *fb, const fb_mapping *s) {     // (a copy; under the lock: our objects only)
    if (fb->n == fb->cap) { fb->cap = fb->cap ? 2 * fb->cap : 8; fb->m = realloc(fb->m, fb->cap * sizeof *fb->m); }
    fb_mapping *d = &fb->m[fb->n++];
    *d = *s;
    d->ranges = malloc(s->nranges * sizeof *d->ranges); memcpy(d->ranges, s->ranges, s->nranges * sizeof *d->ranges);
    d->families = malloc(s->nfamilies * sizeof *d->families);
    for (uint32_t i = 0; i < s->nfamilies; i++) d->families[i] = wcs_dup(s->families[i]);
    d->locale = wcs_dup(s->locale); d->base = wcs_dup(s->base);
    if (d->coll) dw_addref(d->coll);
}
static void fallback_destroy(dw_obj *o) {
    dw_fallback *fb = (dw_fallback *)o;
    for (uint32_t i = 0; i < fb->n; i++) {
        fb_mapping *m = &fb->m[i];
        for (uint32_t j = 0; j < m->nfamilies; j++) free(m->families[j]);
        free(m->families); free(m->ranges); free(m->locale); free(m->base);
        if (m->coll) dw_release(m->coll);
    }
    free(fb->m); free(fb);
}
static int mapping_has(const fb_mapping *m, uint32_t cp) { for (uint32_t i = 0; i < m->nranges; i++) if (cp >= m->ranges[i].first && cp <= m->ranges[i].last) return 1; return 0; }
// A mapping's locale applies to the text's locale and to its subtags ("zh" to "zh-Hant-TW"); no locale, to any.
static int mapping_for(const fb_mapping *m, const WCHAR *locale, const WCHAR *base_family) {
    if (m->base[0] && !(base_family && wcs_ieq(m->base, base_family))) return 0;
    if (!m->locale[0]) return 1;
    size_t n = vx_wcslen(m->locale);
    if (!locale) return 0;
    for (size_t i = 0; i < n; i++) if (!locale[i] || tolower(locale[i] < 128 ? locale[i] : 0) != tolower(m->locale[i] < 128 ? m->locale[i] : 1)) return 0;
    return !locale[n] || locale[n] == '-';
}
static void custom_map(dw_fallback *fb, const WCHAR *text, uint32_t got, const WCHAR *locale, void *base_coll, const WCHAR *base_family, int weight, int style,
                       int stretch, uint32_t *mapped, dw_font **font, float *scale) {
    dw_font *base;
    dw_face *bf = fallback_base(base_coll, base_family, weight, stretch, style, &base);
    uint32_t u, cp0 = next_cp(text, got, 0, &u);
    if (bf && face_glyph(bf, cp0)) { *font = base; *mapped = covered_run(bf, NULL, text, got); return; }
    for (uint32_t i = 0; i < fb->n; i++) {
        fb_mapping *m = &fb->m[i];
        if (!mapping_has(m, cp0) || !mapping_for(m, locale, base_family)) continue;
        dw_collection *c = m->coll ? m->coll : system_collection();
        for (uint32_t j = 0; j < m->nfamilies; j++) {
            uint32_t idx;
            dw_font *ft = find_family(c, m->families[j], &idx) ? family_match(c->families[idx], weight, stretch, style) : NULL;
            dw_face *f = ft ? font_face(ft) : NULL;
            if (!f || !face_glyph(f, cp0)) continue;
            // what the font has, within the mapping's ranges, until the base font has the text again
            uint32_t run = covered_run(f, bf, text, got), at = u;
            while (at < run) {
                uint32_t k, cp = next_cp(text, got, at, &k);
                if (!continues_cluster(cp) && !mapping_has(m, cp)) break;
                at += k;
            }
            *font = ft; *mapped = at; *scale = m->scale;
            return;
        }
    }
    if (fb->system) { system_map(text, got, locale, base_coll, base_family, weight, style, stretch, mapped, font); return; }
    *mapped = unmapped_run(text, got);
}
static HRESULT fallback_map(dw_obj *fb, void *source, uint32_t pos, uint32_t length, void *base_coll, const WCHAR *base_family, int weight, int style, int stretch,
                            uint32_t *mapped, dw_font **font, float *scale) {
    *mapped = 0; *font = NULL; *scale = 1.0f;
    if (!length) return S_OK;
    uint32_t got; const WCHAR *locale;
    WCHAR *text = source_text(source, pos, length, &got, &locale);      // (the program's code: without the lock)
    if (got) {
        if (fb == &g_fallback) system_map(text, got, locale, base_coll, base_family, weight, style, stretch, mapped, font);
        else custom_map((dw_fallback *)fb, text, got, locale, base_coll, base_family, weight, style, stretch, mapped, font, scale);
        uint32_t u;
        TR("IDWriteFontFallback::MapCharacters(U+%04X.., %u) -> %s for %u", next_cp(text, got, 0, &u), got, *font ? (*font)->ps : "no font", *mapped);
    }
    free(text);
    return S_OK;
}
static WINAPI HRESULT fbb_AddMapping(void *this, const unicode_range *ranges, uint32_t nranges, const WCHAR **families, uint32_t nfamilies, void *coll,
                                     const WCHAR *locale, const WCHAR *base, float scale) {
    if (!ranges || !nranges || !families || !nfamilies || (coll && !is_ours(coll, g_collection_vtbl)) || !(scale > 0)) return E_INVALIDARG;
    for (uint32_t i = 0; i < nranges; i++) if (ranges[i].first > ranges[i].last) return E_INVALIDARG;
    WCHAR **names = malloc(nfamilies * sizeof *names);
    for (uint32_t i = 0; i < nfamilies; i++) names[i] = (WCHAR *)VX_LOW(families[i]);   // (the program's array of its strings)
    for (uint32_t i = 0; i < nfamilies; i++) if (!names[i]) { free(names); return E_INVALIDARG; }
    static const WCHAR empty[1] = { 0 };
    fb_mapping m = { (unicode_range *)ranges, nranges, names, nfamilies, coll, (WCHAR *)(locale ? locale : empty), (WCHAR *)(base ? base : empty), scale };
    lock();
    mapping_add(this, &m);
    unlock();
    free(names);
    return S_OK;
}
static WINAPI HRESULT fbb_AddMappings(void *this, void *fallback) {
    dw_fallback *b = this;
    if (fallback == &g_fallback) { b->system = 1; return S_OK; }
    if (!is_ours(fallback, g_fallback_vtbl)) return E_INVALIDARG;
    dw_fallback *f = fallback;
    lock();
    for (uint32_t i = 0; i < f->n; i++) mapping_add(b, &f->m[i]);
    b->system |= f->system;
    unlock();
    return S_OK;
}
static WINAPI HRESULT fbb_CreateFontFallback(void *this, void **out) {
    dw_fallback *b = this;
    if (!out) return E_INVALIDARG;
    dw_fallback *f = dw_new(sizeof *f, g_fallback_vtbl, g_fallback_iids, "IDWriteFontFallback", fallback_destroy);
    lock();
    for (uint32_t i = 0; i < b->n; i++) mapping_add(f, &b->m[i]);
    f->system = b->system;
    unlock();
    TR("IDWriteFontFallbackBuilder::CreateFontFallback: %u mappings%s", f->n, f->system ? ", then the system's" : "");
    *out = f;
    return S_OK;
}
static WINAPI HRESULT factory_CreateFontFallbackBuilder(void *this, void **out) {
    (void)this;
    if (!out) return E_INVALIDARG;
    *out = dw_new(sizeof(dw_fallback), g_fbbuilder_vtbl, g_fbbuilder_iids, "IDWriteFontFallbackBuilder", fallback_destroy);
    return S_OK;
}

// ---- IDWriteGdiInterop ---------------------------------------------------------------------------------------------------------
static dw_obj g_gdi;
// A LOGFONT's font in a collection: its face name as a family, its weight (0: normal) and italic.
static HRESULT font_of_logfont(const logfont_w *lf, dw_collection *c, void **out) {
    if (!lf || !out) return E_INVALIDARG;
    *out = NULL;
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
static WINAPI HRESULT gdi_CreateFontFromLOGFONT(void *this, const logfont_w *lf, void **out) { (void)this; return font_of_logfont(lf, system_collection(), out); }
static WINAPI HRESULT gdi_CreateBitmapRenderTarget(void *this, HANDLE hdc, uint32_t w, uint32_t h, void **out);
// The face of the font selected into a DC (gdi32's GetCurrentObject and GetObjectW give its LOGFONT).
static void *gdi_fn(const char *name);
static WINAPI HRESULT gdi_CreateFontFaceFromHdc(void *this, void *hdc, void **out) {
    (void)this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    HANDLE hfont = hdc ? ((HANDLE (WINAPI *)(HANDLE, uint32_t))gdi_fn("GetCurrentObject"))(hdc, 6) : NULL;      // OBJ_FONT
    logfont_w lf;
    memset(&lf, 0, sizeof lf);
    if (!hfont || ((int (WINAPI *)(HANDLE, int, void *))gdi_fn("GetObjectW"))(hfont, (int)sizeof lf, &lf) < (int)sizeof lf) return E_INVALIDARG;
    dw_font *ft = NULL;
    HRESULT hr = font_of_logfont(&lf, system_collection(), (void **)&ft);
    if (hr < 0) return hr;
    dw_face *f = font_face(ft);
    if (f) { dw_addref(f); *out = f; }
    dw_release(ft);
    return f ? S_OK : DWRITE_E_FILEFORMAT;
}
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
// IDWriteGdiInterop1
static WINAPI HRESULT gdi_CreateFontFromLOGFONT1(void *this, const logfont_w *lf, void *coll, void **out) {
    (void)this;
    if (out) *out = NULL;
    if (coll && !is_ours(coll, g_collection_vtbl)) return E_INVALIDARG;
    return font_of_logfont(lf, coll ? coll : system_collection(), out);
}
// FONTSIGNATURE: OS/2's Unicode ranges (ulUnicodeRange1-4) and code page ranges (ulCodePageRange1-2, version 1 on).
static HRESULT face_signature(dw_face *f, font_signature *sig) {
    memset(sig, 0, sizeof *sig);
    const uint8_t *t; uint32_t l; CFDataRef k = NULL;
    if (TABLE(f, TAG('O', 'S', '/', '2'), t, l, k) && l >= 58) {
        for (int i = 0; i < 4; i++) sig->usb[i] = be32(t + 42 + 4 * i);
        if (be16(t) >= 1 && l >= 86) { sig->csb[0] = be32(t + 78); sig->csb[1] = be32(t + 82); }
    }
    unkeep(k);
    return S_OK;
}
static WINAPI HRESULT gdi_GetFontSignature_(void *this, void *face, font_signature *sig) {
    (void)this;
    if (!sig) return E_INVALIDARG;
    if (!is_ours(face, g_face_vtbl)) { memset(sig, 0, sizeof *sig); return E_INVALIDARG; }
    return face_signature(face, sig);
}
static WINAPI HRESULT gdi_GetFontSignature(void *this, void *font, font_signature *sig) {
    (void)this;
    if (!sig) return E_INVALIDARG;
    dw_face *f = is_ours(font, g_font_vtbl) ? font_face(font) : NULL;
    if (!f) { memset(sig, 0, sizeof *sig); return E_INVALIDARG; }
    return face_signature(f, sig);
}
// The fonts of a set a LOGFONT names (its face name as their Win32 or weight-stretch-style family), nearest its weight
// and italic first.
static HRESULT set_by_logfont(void *set, const logfont_w *lf, void **out);
static WINAPI HRESULT gdi_GetMatchingFontsByLOGFONT(void *this, const logfont_w *lf, void *set, void **out) {
    (void)this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!lf || !is_ours(set, g_set_vtbl)) return E_INVALIDARG;
    return set_by_logfont(set, lf, out);
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
    // (no glyph is moved off its place, but the run carries offsets all the same, as DirectWrite's layouts' runs do:
    // renderers read them without looking whether there are any)
    glyph_offset *offsets = calloc((size_t)l->ncl + 1, sizeof *offsets);
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
            glyph_run run = { c->face, a->size, n, glyphs, advances, offsets, 0, 0 };
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
    free(glyphs); free(map); free(advances); free(offsets);
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
// (on a copy that holds no references: the attribute set it was copied from, and the caller, keep the objects)
static void attrs_change(text_attrs *a, int what, const void *v) {
    switch (what) {
    case A_COLL: a->coll = *(dw_collection *const *)v; break;
    case A_FAMILY: memset(a->family, 0, sizeof a->family); wcs_copy(a->family, 128, v); break;
    case A_LOCALE: memset(a->locale, 0, sizeof a->locale); wcs_copy(a->locale, 86, v); break;
    case A_WEIGHT: a->weight = *(const uint32_t *)v; break;
    case A_STYLE: a->style = *(const uint32_t *)v; break;
    case A_STRETCH: a->stretch = *(const uint32_t *)v; break;
    case A_SIZE: a->size = *(const float *)v; break;
    case A_UNDERLINE: a->underline = *(const int32_t *)v != 0; break;
    case A_STRIKE: a->strike = *(const int32_t *)v != 0; break;
    case A_EFFECT: a->effect = *(void *const *)v; break;
    case A_TYPOGRAPHY: a->typography = *(void *const *)v; break;
    }
}
// The index of an attribute set equal to *a, added when there is none. A new set takes its references to our
// objects here; the reference to the program's drawing effect is the caller's to take (outside the lock).
static uint16_t attrs_index(dw_layout *l, const text_attrs *a) {
    for (uint32_t i = 0; i < l->nattrs; i++) if (attrs_equal(&l->attrs[i], a)) return (uint16_t)i;
    l->attrs = realloc(l->attrs, (l->nattrs + 1) * sizeof *l->attrs);
    l->primary = realloc(l->primary, (l->nattrs + 1) * sizeof *l->primary);
    l->attrs[l->nattrs] = *a;
    if (a->coll) dw_addref(a->coll);
    if (a->typography) dw_addref(a->typography);
    l->primary[l->nattrs] = NULL;
    return (uint16_t)l->nattrs++;
}
static HRESULT layout_set(void *this, int what, const void *v, text_range r) {
    dw_layout *l = this;
    if (r.start >= l->len || !r.length) return S_OK;
    uint32_t end = r.length > l->len - r.start ? l->len : r.start + r.length;
    lock();
    if (l->nattrs >= 60000) { unlock(); return E_OUTOFMEMORY; }
    uint32_t first_new = l->nattrs;
    for (uint32_t i = r.start; i < end; i++) {
        text_attrs a = l->attrs[l->attr_of[i]];
        attrs_change(&a, what, v);
        l->attr_of[i] = attrs_index(l, &a);
    }
    // The new sets' drawing effects get their AddRef after the lock (no program code runs under it); until then the
    // caller's reference, or the set a new one was copied from, keeps each alive.
    uint32_t neffects = 0;
    void **effects = l->nattrs > first_new ? malloc((l->nattrs - first_new) * sizeof *effects) : NULL;
    for (uint32_t i = first_new; i < l->nattrs; i++) if (l->attrs[i].effect) effects[neffects++] = l->attrs[i].effect;
    l->dirty = 1;
    unlock();
    for (uint32_t i = 0; i < neffects; i++) com_addref(effects[i]);
    free(effects);
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
    if (l->attrs[0].effect) com_addref(l->attrs[0].effect);
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

// ---- IDWriteStringList ------------------------------------------------------------------------------------------------------
typedef struct { dw_obj o; name_entry *e; uint32_t n, cap; } dw_strlist;
static const GUID *const g_strlist_iids[] = { &IID_IDWriteStringList, NULL };
static void strlist_destroy(dw_obj *o) { dw_strlist *l = (dw_strlist *)o; for (uint32_t i = 0; i < l->n; i++) free(l->e[i].text); free(l->e); free(l); }
static void strlist_add(dw_strlist *l, const char *locale, const WCHAR *text, uint32_t len) {     // (once per text)
    for (uint32_t i = 0; i < l->n; i++) if (wcs_ieq(l->e[i].text, text)) return;
    if (l->n == l->cap) { l->cap = l->cap ? 2 * l->cap : 16; l->e = realloc(l->e, l->cap * sizeof *l->e); }
    name_entry *e = &l->e[l->n++];
    snprintf(e->locale, sizeof e->locale, "%s", locale);
    e->text = malloc(2 * ((size_t)len + 1)); memcpy(e->text, text, 2 * (size_t)len); e->text[len] = 0; e->len = len;
}
static WINAPI uint32_t strlist_GetCount(void *this) { return ((dw_strlist *)this)->n; }
static WINAPI HRESULT strlist_GetLocaleNameLength(void *this, uint32_t i, uint32_t *len) {
    dw_strlist *l = this;
    if (!len) return E_INVALIDARG;
    if (i >= l->n) { *len = 0; return E_INVALIDARG; }
    *len = (uint32_t)strlen(l->e[i].locale);
    return S_OK;
}
static WINAPI HRESULT strlist_GetLocaleName(void *this, uint32_t i, WCHAR *out, uint32_t cap) {
    dw_strlist *l = this;
    if (!out || i >= l->n) { if (out && cap) out[0] = 0; return E_INVALIDARG; }
    size_t n = strlen(l->e[i].locale);
    if (cap < n + 1) { if (cap) out[0] = 0; return E_NOT_SUFFICIENT_BUFFER; }
    for (size_t k = 0; k <= n; k++) out[k] = (WCHAR)l->e[i].locale[k];
    return S_OK;
}
static WINAPI HRESULT strlist_GetStringLength(void *this, uint32_t i, uint32_t *len) {
    dw_strlist *l = this;
    if (!len) return E_INVALIDARG;
    if (i >= l->n) { *len = 0; return E_INVALIDARG; }
    *len = l->e[i].len;
    return S_OK;
}
static WINAPI HRESULT strlist_GetString(void *this, uint32_t i, WCHAR *out, uint32_t cap) {
    dw_strlist *l = this;
    if (!out || i >= l->n) { if (out && cap) out[0] = 0; return E_INVALIDARG; }
    if (cap < l->e[i].len + 1) { if (cap) out[0] = 0; return E_NOT_SUFFICIENT_BUFFER; }
    memcpy(out, l->e[i].text, 2 * ((size_t)l->e[i].len + 1));
    return S_OK;
}

// ---- IDWriteFontSet3, IDWriteFontSetBuilder2 ------------------------------------------------------------------------------
// A font set is a list of faces of files (what font face references name) and their properties. An entry comes from a
// collection's font (its face and file made when first needed) or from a reference, with the properties a builder gave
// it (those of an ID replace what the font's own tables say). The system font set is the system collection's fonts.
// Properties of an entry: family names as its collection names its family (else the name table's typographic or
// legacy family names, English as CoreText names the family), face names, the name table's full, Win32 family and
// PostScript names, weight, stretch and style as decimal numbers (no locale); script language tags and semantic tags
// are not read (the 'meta' table): an entry has none.
typedef struct { uint32_t id; WCHAR *value; char locale[24]; } set_prop;
typedef struct { dw_file *file; uint32_t index; int sims; dw_font *font; dw_face *face; set_prop *props; uint32_t nprops; } set_entry;
typedef struct { dw_obj o; set_entry *e; uint32_t n; int system; } dw_set;
typedef struct { dw_obj o; set_entry *e; uint32_t n, cap; } dw_builder;
static const GUID *const g_set_iids[] = { &IID_IDWriteFontSet, &IID_IDWriteFontSet1, &IID_IDWriteFontSet2, &IID_IDWriteFontSet3, NULL };
static const GUID *const g_builder_iids[] = { &IID_IDWriteFontSetBuilder, &IID_IDWriteFontSetBuilder1, &IID_IDWriteFontSetBuilder2, NULL };
static void entry_copy(set_entry *d, const set_entry *s) {      // (our objects only: fine under the lock)
    *d = *s;
    if (d->file) dw_addref(d->file);
    if (d->font) dw_addref(d->font);
    if (d->face) dw_addref(d->face);
    d->props = s->nprops ? malloc(s->nprops * sizeof *d->props) : NULL;
    for (uint32_t i = 0; i < s->nprops; i++) { d->props[i] = s->props[i]; d->props[i].value = malloc(2 * (vx_wcslen(s->props[i].value) + 1)); memcpy(d->props[i].value, s->props[i].value, 2 * (vx_wcslen(s->props[i].value) + 1)); }
}
// (never under the lock: the last reference to a program's file releases its loader)
static void entry_drop(set_entry *e) {
    if (e->file) dw_release(e->file);
    if (e->font) dw_release(e->font);
    if (e->face) dw_release(e->face);
    for (uint32_t i = 0; i < e->nprops; i++) free(e->props[i].value);
    free(e->props);
}
static void set_destroy(dw_obj *o) { dw_set *s = (dw_set *)o; for (uint32_t i = 0; i < s->n; i++) entry_drop(&s->e[i]); free(s->e); free(s); }
static dw_set *set_new(set_entry *e, uint32_t n, int system) {   // (takes the entries)
    dw_set *s = dw_new(sizeof *s, g_set_vtbl, g_set_iids, "IDWriteFontSet", system ? NULL : set_destroy);
    s->e = e; s->n = n; s->system = system;
    return s;
}
static HRESULT set_of_fonts(dw_font *const *fonts, uint32_t n, void **out) {
    set_entry *e = calloc(n ? n : 1, sizeof *e);
    for (uint32_t i = 0; i < n; i++) { e[i].font = fonts[i]; dw_addref(fonts[i]); e[i].sims = fonts[i]->sims; }
    *out = set_new(e, n, 0);
    return S_OK;
}
static dw_set *g_system_set;
static void system_set_build(void) {
    dw_collection *c = system_collection();
    lock();                                   // (family_sort may be reordering a family's fonts)
    uint32_t n = 0;
    for (uint32_t i = 0; i < c->visible; i++) n += c->families[i]->count;
    set_entry *e = calloc(n ? n : 1, sizeof *e);
    n = 0;
    for (uint32_t i = 0; i < c->visible; i++)
        for (uint32_t j = 0; j < c->families[i]->count; j++) { e[n].font = c->families[i]->fonts[j]; dw_addref(e[n].font); n++; }
    unlock();
    g_system_set = set_new(e, n, 1);
    TR("the system font set: %u fonts", n);
}
static void *system_font_set(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, system_set_build);
    return g_system_set;
}
// The expiration event of the system font set and collection: fonts installed later are not seen, so it is never set.
static HANDLE g_expiration;
static void expiration_build(void) {
    void *create = vx_find_export("kernel32.dll", "CreateEventW");
    if (create) g_expiration = ((HANDLE (WINAPI *)(void *, int32_t, int32_t, const WCHAR *))create)(NULL, 1, 0, NULL);
}
static HANDLE expiration_event(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, expiration_build); return g_expiration; }

// An entry's face and file (NULL when they cannot be had), made outside the lock and kept by the first to finish.
static dw_face *entry_face(set_entry *e) {
    dw_face *f = READY(e->face);
    if (f) return f;
    dw_face *made = NULL;
    if (e->font) { if ((made = font_face(e->font))) dw_addref(made); }
    else if (e->file) face_for_file(e->file, e->index, e->sims, &made);
    if (!made) return NULL;
    dw_face *none = NULL;
    if (!__atomic_compare_exchange_n(&e->face, &none, made, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { dw_release(made); made = none; }
    return made;
}
static dw_file *entry_file(set_entry *e) {
    dw_file *file = READY(e->file);
    if (file) return file;
    dw_face *f = entry_face(e);
    if (!f || !(file = face_file(f))) return NULL;
    lock();
    if (!e->file) { dw_addref(file); e->index = f->index; PUBLISH(e->file, file); }
    file = e->file;
    unlock();
    return file;
}
static void entry_wss(set_entry *e, int *w, int *s, int *st) {
    if (e->font) { font_props(e->font); *w = e->font->weight; *s = e->font->stretch; *st = e->font->style; return; }
    face_wss(entry_face(e), e->sims, w, s, st);
}
static int entry_has_prop(const set_entry *e, uint32_t id) { for (uint32_t i = 0; i < e->nprops; i++) if (e->props[i].id == id) return 1; return 0; }
// The strings of a property of an entry (NULL: it has none).
static dw_strings *entry_property(set_entry *e, uint32_t id) {
    if (entry_has_prop(e, id)) {
        dw_strings *s = strings_new();
        for (uint32_t i = 0; i < e->nprops; i++)
            if (e->props[i].id == id) s->n = add_name(s->e, s->n, 16, e->props[i].locale, e->props[i].value, (uint32_t)vx_wcslen(e->props[i].value));
        if (s->n) return s;
        dw_release(s);
        return NULL;
    }
    dw_face *f = entry_face(e);
    void *out = NULL; int32_t exists = 0;
    switch (id) {
    case PROP_WSS_FAMILY: case PROP_TYPO_FAMILY:
        if (e->font) fam_GetFamilyNames(e->font->family, &out); else if (f) face_GetFamilyNames(f, &out);
        break;
    case PROP_WSS_FACE: case PROP_TYPO_FACE:
        if (e->font) font_GetFaceNames(e->font, &out); else if (f) face_GetFaceNames(f, &out);
        break;
    case PROP_FULL_NAME: info_strings(f, 16, &out, &exists); break;       // (DWRITE_INFORMATIONAL_STRING_ID)
    case PROP_WIN32_FAMILY: info_strings(f, 11, &out, &exists); break;
    case PROP_POSTSCRIPT:
        info_strings(f, 17, &out, &exists);
        if (!out && f && f->ps[0]) { CFStringRef ps = CFStringCreateWithCString(NULL, f->ps, kCFStringEncodingUTF8); dw_strings *s = strings_new(); strings_set_english(s, ps); if (ps) CFRelease(ps); out = s; }
        break;
    case PROP_WEIGHT: case PROP_STRETCH: case PROP_STYLE: {
        int w, s, st;
        entry_wss(e, &w, &s, &st);
        WCHAR num[8]; char a[8];
        int len = snprintf(a, sizeof a, "%d", id == PROP_WEIGHT ? w : id == PROP_STRETCH ? s : st);
        for (int i = 0; i <= len; i++) num[i] = (WCHAR)a[i];
        dw_strings *str = strings_new();
        str->n = add_name(str->e, 0, 16, "", num, (uint32_t)len);
        out = str;
        break;
    }
    default: break;                                        // (script language and semantic tags: none)
    }
    dw_strings *s = out;
    if (s && !s->n) { dw_release(s); s = NULL; }
    return s;
}
// Whether an entry has a property's value (in its locale, or any when it gives none), case aside.
static int entry_matches(set_entry *e, const font_property *p) {
    const WCHAR *value = VX_LOW(p->value), *locale = VX_LOW(p->locale);
    if (!value) return 0;
    int any_locale = !locale || !locale[0];
    if ((p->id == PROP_WSS_FAMILY || p->id == PROP_TYPO_FAMILY) && e->font && !entry_has_prop(e, p->id) && any_locale) {
        uint32_t idx;                              // (a collection's font: its family's names, without reading every face)
        dw_collection *c = e->font->family->coll;
        return find_family(c, value, &idx) && c->families[idx] == e->font->family;
    }
    dw_strings *s = entry_property(e, p->id);
    int hit = 0;
    for (int i = 0; s && i < s->n && !hit; i++) hit = (any_locale || wcsieq_ascii(locale, s->e[i].locale)) && wcs_ieq(value, s->e[i].text);
    if (s) dw_release(s);
    return hit;
}
static int entry_matches_props(set_entry *e, const font_property *p, uint32_t n, int any) {
    if (!n) return !any;
    for (uint32_t i = 0; i < n; i++) {
        int m = entry_matches(e, &p[i]);
        if (any && m) return 1;
        if (!any && !m) return 0;
    }
    return !any;
}
static int props_valid(const font_property *p, uint32_t n) {
    if (n && !p) return 0;
    for (uint32_t i = 0; i < n; i++) if (p[i].id == PROP_NONE || p[i].id >= PROP_COUNT || !p[i].value) return 0;
    return 1;
}
// A new set of some of a set's entries (indices into it), in that order.
static dw_set *set_subset(dw_set *s, const uint32_t *idx, uint32_t n) {
    set_entry *e = calloc(n ? n : 1, sizeof *e);
    lock();
    for (uint32_t i = 0; i < n; i++) entry_copy(&e[i], &s->e[idx[i]]);
    unlock();
    return set_new(e, n, 0);
}
static uint32_t *set_filter(dw_set *s, const font_property *p, uint32_t np, int any, uint32_t *count) {
    uint32_t *idx = malloc((s->n ? s->n : 1) * sizeof *idx), n = 0;
    for (uint32_t i = 0; i < s->n; i++) if (entry_matches_props(&s->e[i], p, np, any)) idx[n++] = i;
    *count = n;
    return idx;
}
// Entries ordered by how well they match a weight, stretch and style (the order they had among equals).
static long *g_set_ranks;                                    // (under the lock)
static int compare_ranked(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return g_set_ranks[x] < g_set_ranks[y] ? -1 : g_set_ranks[x] > g_set_ranks[y] ? 1 : x < y ? -1 : x > y;
}
static void set_rank(dw_set *s, uint32_t *idx, uint32_t n, int w, int st, int sty) {
    long *ranks = calloc(s->n ? s->n : 1, sizeof *ranks);
    for (uint32_t i = 0; i < n; i++) { int fw, fs, fst; entry_wss(&s->e[idx[i]], &fw, &fs, &fst); ranks[idx[i]] = wss_rank(w, st, sty, fw, fs, fst); }
    lock();
    g_set_ranks = ranks;
    qsort(idx, n, sizeof *idx, compare_ranked);
    unlock();
    free(ranks);
}

static WINAPI uint32_t set_GetFontCount(void *this) { return ((dw_set *)this)->n; }
static WINAPI HRESULT set_GetFontFaceReference(void *this, uint32_t i, void **out) {
    dw_set *s = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (i >= s->n) return E_INVALIDARG;
    dw_file *file = entry_file(&s->e[i]);
    if (!file) return DWRITE_E_FILEFORMAT;
    *out = ref_new(file, s->e[i].index, s->e[i].sims, entry_face(&s->e[i]));
    return S_OK;
}
static WINAPI HRESULT set_FindFontFaceReference(void *this, void *ref, uint32_t *index, int32_t *exists) {
    dw_set *s = this;
    if (!index || !exists) return E_INVALIDARG;
    *index = UINT32_MAX; *exists = 0;
    if (!is_ours(ref, g_ref_vtbl)) return E_INVALIDARG;
    dw_ref *r = ref;
    for (uint32_t i = 0; i < s->n && !*exists; i++) {
        set_entry *e = &s->e[i];
        if (e->sims != r->sims) continue;
        if (e->font && r->face && r->face->ps[0] && e->font->ps[0] && strcmp(e->font->ps, r->face->ps)) continue;   // (not that face)
        dw_file *file = entry_file(e);
        if (file && file_same(file, r->file) && e->index == r->index) { *index = i; *exists = 1; }
    }
    return S_OK;
}
static WINAPI HRESULT set_FindFontFace(void *this, void *face, uint32_t *index, int32_t *exists) {
    dw_set *s = this;
    if (!index || !exists) return E_INVALIDARG;
    *index = UINT32_MAX; *exists = 0;
    if (!is_ours(face, g_face_vtbl)) return E_INVALIDARG;
    dw_face *f = face;
    for (uint32_t i = 0; i < s->n && !*exists; i++) {
        set_entry *e = &s->e[i];
        if (e->sims != f->sims) continue;
        if (e->font && f->ps[0] && e->font->ps[0] && strcmp(e->font->ps, f->ps)) continue;
        dw_face *ef = entry_face(e);
        if (ef && faces_equal(ef, f)) { *index = i; *exists = 1; }
    }
    return S_OK;
}
// The values of a property in the set, each once: of each font the string in the first of the preferred locales
// (a list separated by semicolons) it has, else its English one, else its first.
static HRESULT set_values(dw_set *s, uint32_t id, const WCHAR *locales, void **out) {
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (id == PROP_NONE || id >= PROP_COUNT) return E_INVALIDARG;
    dw_strlist *l = dw_new(sizeof *l, g_strlist_vtbl, g_strlist_iids, "IDWriteStringList", strlist_destroy);
    for (uint32_t i = 0; i < s->n; i++) {
        dw_strings *v = entry_property(&s->e[i], id);
        if (!v) continue;
        int pick = -1;
        for (const WCHAR *p = locales; p && *p && pick < 0;) {
            WCHAR one[86]; int k = 0;
            while (*p && *p != ';') { if (k < 85) one[k++] = *p; p++; }
            one[k] = 0;
            if (*p == ';') p++;
            for (int j = 0; j < v->n && pick < 0; j++) if (k && wcsieq_ascii(one, v->e[j].locale)) pick = j;
        }
        for (int j = 0; j < v->n && pick < 0; j++) if (!strcmp(v->e[j].locale, "en-us")) pick = j;
        if (pick < 0) pick = 0;
        strlist_add(l, v->e[pick].locale, v->e[pick].text, v->e[pick].len);
        dw_release(v);
    }
    *out = l;
    return S_OK;
}
static WINAPI HRESULT set_GetPropertyValues__(void *this, uint32_t id, void **out) { return set_values(this, id, NULL, out); }
static WINAPI HRESULT set_GetPropertyValues_(void *this, uint32_t id, const WCHAR *locales, void **out) { return set_values(this, id, locales, out); }
static WINAPI HRESULT set_GetPropertyValues(void *this, uint32_t i, uint32_t id, int32_t *exists, void **out) {
    dw_set *s = this;
    if (!exists || !out) return E_INVALIDARG;
    *exists = 0; *out = NULL;
    if (i >= s->n || id == PROP_NONE || id >= PROP_COUNT) return E_INVALIDARG;
    dw_strings *v = entry_property(&s->e[i], id);
    *exists = v != NULL; *out = v;
    return S_OK;
}
static WINAPI HRESULT set_GetPropertyOccurrenceCount(void *this, const font_property *p, uint32_t *count) {
    dw_set *s = this;
    if (!count) return E_INVALIDARG;
    *count = 0;
    if (!props_valid(p, 1)) return E_INVALIDARG;
    for (uint32_t i = 0; i < s->n; i++) *count += (uint32_t)entry_matches(&s->e[i], p);
    return S_OK;
}
// The fonts of a family, nearest the weight, stretch and style first.
static HRESULT set_matching(dw_set *s, const font_property *family, int w, int st, int sty, void **out) {
    uint32_t n;
    uint32_t *idx = set_filter(s, family, family ? 1 : 0, 0, &n);
    set_rank(s, idx, n, w, st, sty);
    *out = set_subset(s, idx, n);
    free(idx);
    return S_OK;
}
static HRESULT set_by_logfont(void *set, const logfont_w *lf, void **out) {
    dw_set *s = set;
    WCHAR name[33];
    memcpy(name, lf->face, sizeof lf->face);
    name[32] = 0;
    font_property p[2] = { { PROP_WSS_FAMILY, name, NULL }, { PROP_WIN32_FAMILY, name, NULL } };
    uint32_t n, *idx = set_filter(s, p, 2, 1, &n);
    set_rank(s, idx, n, lf->weight ? (int)lf->weight : 400, 5, lf->italic ? STYLE_ITALIC : STYLE_NORMAL);
    *out = set_subset(s, idx, n);
    free(idx);
    return S_OK;
}
static WINAPI HRESULT set_GetMatchingFonts_(void *this, const WCHAR *family, uint32_t w, uint32_t st, uint32_t sty, void **out) {
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!family || w < 1 || w > 999 || st < 1 || st > 9 || sty > 2) return E_INVALIDARG;
    font_property p = { PROP_WSS_FAMILY, family, NULL };
    return set_matching(this, &p, (int)w, (int)st, (int)sty, out);
}
static WINAPI HRESULT set_GetMatchingFonts(void *this, const font_property *p, uint32_t np, void **out) {
    dw_set *s = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!props_valid(p, np)) return E_INVALIDARG;
    uint32_t n, *idx = set_filter(s, p, np, 0, &n);
    *out = set_subset(s, idx, n);
    free(idx);
    return S_OK;
}
static WINAPI HRESULT set_GetMatchingFonts1(void *this, const font_property *p, const axis_value *v, uint32_t nv, void **out) {
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if ((p && !props_valid(p, 1)) || (nv && !v)) return E_INVALIDARG;
    int w, st, sty;
    wss_from_axes(v, nv, &w, &st, &sty);
    return set_matching(this, p, w, st, sty, out);
}
static WINAPI HRESULT set_GetFirstFontResources(void *this, void **out) {         // one entry per face of a file
    dw_set *s = this;
    if (!out) return E_INVALIDARG;
    uint32_t *idx = malloc((s->n ? s->n : 1) * sizeof *idx), n = 0;
    for (uint32_t i = 0; i < s->n; i++) {
        dw_file *fi = entry_file(&s->e[i]);
        int seen = 0;
        for (uint32_t j = 0; j < n && !seen && fi; j++) seen = s->e[idx[j]].index == s->e[i].index && file_same(READY(s->e[idx[j]].file), fi);
        if (!seen) idx[n++] = i;
    }
    *out = set_subset(s, idx, n);
    free(idx);
    return S_OK;
}
static WINAPI HRESULT set_GetFilteredFonts__(void *this, const uint32_t *indices, uint32_t n, void **out) {
    dw_set *s = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (n && !indices) return E_INVALIDARG;
    for (uint32_t i = 0; i < n; i++) if (indices[i] >= s->n) return E_INVALIDARG;
    *out = set_subset(s, indices, n);
    return S_OK;
}
// Axis ranges: no font has an axis, so no range is met (all of no ranges are).
static uint32_t ranges_filter(dw_set *s, uint32_t nr, int any, uint32_t *idx) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->n; i++) if (!nr && !any) { if (idx) idx[n] = i; n++; }
    return n;
}
static WINAPI HRESULT set_GetFilteredFonts_(void *this, const axis_range *r, uint32_t nr, int32_t any, void **out) {
    dw_set *s = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (nr && !r) return E_INVALIDARG;
    uint32_t *idx = malloc((s->n ? s->n : 1) * sizeof *idx), n = ranges_filter(s, nr, any, idx);
    *out = set_subset(s, idx, n);
    free(idx);
    return S_OK;
}
static WINAPI HRESULT set_GetFilteredFonts(void *this, const font_property *p, uint32_t np, int32_t any, void **out) {
    dw_set *s = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!props_valid(p, np)) return E_INVALIDARG;
    uint32_t n, *idx = set_filter(s, p, np, any, &n);
    *out = set_subset(s, idx, n);
    free(idx);
    return S_OK;
}
static HRESULT indices_out(const uint32_t *idx, uint32_t n, uint32_t *out, uint32_t cap, uint32_t *actual) {
    *actual = n;
    if (n > cap || (n && !out)) return E_NOT_SUFFICIENT_BUFFER;
    if (n) memcpy(out, idx, n * sizeof *idx);
    return S_OK;
}
static WINAPI HRESULT set_GetFilteredFontIndices_(void *this, const axis_range *r, uint32_t nr, int32_t any, uint32_t *out, uint32_t cap, uint32_t *actual) {
    dw_set *s = this;
    if (!actual) return E_INVALIDARG;
    *actual = 0;
    if (nr && !r) return E_INVALIDARG;
    uint32_t *idx = malloc((s->n ? s->n : 1) * sizeof *idx), n = ranges_filter(s, nr, any, idx);
    HRESULT hr = indices_out(idx, n, out, cap, actual);
    free(idx);
    return hr;
}
static WINAPI HRESULT set_GetFilteredFontIndices(void *this, const font_property *p, uint32_t np, int32_t any, uint32_t *out, uint32_t cap, uint32_t *actual) {
    dw_set *s = this;
    if (!actual) return E_INVALIDARG;
    *actual = 0;
    if (!props_valid(p, np)) return E_INVALIDARG;
    uint32_t n, *idx = set_filter(s, p, np, any, &n);
    HRESULT hr = indices_out(idx, n, out, cap, actual);
    free(idx);
    return hr;
}
static WINAPI HRESULT set_GetFontAxisRanges_(void *this, uint32_t i, axis_range *r, uint32_t cap, uint32_t *actual) {
    (void)r; (void)cap;
    if (!actual) return E_INVALIDARG;
    *actual = 0;
    return i < ((dw_set *)this)->n ? S_OK : E_INVALIDARG;
}
static WINAPI HRESULT set_GetFontAxisRanges(void *this, axis_range *r, uint32_t cap, uint32_t *actual) { (void)this; (void)r; (void)cap; if (!actual) return E_INVALIDARG; *actual = 0; return S_OK; }
static WINAPI HRESULT set_CreateFontResource(void *this, uint32_t i, void **out) {
    dw_set *s = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (i >= s->n) return E_INVALIDARG;
    dw_file *file = entry_file(&s->e[i]);
    return file ? resource_new(file, s->e[i].index, out) : DWRITE_E_FILEFORMAT;
}
static WINAPI HRESULT set_CreateFontFace(void *this, uint32_t i, void **out) {
    dw_set *s = this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (i >= s->n) return E_INVALIDARG;
    dw_face *f = entry_face(&s->e[i]);
    if (!f) return DWRITE_E_FILEFORMAT;
    dw_addref(f);
    *out = f;
    return S_OK;
}
static WINAPI uint32_t set_GetFontLocality(void *this, uint32_t i) { (void)this; (void)i; return LOCALITY_LOCAL; }
static WINAPI HANDLE set_GetExpirationEvent(void *this) { return ((dw_set *)this)->system ? expiration_event() : NULL; }
// Where a font comes from: the Mac's fonts are the machine's, those in the user's Library/Fonts the user's.
static WINAPI uint32_t set_GetFontSourceType(void *this, uint32_t i) {
    dw_set *s = this;
    if (i >= s->n) return SOURCE_UNKNOWN;
    set_entry *e = &s->e[i];
    char path[1024] = "";
    dw_font *real = e->font ? (e->font->base ? e->font->base : e->font) : NULL;
    if (real && real->desc) {
        CFURLRef url = CTFontDescriptorCopyAttribute(real->desc, kCTFontURLAttribute);
        if (!url || !CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, sizeof path)) path[0] = 0;
        if (url) CFRelease(url);
    } else if (!e->font && e->file && e->file->kind == F_LOCAL) snprintf(path, sizeof path, "%s", e->file->unix_path);
    if (!path[0]) return SOURCE_UNKNOWN;
    const char *home = getenv("HOME");
    char user[1024];
    snprintf(user, sizeof user, "%s/Library/Fonts/", home ? home : "/nonexistent");
    if (!strncmp(path, user, strlen(user))) return SOURCE_PER_USER;
    return !strncmp(path, "/System/", 8) || !strncmp(path, "/Library/", 9) ? SOURCE_PER_MACHINE : SOURCE_UNKNOWN;
}
static WINAPI uint32_t set_GetFontSourceNameLength(void *this, uint32_t i) { (void)this; (void)i; return 0; }    // (no source has a name)
static WINAPI HRESULT set_GetFontSourceName(void *this, uint32_t i, WCHAR *out, uint32_t cap) {
    if (!out || !cap) return E_NOT_SUFFICIENT_BUFFER;
    out[0] = 0;
    return i < ((dw_set *)this)->n ? S_OK : E_INVALIDARG;
}

// IDWriteFontSetBuilder2
static void builder_destroy(dw_obj *o) { dw_builder *b = (dw_builder *)o; for (uint32_t i = 0; i < b->n; i++) entry_drop(&b->e[i]); free(b->e); free(b); }
static void builder_add(dw_builder *b, const set_entry *e) {     // (a copy of e)
    lock();
    if (b->n == b->cap) { b->cap = b->cap ? 2 * b->cap : 16; b->e = realloc(b->e, b->cap * sizeof *b->e); }
    entry_copy(&b->e[b->n++], e);
    unlock();
}
static HRESULT builder_add_props(dw_builder *b, dw_file *file, uint32_t index, int sims, dw_face *face, const font_property *p, uint32_t np) {
    if (!props_valid(p, np)) return E_INVALIDARG;
    set_prop *props = np ? calloc(np, sizeof *props) : NULL;
    for (uint32_t i = 0; i < np; i++) {
        const WCHAR *v = VX_LOW(p[i].value), *l = VX_LOW(p[i].locale);
        size_t n = vx_wcslen(v);
        props[i].id = p[i].id;
        props[i].value = malloc(2 * (n + 1)); memcpy(props[i].value, v, 2 * (n + 1));
        size_t k = 0;
        for (; l && l[k] && k < sizeof props[i].locale - 1; k++) props[i].locale[k] = (char)tolower(l[k] < 128 ? l[k] : '?');
        props[i].locale[k] = 0;
    }
    set_entry e = { file, index, sims, NULL, face && face->sims == sims ? face : NULL, props, np };
    builder_add(b, &e);
    for (uint32_t i = 0; i < np; i++) free(props[i].value);
    free(props);
    return S_OK;
}
static WINAPI HRESULT builder_AddFontFaceReference_(void *this, void *ref, const font_property *p, uint32_t np) {
    if (!is_ours(ref, g_ref_vtbl)) return E_INVALIDARG;
    dw_ref *r = ref;
    return builder_add_props(this, r->file, r->index, r->sims, r->face, p, np);
}
static WINAPI HRESULT builder_AddFontFaceReference(void *this, void *ref) { return builder_AddFontFaceReference_(this, ref, NULL, 0); }
static WINAPI HRESULT builder_AddFontSet(void *this, void *set) {
    if (!is_ours(set, g_set_vtbl)) return E_INVALIDARG;
    dw_set *s = set;
    for (uint32_t i = 0; i < s->n; i++) builder_add(this, &s->e[i]);
    return S_OK;
}
static WINAPI HRESULT builder_CreateFontSet(void *this, void **out) {
    dw_builder *b = this;
    if (!out) return E_INVALIDARG;
    lock();
    set_entry *e = calloc(b->n ? b->n : 1, sizeof *e);
    for (uint32_t i = 0; i < b->n; i++) entry_copy(&e[i], &b->e[i]);
    uint32_t n = b->n;
    unlock();
    *out = set_new(e, n, 0);
    TR("IDWriteFontSetBuilder::CreateFontSet -> %u fonts", n);
    return S_OK;
}
// Every face of a file (read now: a program's file through its loader, without the lock).
static WINAPI HRESULT builder_AddFontFile(void *this, void *file) {
    if (!is_ours(file, g_file_vtbl)) return E_INVALIDARG;
    const uint8_t *d; size_t n; int type;
    uint32_t faces = file_bytes(file, &d, &n) ? sfnt_faces(d, n, &type) : 0;
    TR("IDWriteFontSetBuilder::AddFontFile(%u bytes) -> %u faces", (unsigned)n, faces);
    if (!faces) return DWRITE_E_FILEFORMAT;
    for (uint32_t i = 0; i < faces; i++) builder_add_props(this, file, i, 0, NULL, NULL, 0);
    return S_OK;
}
static WINAPI HRESULT builder_AddFont(void *this, void *file, uint32_t index, uint32_t sims, const axis_value *v, uint32_t nv, const axis_range *r, uint32_t nr,
                                      const font_property *p, uint32_t np) {
    (void)v; (void)r;
    if (!is_ours(file, g_file_vtbl) || sims > 3 || (nv && !v) || (nr && !r)) return E_INVALIDARG;
    const uint8_t *d; size_t n; int type;
    if (!file_bytes(file, &d, &n)) return ((dw_file *)file)->kind == F_LOCAL ? DWRITE_E_FILENOTFOUND : DWRITE_E_FILEFORMAT;
    if (index >= sfnt_faces(d, n, &type)) return DWRITE_E_FILEFORMAT;
    return builder_add_props(this, file, index, (int)sims, NULL, p, np);
}
static WINAPI HRESULT builder_AddFontFilePath(void *this, const WCHAR *path) {
    if (!path) return E_INVALIDARG;
    char *win = vx_utf8_dup(path), unix_path[1024];
    int ok = vx_unix_path(win, unix_path, sizeof unix_path);
    free(win);
    dw_file *f = ok ? local_file(unix_path) : NULL;
    if (!f) return DWRITE_E_FILENOTFOUND;
    HRESULT hr = builder_AddFontFile(this, f);
    dw_release(f);
    return hr;
}

// ---- IDWriteFontDownloadQueue: every font is local, so it is always empty -------------------------------------------------
typedef struct { dw_obj o; void *listener[32]; uint32_t token[32]; int n; uint32_t next; } dw_queue;
static const GUID *const g_queue_iids[] = { &IID_IDWriteFontDownloadQueue, NULL };
static dw_queue g_queue;
static WINAPI HRESULT queue_AddListener(void *this, void *listener, uint32_t *token) {
    dw_queue *q = this;
    if (!listener || !token) return E_INVALIDARG;
    com_addref(listener);                                  // (the program's: before the lock)
    lock();
    HRESULT hr = q->n < 32 ? S_OK : E_OUTOFMEMORY;
    if (hr == S_OK) { q->listener[q->n] = listener; *token = q->token[q->n] = ++q->next; q->n++; }
    unlock();
    if (hr != S_OK) com_release(listener);
    return hr;
}
static WINAPI HRESULT queue_RemoveListener(void *this, uint32_t token) {
    dw_queue *q = this;
    void *gone = NULL;
    lock();
    for (int i = 0; i < q->n && !gone; i++)
        if (q->token[i] == token) { gone = q->listener[i]; q->listener[i] = q->listener[q->n - 1]; q->token[i] = q->token[q->n - 1]; q->n--; }
    unlock();
    if (!gone) return E_INVALIDARG;
    com_release(gone);
    return S_OK;
}
static WINAPI int32_t queue_IsEmpty(void *this) { (void)this; return 1; }
static WINAPI HRESULT queue_BeginDownload(void *this, void *ctx) { (void)this; (void)ctx; return S_FALSE; }       // (nothing queued)
static WINAPI HRESULT queue_CancelDownload(void *this) { (void)this; return S_OK; }
static WINAPI uint64_t queue_GetGenerationCount(void *this) { (void)this; return 0; }

// ---- IDWriteFactory7 ----------------------------------------------------------------------------------------------------------
typedef struct { dw_obj o; void *file_loaders[64]; int nfile; void *coll_loaders[64]; int ncoll; } dw_factory;
static const GUID *const g_factory_iids[] = { &IID_IDWriteFactory, &IID_IDWriteFactory1, &IID_IDWriteFactory2, &IID_IDWriteFactory3,
                                              &IID_IDWriteFactory4, &IID_IDWriteFactory5, &IID_IDWriteFactory6, &IID_IDWriteFactory7, NULL };
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
    com_addref(l);                       // (the program's AddRef before the lock, given back when it is not registered)
    lock();
    HRESULT hr = loader_index(list, *n, l) >= 0 ? DWRITE_E_ALREADYREGISTERED : *n >= 64 ? E_OUTOFMEMORY : S_OK;
    if (hr == S_OK) list[(*n)++] = l;
    unlock();
    if (hr != S_OK) com_release(l);
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
// A font added to a custom collection, in the family `family` (NULL: as CoreText names the face's): a face it takes,
// or a system font's descriptor (its face made when asked for). 0: not added (the caller releases the face).
static int collection_add_font(dw_collection *c, dw_face *f, CTFontDescriptorRef desc, CFStringRef family, int sims) {
    CFStringRef name = family ? CFRetain(family) : f ? CTFontCopyFamilyName(f->ct) : NULL;
    if (!name) name = CFStringCreateWithCString(NULL, f && f->ps[0] ? f->ps : "Unknown", kCFStringEncodingUTF8);
    dw_family *fam = family_named(c, name, c->count);
    if (!fam && c->count == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 8;
        c->families = realloc(c->families, c->cap * sizeof *c->families);
    }
    if (!fam) fam = family_new(c, name, 1);
    CFRelease(name);
    if (!fam) return 0;
    dw_font *ft = font_new(fam, f ? NULL : desc, f);
    ft->sims = sims;
    return 1;
}
static int collection_add_face(dw_collection *c, dw_face *f) { return collection_add_font(c, f, NULL, NULL, f->sims); }
// A collection of a font set's fonts, by family: the set's family name property for the model when the set has one
// for an entry (its English value, else its first), else a system font's family, else CoreText's name for the face.
static HRESULT collection_from_set(dw_set *s, int model, void **out) {
    *out = NULL;
    if (s->system) {                           // (the system collection: one grouping for both models)
        dw_collection *c = system_collection();
        dw_addref(c);
        *out = c;
        return S_OK;
    }
    dw_collection *c = collection_new(0, 8);
    c->model = model;
    for (uint32_t i = 0; i < s->n; i++) {
        set_entry *e = &s->e[i];
        uint32_t id = model == FAMILY_MODEL_TYPOGRAPHIC ? PROP_TYPO_FAMILY : PROP_WSS_FAMILY;
        CFStringRef name = NULL;
        if (entry_has_prop(e, id)) {
            dw_strings *v = entry_property(e, id);
            int pick = 0;
            for (int j = 0; v && j < v->n; j++) if (!strcmp(v->e[j].locale, "en-us")) { pick = j; break; }
            if (v) name = CFStringCreateWithCharacters(NULL, v->e[pick].text, (CFIndex)v->e[pick].len);
            if (v) dw_release(v);
        } else if (e->font) name = CFRetain(e->font->family->name);
        dw_font *real = e->font ? (e->font->base ? e->font->base : e->font) : NULL;
        if (real && real->desc) {
            lock();
            collection_add_font(c, NULL, real->desc, name, e->sims);
            unlock();
        } else {
            dw_file *file = entry_file(e);           // (a program's file is read without the lock)
            dw_face *f = NULL;
            if (file && face_from_file(file, e->index, e->sims, &f) >= 0) {
                lock();
                int added = collection_add_font(c, f, NULL, name, e->sims);
                unlock();
                if (!added) dw_release(f);
            }
        }
        if (name) CFRelease(name);
    }
    if (g_trace) {
        char first[128] = "";
        if (c->count) cf_utf8(c->families[0]->name, first, sizeof first);
        TR("CreateFontCollectionFromFontSet(%u fonts) -> %u families (%s%s)", s->n, c->count, first, c->count > 1 ? ", ..." : "");
    }
    *out = c;
    return S_OK;
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
                if (face_from_file(file, i, 0, &f) < 0) continue;
                lock();
                int added = collection_add_face(c, f);
                unlock();
                if (!added) dw_release(f);
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
    if (is_ours(loader, g_inmem_vtbl)) {                  // our in-memory loader: its key is the file's number
        uint32_t index;
        if (key_size != 4) return E_INVALIDARG;
        memcpy(&index, key, 4);
        dw_file *f = inmem_file(loader, index);
        if (!f) return E_INVALIDARG;
        *out = f;
        return S_OK;
    }
    dw_file *f = dw_new(sizeof *f, g_file_vtbl, g_file_iids, "IDWriteFontFile", file_destroy);
    f->kind = F_CUSTOM;
    f->loader = loader; com_addref(loader);
    f->key = malloc(key_size ? key_size : 1); memcpy(f->key, key, key_size); f->key_size = key_size;
    TR("IDWriteFactory::CreateCustomFontFileReference(%u-byte key)", key_size);
    *out = f;
    return S_OK;
}
static WINAPI HRESULT factory_CreateFontFace(void *this, uint32_t type, uint32_t nfiles, void *const *files, uint32_t index, uint32_t sims, void **out) {
    (void)this; (void)type;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!nfiles || !files || !is_ours(files[0], g_file_vtbl) || sims > 3) return E_INVALIDARG;
    dw_file *file = files[0];
    dw_face *f;
    HRESULT hr = face_for_file(file, index, (int)sims, &f);
    TR("IDWriteFactory::CreateFontFace(face %u of a %s file) -> %s (%08x)", index, file->kind == F_LOCAL ? "local" : file->kind == F_CUSTOM ? "program's" : "memory", hr >= 0 ? f->ps : "-", (unsigned)hr);
    if (hr < 0) return hr;
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
// IDWriteNumberSubstitution has no methods of its own: the object only carries the choice (the layout does not
// substitute digits).
static const GUID *const g_numsub_iids[] = { &IID_IDWriteNumberSubstitution, NULL };
static void numsub_destroy(dw_obj *o) { free(o); }
static WINAPI HRESULT factory_CreateNumberSubstitution(void *this, uint32_t method, const WCHAR *locale, int32_t ignore, void **out) {
    (void)this; (void)locale; (void)ignore;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (method > 4) return E_INVALIDARG;                  // DWRITE_NUMBER_SUBSTITUTION_METHOD_TRADITIONAL
    *out = dw_new(sizeof(dw_obj), g_numsub_vtbl, g_numsub_iids, "IDWriteNumberSubstitution", numsub_destroy);
    return S_OK;
}
static WINAPI HRESULT factory_CreateTypography(void *this, void **out) {
    (void)this;
    if (!out) return E_INVALIDARG;
    *out = dw_new(sizeof(dw_typography), g_typography_vtbl, g_typography_iids, "IDWriteTypography", typography_destroy);
    return S_OK;
}
static WINAPI HRESULT factory_CreateTextAnalyzer(void *this, void **out) { (void)this; if (out) *out = NULL; return dw_not_implemented("IDWriteFactory::CreateTextAnalyzer"); }

// IDWriteFactory3
static WINAPI HRESULT factory_CreateGlyphRunAnalysis3(void *this, const glyph_run *run, const matrix *m, uint32_t mode, uint32_t measuring, uint32_t grid,
                                                      uint32_t aa, float x, float y, void **out) {
    if (mode == MODE_NATURAL_SYMMETRIC_DOWNSAMPLED) mode = MODE_NATURAL_SYMMETRIC;      // (no hinting: the same)
    return factory_CreateGlyphRunAnalysis2(this, run, m, mode, measuring, grid, aa, x, y, out);
}
static WINAPI HRESULT factory_CreateCustomRenderingParams3(void *this, float gamma, float contrast, float gray, float cleartype, uint32_t geometry, uint32_t mode,
                                                           uint32_t grid, void **out) {
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (mode > MODE_NATURAL_SYMMETRIC_DOWNSAMPLED || grid > 2) return E_INVALIDARG;
    HRESULT hr = factory_CreateCustomRenderingParams2(this, gamma, contrast, gray, cleartype, geometry, mode == MODE_NATURAL_SYMMETRIC_DOWNSAMPLED ? MODE_NATURAL_SYMMETRIC : mode, grid, out);
    if (hr >= 0) ((dw_params *)*out)->mode = mode;
    return hr;
}
static WINAPI HRESULT factory_CreateFontFaceReference_(void *this, void *file, uint32_t index, uint32_t sims, void **out) {
    (void)this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!is_ours(file, g_file_vtbl) || sims > 3) return E_INVALIDARG;
    *out = ref_new(file, index, (int)sims, NULL);
    return S_OK;
}
static WINAPI HRESULT factory_CreateFontFaceReference(void *this, const WCHAR *path, const uint64_t *time, uint32_t index, uint32_t sims, void **out) {
    if (!out) return E_INVALIDARG;
    *out = NULL;
    void *file = NULL;
    HRESULT hr = factory_CreateFontFileReference(this, path, time, &file);
    if (hr < 0) return hr;
    hr = factory_CreateFontFaceReference_(this, file, index, sims, out);
    dw_release(file);
    return hr;
}
static WINAPI HRESULT factory_GetSystemFontSet(void *this, void **out) {
    (void)this;
    if (!out) return E_INVALIDARG;
    void *s = system_font_set();
    dw_addref(s);
    *out = s;
    return S_OK;
}
static WINAPI HRESULT factory_CreateFontSetBuilder(void *this, void **out) {
    (void)this;
    if (!out) return E_INVALIDARG;
    *out = dw_new(sizeof(dw_builder), g_builder_vtbl, g_builder_iids, "IDWriteFontSetBuilder", builder_destroy);
    return S_OK;
}
static WINAPI HRESULT factory_CreateFontCollectionFromFontSet(void *this, void *set, void **out) {
    (void)this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    return is_ours(set, g_set_vtbl) ? collection_from_set(set, FAMILY_MODEL_WSS, out) : E_INVALIDARG;
}
static WINAPI HRESULT factory_GetSystemFontCollection3(void *this, int32_t downloadable, void **out, int32_t check) { (void)downloadable; return factory_GetSystemFontCollection(this, out, check); }
static WINAPI HRESULT factory_GetFontDownloadQueue(void *this, void **out) { (void)this; if (!out) return E_INVALIDARG; dw_addref(&g_queue); *out = &g_queue; return S_OK; }
// IDWriteFactory4
static WINAPI HRESULT factory_TranslateColorGlyphRun4(void *this, point_2f origin, const glyph_run *run, const void *desc, uint32_t formats, uint32_t measuring,
                                                      const matrix *m, uint32_t palette, void **layers) {
    (void)this; (void)origin; (void)run; (void)desc; (void)formats; (void)measuring; (void)m; (void)palette;
    if (layers) *layers = NULL;
    return DWRITE_E_NOCOLOR;
}
// Where each glyph of a run is drawn: from the baseline origin by the advances (the face's when the run has none) and
// the offsets; leftwards for a right-to-left run.
static HRESULT glyph_origins(const glyph_run *run, point_2f origin, point_2f *out) {
    if (!run || !out) return E_INVALIDARG;
    glyph_run r = *run;                          // (its pointers as this runtime sees them: vx.h)
    r.face = VX_LOW(r.face); r.glyphs = VX_LOW(r.glyphs); r.advances = VX_LOW(r.advances); r.offsets = VX_LOW(r.offsets);
    if (!is_ours(r.face, g_face_vtbl) || (r.count && !r.glyphs)) return E_INVALIDARG;
    float pen = origin.x;
    for (uint32_t i = 0; i < r.count; i++) {
        float x, y;
        glyph_origin(r.face, &r, r.bidi & 1, i, &pen, &x, &y);
        out[i] = (point_2f){ x, origin.y + y };
    }
    return S_OK;
}
static WINAPI HRESULT factory_ComputeGlyphOrigins_(void *this, const glyph_run *run, point_2f origin, point_2f *out) { (void)this; return glyph_origins(run, origin, out); }
static WINAPI HRESULT factory_ComputeGlyphOrigins(void *this, const glyph_run *run, uint32_t measuring, point_2f origin, const matrix *m, point_2f *out) {
    (void)this; (void)measuring; (void)m;
    return glyph_origins(run, origin, out);
}
// IDWriteFactory5
static WINAPI HRESULT factory_CreateInMemoryFontFileLoader(void *this, void **out) {
    (void)this;
    if (!out) return E_INVALIDARG;
    *out = dw_new(sizeof(dw_inmem), g_inmem_vtbl, g_inmem_iids, "IDWriteInMemoryFontFileLoader", inmem_destroy);
    return S_OK;
}
static WINAPI HRESULT factory_CreateHttpFontFileLoader(void *this, const WCHAR *referrer, const WCHAR *headers, void **out) {
    (void)this; (void)referrer; (void)headers;
    if (out) *out = NULL;
    return dw_not_implemented("IDWriteFactory5::CreateHttpFontFileLoader (remote fonts)");
}
static WINAPI uint32_t factory_AnalyzeContainerType(void *this, const void *data, uint32_t size) {
    (void)this;
    if (!data || size < 4) return CONTAINER_UNKNOWN;
    return !memcmp(data, "wOFF", 4) ? CONTAINER_WOFF : !memcmp(data, "wOF2", 4) ? CONTAINER_WOFF2 : CONTAINER_UNKNOWN;
}
// WOFF 1.0 (W3C): a 44-byte header (signature 'wOFF', the sfnt version, the WOFF file's length, u16 table count,
// u16 reserved (zero), totalSfntSize (the sfnt it unpacks to: header, directory and 4-byte padded tables), version,
// metadata and private blocks: offset and lengths) then 20-byte table entries (tag, offset, compressed length, original
// length, checksum); a table whose compressed length is less than its original length is zlib-compressed.
// Everything is checked before anything is inflated or allocated for the font: the length is the file's, the tables'
// data lie after the directory and do not overlap, no tag twice, and totalSfntSize is what the tables add up to and at
// most WOFF_MAX_SFNT, so the font costs at most that much memory however its file is made.
#define WOFF_MAX_SFNT (128u << 20)
typedef struct { uint32_t tag, off, clen, olen; } woff_entry;
static int woff_by_offset(const void *a, const void *b) {
    const woff_entry *x = a, *y = b;
    return x->off != y->off ? (x->off < y->off ? -1 : 1) : x->clen != y->clen ? (x->clen < y->clen ? -1 : 1) : 0;
}
static int woff_by_tag(const void *a, const void *b) {
    const woff_entry *x = a, *y = b;
    return x->tag != y->tag ? (x->tag < y->tag ? -1 : 1) : 0;
}
static HRESULT woff_unpack(const uint8_t *d, size_t n, uint8_t **out, size_t *size) {
    *out = NULL; *size = 0;
    if (n < 44 || memcmp(d, "wOFF", 4)) return DWRITE_E_FILEFORMAT;
    uint32_t flavor = be32(d + 4), length = be32(d + 8), total = be32(d + 16);
    uint16_t count = be16(d + 12);
    uint64_t dir_end = 44 + 20ull * count;
    if (length != n || be16(d + 14) || !count || dir_end > n || total > WOFF_MAX_SFNT) return DWRITE_E_FILEFORMAT;
    if ((uint64_t)be32(d + 24) + be32(d + 28) > n || (uint64_t)be32(d + 36) + be32(d + 40) > n) return DWRITE_E_FILEFORMAT;
    woff_entry *e = malloc(count * sizeof *e);
    if (!e) return E_OUTOFMEMORY;
    uint64_t need = 12 + 16ull * count;
    int ok = 1;
    for (uint16_t i = 0; i < count && ok; i++) {
        const uint8_t *r = d + 44 + 20 * i;
        e[i] = (woff_entry){ be32(r), be32(r + 4), be32(r + 8), be32(r + 12) };
        ok = e[i].off >= dir_end && (uint64_t)e[i].off + e[i].clen <= n && e[i].clen <= e[i].olen;
        need += align4(e[i].olen);
    }
    if (ok && need != total) ok = 0;
    if (ok) {
        qsort(e, count, sizeof *e, woff_by_offset);
        for (uint16_t i = 1; i < count && ok; i++) ok = e[i].off >= (uint64_t)e[i - 1].off + e[i - 1].clen;
    }
    if (ok) {
        qsort(e, count, sizeof *e, woff_by_tag);                // (the sfnt's records in tag order, as OpenType asks)
        for (uint16_t i = 1; i < count && ok; i++) ok = e[i].tag != e[i - 1].tag;
    }
    if (!ok) { free(e); return DWRITE_E_FILEFORMAT; }
    uint8_t *b = calloc(1, total);
    if (!b) { free(e); return E_OUTOFMEMORY; }
    sfnt_header(b, flavor, count);
    uint64_t at = 12 + 16ull * count;
    for (uint16_t i = 0; i < count && ok; i++) {
        if (e[i].clen == e[i].olen) memcpy(b + at, d + e[i].off, e[i].olen);
        else {
            uLongf got = e[i].olen;
            ok = uncompress(b + at, &got, d + e[i].off, e[i].clen) == Z_OK && got == e[i].olen;
        }
        if (ok) sfnt_record(b, i, e[i].tag, (uint32_t)at, e[i].olen);
        at += align4(e[i].olen);
    }
    free(e);
    if (!ok) { free(b); return DWRITE_E_FILEFORMAT; }
    *out = b; *size = total;
    return S_OK;
}
static WINAPI HRESULT factory_UnpackFontFile(void *this, uint32_t type, const void *data, uint32_t size, void **out) {
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!data) return E_INVALIDARG;
    uint32_t actual = factory_AnalyzeContainerType(this, data, size);
    if (type == CONTAINER_WOFF2 && actual == CONTAINER_WOFF2) return dw_not_implemented("IDWriteFactory5::UnpackFontFile (WOFF2)");
    if (type != CONTAINER_WOFF || actual != CONTAINER_WOFF) return DWRITE_E_FILEFORMAT;
    size_t n;
    uint8_t *sfnt;
    HRESULT hr = woff_unpack(data, size, &sfnt, &n);
    if (hr < 0) return hr;
    dw_stream *s = stream_new(sfnt, n, 0, NULL);
    if (!s) { free(sfnt); return E_OUTOFMEMORY; }
    s->owned = sfnt;
    *out = s;
    return S_OK;
}
// IDWriteFactory6, IDWriteFactory7 (axis values change nothing: no font has axes)
static WINAPI HRESULT factory_CreateFontFaceReference6(void *this, void *file, uint32_t index, uint32_t sims, const axis_value *v, uint32_t n, void **out) {
    if (n && !v) { if (out) *out = NULL; return E_INVALIDARG; }
    return factory_CreateFontFaceReference_(this, file, index, sims, out);
}
static WINAPI HRESULT factory_CreateFontResource(void *this, void *file, uint32_t index, void **out) {
    (void)this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    return is_ours(file, g_file_vtbl) ? resource_new(file, index, out) : E_INVALIDARG;
}
static WINAPI HRESULT factory_GetSystemFontSet6(void *this, int32_t downloadable, void **out) { (void)downloadable; return factory_GetSystemFontSet(this, out); }
// The system collection groups CoreText's families for either model (it says WEIGHT_STRETCH_STYLE).
static WINAPI HRESULT factory_GetSystemFontCollection6(void *this, int32_t downloadable, uint32_t model, void **out) {
    (void)downloadable;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (model > FAMILY_MODEL_WSS) return E_INVALIDARG;
    return factory_GetSystemFontCollection(this, out, 0);
}
static WINAPI HRESULT factory_CreateFontCollectionFromFontSet6(void *this, void *set, uint32_t model, void **out) {
    (void)this;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!is_ours(set, g_set_vtbl) || model > FAMILY_MODEL_WSS) return E_INVALIDARG;
    return collection_from_set(set, (int)model, out);
}
static WINAPI HRESULT factory_CreateTextFormat6(void *this, const WCHAR *family, void *coll, const axis_value *v, uint32_t n, float size, const WCHAR *locale, void **out) {
    (void)this; (void)family; (void)coll; (void)v; (void)n; (void)size; (void)locale;
    if (out) *out = NULL;
    return dw_not_implemented("IDWriteFactory6::CreateTextFormat (IDWriteTextFormat3)");
}

// DWriteCreateFactory: the shared factory (one per process) or an isolated one; IDWriteFactory up to IDWriteFactory7.
static WINAPI HRESULT DWriteCreateFactory(uint32_t type, const GUID *iid, void **out) {
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (type > 1 || !iid) return E_INVALIDARG;
    int ok = d3d_same_guid(iid, &IID_IUnknown_);
    for (const GUID *const *g = g_factory_iids; *g && !ok; g++) ok = d3d_same_guid(iid, *g);
    TR("DWriteCreateFactory(%s) -> %s", type ? "isolated" : "shared", ok ? "IDWriteFactory7" : "interface not offered");
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
    M("IDWriteFactory2_CreateGlyphRunAnalysis", factory_CreateGlyphRunAnalysis2),
    M("IDWriteFactory3_CreateGlyphRunAnalysis", factory_CreateGlyphRunAnalysis3), M("IDWriteFactory3_CreateCustomRenderingParams", factory_CreateCustomRenderingParams3),
    M("CreateFontFaceReference_", factory_CreateFontFaceReference_), M("CreateFontFaceReference", factory_CreateFontFaceReference),
    M("GetSystemFontSet", factory_GetSystemFontSet), M("CreateFontSetBuilder", factory_CreateFontSetBuilder),
    M("CreateFontCollectionFromFontSet", factory_CreateFontCollectionFromFontSet), M("IDWriteFactory3_GetSystemFontCollection", factory_GetSystemFontCollection3),
    M("GetFontDownloadQueue", factory_GetFontDownloadQueue), M("IDWriteFactory4_TranslateColorGlyphRun", factory_TranslateColorGlyphRun4),
    M("ComputeGlyphOrigins_", factory_ComputeGlyphOrigins_), M("ComputeGlyphOrigins", factory_ComputeGlyphOrigins),
    M("IDWriteFactory5_CreateFontSetBuilder", factory_CreateFontSetBuilder), M("CreateInMemoryFontFileLoader", factory_CreateInMemoryFontFileLoader),
    M("CreateHttpFontFileLoader", factory_CreateHttpFontFileLoader), M("AnalyzeContainerType", factory_AnalyzeContainerType),
    M("UnpackFontFile", factory_UnpackFontFile), M("IDWriteFactory6_CreateFontFaceReference", factory_CreateFontFaceReference6),
    M("CreateFontResource", factory_CreateFontResource), M("IDWriteFactory6_GetSystemFontSet", factory_GetSystemFontSet6),
    M("IDWriteFactory6_GetSystemFontCollection", factory_GetSystemFontCollection6),
    M("IDWriteFactory6_CreateFontCollectionFromFontSet", factory_CreateFontCollectionFromFontSet6),
    M("IDWriteFactory6_CreateFontSetBuilder", factory_CreateFontSetBuilder), M("IDWriteFactory6_CreateTextFormat", factory_CreateTextFormat6),
    M("IDWriteFactory7_GetSystemFontSet", factory_GetSystemFontSet6), M("IDWriteFactory7_GetSystemFontCollection", factory_GetSystemFontCollection6), { 0 } };
static const d3d_method m_collection[] = {
    IUNKNOWN, M("GetFontFamilyCount", coll_GetFontFamilyCount), M("GetFontFamily", coll_GetFontFamily), M("FindFamilyName", coll_FindFamilyName),
    M("GetFontFromFontFace", coll_GetFontFromFontFace), M("GetFontSet", coll_GetFontSet), M("IDWriteFontCollection1_GetFontFamily", coll_GetFontFamily),
    M("IDWriteFontCollection2_GetFontFamily", coll_GetFontFamily), M("GetMatchingFonts", coll_GetMatchingFonts),
    M("GetFontFamilyModel", coll_GetFontFamilyModel), M("IDWriteFontCollection2_GetFontSet", coll_GetFontSet),
    M("GetExpirationEvent", coll_GetExpirationEvent), { 0 } };
static const d3d_method m_family[] = {
    IUNKNOWN, M("GetFontCollection", fam_GetFontCollection), M("GetFontCount", fam_GetFontCount), M("GetFont", fam_GetFont),
    M("GetFamilyNames", fam_GetFamilyNames), M("GetFirstMatchingFont", fam_GetFirstMatchingFont), M("GetMatchingFonts", fam_GetMatchingFonts),
    M("GetFontLocality", fam_GetFontLocality), M("IDWriteFontFamily1_GetFont", fam_GetFont), M("GetFontFaceReference", fam_GetFontFaceReference),
    M("IDWriteFontFamily2_GetMatchingFonts", fam_GetMatchingFonts2), M("GetFontSet", fam_GetFontSet), { 0 } };
static const d3d_method m_list[] = {
    IUNKNOWN, M("GetFontCollection", list_GetFontCollection), M("GetFontCount", list_GetFontCount), M("GetFont", list_GetFont),
    M("GetFontLocality", list_GetFontLocality), M("IDWriteFontList1_GetFont", list_GetFont), M("GetFontFaceReference", list_GetFontFaceReference),
    M("GetFontSet", list_GetFontSet), { 0 } };
static const d3d_method m_font[] = {
    IUNKNOWN, M("GetFontFamily", font_GetFontFamily), M("GetWeight", font_GetWeight), M("GetStretch", font_GetStretch), M("GetStyle", font_GetStyle),
    M("IsSymbolFont", font_IsSymbolFont), M("GetFaceNames", font_GetFaceNames), M("GetInformationalStrings", font_GetInformationalStrings),
    M("GetSimulations", font_GetSimulations), M("GetMetrics", font_GetMetrics), M("HasCharacter", font_HasCharacter), M("CreateFontFace", font_CreateFontFace),
    M("IDWriteFont1_GetMetrics", font_GetMetrics1), M("GetPanose", font_GetPanose), M("GetUnicodeRanges", font_GetUnicodeRanges),
    M("IsMonospacedFont", font_IsMonospacedFont), M("IsColorFont", font_IsColorFont), M("IDWriteFont3_CreateFontFace", font_CreateFontFace),
    M("Equals", font_Equals), M("GetFontFaceReference", font_GetFontFaceReference), M("IDWriteFont3_HasCharacter", font_HasCharacter3),
    M("GetLocality", font_GetLocality), { 0 } };
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
    M("GetPaletteEntries", face_GetPaletteEntries), M("IDWriteFontFace2_GetRecommendedRenderingMode", face_GetRecommendedRenderingMode2),
    M("GetFontFaceReference", face_GetFontFaceReference), M("GetPanose", face_GetPanose), M("GetWeight", face_GetWeight), M("GetStretch", face_GetStretch),
    M("GetStyle", face_GetStyle), M("GetFamilyNames", face_GetFamilyNames), M("GetFaceNames", face_GetFaceNames),
    M("GetInformationalStrings", face_GetInformationalStrings), M("HasCharacter", face_HasCharacter),
    M("IDWriteFontFace3_GetRecommendedRenderingMode", face_GetRecommendedRenderingMode3), M("IsCharacterLocal", face_IsCharacterLocal),
    M("IsGlyphLocal", face_IsGlyphLocal), M("AreCharactersLocal", face_AreCharactersLocal), M("AreGlyphsLocal", face_AreGlyphsLocal),
    M("GetGlyphImageFormats_", face_GetGlyphImageFormats_), M("GetGlyphImageFormats", face_GetGlyphImageFormats), M("GetGlyphImageData", face_GetGlyphImageData),
    M("ReleaseGlyphImageData", face_ReleaseGlyphImageData), M("GetFontAxisValueCount", no_axes_count), M("GetFontAxisValues", no_axes_values),
    M("HasVariations", face_HasVariations), M("GetFontResource", face_GetFontResource), M("Equals", face_Equals), { 0 } };
static const d3d_method m_ref[] = {
    IUNKNOWN, M("CreateFontFace", ref_CreateFontFace), M("CreateFontFaceWithSimulations", ref_CreateFontFaceWithSimulations), M("Equals", ref_Equals),
    M("GetFontFaceIndex", ref_GetFontFaceIndex), M("GetSimulations", ref_GetSimulations), M("GetFontFile", ref_GetFontFile),
    M("GetLocalFileSize", ref_GetFileSize), M("GetFileSize", ref_GetFileSize), M("GetFileTime", ref_GetFileTime), M("GetLocality", ref_GetLocality),
    M("EnqueueFontDownloadRequest", ref_EnqueueFontDownloadRequest), M("EnqueueCharacterDownloadRequest", ref_EnqueueCharacterDownloadRequest),
    M("EnqueueGlyphDownloadRequest", ref_EnqueueGlyphDownloadRequest), M("EnqueueFileFragmentDownloadRequest", ref_EnqueueFileFragmentDownloadRequest),
    M("IDWriteFontFaceReference1_CreateFontFace", ref_CreateFontFace), M("GetFontAxisValueCount", no_axes_count), M("GetFontAxisValues", no_axes_values), { 0 } };
static const d3d_method m_resource[] = {
    IUNKNOWN, M("GetFontFile", res_GetFontFile), M("GetFontFaceIndex", res_GetFontFaceIndex), M("GetFontAxisCount", no_axes_count),
    M("GetDefaultFontAxisValues", no_axes_values), M("GetFontAxisRanges", no_axes_values), M("GetFontAxisAttributes", res_GetFontAxisAttributes),
    M("GetAxisNames", res_GetAxisNames), M("GetAxisValueNameCount", res_GetAxisValueNameCount), M("GetAxisValueNames", res_GetAxisValueNames),
    M("HasVariations", res_HasVariations), M("CreateFontFace", res_CreateFontFace), M("CreateFontFaceReference", res_CreateFontFaceReference), { 0 } };
static const d3d_method m_set[] = {
    IUNKNOWN, M("GetFontCount", set_GetFontCount), M("GetFontFaceReference", set_GetFontFaceReference), M("FindFontFaceReference", set_FindFontFaceReference),
    M("FindFontFace", set_FindFontFace), M("GetPropertyValues__", set_GetPropertyValues__), M("GetPropertyValues_", set_GetPropertyValues_),
    M("GetPropertyValues", set_GetPropertyValues), M("GetPropertyOccurrenceCount", set_GetPropertyOccurrenceCount),
    M("GetMatchingFonts_", set_GetMatchingFonts_), M("GetMatchingFonts", set_GetMatchingFonts), M("IDWriteFontSet1_GetMatchingFonts", set_GetMatchingFonts1),
    M("GetFirstFontResources", set_GetFirstFontResources), M("GetFilteredFonts__", set_GetFilteredFonts__), M("GetFilteredFonts_", set_GetFilteredFonts_),
    M("GetFilteredFonts", set_GetFilteredFonts), M("GetFilteredFontIndices_", set_GetFilteredFontIndices_), M("GetFilteredFontIndices", set_GetFilteredFontIndices),
    M("GetFontAxisRanges_", set_GetFontAxisRanges_), M("GetFontAxisRanges", set_GetFontAxisRanges),
    M("IDWriteFontSet1_GetFontFaceReference", set_GetFontFaceReference), M("CreateFontResource", set_CreateFontResource), M("CreateFontFace", set_CreateFontFace),
    M("GetFontLocality", set_GetFontLocality), M("GetExpirationEvent", set_GetExpirationEvent), M("GetFontSourceType", set_GetFontSourceType),
    M("GetFontSourceNameLength", set_GetFontSourceNameLength), M("GetFontSourceName", set_GetFontSourceName), { 0 } };
static const d3d_method m_builder[] = {
    IUNKNOWN, M("AddFontFaceReference_", builder_AddFontFaceReference_), M("AddFontFaceReference", builder_AddFontFaceReference),
    M("AddFontSet", builder_AddFontSet), M("CreateFontSet", builder_CreateFontSet), M("AddFontFile", builder_AddFontFile), M("AddFont", builder_AddFont),
    M("IDWriteFontSetBuilder2_AddFontFile", builder_AddFontFilePath), { 0 } };
static const d3d_method m_strlist[] = {
    IUNKNOWN, M("GetCount", strlist_GetCount), M("GetLocaleNameLength", strlist_GetLocaleNameLength), M("GetLocaleName", strlist_GetLocaleName),
    M("GetStringLength", strlist_GetStringLength), M("GetString", strlist_GetString), { 0 } };
static const d3d_method m_inmem[] = {
    IUNKNOWN, M("CreateStreamFromKey", inmem_CreateStreamFromKey), M("CreateInMemoryFontFileReference", inmem_CreateInMemoryFontFileReference),
    M("GetFileCount", inmem_GetFileCount), { 0 } };
static const d3d_method m_queue[] = {
    IUNKNOWN, M("AddListener", queue_AddListener), M("RemoveListener", queue_RemoveListener), M("IsEmpty", queue_IsEmpty), M("BeginDownload", queue_BeginDownload),
    M("CancelDownload", queue_CancelDownload), M("GetGenerationCount", queue_GetGenerationCount), { 0 } };
static const d3d_method m_fbbuilder[] = {
    IUNKNOWN, M("AddMapping", fbb_AddMapping), M("AddMappings", fbb_AddMappings), M("CreateFontFallback", fbb_CreateFontFallback), { 0 } };
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
    M("GetGrayscaleEnhancedContrast", params_GetGrayscaleEnhancedContrast), M("GetGridFitMode", params_GetGridFitMode),
    M("GetRenderingMode1", params_GetRenderingMode1), { 0 } };
static const d3d_method m_analysis[] = {
    IUNKNOWN, M("GetAlphaTextureBounds", analysis_GetAlphaTextureBounds), M("CreateAlphaTexture", analysis_CreateAlphaTexture),
    M("GetAlphaBlendParams", analysis_GetAlphaBlendParams), { 0 } };
static const d3d_method m_fallback[] = { IUNKNOWN, M("MapCharacters", fallback_MapCharacters), M("IDWriteFontFallback1_MapCharacters", fallback_MapCharacters1), { 0 } };
static const d3d_method m_gdi[] = {
    IUNKNOWN, M("CreateFontFromLOGFONT", gdi_CreateFontFromLOGFONT), M("ConvertFontToLOGFONT", gdi_ConvertFontToLOGFONT),
    M("ConvertFontFaceToLOGFONT", gdi_ConvertFontFaceToLOGFONT), M("CreateFontFaceFromHdc", gdi_CreateFontFaceFromHdc),
    M("CreateBitmapRenderTarget", gdi_CreateBitmapRenderTarget), M("IDWriteGdiInterop1_CreateFontFromLOGFONT", gdi_CreateFontFromLOGFONT1),
    M("GetFontSignature_", gdi_GetFontSignature_), M("GetFontSignature", gdi_GetFontSignature), M("GetMatchingFontsByLOGFONT", gdi_GetMatchingFontsByLOGFONT), { 0 } };
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
static const GUID *const g_gdi_iids[] = { &IID_IDWriteGdiInterop, &IID_IDWriteGdiInterop1, NULL };
static const GUID *const g_memory_loader_iids[] = { &IID_IDWriteFontFileLoader, NULL };

static const vx_export dwrite_exports[] = { VX_EXPORT(DWriteCreateFactory) };

void vx_dwrite_init(void) {
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_lock, &ma);
    const char *t = getenv("VXRT_TRACE_DWRITE");
    g_trace = t && *t == '1';
    g_factory_vtbl = D3D_VTABLE(IDWriteFactory7, m_factory);
    g_collection_vtbl = D3D_VTABLE(IDWriteFontCollection3, m_collection);
    g_family_vtbl = D3D_VTABLE(IDWriteFontFamily2, m_family);
    g_list_vtbl = D3D_VTABLE(IDWriteFontList2, m_list);
    g_font_vtbl = D3D_VTABLE(IDWriteFont3, m_font);
    g_face_vtbl = D3D_VTABLE(IDWriteFontFace5, m_face);
    g_ref_vtbl = D3D_VTABLE(IDWriteFontFaceReference1, m_ref);
    g_resource_vtbl = D3D_VTABLE(IDWriteFontResource, m_resource);
    g_set_vtbl = D3D_VTABLE(IDWriteFontSet3, m_set);
    g_builder_vtbl = D3D_VTABLE(IDWriteFontSetBuilder2, m_builder);
    g_strlist_vtbl = D3D_VTABLE(IDWriteStringList, m_strlist);
    g_inmem_vtbl = D3D_VTABLE(IDWriteInMemoryFontFileLoader, m_inmem);
    g_queue_vtbl = D3D_VTABLE(IDWriteFontDownloadQueue, m_queue);
    g_fbbuilder_vtbl = D3D_VTABLE(IDWriteFontFallbackBuilder, m_fbbuilder);
    g_strings_vtbl = D3D_VTABLE(IDWriteLocalizedStrings, m_strings);
    g_file_vtbl = D3D_VTABLE(IDWriteFontFile, m_file);
    g_local_loader_vtbl = D3D_VTABLE(IDWriteLocalFontFileLoader, m_local_loader);
    g_stream_vtbl = D3D_VTABLE(IDWriteFontFileStream, m_stream);
    g_params_vtbl = D3D_VTABLE(IDWriteRenderingParams3, m_params);
    g_analysis_vtbl = D3D_VTABLE(IDWriteGlyphRunAnalysis, m_analysis);
    g_fallback_vtbl = D3D_VTABLE(IDWriteFontFallback1, m_fallback);
    g_gdi_vtbl = D3D_VTABLE(IDWriteGdiInterop1, m_gdi);
    g_format_vtbl = D3D_VTABLE(IDWriteTextFormat, m_format);
    g_layout_vtbl = D3D_VTABLE(IDWriteTextLayout, m_layout);
    g_typography_vtbl = D3D_VTABLE(IDWriteTypography, m_typography);
    g_target_vtbl = D3D_VTABLE(IDWriteBitmapRenderTarget, m_target);
    g_memory_loader_vtbl[0] = (void *)dw_query; g_memory_loader_vtbl[1] = (void *)dw_addref; g_memory_loader_vtbl[2] = (void *)dw_release;
    g_memory_loader_vtbl[3] = (void *)memory_CreateStreamFromKey;
    g_numsub_vtbl[0] = (void *)dw_query; g_numsub_vtbl[1] = (void *)dw_addref; g_numsub_vtbl[2] = (void *)dw_release;
    g_shared = (dw_factory){ { g_factory_vtbl, 1, NULL, NULL, g_factory_iids, "IDWriteFactory" }, { 0 }, 0, { 0 }, 0 };
    g_local_loader = (dw_obj){ g_local_loader_vtbl, 1, NULL, NULL, g_local_loader_iids, "IDWriteLocalFontFileLoader" };
    g_memory_loader = (dw_obj){ g_memory_loader_vtbl, 1, NULL, NULL, g_memory_loader_iids, "IDWriteFontFileLoader" };
    g_fallback = (dw_obj){ g_fallback_vtbl, 1, NULL, NULL, g_fallback_iids, "IDWriteFontFallback" };
    g_gdi = (dw_obj){ g_gdi_vtbl, 1, NULL, NULL, g_gdi_iids, "IDWriteGdiInterop" };
    g_queue = (dw_queue){ { g_queue_vtbl, 1, NULL, NULL, g_queue_iids, "IDWriteFontDownloadQueue" }, { 0 }, { 0 }, 0, 0 };
    vx_register_module("dwrite.dll", dwrite_exports, sizeof dwrite_exports / sizeof dwrite_exports[0]);
}
#ifdef __aarch64__
#include "thunks/dwrite.m.inc"
#endif
