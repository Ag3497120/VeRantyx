// DirectWrite on CoreText (src/dwrite.m): the factory and the system font collection (families, names, matching),
// font faces (glyphs, metrics, tables, files through the local loader), a font given through a loader of the
// program's own (a custom font file and a custom collection), font fallback for made-up Latin, Japanese and emoji
// text, glyph outlines into a geometry sink, glyph runs rasterized into alpha textures, and text formats and layouts
// (measured, wrapped, hit-tested, drawn through a renderer into a GDI bitmap render target).
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <dwrite_3.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)

static void family_name(IDWriteFontFamily *fam, WCHAR *out, UINT32 cap) {
    IDWriteLocalizedStrings *names = NULL;
    out[0] = 0;
    if (FAILED(IDWriteFontFamily_GetFamilyNames(fam, &names)) || !names) return;
    UINT32 i = 0; BOOL exists = FALSE;
    IDWriteLocalizedStrings_FindLocaleName(names, L"en-us", &i, &exists);
    if (!exists) i = 0;
    IDWriteLocalizedStrings_GetString(names, i, out, cap);
    IDWriteLocalizedStrings_Release(names);
}

// ---- a text source for MapCharacters ----------------------------------------------------------------------------------------
typedef struct { IDWriteTextAnalysisSource iface; const WCHAR *text; UINT32 len; } source;
static HRESULT WINAPI src_qi(IDWriteTextAnalysisSource *s, REFIID iid, void **out) { (void)iid; *out = s; return S_OK; }
static ULONG WINAPI src_ref(IDWriteTextAnalysisSource *s) { (void)s; return 1; }
static HRESULT WINAPI src_text(IDWriteTextAnalysisSource *s, UINT32 pos, const WCHAR **t, UINT32 *n) {
    source *x = (source *)s;
    if (pos >= x->len) { *t = NULL; *n = 0; return S_OK; }
    *t = x->text + pos; *n = x->len - pos;
    return S_OK;
}
static HRESULT WINAPI src_before(IDWriteTextAnalysisSource *s, UINT32 pos, const WCHAR **t, UINT32 *n) {
    source *x = (source *)s;
    if (!pos || pos > x->len) { *t = NULL; *n = 0; return S_OK; }
    *t = x->text; *n = pos;
    return S_OK;
}
static DWRITE_READING_DIRECTION WINAPI src_dir(IDWriteTextAnalysisSource *s) { (void)s; return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT; }
static HRESULT WINAPI src_locale(IDWriteTextAnalysisSource *s, UINT32 pos, UINT32 *n, const WCHAR **loc) { source *x = (source *)s; *n = x->len - pos; *loc = L"en-us"; return S_OK; }
static HRESULT WINAPI src_numsub(IDWriteTextAnalysisSource *s, UINT32 pos, UINT32 *n, IDWriteNumberSubstitution **sub) { source *x = (source *)s; *n = x->len - pos; *sub = NULL; return S_OK; }
static IDWriteTextAnalysisSourceVtbl src_vtbl = { src_qi, src_ref, src_ref, src_text, src_before, src_dir, src_locale, src_numsub };

// ---- a geometry sink for GetGlyphRunOutline ---------------------------------------------------------------------------------
typedef struct { float x, y; } pt;
typedef struct { pt a, b, c; } bezier;
typedef struct sink sink;
typedef struct {
    HRESULT (WINAPI *qi)(sink *, REFIID, void **);
    ULONG (WINAPI *addref)(sink *);
    ULONG (WINAPI *release)(sink *);
    void (WINAPI *set_fill_mode)(sink *, int);
    void (WINAPI *set_segment_flags)(sink *, int);
    void (WINAPI *begin_figure)(sink *, pt, int);
    void (WINAPI *add_lines)(sink *, const pt *, UINT32);
    void (WINAPI *add_beziers)(sink *, const bezier *, UINT32);
    void (WINAPI *end_figure)(sink *, int);
    HRESULT (WINAPI *close)(sink *);
} sink_vtbl;
struct sink { const sink_vtbl *vtbl; int fill_mode, figures, open, ends_closed, lines, beziers, bad_order; float x0, y0, x1, y1; };
static void grow(sink *s, pt p) {
    if (p.x < s->x0) s->x0 = p.x; if (p.x > s->x1) s->x1 = p.x;
    if (p.y < s->y0) s->y0 = p.y; if (p.y > s->y1) s->y1 = p.y;
}
static HRESULT WINAPI sk_qi(sink *s, REFIID iid, void **out) { (void)iid; *out = s; return S_OK; }
static ULONG WINAPI sk_ref(sink *s) { (void)s; return 1; }
static void WINAPI sk_fill(sink *s, int m) { s->fill_mode = m; }
static void WINAPI sk_flags(sink *s, int f) { (void)s; (void)f; }
static void WINAPI sk_begin(sink *s, pt p, int b) { (void)b; if (s->open) s->bad_order++; s->open = 1; s->figures++; grow(s, p); }
static void WINAPI sk_lines(sink *s, const pt *p, UINT32 n) { if (!s->open) s->bad_order++; s->lines += n; for (UINT32 i = 0; i < n; i++) grow(s, p[i]); }
static void WINAPI sk_beziers(sink *s, const bezier *b, UINT32 n) { if (!s->open) s->bad_order++; s->beziers += n; for (UINT32 i = 0; i < n; i++) grow(s, b[i].c); }
static void WINAPI sk_end(sink *s, int e) { if (!s->open) s->bad_order++; s->open = 0; s->ends_closed += e == 1; }
static HRESULT WINAPI sk_close(sink *s) { (void)s; return S_OK; }
static const sink_vtbl sink_v = { sk_qi, sk_ref, sk_ref, sk_fill, sk_flags, sk_begin, sk_lines, sk_beziers, sk_end, sk_close };
static void sink_init(sink *s) { memset(s, 0, sizeof *s); s->vtbl = &sink_v; s->x0 = s->y0 = 1e9f; s->x1 = s->y1 = -1e9f; }

// ---- a font file loader of the program's own: a font's bytes in memory ------------------------------------------------------
typedef struct { IDWriteFontFileStream iface; LONG refs; const BYTE *data; UINT64 size; } mem_stream;
static HRESULT WINAPI ms_qi(IDWriteFontFileStream *s, REFIID iid, void **out) { (void)iid; *out = s; IDWriteFontFileStream_AddRef(s); return S_OK; }
static ULONG WINAPI ms_addref(IDWriteFontFileStream *s) { return InterlockedIncrement(&((mem_stream *)s)->refs); }
static ULONG WINAPI ms_release(IDWriteFontFileStream *s) { LONG r = InterlockedDecrement(&((mem_stream *)s)->refs); if (!r) HeapFree(GetProcessHeap(), 0, s); return r; }
static int fragments_out;
static HRESULT WINAPI ms_read(IDWriteFontFileStream *s, const void **start, UINT64 off, UINT64 n, void **ctx) {
    mem_stream *m = (mem_stream *)s;
    if (off > m->size || n > m->size - off) return E_FAIL;
    *start = m->data + off; *ctx = (void *)1; fragments_out++;
    return S_OK;
}
static void WINAPI ms_release_fragment(IDWriteFontFileStream *s, void *ctx) { (void)s; if (ctx == (void *)1) fragments_out--; }
static HRESULT WINAPI ms_size(IDWriteFontFileStream *s, UINT64 *n) { *n = ((mem_stream *)s)->size; return S_OK; }
static HRESULT WINAPI ms_time(IDWriteFontFileStream *s, UINT64 *t) { (void)s; *t = 0; return E_NOTIMPL; }
static IDWriteFontFileStreamVtbl ms_vtbl = { ms_qi, ms_addref, ms_release, ms_read, ms_release_fragment, ms_size, ms_time };

typedef struct { IDWriteFontFileLoader iface; LONG refs; const BYTE *data; UINT64 size; int streams; } mem_loader;
static HRESULT WINAPI ml_qi(IDWriteFontFileLoader *l, REFIID iid, void **out) { (void)iid; *out = l; return S_OK; }
static ULONG WINAPI ml_addref(IDWriteFontFileLoader *l) { return InterlockedIncrement(&((mem_loader *)l)->refs); }
static ULONG WINAPI ml_release(IDWriteFontFileLoader *l) { return InterlockedDecrement(&((mem_loader *)l)->refs); }
static HRESULT WINAPI ml_stream(IDWriteFontFileLoader *l, const void *key, UINT32 n, IDWriteFontFileStream **out) {
    mem_loader *m = (mem_loader *)l;
    if (n != 4 || memcmp(key, "MEM1", 4)) return E_INVALIDARG;
    mem_stream *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *s);
    s->iface.lpVtbl = &ms_vtbl; s->refs = 1; s->data = m->data; s->size = m->size;
    m->streams++;
    *out = &s->iface;
    return S_OK;
}
static IDWriteFontFileLoaderVtbl ml_vtbl = { ml_qi, ml_addref, ml_release, ml_stream };

// a collection loader handing out one file
typedef struct { IDWriteFontFileEnumerator iface; LONG refs; IDWriteFontFile *file; int at; } one_enum;
static HRESULT WINAPI oe_qi(IDWriteFontFileEnumerator *e, REFIID iid, void **out) { (void)iid; *out = e; return S_OK; }
static ULONG WINAPI oe_addref(IDWriteFontFileEnumerator *e) { return InterlockedIncrement(&((one_enum *)e)->refs); }
static ULONG WINAPI oe_release(IDWriteFontFileEnumerator *e) { one_enum *o = (one_enum *)e; LONG r = InterlockedDecrement(&o->refs); if (!r) { IDWriteFontFile_Release(o->file); HeapFree(GetProcessHeap(), 0, o); } return r; }
static HRESULT WINAPI oe_next(IDWriteFontFileEnumerator *e, BOOL *more) { one_enum *o = (one_enum *)e; *more = o->at++ == 0; return S_OK; }
static HRESULT WINAPI oe_current(IDWriteFontFileEnumerator *e, IDWriteFontFile **f) { one_enum *o = (one_enum *)e; IDWriteFontFile_AddRef(o->file); *f = o->file; return S_OK; }
static IDWriteFontFileEnumeratorVtbl oe_vtbl = { oe_qi, oe_addref, oe_release, oe_next, oe_current };
typedef struct { IDWriteFontCollectionLoader iface; IDWriteFontFile *file; } one_loader;
static HRESULT WINAPI ol_qi(IDWriteFontCollectionLoader *l, REFIID iid, void **out) { (void)iid; *out = l; return S_OK; }
static ULONG WINAPI ol_ref(IDWriteFontCollectionLoader *l) { (void)l; return 1; }
static HRESULT WINAPI ol_enum(IDWriteFontCollectionLoader *l, IDWriteFactory *f, const void *key, UINT32 n, IDWriteFontFileEnumerator **out) {
    (void)f; (void)key; (void)n;
    one_enum *e = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *e);
    e->iface.lpVtbl = &oe_vtbl; e->refs = 1; e->file = ((one_loader *)l)->file;
    IDWriteFontFile_AddRef(e->file);
    *out = &e->iface;
    return S_OK;
}
static IDWriteFontCollectionLoaderVtbl ol_vtbl = { ol_qi, ol_ref, ol_ref, ol_enum };

// ---- a text renderer: records the glyph runs, and draws them into a bitmap render target when it is the context ------------
typedef struct { IDWriteTextRenderer iface; int runs, glyphs, faces; IDWriteFontFace *face[8]; float first_x, first_y, last_x; UINT32 positions; } renderer;
static HRESULT WINAPI r_qi(IDWriteTextRenderer *r, REFIID iid, void **out) { (void)iid; *out = r; return S_OK; }
static ULONG WINAPI r_ref(IDWriteTextRenderer *r) { (void)r; return 1; }
static HRESULT WINAPI r_snap(IDWriteTextRenderer *r, void *ctx, BOOL *off) { (void)r; (void)ctx; *off = FALSE; return S_OK; }
static HRESULT WINAPI r_transform(IDWriteTextRenderer *r, void *ctx, DWRITE_MATRIX *m) { (void)r; (void)ctx; DWRITE_MATRIX id = { 1, 0, 0, 1, 0, 0 }; *m = id; return S_OK; }
static HRESULT WINAPI r_ppd(IDWriteTextRenderer *r, void *ctx, FLOAT *ppd) { (void)r; (void)ctx; *ppd = 1.0f; return S_OK; }
static HRESULT WINAPI r_run(IDWriteTextRenderer *ri, void *ctx, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN *run,
                            const DWRITE_GLYPH_RUN_DESCRIPTION *desc, IUnknown *effect) {
    renderer *r = (renderer *)ri;
    (void)effect;
    if (!r->runs) { r->first_x = x; r->first_y = y; }
    r->last_x = x;
    r->runs++; r->glyphs += run->glyphCount; r->positions += desc->stringLength;
    int known = 0;
    for (int i = 0; i < r->faces; i++) known |= r->face[i] == run->fontFace;
    if (!known && r->faces < 8) r->face[r->faces++] = run->fontFace;
    if (ctx) {
        RECT box;
        IDWriteBitmapRenderTarget_DrawGlyphRun((IDWriteBitmapRenderTarget *)ctx, x, y, mode, run, NULL, RGB(255, 255, 255), &box);
    }
    return S_OK;
}
static HRESULT WINAPI r_line(IDWriteTextRenderer *r, void *ctx, FLOAT x, FLOAT y, const DWRITE_UNDERLINE *u, IUnknown *e) { (void)r; (void)ctx; (void)x; (void)y; (void)u; (void)e; return S_OK; }
static HRESULT WINAPI r_strike(IDWriteTextRenderer *r, void *ctx, FLOAT x, FLOAT y, const DWRITE_STRIKETHROUGH *u, IUnknown *e) { (void)r; (void)ctx; (void)x; (void)y; (void)u; (void)e; return S_OK; }
static HRESULT WINAPI r_inline(IDWriteTextRenderer *r, void *ctx, FLOAT x, FLOAT y, IDWriteInlineObject *o, BOOL s, BOOL rtl, IUnknown *e) { (void)r; (void)ctx; (void)x; (void)y; (void)o; (void)s; (void)rtl; (void)e; return E_NOTIMPL; }
static IDWriteTextRendererVtbl renderer_vtbl = { r_qi, r_ref, r_ref, r_snap, r_transform, r_ppd, r_run, r_line, r_strike, r_inline };

static IDWriteFontFace *face_of(IDWriteFontCollection *c, const WCHAR *family, DWRITE_FONT_WEIGHT w, DWRITE_FONT_STYLE st, IDWriteFont **font_out) {
    UINT32 i; BOOL exists = FALSE;
    if (FAILED(IDWriteFontCollection_FindFamilyName(c, family, &i, &exists)) || !exists) return NULL;
    IDWriteFontFamily *fam = NULL; IDWriteFont *font = NULL; IDWriteFontFace *face = NULL;
    IDWriteFontCollection_GetFontFamily(c, i, &fam);
    if (fam && SUCCEEDED(IDWriteFontFamily_GetFirstMatchingFont(fam, w, DWRITE_FONT_STRETCH_NORMAL, st, &font)) && font) IDWriteFont_CreateFontFace(font, &face);
    if (fam) IDWriteFontFamily_Release(fam);
    if (font_out) *font_out = font; else if (font) IDWriteFont_Release(font);
    return face;
}
static UINT16 glyph(IDWriteFontFace *f, UINT32 cp) { UINT16 g = 0; IDWriteFontFace_GetGlyphIndices(f, &cp, 1, &g); return g; }

// The pixels with coverage of a glyph run, and the box the outline gives them.
static int raster(IDWriteFactory2 *f2, IDWriteFactory *f, IDWriteFontFace *face, const UINT16 *g, UINT32 n, float em, DWRITE_RENDERING_MODE mode,
                  int grayscale, DWRITE_TEXTURE_TYPE type, RECT *bounds, int *only_bilevel) {
    DWRITE_GLYPH_RUN run = { face, em, n, g, NULL, NULL, FALSE, 0 };
    IDWriteGlyphRunAnalysis *a = NULL;
    HRESULT hr = grayscale ? IDWriteFactory2_CreateGlyphRunAnalysis(f2, &run, NULL, mode, DWRITE_MEASURING_MODE_NATURAL, DWRITE_GRID_FIT_MODE_DISABLED, DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 10.0f, 40.0f, &a)
                           : IDWriteFactory_CreateGlyphRunAnalysis(f, &run, 1.0f, NULL, mode, DWRITE_MEASURING_MODE_NATURAL, 10.0f, 40.0f, &a);
    CHECK(SUCCEEDED(hr) && a);
    if (!a) return -1;
    memset(bounds, 0, sizeof *bounds);
    CHECK(SUCCEEDED(IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(a, type, bounds)));
    int w = bounds->right - bounds->left, h = bounds->bottom - bounds->top, covered = 0, bpp = type == DWRITE_TEXTURE_CLEARTYPE_3x1 ? 3 : 1;
    *only_bilevel = 1;
    if (w > 0 && h > 0) {
        BYTE *buf = malloc((size_t)w * h * bpp);
        CHECK(IDWriteGlyphRunAnalysis_CreateAlphaTexture(a, type, bounds, buf, (UINT32)(w * h * bpp) - 1) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
        CHECK(SUCCEEDED(IDWriteGlyphRunAnalysis_CreateAlphaTexture(a, type, bounds, buf, (UINT32)(w * h * bpp))));
        int top = 0, bottom = 0, left = 0, right = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w * bpp; x++) {
                BYTE v = buf[(size_t)y * w * bpp + x];
                if (!v) continue;
                covered++;
                if (v != 255) *only_bilevel = 0;
                if (y == 0) top = 1;
                if (y == h - 1) bottom = 1;
                if (x / bpp == 0) left = 1;
                if (x / bpp == w - 1) right = 1;
            }
        CHECK(top && bottom && left && right);              // the bounds are tight: coverage on each edge
        if (bpp == 3)                                       // grayscale coverage in each subpixel
            for (int i = 0; i + 2 < w * h * 3; i += 3) if (buf[i] != buf[i + 1] || buf[i] != buf[i + 2]) { CHECK(!"subpixels differ"); break; }
        free(buf);
    }
    float blend_gamma = 0, blend_contrast = -1, blend_ct = -1;
    IDWriteRenderingParams *p = NULL;
    IDWriteFactory_CreateRenderingParams(f, &p);
    CHECK(SUCCEEDED(IDWriteGlyphRunAnalysis_GetAlphaBlendParams(a, p, &blend_gamma, &blend_contrast, &blend_ct)));
    CHECK(blend_gamma > 0 && blend_contrast >= 0 && blend_ct >= 0);
    if (p) IDWriteRenderingParams_Release(p);
    IDWriteGlyphRunAnalysis_Release(a);
    return covered;
}

// Four threads at once through what is made on first use: the system collection, family order, faces, rasterizing.
static volatile LONG thread_failures, thread_glyphs;
static DWORD WINAPI worker(void *arg) {
    int me = (int)(INT_PTR)arg;
    IDWriteFactory *f = NULL; IDWriteFontCollection *c = NULL;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown **)&f)) || FAILED(IDWriteFactory_GetSystemFontCollection(f, &c, FALSE))) {
        InterlockedIncrement(&thread_failures); return 1;
    }
    UINT32 n = IDWriteFontCollection_GetFontFamilyCount(c);
    for (UINT32 i = (UINT32)me; i < n; i += 9) {
        IDWriteFontFamily *fam = NULL; IDWriteFont *font = NULL; IDWriteFontFace *face = NULL;
        if (FAILED(IDWriteFontCollection_GetFontFamily(c, i, &fam))) { InterlockedIncrement(&thread_failures); continue; }
        if (SUCCEEDED(IDWriteFontFamily_GetFirstMatchingFont(fam, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)) &&
            SUCCEEDED(IDWriteFont_CreateFontFace(font, &face))) {
            UINT32 cp = 'a'; UINT16 g = 0; DWRITE_GLYPH_METRICS gm;
            IDWriteFontFace_GetGlyphIndices(face, &cp, 1, &g);
            IDWriteFontFace_GetDesignGlyphMetrics(face, &g, 1, &gm, FALSE);
            DWRITE_GLYPH_RUN run = { face, 20.0f, 1, &g, NULL, NULL, FALSE, 0 };
            IDWriteGlyphRunAnalysis *a = NULL;
            if (g && SUCCEEDED(IDWriteFactory_CreateGlyphRunAnalysis(f, &run, 1.0f, NULL, DWRITE_RENDERING_MODE_NATURAL, DWRITE_MEASURING_MODE_NATURAL, 0, 0, &a))) {
                RECT r; IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(a, DWRITE_TEXTURE_CLEARTYPE_3x1, &r);
                if (r.right > r.left) InterlockedIncrement(&thread_glyphs);
                IDWriteGlyphRunAnalysis_Release(a);
            }
        } else InterlockedIncrement(&thread_failures);
        if (face) IDWriteFontFace_Release(face);
        if (font) IDWriteFont_Release(font);
        IDWriteFontFamily_Release(fam);
    }
    IDWriteFontCollection_Release(c);
    IDWriteFactory_Release(f);
    return 0;
}

int main(void) {
    HANDLE threads[4];
    for (int i = 0; i < 4; i++) threads[i] = CreateThread(NULL, 0, worker, (void *)(INT_PTR)i, 0, NULL);
    WaitForMultipleObjects(4, threads, TRUE, 120000);
    for (int i = 0; i < 4; i++) CloseHandle(threads[i]);
    CHECK(thread_failures == 0 && thread_glyphs > 50);
    IDWriteFactory *f = NULL;
    IUnknown *unk = NULL;
    CHECK(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory3, &unk) == E_NOINTERFACE && !unk);
    CHECK(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown **)&f)) && f);
    if (!f) { printf("dwrite: no factory -> FAILED\n"); return 1; }
    IDWriteFactory *f_again = NULL, *isolated = NULL;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown **)&f_again);
    CHECK(f_again == f);
    CHECK(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, &IID_IDWriteFactory, (IUnknown **)&isolated)) && isolated && isolated != f);
    if (f_again) IDWriteFactory_Release(f_again);
    if (isolated) IDWriteFactory_Release(isolated);
    IDWriteFactory2 *f2 = NULL;
    IUnknown *f3 = (IUnknown *)1;
    CHECK(SUCCEEDED(IDWriteFactory_QueryInterface(f, &IID_IDWriteFactory2, (void **)&f2)) && f2);
    CHECK(IDWriteFactory_QueryInterface(f, &IID_IDWriteFactory3, (void **)&f3) == E_NOINTERFACE && !f3);

    // The system collection: families with names that find themselves.
    IDWriteFontCollection *sys = NULL;
    CHECK(SUCCEEDED(IDWriteFactory_GetSystemFontCollection(f, &sys, FALSE)) && sys);
    UINT32 nfam = IDWriteFontCollection_GetFontFamilyCount(sys);
    CHECK(nfam > 10);
    int named = 0, refound = 0;
    for (UINT32 i = 0; i < nfam; i += nfam / 10 + 1) {
        IDWriteFontFamily *fam = NULL;
        IDWriteFontCollection_GetFontFamily(sys, i, &fam);
        WCHAR name[128];
        family_name(fam, name, 128);
        if (name[0]) named++;
        UINT32 at = 0; BOOL exists = FALSE;
        if (SUCCEEDED(IDWriteFontCollection_FindFamilyName(sys, name, &at, &exists)) && exists && at == i) refound++;
        CHECK(IDWriteFontFamily_GetFontCount(fam) > 0);
        IDWriteFontFamily_Release(fam);
    }
    CHECK(named == refound && named >= 10);
    UINT32 none_at = 0; BOOL no_family = TRUE;
    CHECK(SUCCEEDED(IDWriteFontCollection_FindFamilyName(sys, L"No Such Family Xq", &none_at, &no_family)) && !no_family && none_at == UINT32_MAX);

    // A Latin face: metrics, glyphs, tables.
    IDWriteFont *font = NULL;
    IDWriteFontFace *face = face_of(sys, L"helvetica", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font);
    CHECK(face && font);
    if (!face) { printf("dwrite: no Helvetica face -> FAILED\n"); return 1; }
    CHECK(IDWriteFont_GetWeight(font) == DWRITE_FONT_WEIGHT_NORMAL && IDWriteFont_GetStyle(font) == DWRITE_FONT_STYLE_NORMAL && IDWriteFont_GetSimulations(font) == 0);
    DWRITE_FONT_METRICS m;
    IDWriteFontFace_GetMetrics(face, &m);
    CHECK(m.designUnitsPerEm >= 256 && m.ascent > 0 && m.descent > 0 && m.ascent + m.descent < 3 * m.designUnitsPerEm);
    CHECK(m.capHeight > 0 && m.capHeight < m.ascent && m.xHeight > 0 && m.xHeight < m.capHeight);
    CHECK(m.underlineThickness > 0 && m.strikethroughThickness > 0);
    DWRITE_FONT_METRICS fm;
    IDWriteFont_GetMetrics(font, &fm);
    CHECK(!memcmp(&fm, &m, sizeof m));
    UINT32 hello[] = { 'H', 'e', 'l', 'l', 'o' };
    UINT16 gi[5] = { 0 };
    CHECK(SUCCEEDED(IDWriteFontFace_GetGlyphIndices(face, hello, 5, gi)));
    CHECK(gi[0] && gi[1] && gi[2] && gi[2] == gi[3] && gi[4] && gi[0] != gi[1]);
    CHECK(gi[0] < IDWriteFontFace_GetGlyphCount(face));
    DWRITE_GLYPH_METRICS gm[5];
    CHECK(SUCCEEDED(IDWriteFontFace_GetDesignGlyphMetrics(face, gi, 5, gm, FALSE)));
    for (int i = 0; i < 5; i++) {
        CHECK(gm[i].advanceWidth > 0 && gm[i].advanceWidth < m.designUnitsPerEm * 2u);
        CHECK(gm[i].leftSideBearing + gm[i].rightSideBearing < (INT32)gm[i].advanceWidth);    // a black box of some width
        CHECK(gm[i].verticalOriginY - gm[i].topSideBearing <= m.ascent + m.designUnitsPerEm / 4);
    }
    // 'H' stands on the baseline and reaches the cap height
    CHECK(abs((gm[0].verticalOriginY - gm[0].topSideBearing) - m.capHeight) <= m.designUnitsPerEm / 50);
    IDWriteFontFace1 *face1 = NULL;
    CHECK(SUCCEEDED(IDWriteFontFace_QueryInterface(face, &IID_IDWriteFontFace1, (void **)&face1)) && face1);
    INT32 adv[5];
    if (face1) {
        CHECK(SUCCEEDED(IDWriteFontFace1_GetDesignGlyphAdvances(face1, 5, gi, adv, FALSE)));
        for (int i = 0; i < 5; i++) CHECK(adv[i] == (INT32)gm[i].advanceWidth);
        DWRITE_FONT_METRICS1 m1;
        IDWriteFontFace1_GetMetrics(face1, &m1);
        CHECK(m1.designUnitsPerEm == m.designUnitsPerEm && m1.glyphBoxTop > 0 && m1.glyphBoxBottom < 0 && m1.glyphBoxRight > m1.glyphBoxLeft);
        DWRITE_UNICODE_RANGE ranges[64]; UINT32 nr = 0;
        HRESULT hr = IDWriteFontFace1_GetUnicodeRanges(face1, 64, ranges, &nr);
        CHECK((SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) && nr > 0);
        int has_a = 0;
        for (UINT32 i = 0; i < nr && i < 64; i++) if (ranges[i].first <= 'a' && ranges[i].last >= 'a') has_a = 1;
        CHECK(has_a);
        IDWriteFontFace1_Release(face1);
    }
    IUnknown *face5 = (IUnknown *)1;
    CHECK(IDWriteFontFace_QueryInterface(face, &IID_IDWriteFontFace5, (void **)&face5) == E_NOINTERFACE && !face5);
    const void *tdata = NULL; UINT32 tsize = 0; void *tctx = NULL; BOOL texists = FALSE;
    CHECK(SUCCEEDED(IDWriteFontFace_TryGetFontTable(face, DWRITE_MAKE_OPENTYPE_TAG('h', 'e', 'a', 'd'), &tdata, &tsize, &tctx, &texists)));
    CHECK(texists && tsize >= 54 && tdata);
    if (texists && tdata && tsize >= 54) {
        const BYTE *h = tdata;
        CHECK(((h[18] << 8) | h[19]) == m.designUnitsPerEm);
        CHECK(((h[12] << 24) | (h[13] << 16) | (h[14] << 8) | h[15]) == 0x5F0F3CF5);         // head's magic number
    }
    IDWriteFontFace_ReleaseFontTable(face, tctx);
    CHECK(SUCCEEDED(IDWriteFontFace_TryGetFontTable(face, DWRITE_MAKE_OPENTYPE_TAG('z', 'z', 'z', 'q'), &tdata, &tsize, &tctx, &texists)) && !texists);
    IDWriteLocalizedStrings *ps = NULL; BOOL ps_exists = FALSE;
    CHECK(SUCCEEDED(IDWriteFont_GetInformationalStrings(font, DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME, &ps, &ps_exists)) && ps_exists && ps);
    if (ps) { WCHAR n[64]; CHECK(SUCCEEDED(IDWriteLocalizedStrings_GetString(ps, 0, n, 64)) && !wcsncmp(n, L"Helvetica", 9)); IDWriteLocalizedStrings_Release(ps); }
    IDWriteFontCollection *back_coll = NULL; IDWriteFont *back = NULL;
    CHECK(SUCCEEDED(IDWriteFontCollection_GetFontFromFontFace(sys, face, &back)) && back == font);
    if (back) IDWriteFont_Release(back);
    (void)back_coll;

    // Its file through the local loader, and the same face made again from the file.
    UINT32 nfiles = 0;
    CHECK(SUCCEEDED(IDWriteFontFace_GetFiles(face, &nfiles, NULL)) && nfiles == 1);
    IDWriteFontFile *file = NULL;
    CHECK(SUCCEEDED(IDWriteFontFace_GetFiles(face, &nfiles, &file)) && file);
    const void *key = NULL; UINT32 keysize = 0;
    IDWriteFontFileLoader *loader = NULL; IDWriteLocalFontFileLoader *local = NULL;
    BYTE *bytes = NULL; UINT64 nbytes = 0;
    WCHAR path[512] = { 0 };
    if (file) {
        CHECK(SUCCEEDED(IDWriteFontFile_GetReferenceKey(file, &key, &keysize)) && key && keysize);
        CHECK(SUCCEEDED(IDWriteFontFile_GetLoader(file, &loader)) && loader);
        CHECK(SUCCEEDED(IDWriteFontFileLoader_QueryInterface(loader, &IID_IDWriteLocalFontFileLoader, (void **)&local)) && local);
        BOOL supported = FALSE; DWRITE_FONT_FILE_TYPE ftype; DWRITE_FONT_FACE_TYPE fctype; UINT32 nfaces = 0;
        CHECK(SUCCEEDED(IDWriteFontFile_Analyze(file, &supported, &ftype, &fctype, &nfaces)) && supported && nfaces >= 1);
        CHECK(IDWriteFontFace_GetIndex(face) < nfaces && IDWriteFontFace_GetType(face) == fctype);
        if (local) {
            UINT32 plen = 0;
            CHECK(SUCCEEDED(IDWriteLocalFontFileLoader_GetFilePathLengthFromKey(local, key, keysize, &plen)) && plen > 3 && plen < 512);
            CHECK(IDWriteLocalFontFileLoader_GetFilePathFromKey(local, key, keysize, path, plen) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
            CHECK(SUCCEEDED(IDWriteLocalFontFileLoader_GetFilePathFromKey(local, key, keysize, path, 512)) && wcslen(path) == plen);
            IDWriteFontFileStream *st = NULL;
            CHECK(SUCCEEDED(IDWriteLocalFontFileLoader_CreateStreamFromKey(local, key, keysize, &st)) && st);
            if (st) {
                CHECK(SUCCEEDED(IDWriteFontFileStream_GetFileSize(st, &nbytes)) && nbytes > 1000);
                const void *frag = NULL; void *fctx = NULL;
                CHECK(SUCCEEDED(IDWriteFontFileStream_ReadFileFragment(st, &frag, 0, nbytes, &fctx)) && frag);
                if (frag) { bytes = malloc(nbytes); memcpy(bytes, frag, nbytes); }
                IDWriteFontFileStream_ReleaseFileFragment(st, fctx);
                const void *past = NULL; void *pctx = NULL;
                CHECK(FAILED(IDWriteFontFileStream_ReadFileFragment(st, &past, nbytes - 2, 4, &pctx)) && !past);
                IDWriteFontFileStream_Release(st);
            }
            IDWriteLocalFontFileLoader_Release(local);
        }
        if (bytes) {                      // the path is the file Windows programs open
            HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            CHECK(h != INVALID_HANDLE_VALUE);
            BYTE first[4] = { 0 }; DWORD got = 0;
            if (h != INVALID_HANDLE_VALUE) { ReadFile(h, first, 4, &got, NULL); CloseHandle(h); }
            CHECK(got == 4 && !memcmp(first, bytes, 4));
        }
        IDWriteFontFile *again = NULL;
        CHECK(SUCCEEDED(IDWriteFactory_CreateFontFileReference(f, path, NULL, &again)) && again);
        IDWriteFontFace *face2 = NULL;
        if (again) {
            CHECK(SUCCEEDED(IDWriteFactory_CreateFontFace(f, IDWriteFontFace_GetType(face), 1, &again, IDWriteFontFace_GetIndex(face), DWRITE_FONT_SIMULATIONS_NONE, &face2)) && face2);
            IDWriteFontFile_Release(again);
        }
        if (face2) {
            DWRITE_FONT_METRICS m2;
            IDWriteFontFace_GetMetrics(face2, &m2);
            CHECK(!memcmp(&m2, &m, sizeof m));
            CHECK(glyph(face2, 'H') == gi[0]);
            IDWriteFontFace_Release(face2);
        }
        CHECK(IDWriteFactory_CreateFontFileReference(f, L"C:\\no\\such\\font.ttf", NULL, &again) == DWRITE_E_FILENOTFOUND);
    }
    if (loader) IDWriteFontFileLoader_Release(loader);

    // The same bytes through a loader of the program's own: a face, and a collection of the file's faces.
    if (bytes) {
        mem_loader ml = { { &ml_vtbl }, 1, bytes, nbytes, 0 };
        IDWriteFontFile *mf = NULL;
        CHECK(IDWriteFactory_CreateCustomFontFileReference(f, "MEM1", 4, &ml.iface, &mf) == E_INVALIDARG);   // not registered yet
        CHECK(SUCCEEDED(IDWriteFactory_RegisterFontFileLoader(f, &ml.iface)));
        CHECK(IDWriteFactory_RegisterFontFileLoader(f, &ml.iface) == DWRITE_E_ALREADYREGISTERED);
        CHECK(SUCCEEDED(IDWriteFactory_CreateCustomFontFileReference(f, "MEM1", 4, &ml.iface, &mf)) && mf);
        if (mf) {
            BOOL supported = FALSE; DWRITE_FONT_FILE_TYPE ftype; DWRITE_FONT_FACE_TYPE fctype; UINT32 nfaces = 0;
            CHECK(SUCCEEDED(IDWriteFontFile_Analyze(mf, &supported, &ftype, &fctype, &nfaces)) && supported && nfaces >= 1);
            IDWriteFontFace *mface = NULL;
            CHECK(SUCCEEDED(IDWriteFactory_CreateFontFace(f, fctype, 1, &mf, IDWriteFontFace_GetIndex(face), DWRITE_FONT_SIMULATIONS_NONE, &mface)) && mface);
            CHECK(ml.streams >= 1 && fragments_out == 0);
            if (mface) {
                CHECK(glyph(mface, 'H') == gi[0] && glyph(mface, 'o') == gi[4]);
                DWRITE_GLYPH_METRICS mm;
                IDWriteFontFace_GetDesignGlyphMetrics(mface, gi, 1, &mm, FALSE);
                CHECK(mm.advanceWidth == gm[0].advanceWidth && mm.leftSideBearing == gm[0].leftSideBearing);
                IDWriteFontFace_Release(mface);
            }
            one_loader cl = { { &ol_vtbl }, mf };
            IDWriteFontCollection *custom = NULL;
            CHECK(IDWriteFactory_CreateCustomFontCollection(f, &cl.iface, "k", 1, &custom) == E_INVALIDARG);
            CHECK(SUCCEEDED(IDWriteFactory_RegisterFontCollectionLoader(f, &cl.iface)));
            CHECK(SUCCEEDED(IDWriteFactory_CreateCustomFontCollection(f, &cl.iface, "k", 1, &custom)) && custom);
            if (custom) {
                CHECK(IDWriteFontCollection_GetFontFamilyCount(custom) >= 1);
                UINT32 at; BOOL ex = FALSE;
                CHECK(SUCCEEDED(IDWriteFontCollection_FindFamilyName(custom, L"Helvetica", &at, &ex)) && ex);
                IDWriteFontFace *cface = face_of(custom, L"Helvetica", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, NULL);
                CHECK(cface && glyph(cface, 'e') == gi[1]);
                IDWriteFont *cfont = NULL;
                if (cface) { CHECK(SUCCEEDED(IDWriteFontCollection_GetFontFromFontFace(custom, cface, &cfont)) && cfont); IDWriteFontFace_Release(cface); }
                if (cfont) IDWriteFont_Release(cfont);
                IDWriteFontCollection_Release(custom);
            }
            IDWriteFactory_UnregisterFontCollectionLoader(f, &cl.iface);
            IDWriteFontFile_Release(mf);
        }
        CHECK(SUCCEEDED(IDWriteFactory_UnregisterFontFileLoader(f, &ml.iface)));
        CHECK(ml.refs == 1);
        free(bytes);
    }

    // Matching: a family with one upright face of normal weight gets bold and oblique simulated.
    int simulated = 0;
    for (UINT32 i = 0; i < nfam && !simulated; i++) {
        IDWriteFontFamily *fam = NULL; IDWriteFont *only = NULL, *sim = NULL;
        IDWriteFontCollection_GetFontFamily(sys, i, &fam);
        if (IDWriteFontFamily_GetFontCount(fam) == 1 && SUCCEEDED(IDWriteFontFamily_GetFont(fam, 0, &only)) &&
            IDWriteFont_GetWeight(only) == DWRITE_FONT_WEIGHT_NORMAL && IDWriteFont_GetStyle(only) == DWRITE_FONT_STYLE_NORMAL &&
            IDWriteFont_GetStretch(only) == DWRITE_FONT_STRETCH_NORMAL) {
            CHECK(SUCCEEDED(IDWriteFontFamily_GetFirstMatchingFont(fam, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_ITALIC, &sim)) && sim);
            if (sim) {
                CHECK(IDWriteFont_GetSimulations(sim) == (DWRITE_FONT_SIMULATIONS_BOLD | DWRITE_FONT_SIMULATIONS_OBLIQUE));
                IDWriteFontFace *sface = NULL;
                CHECK(SUCCEEDED(IDWriteFont_CreateFontFace(sim, &sface)) && sface);
                if (sface) { CHECK(IDWriteFontFace_GetSimulations(sface) == 3); IDWriteFontFace_Release(sface); }
                IDWriteFont_Release(sim);
                simulated = 1;
            }
        }
        if (only) IDWriteFont_Release(only);
        IDWriteFontFamily_Release(fam);
    }
    CHECK(simulated);

    // Fallback: made-up Latin, Japanese and emoji text with Helvetica as the base.
    IDWriteFontFallback *fb = NULL;
    CHECK(SUCCEEDED(IDWriteFactory2_GetSystemFontFallback(f2, &fb)) && fb);
    if (fb) {
        static const struct { const WCHAR *text; int base, all; } cases[] = {
            { L"Quiet fox", 1, 1 },
            { L"\u3053\u3093\u306b\u3061\u306f\u4e16\u754c", 0, 1 },    // Japanese
            { L"\U0001F600", 0, 1 },                                        // an emoji (a surrogate pair)
            { L"ab\u65e5\u672c", 1, 0 },                                   // Latin, then Japanese
            { L"\u0378x", 0, 0 },                                          // an unassigned code point: no font
        };
        for (int c = 0; c < 5; c++) {
            source s = { { &src_vtbl }, cases[c].text, (UINT32)wcslen(cases[c].text) };
            UINT32 mapped = 0; IDWriteFont *mf = NULL; FLOAT scale = 0;
            CHECK(SUCCEEDED(IDWriteFontFallback_MapCharacters(fb, &s.iface, 0, s.len, sys, L"Helvetica", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                             DWRITE_FONT_STRETCH_NORMAL, &mapped, &mf, &scale)));
            CHECK(scale == 1.0f && mapped >= 1 && mapped <= s.len);
            if (cases[c].all) CHECK(mapped == s.len);
            if (c == 3) CHECK(mapped == 2);
            if (c == 4) { CHECK(!mf && mapped == 1); continue; }
            CHECK(mf != NULL);
            if (!mf) continue;
            IDWriteFontFamily *fam = NULL;
            IDWriteFont_GetFontFamily(mf, &fam);
            WCHAR name[128];
            family_name(fam, name, 128);
            CHECK(cases[c].base ? !wcscmp(name, L"Helvetica") : wcscmp(name, L"Helvetica") != 0);
            BOOL has = FALSE;
            UINT32 cp = s.text[0] >= 0xD800 && s.text[0] < 0xDC00 ? 0x10000 + ((s.text[0] - 0xD800u) << 10) + (s.text[1] - 0xDC00u) : s.text[0];
            CHECK(SUCCEEDED(IDWriteFont_HasCharacter(mf, cp, &has)) && has);
            IDWriteFontFace *mface = NULL;
            IDWriteFont_CreateFontFace(mf, &mface);
            CHECK(mface && glyph(mface, cp) != 0);
            if (mface) IDWriteFontFace_Release(mface);
            IDWriteFontFamily_Release(fam);
            IDWriteFont_Release(mf);
            if (c == 1) {                      // the rest of a run after the first part
                source t = { { &src_vtbl }, L"ab\u65e5\u672c", 4 };
                UINT32 n2 = 0; IDWriteFont *f2font = NULL; FLOAT sc;
                CHECK(SUCCEEDED(IDWriteFontFallback_MapCharacters(fb, &t.iface, 2, 2, sys, L"Helvetica", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                                 DWRITE_FONT_STRETCH_NORMAL, &n2, &f2font, &sc)) && n2 == 2 && f2font);
                if (f2font) IDWriteFont_Release(f2font);
            }
        }
        IDWriteFontFallback_Release(fb);
    }
    // A Japanese family by its Japanese name (localized names in the name table).
    UINT32 jidx = 0, hidx = 0; BOOL jex = FALSE, hex = FALSE;
    IDWriteFontCollection_FindFamilyName(sys, L"Hiragino Sans", &hidx, &hex);
    if (hex) {
        CHECK(SUCCEEDED(IDWriteFontCollection_FindFamilyName(sys, L"\u30d2\u30e9\u30ae\u30ce\u89d2\u30b4\u30b7\u30c3\u30af", &jidx, &jex)) && jex && jidx == hidx);
        IDWriteFontFamily *hf = NULL; IDWriteLocalizedStrings *hn = NULL;
        IDWriteFontCollection_GetFontFamily(sys, hidx, &hf);
        if (hf && SUCCEEDED(IDWriteFontFamily_GetFamilyNames(hf, &hn)) && hn) {
            UINT32 at = 0; BOOL ja = FALSE;
            CHECK(SUCCEEDED(IDWriteLocalizedStrings_FindLocaleName(hn, L"ja-JP", &at, &ja)) && ja);
            IDWriteLocalizedStrings_Release(hn);
        }
        if (hf) IDWriteFontFamily_Release(hf);
    }

    // Outlines: 'H' (lines) and 'o' (curves) at 32 DIPs, y down from the baseline.
    sink s; sink_init(&s);
    float em = 32.0f, scale = em / m.designUnitsPerEm;
    CHECK(SUCCEEDED(IDWriteFontFace_GetGlyphRunOutline(face, em, gi, NULL, NULL, 1, FALSE, FALSE, (IDWriteGeometrySink *)&s)));
    CHECK(s.figures >= 1 && s.lines >= 8 && s.bad_order == 0 && s.ends_closed == s.figures && !s.open && s.fill_mode == 1);
    CHECK(fabsf(s.y0 + m.capHeight * scale) < 1.0f && fabsf(s.y1) < 0.5f);                       // cap height up to the baseline
    CHECK(fabsf(s.x0 - gm[0].leftSideBearing * scale) < 0.5f && fabsf(s.x1 - (gm[0].advanceWidth - gm[0].rightSideBearing) * scale) < 0.5f);
    sink o; sink_init(&o);
    CHECK(SUCCEEDED(IDWriteFontFace_GetGlyphRunOutline(face, em, &gi[4], NULL, NULL, 1, FALSE, FALSE, (IDWriteGeometrySink *)&o)));
    CHECK(o.figures == 2 && o.beziers > 0 && o.bad_order == 0);
    FLOAT advs[2] = { 50.0f, 50.0f };
    sink two; sink_init(&two);
    UINT16 hh[2] = { gi[0], gi[0] };
    CHECK(SUCCEEDED(IDWriteFontFace_GetGlyphRunOutline(face, em, hh, advs, NULL, 2, FALSE, FALSE, (IDWriteGeometrySink *)&two)));
    CHECK(two.figures == 2 * s.figures && fabsf(two.x1 - (s.x1 + 50.0f)) < 0.01f);

    // Rasterized: "Hello" at 24 DIPs, its baseline at (10, 40).
    RECT r; int bilevel;
    int cov = raster(f2, f, face, gi, 5, 24.0f, DWRITE_RENDERING_MODE_NATURAL, 0, DWRITE_TEXTURE_CLEARTYPE_3x1, &r, &bilevel);
    CHECK(cov > 50 && !bilevel);
    CHECK(r.left >= 9 && r.left <= 13 && r.bottom >= 39 && r.bottom <= 42 && r.top < 40 - 14 && r.top > 40 - 24 && r.right > 10 + 40 && r.right < 10 + 24 * 4);
    RECT none;
    CHECK(raster(f2, f, face, gi, 5, 24.0f, DWRITE_RENDERING_MODE_NATURAL, 0, DWRITE_TEXTURE_ALIASED_1x1, &none, &bilevel) == 0 && none.right == none.left);
    RECT ra;
    CHECK(raster(f2, f, face, gi, 5, 24.0f, DWRITE_RENDERING_MODE_ALIASED, 0, DWRITE_TEXTURE_ALIASED_1x1, &ra, &bilevel) > 50 && bilevel);
    CHECK(abs(ra.left - r.left) <= 1 && abs(ra.top - r.top) <= 1 && abs(ra.right - r.right) <= 1 && abs(ra.bottom - r.bottom) <= 1);
    CHECK(raster(f2, f, face, gi, 5, 24.0f, DWRITE_RENDERING_MODE_ALIASED, 0, DWRITE_TEXTURE_CLEARTYPE_3x1, &none, &bilevel) == 0 && none.right == none.left);
    RECT rg;
    CHECK(raster(f2, f, face, gi, 5, 24.0f, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, 1, DWRITE_TEXTURE_ALIASED_1x1, &rg, &bilevel) > 50 && !bilevel);
    CHECK(rg.left == r.left && rg.top == r.top && rg.right == r.right && rg.bottom == r.bottom);
    UINT16 space = glyph(face, ' ');
    RECT rs;
    CHECK(raster(f2, f, face, &space, 1, 24.0f, DWRITE_RENDERING_MODE_NATURAL, 0, DWRITE_TEXTURE_CLEARTYPE_3x1, &rs, &bilevel) == 0 && rs.right == rs.left);
    DWRITE_GLYPH_RUN bad = { face, 24.0f, 1, gi, NULL, NULL, FALSE, 0 };
    IDWriteGlyphRunAnalysis *ba = (IDWriteGlyphRunAnalysis *)1;
    CHECK(IDWriteFactory_CreateGlyphRunAnalysis(f, &bad, 1.0f, NULL, DWRITE_RENDERING_MODE_OUTLINE, DWRITE_MEASURING_MODE_NATURAL, 0, 0, &ba) == E_INVALIDARG && !ba);

    // Rendering parameters.
    IDWriteRenderingParams *rp = NULL;
    CHECK(SUCCEEDED(IDWriteFactory_CreateCustomRenderingParams(f, 2.2f, 1.0f, 0.5f, DWRITE_PIXEL_GEOMETRY_RGB, DWRITE_RENDERING_MODE_NATURAL, &rp)) && rp);
    if (rp) {
        CHECK(IDWriteRenderingParams_GetGamma(rp) == 2.2f && IDWriteRenderingParams_GetEnhancedContrast(rp) == 1.0f && IDWriteRenderingParams_GetClearTypeLevel(rp) == 0.5f);
        CHECK(IDWriteRenderingParams_GetPixelGeometry(rp) == DWRITE_PIXEL_GEOMETRY_RGB && IDWriteRenderingParams_GetRenderingMode(rp) == DWRITE_RENDERING_MODE_NATURAL);
        DWRITE_RENDERING_MODE mode = 99;
        CHECK(SUCCEEDED(IDWriteFontFace_GetRecommendedRenderingMode(face, 12.0f, 1.0f, DWRITE_MEASURING_MODE_NATURAL, rp, &mode)) && mode == DWRITE_RENDERING_MODE_NATURAL);
        CHECK(SUCCEEDED(IDWriteFontFace_GetRecommendedRenderingMode(face, 300.0f, 1.0f, DWRITE_MEASURING_MODE_NATURAL, NULL, &mode)) && mode == DWRITE_RENDERING_MODE_OUTLINE);
        IDWriteRenderingParams_Release(rp);
    }
    CHECK(IDWriteFactory_CreateCustomRenderingParams(f, -1.0f, 1.0f, 0.5f, DWRITE_PIXEL_GEOMETRY_RGB, DWRITE_RENDERING_MODE_NATURAL, &rp) == E_INVALIDARG);

    // Text formats and layouts: "Hello world" in Helvetica at 16 DIPs.
    IDWriteTextFormat *fmt = NULL;
    CHECK(IDWriteFactory_CreateTextFormat(f, L"Helvetica", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 0.0f, L"en-us", &fmt) == E_INVALIDARG && !fmt);
    CHECK(SUCCEEDED(IDWriteFactory_CreateTextFormat(f, L"Helvetica", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"en-us", &fmt)) && fmt);
    if (fmt) {
        WCHAR fam[32];
        CHECK(IDWriteTextFormat_GetFontSize(fmt) == 16.0f && IDWriteTextFormat_GetFontFamilyNameLength(fmt) == 9);
        CHECK(SUCCEEDED(IDWriteTextFormat_GetFontFamilyName(fmt, fam, 32)) && !wcscmp(fam, L"Helvetica"));
        CHECK(IDWriteTextFormat_GetFontFamilyName(fmt, fam, 9) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
        CHECK(IDWriteTextFormat_GetWordWrapping(fmt) == DWRITE_WORD_WRAPPING_WRAP && IDWriteTextFormat_GetTextAlignment(fmt) == DWRITE_TEXT_ALIGNMENT_LEADING);
        static const WCHAR text[] = L"Hello world";
        UINT32 cps[11]; UINT16 tg[11]; DWRITE_GLYPH_METRICS tm[11];
        for (int i = 0; i < 11; i++) cps[i] = text[i];
        IDWriteFontFace_GetGlyphIndices(face, cps, 11, tg);
        IDWriteFontFace_GetDesignGlyphMetrics(face, tg, 11, tm, FALSE);
        float k = 16.0f / m.designUnitsPerEm, hello = 0, world = 0, all = 0;
        for (int i = 0; i < 11; i++) { all += tm[i].advanceWidth * k; if (i < 5) hello += tm[i].advanceWidth * k; if (i > 5) world += tm[i].advanceWidth * k; }
        IDWriteTextLayout *lay = NULL;
        CHECK(SUCCEEDED(IDWriteFactory_CreateTextLayout(f, text, 11, fmt, 1000.0f, 100.0f, &lay)) && lay);
        if (lay) {
            DWRITE_TEXT_METRICS tmx;
            CHECK(SUCCEEDED(IDWriteTextLayout_GetMetrics(lay, &tmx)));
            CHECK(tmx.lineCount == 1 && fabsf(tmx.width - all) < 0.01f && fabsf(tmx.widthIncludingTrailingWhitespace - all) < 0.01f);
            float line_h = (m.ascent + m.descent + m.lineGap) * k;
            CHECK(fabsf(tmx.height - line_h) < 0.01f && tmx.left == 0 && tmx.top == 0 && tmx.layoutWidth == 1000.0f);
            DWRITE_LINE_METRICS lm[2]; UINT32 nl = 0;
            CHECK(SUCCEEDED(IDWriteTextLayout_GetLineMetrics(lay, lm, 2, &nl)) && nl == 1 && lm[0].length == 11 && fabsf(lm[0].baseline - m.ascent * k) < 0.01f);
            DWRITE_CLUSTER_METRICS cm[11]; UINT32 nc = 0;
            CHECK(SUCCEEDED(IDWriteTextLayout_GetClusterMetrics(lay, cm, 11, &nc)) && nc == 11 && cm[5].isWhitespace && cm[5].canWrapLineAfter && !cm[0].isWhitespace);
            FLOAT px, py; DWRITE_HIT_TEST_METRICS hm;
            CHECK(SUCCEEDED(IDWriteTextLayout_HitTestTextPosition(lay, 6, FALSE, &px, &py, &hm)) && fabsf(px - (hello + tm[5].advanceWidth * k)) < 0.01f && hm.textPosition == 6);
            BOOL trailing, inside;
            CHECK(SUCCEEDED(IDWriteTextLayout_HitTestPoint(lay, px + 0.5f, 5.0f, &trailing, &inside, &hm)) && inside && !trailing && hm.textPosition == 6);
            FLOAT minw = 0;
            CHECK(SUCCEEDED(IDWriteTextLayout_DetermineMinWidth(lay, &minw)) && fabsf(minw - (hello > world ? hello : world)) < 0.01f);
            renderer r = { { &renderer_vtbl } };
            CHECK(SUCCEEDED(IDWriteTextLayout_Draw(lay, NULL, &r.iface, 10.0f, 20.25f)));
            CHECK(r.runs >= 1 && r.glyphs == 11 && r.positions == 11 && r.faces == 1 && r.face[0] == face);
            CHECK(r.first_x == 10.0f && r.first_y == roundf(20.25f + m.ascent * k));     // the baseline on a whole pixel
            // narrower: "Hello " and "world" on two lines
            CHECK(SUCCEEDED(IDWriteTextLayout_SetMaxWidth(lay, all - 5.0f)));
            CHECK(SUCCEEDED(IDWriteTextLayout_GetMetrics(lay, &tmx)) && tmx.lineCount == 2);
            CHECK(SUCCEEDED(IDWriteTextLayout_GetLineMetrics(lay, lm, 2, &nl)) && nl == 2 && lm[0].length == 6 && lm[0].trailingWhitespaceLength == 1 && lm[1].length == 5);
            CHECK(fabsf(tmx.width - (hello > world ? hello : world)) < 0.01f && fabsf(tmx.height - 2 * line_h) < 0.02f);
            CHECK(IDWriteTextLayout_GetLineMetrics(lay, lm, 1, &nl) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && nl == 2);
            CHECK(SUCCEEDED(IDWriteTextLayout_SetTextAlignment(lay, DWRITE_TEXT_ALIGNMENT_TRAILING)) && SUCCEEDED(IDWriteTextLayout_GetMetrics(lay, &tmx)) && tmx.left > 0);
            // a range in a bigger size
            CHECK(SUCCEEDED(IDWriteTextLayout_SetMaxWidth(lay, 1000.0f)) && SUCCEEDED(IDWriteTextLayout_SetTextAlignment(lay, DWRITE_TEXT_ALIGNMENT_LEADING)));
            DWRITE_TEXT_RANGE first5 = { 0, 5 }, got;
            CHECK(SUCCEEDED(IDWriteTextLayout_SetFontSize(lay, 32.0f, first5)));
            FLOAT size = 0;
            CHECK(SUCCEEDED(IDWriteTextLayout_GetFontSize(lay, 2, &size, &got)) && size == 32.0f && got.startPosition == 0 && got.length == 5);
            CHECK(SUCCEEDED(IDWriteTextLayout_GetMetrics(lay, &tmx)) && fabsf(tmx.width - (all + hello)) < 0.02f && tmx.height > 1.9f * line_h);
            IDWriteTypography *typo = NULL;
            CHECK(SUCCEEDED(IDWriteFactory_CreateTypography(f, &typo)) && typo);
            if (typo) {
                DWRITE_FONT_FEATURE feat = { DWRITE_FONT_FEATURE_TAG_KERNING, 0 }, back;
                CHECK(SUCCEEDED(IDWriteTypography_AddFontFeature(typo, feat)) && IDWriteTypography_GetFontFeatureCount(typo) == 1);
                CHECK(SUCCEEDED(IDWriteTypography_GetFontFeature(typo, 0, &back)) && back.nameTag == DWRITE_FONT_FEATURE_TAG_KERNING);
                CHECK(SUCCEEDED(IDWriteTextLayout_SetTypography(lay, typo, first5)));
                IDWriteTypography_Release(typo);
            }
            IDWriteTextLayout_Release(lay);
        }
        // lines from newlines, and text in two fonts (Japanese after Latin: fallback)
        CHECK(SUCCEEDED(IDWriteFactory_CreateTextLayout(f, L"ab\ncd\n", 6, fmt, 1000.0f, 100.0f, &lay)) && lay);
        if (lay) {
            DWRITE_LINE_METRICS lm[4]; UINT32 nl = 0;
            CHECK(SUCCEEDED(IDWriteTextLayout_GetLineMetrics(lay, lm, 4, &nl)) && nl == 3 && lm[0].length == 3 && lm[0].newlineLength == 1 && lm[2].length == 0);
            IDWriteTextLayout_Release(lay);
        }
        CHECK(SUCCEEDED(IDWriteFactory_CreateTextLayout(f, L"Hi \u65e5\u672c", 5, fmt, 1000.0f, 100.0f, &lay)) && lay);
        if (lay) {
            renderer r = { { &renderer_vtbl } };
            CHECK(SUCCEEDED(IDWriteTextLayout_Draw(lay, NULL, &r.iface, 0.0f, 0.0f)) && r.runs == 2 && r.faces == 2 && r.glyphs == 5 && r.last_x > r.first_x);
            IDWriteTextLayout_Release(lay);
        }
        // GDI-compatible: whole-pixel advances; drawn into a bitmap render target
        CHECK(SUCCEEDED(IDWriteFactory_CreateGdiCompatibleTextLayout(f, text, 11, fmt, 1000.0f, 100.0f, 1.0f, NULL, FALSE, &lay)) && lay);
        IDWriteGdiInterop *gi2 = NULL;
        IDWriteFactory_GetGdiInterop(f, &gi2);
        IDWriteBitmapRenderTarget *target = NULL;
        CHECK(gi2 && SUCCEEDED(IDWriteGdiInterop_CreateBitmapRenderTarget(gi2, NULL, 200, 40, &target)) && target);
        if (lay && target) {
            DWRITE_TEXT_METRICS tmx;
            CHECK(SUCCEEDED(IDWriteTextLayout_GetMetrics(lay, &tmx)) && tmx.width == floorf(tmx.width) && fabsf(tmx.width - all) < 6.0f);
            SIZE sz;
            CHECK(SUCCEEDED(IDWriteBitmapRenderTarget_GetSize(target, &sz)) && sz.cx == 200 && sz.cy == 40);
            HDC dc = IDWriteBitmapRenderTarget_GetMemoryDC(target);
            CHECK(dc != NULL && IDWriteBitmapRenderTarget_GetPixelsPerDip(target) == 1.0f);
            renderer r = { { &renderer_vtbl } };
            CHECK(SUCCEEDED(IDWriteTextLayout_Draw(lay, target, &r.iface, 4.0f, 8.0f)));
            int lit = 0, outside = 0;
            for (int y = 0; y < 40; y++)
                for (int x = 0; x < 200; x++) {
                    COLORREF c = GetPixel(dc, x, y);
                    if (c && c != CLR_INVALID) { lit++; if (x < 4 || x > 4 + tmx.width + 2 || y < 8 || y > 8 + tmx.height + 1) outside++; }
                }
            CHECK(lit > 100 && outside == 0);
            // what programs do next: BitBlt the target's memory DC elsewhere, pixel for pixel
            BITMAPINFO bmi = { { sizeof(BITMAPINFOHEADER), 200, 40, 1, 32, BI_RGB, 0, 0, 0, 0, 0 } };   // (bottom-up)
            void *bits = NULL;
            HDC copy = CreateCompatibleDC(NULL);
            HBITMAP cb = CreateDIBSection(copy, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
            HGDIOBJ old = SelectObject(copy, cb);
            CHECK(BitBlt(copy, 0, 0, 200, 40, dc, 0, 0, SRCCOPY));
            int same = 1;
            for (int y = 0; y < 40 && same; y++)
                for (int x = 0; x < 200 && same; x++) {
                    DWORD px = ((DWORD *)bits)[(39 - y) * 200 + x] & 0xFFFFFF;
                    COLORREF c = GetPixel(dc, x, y);
                    same = px == ((c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF));
                    if (!same) printf("dwrite: render target copy differs at %d,%d: %06lx vs %06lx\n", x, y, (unsigned long)px, (unsigned long)c);
                }
            CHECK(same);
            SelectObject(copy, old); DeleteObject(cb); DeleteDC(copy);
            CHECK(SUCCEEDED(IDWriteBitmapRenderTarget_Resize(target, 300, 50)) && SUCCEEDED(IDWriteBitmapRenderTarget_GetSize(target, &sz)) && sz.cx == 300);
            printf("dwrite: \"Hello world\" drawn into the render target lights %d pixels\n", lit);
        }
        if (target) IDWriteBitmapRenderTarget_Release(target);
        if (gi2) IDWriteGdiInterop_Release(gi2);
        if (lay) IDWriteTextLayout_Release(lay);
        IDWriteTextFormat_Release(fmt);
    }

    // GDI interop: a LOGFONT and back.
    IDWriteGdiInterop *gdi = NULL;
    CHECK(SUCCEEDED(IDWriteFactory_GetGdiInterop(f, &gdi)) && gdi);
    if (gdi) {
        LOGFONTW lf = { 0 };
        lf.lfWeight = FW_BOLD;
        wcscpy(lf.lfFaceName, L"Helvetica");
        IDWriteFont *lfont = NULL;
        CHECK(SUCCEEDED(IDWriteGdiInterop_CreateFontFromLOGFONT(gdi, &lf, &lfont)) && lfont);
        if (lfont) {
            CHECK(IDWriteFont_GetWeight(lfont) == DWRITE_FONT_WEIGHT_BOLD);
            LOGFONTW back_lf; BOOL is_system = FALSE;
            CHECK(SUCCEEDED(IDWriteGdiInterop_ConvertFontToLOGFONT(gdi, lfont, &back_lf, &is_system)) && is_system);
            CHECK(!wcscmp(back_lf.lfFaceName, L"Helvetica") && back_lf.lfWeight == FW_BOLD && !back_lf.lfItalic);
            IDWriteFont_Release(lfont);
        }
        wcscpy(lf.lfFaceName, L"No Such Family Xq");
        CHECK(IDWriteGdiInterop_CreateFontFromLOGFONT(gdi, &lf, &lfont) == DWRITE_E_NOFONT);
        IDWriteGdiInterop_Release(gdi);
    }

    if (file) IDWriteFontFile_Release(file);
    IDWriteFontFace_Release(face);
    IDWriteFont_Release(font);
    IDWriteFontCollection_Release(sys);
    IDWriteFactory2_Release(f2);
    IDWriteFactory_Release(f);
    printf("dwrite: %d families; Hello at 24 DIPs covers %ld x %ld pixels -> %s\n", (int)nfam, r.right - r.left, r.bottom - r.top, failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
