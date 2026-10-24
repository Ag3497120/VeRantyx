// DirectWrite on CoreText (src/dwrite.m): the factory and the system font collection (families, names, matching),
// font faces (glyphs, metrics, tables, files through the local loader), a font given through a loader of the
// program's own (a custom font file and a custom collection), font fallback for made-up Latin, Japanese and emoji
// text, glyph outlines into a geometry sink, glyph runs rasterized into alpha textures, and text formats and layouts
// (measured, wrapped, hit-tested, drawn through a renderer into a GDI bitmap render target). The program's code that
// DirectWrite calls (a font file loader and its streams, a drawing effect's AddRef and Release) waits for a thread of
// the program's own that calls DirectWrite: it gets in (DirectWrite does not call the program holding a lock).
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <dwrite_3.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static int failures;
// A method by its slot's name: MinGW's C macros for a newer interface name the older version's slot, and that name is
// itself a macro when the older version has the method too (IDWriteFactory7_CreateCustomRenderingParams expands to
// ->IDWriteFactory3_CreateCustomRenderingParams(...), which expands again).
#define VCALL(obj, name) ((obj)->lpVtbl->name)
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

// ---- the program's code called by DirectWrite, waiting for a thread of the program's that calls DirectWrite -------------
// (as a browser's font loader may wait for another of its threads). The thread asks for the font of a face, which
// DirectWrite answers under its lock; the program's code waits up to 3 seconds for it.
static IDWriteFontCollection *probe_coll;
static IDWriteFontFace *probe_face;
static volatile LONG probe_calls, probe_stuck;
static HANDLE probe_threads[64];
static volatile LONG nprobe_threads;
static DWORD WINAPI probe_thread(void *arg) {
    (void)arg;
    IDWriteFont *font = NULL;
    if (SUCCEEDED(IDWriteFontCollection_GetFontFromFontFace(probe_coll, probe_face, &font)) && font) IDWriteFont_Release(font);
    return 0;
}
static void probe(void) {
    if (!probe_coll || probe_stuck) return;           // (one stuck thread is enough to fail: no more waits)
    HANDLE t = CreateThread(NULL, 0, probe_thread, NULL, 0, NULL);
    if (!t) { InterlockedIncrement(&probe_stuck); return; }
    InterlockedIncrement(&probe_calls);
    if (WaitForSingleObject(t, 3000) != WAIT_OBJECT_0) InterlockedIncrement(&probe_stuck);
    LONG i = InterlockedIncrement(&nprobe_threads) - 1;
    if (i < 64) probe_threads[i] = t; else { WaitForSingleObject(t, 10000); CloseHandle(t); }
}
static void probe_finish(void) {                    // the threads that were not waited for to the end
    for (LONG i = 0; i < nprobe_threads && i < 64; i++) { WaitForSingleObject(probe_threads[i], 10000); CloseHandle(probe_threads[i]); }
    nprobe_threads = 0;
}

// a drawing effect (any IUnknown of the program's)
typedef struct { IUnknown iface; LONG refs; int probing; } effect;
static HRESULT WINAPI fx_qi(IUnknown *u, REFIID iid, void **out) { (void)iid; *out = u; IUnknown_AddRef(u); return S_OK; }
static ULONG WINAPI fx_addref(IUnknown *u) { effect *e = (effect *)u; if (e->probing) probe(); return InterlockedIncrement(&e->refs); }
static ULONG WINAPI fx_release(IUnknown *u) { effect *e = (effect *)u; if (e->probing) probe(); return InterlockedDecrement(&e->refs); }
static IUnknownVtbl fx_vtbl = { fx_qi, fx_addref, fx_release };

// ---- a font file loader of the program's own: a font's bytes in memory ------------------------------------------------------
typedef struct { IDWriteFontFileStream iface; LONG refs; const BYTE *data; UINT64 size; int probing; } mem_stream;
static HRESULT WINAPI ms_qi(IDWriteFontFileStream *s, REFIID iid, void **out) { (void)iid; *out = s; IDWriteFontFileStream_AddRef(s); return S_OK; }
static ULONG WINAPI ms_addref(IDWriteFontFileStream *s) { return InterlockedIncrement(&((mem_stream *)s)->refs); }
static ULONG WINAPI ms_release(IDWriteFontFileStream *s) {
    mem_stream *m = (mem_stream *)s;
    if (m->probing) probe();
    LONG r = InterlockedDecrement(&m->refs);
    if (!r) HeapFree(GetProcessHeap(), 0, s);
    return r;
}
static int fragments_out;
static HRESULT WINAPI ms_read(IDWriteFontFileStream *s, const void **start, UINT64 off, UINT64 n, void **ctx) {
    mem_stream *m = (mem_stream *)s;
    if (m->probing) probe();
    if (off > m->size || n > m->size - off) return E_FAIL;
    *start = m->data + off; *ctx = (void *)1; fragments_out++;
    return S_OK;
}
static void WINAPI ms_release_fragment(IDWriteFontFileStream *s, void *ctx) { if (((mem_stream *)s)->probing) probe(); if (ctx == (void *)1) fragments_out--; }
static HRESULT WINAPI ms_size(IDWriteFontFileStream *s, UINT64 *n) { if (((mem_stream *)s)->probing) probe(); *n = ((mem_stream *)s)->size; return S_OK; }
static HRESULT WINAPI ms_time(IDWriteFontFileStream *s, UINT64 *t) { (void)s; *t = 0; return E_NOTIMPL; }
static IDWriteFontFileStreamVtbl ms_vtbl = { ms_qi, ms_addref, ms_release, ms_read, ms_release_fragment, ms_size, ms_time };

// (probing: its streams wait for a probe in each method; probe_release: its Release does)
typedef struct { IDWriteFontFileLoader iface; LONG refs; const BYTE *data; UINT64 size; int streams, probing, probe_release; } mem_loader;
static HRESULT WINAPI ml_qi(IDWriteFontFileLoader *l, REFIID iid, void **out) { (void)iid; *out = l; return S_OK; }
static ULONG WINAPI ml_addref(IDWriteFontFileLoader *l) { return InterlockedIncrement(&((mem_loader *)l)->refs); }
static ULONG WINAPI ml_release(IDWriteFontFileLoader *l) { if (((mem_loader *)l)->probe_release) probe(); return InterlockedDecrement(&((mem_loader *)l)->refs); }
static HRESULT WINAPI ml_stream(IDWriteFontFileLoader *l, const void *key, UINT32 n, IDWriteFontFileStream **out) {
    mem_loader *m = (mem_loader *)l;
    if (m->probing) probe();
    if (n != 4 || memcmp(key, "MEM1", 4)) return E_INVALIDARG;
    mem_stream *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *s);
    s->iface.lpVtbl = &ms_vtbl; s->refs = 1; s->data = m->data; s->size = m->size; s->probing = m->probing;
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
typedef struct { IDWriteTextRenderer iface; int runs, glyphs, faces, no_offsets, moved; IDWriteFontFace *face[8]; float first_x, first_y, last_x; UINT32 positions; } renderer;
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
    if (!run->glyphOffsets) r->no_offsets++;              // (DirectWrite's layouts hand their runs offsets, all 0 here)
    else for (UINT32 i = 0; i < run->glyphCount; i++) r->moved += run->glyphOffsets[i].advanceOffset != 0 || run->glyphOffsets[i].ascenderOffset != 0;
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

// The pixels with coverage of a glyph run, and the box the outline gives them. raster_ink: the coverage summed (a
// pixel's three subpixels as their mean), raster_colour: 3x1 pixels whose subpixels differ.
static double raster_ink;
static int raster_colour;
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
    raster_ink = 0; raster_colour = 0;
    if (w > 0 && h > 0) {
        BYTE *buf = malloc((size_t)w * h * bpp);
        CHECK(IDWriteGlyphRunAnalysis_CreateAlphaTexture(a, type, bounds, buf, (UINT32)(w * h * bpp) - 1) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
        CHECK(SUCCEEDED(IDWriteGlyphRunAnalysis_CreateAlphaTexture(a, type, bounds, buf, (UINT32)(w * h * bpp))));
        int top = 0, bottom = 0, left = 0, right = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w * bpp; x++) {
                BYTE v = buf[(size_t)y * w * bpp + x];
                raster_ink += v / (double)bpp;
                if (!v) continue;
                covered++;
                if (v != 255) *only_bilevel = 0;
                if (y == 0) top = 1;
                if (y == h - 1) bottom = 1;
                if (x / bpp == 0) left = 1;
                if (x / bpp == w - 1) right = 1;
            }
        CHECK(top && bottom && left && right);              // the bounds are tight: coverage on each edge
        if (bpp == 3)
            for (int i = 0; i + 2 < w * h * 3; i += 3) raster_colour += buf[i] != buf[i + 1] || buf[i] != buf[i + 2];
        if (bpp == 3 && grayscale) CHECK(raster_colour == 0);       // grayscale coverage in each subpixel
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

// ---- the newer versions: IDWriteFactory3..7, IDWriteFontFace3..5, IDWriteFont3, collections, font sets and their
// builders, font face references and resources, the in-memory loader, WOFF, fallback builders --------------------------------
typedef struct { IUnknown iface; LONG refs; } counted;          // an owner of bytes, a download listener: the program's
static HRESULT WINAPI cu_qi(IUnknown *u, REFIID iid, void **out) { (void)iid; *out = u; IUnknown_AddRef(u); return S_OK; }
static ULONG WINAPI cu_addref(IUnknown *u) { return InterlockedIncrement(&((counted *)u)->refs); }
static ULONG WINAPI cu_release(IUnknown *u) { return InterlockedDecrement(&((counted *)u)->refs); }
static IUnknownVtbl cu_vtbl = { cu_qi, cu_addref, cu_release };

static UINT16 rd16(const BYTE *p) { return (UINT16)(p[0] << 8 | p[1]); }
static UINT32 rd32(const BYTE *p) { return (UINT32)p[0] << 24 | (UINT32)p[1] << 16 | (UINT32)p[2] << 8 | p[3]; }
static void wr16(BYTE *p, UINT16 v) { p[0] = (BYTE)(v >> 8); p[1] = (BYTE)v; }
static void wr32(BYTE *p, UINT32 v) { p[0] = (BYTE)(v >> 24); p[1] = (BYTE)(v >> 16); p[2] = (BYTE)(v >> 8); p[3] = (BYTE)v; }
// A zlib stream (RFC 1950, 1951) of 1 + 258 k copies of a byte: the byte as a literal, then k matches of length 258 at
// distance 1, in one block with the fixed Huffman codes (written most significant bit first into the LSB-first stream).
typedef struct { BYTE *p; UINT32 bit; } bitw;
static void put_bits(bitw *b, UINT32 v, int n) { for (int i = 0; i < n; i++, b->bit++) if (v >> i & 1) b->p[b->bit >> 3] |= (BYTE)(1 << (b->bit & 7)); }
static void put_code(bitw *b, UINT32 code, int n) { for (int i = n - 1; i >= 0; i--) put_bits(b, code >> i & 1, 1); }
static UINT32 zlib_run(BYTE *out, BYTE v, UINT32 k) {           // (out: 32 + 2 k bytes)
    memset(out, 0, 32 + 2 * k);
    out[0] = 0x78; out[1] = 0x01;
    bitw b = { out + 2, 0 };
    put_bits(&b, 1, 1); put_bits(&b, 1, 2);                   // the last block, fixed codes
    if (v < 144) put_code(&b, 0x30 + v, 8); else put_code(&b, 0x190 + v - 144, 9);
    for (UINT32 i = 0; i < k; i++) { put_code(&b, 0xC5, 8); put_code(&b, 0, 5); }    // length 258 (285), distance 1 (0)
    put_code(&b, 0, 7);                                       // end of block (256)
    UINT32 n = 2 + (b.bit + 7) / 8, a = 1, s = 0;
    for (UINT32 i = 0; i < 1 + 258 * k; i++) { a = (a + v) % 65521; s = (s + a) % 65521; }
    wr32(out + n, s << 16 | a);                               // Adler-32
    return n + 4;
}
// A WOFF file's totalSfntSize: the sfnt header and records, and its tables padded to 4 bytes.
static void woff_set_total(BYTE *w) {
    UINT16 n = rd16(w + 12);
    UINT32 t = 12 + 16 * n;
    for (UINT16 i = 0; i < n; i++) t += (rd32(w + 44 + 20 * i + 12) + 3) & ~3u;
    wr32(w + 16, t);
}
// A WOFF 1.0 file of a single-face font: its tables stored as they are, and a made-up table 'zqzq' of 775 'Q's
// compressed.
static BYTE *make_woff(const BYTE *sfnt, UINT32 *size) {
    UINT16 n = rd16(sfnt + 4);
    BYTE z[64];
    UINT32 zn = zlib_run(z, 'Q', 3), total = 44 + 20 * (n + 1) + ((zn + 3) & ~3u);
    for (UINT16 i = 0; i < n; i++) total += (rd32(sfnt + 12 + 16 * i + 12) + 3) & ~3u;
    BYTE *w = calloc(1, total);
    memcpy(w, "wOFF", 4); wr32(w + 4, rd32(sfnt)); wr32(w + 8, total); wr16(w + 12, (UINT16)(n + 1)); wr16(w + 20, 1);
    UINT32 at = 44 + 20 * (n + 1);
    for (UINT16 i = 0; i <= n; i++) {
        BYTE *e = w + 44 + 20 * i;
        if (i < n) {
            const BYTE *r = sfnt + 12 + 16 * i;
            UINT32 len = rd32(r + 12);
            memcpy(e, r, 4); wr32(e + 4, at); wr32(e + 8, len); wr32(e + 12, len); memcpy(e + 16, r + 4, 4);
            memcpy(w + at, sfnt + rd32(r + 8), len);
            at += (len + 3) & ~3u;
        } else {
            memcpy(e, "zqzq", 4); wr32(e + 4, at); wr32(e + 8, zn); wr32(e + 12, 775);
            memcpy(w + at, z, zn);
        }
    }
    woff_set_total(w);
    *size = total;
    return w;
}
// A made-up WOFF file of count tables (tags 'q', 'z' and the table's number) whose data is a zlib stream of 1 + 258 k
// 'Q's: one stream all the tables point at (shared), or a copy of it for each. Its totalSfntSize is what the tables add
// up to (all ones when that does not fit 32 bits).
static BYTE *make_runs_woff(UINT32 count, UINT32 k, int shared, UINT32 *size) {
    BYTE *z = malloc(32 + 2 * (size_t)k);
    UINT32 zn = zlib_run(z, 'Q', k), za = (zn + 3) & ~3u, olen = 1 + 258 * k;
    UINT32 total = 44 + 20 * count + za * (shared ? 1 : count);
    BYTE *w = calloc(1, total);
    memcpy(w, "wOFF", 4); wr32(w + 4, 0x00010000); wr32(w + 8, total); wr16(w + 12, (UINT16)count); wr16(w + 20, 1);
    UINT64 sfnt = 12 + 16ull * count + (UINT64)count * ((olen + 3) & ~3u);
    wr32(w + 16, sfnt > 0xFFFFFFFFu ? 0xFFFFFFFFu : (UINT32)sfnt);
    UINT32 at = 44 + 20 * count;
    for (UINT32 i = 0; i < count; i++) {
        BYTE *e = w + 44 + 20 * i;
        e[0] = 'q'; e[1] = 'z'; e[2] = (BYTE)(i >> 8); e[3] = (BYTE)i;
        wr32(e + 4, at); wr32(e + 8, zn); wr32(e + 12, olen);
        if (!shared || !i) memcpy(w + at, z, zn);
        if (!shared) at += za;
    }
    free(z);
    *size = total;
    return w;
}
// UnpackFontFile (WOFF) of a copy of a WOFF file with one field changed (a u32 at at, or a u16 when wide is 0); a
// failure that still hands back a stream, or a success without one, counts as E_UNEXPECTED.
static HRESULT unpack_changed(IDWriteFactory7 *f7, const BYTE *w, UINT32 n, UINT32 at, UINT32 v, int wide) {
    BYTE *c = malloc(n);
    memcpy(c, w, n);
    if (at + (wide ? 4 : 2) <= n) { if (wide) wr32(c + at, v); else wr16(c + at, (UINT16)v); }
    IDWriteFontFileStream *st = (IDWriteFontFileStream *)(UINT_PTR)1;
    HRESULT hr = IDWriteFactory7_UnpackFontFile(f7, DWRITE_CONTAINER_TYPE_WOFF, c, n, &st);
    if (SUCCEEDED(hr)) { if (st && st != (IDWriteFontFileStream *)(UINT_PTR)1) IDWriteFontFileStream_Release(st); else hr = E_UNEXPECTED; }
    else if (st) hr = E_UNEXPECTED;
    free(c);
    return hr;
}
static const BYTE *table_of(const BYTE *d, UINT32 n, const char *tag, UINT32 *len) {
    for (UINT16 i = 0; i < rd16(d + 4) && 12 + 16u * (i + 1) <= n; i++) {
        const BYTE *r = d + 12 + 16 * i;
        if (!memcmp(r, tag, 4) && (UINT64)rd32(r + 8) + rd32(r + 12) <= n) { *len = rd32(r + 12); return d + rd32(r + 8); }
    }
    return NULL;
}
// The bytes of a face's file (through its loader), and the file's type.
static BYTE *face_bytes(IDWriteFontFace *face, UINT32 *size, DWRITE_FONT_FILE_TYPE *type) {
    UINT32 n = 1; IDWriteFontFile *file = NULL; BYTE *out = NULL;
    *size = 0;
    if (FAILED(IDWriteFontFace_GetFiles(face, &n, &file)) || !file) return NULL;
    const void *key; UINT32 ks; IDWriteFontFileLoader *ld = NULL; IDWriteFontFileStream *st = NULL;
    BOOL sup; DWRITE_FONT_FACE_TYPE ft; UINT32 nf;
    IDWriteFontFile_Analyze(file, &sup, type, &ft, &nf);
    if (SUCCEEDED(IDWriteFontFile_GetReferenceKey(file, &key, &ks)) && SUCCEEDED(IDWriteFontFile_GetLoader(file, &ld)) &&
        SUCCEEDED(IDWriteFontFileLoader_CreateStreamFromKey(ld, key, ks, &st))) {
        UINT64 sz = 0; const void *frag; void *ctx;
        if (SUCCEEDED(IDWriteFontFileStream_GetFileSize(st, &sz)) && SUCCEEDED(IDWriteFontFileStream_ReadFileFragment(st, &frag, 0, sz, &ctx))) {
            out = malloc(sz); memcpy(out, frag, sz); *size = (UINT32)sz;
            IDWriteFontFileStream_ReleaseFileFragment(st, ctx);
        }
        IDWriteFontFileStream_Release(st);
    }
    if (ld) IDWriteFontFileLoader_Release(ld);
    IDWriteFontFile_Release(file);
    return out;
}
// A made-up collection ('ttcf', one face) of a single-face font's tables, its face listing extra more records that
// each name the font's largest table's bytes again (tags 'z', 'z' and the record's number).
static BYTE *make_ttc(const BYTE *sfnt, UINT32 extra, UINT32 *size) {
    UINT16 m = rd16(sfnt + 4);
    UINT32 dir = 16, at = dir + 12 + 16 * (m + extra), total = at, big = 0, big_at = 0;
    for (UINT16 i = 0; i < m; i++) total += (rd32(sfnt + 12 + 16 * i + 12) + 3) & ~3u;
    BYTE *w = calloc(1, total);
    memcpy(w, "ttcf", 4); wr32(w + 4, 0x00010000); wr32(w + 8, 1); wr32(w + 12, dir);
    memcpy(w + dir, sfnt, 12); wr16(w + dir + 4, (UINT16)(m + extra));
    for (UINT16 i = 0; i < m; i++) {
        const BYTE *r = sfnt + 12 + 16 * i;
        BYTE *e = w + dir + 12 + 16 * i;
        UINT32 len = rd32(r + 12);
        memcpy(e, r, 16); wr32(e + 8, at);
        memcpy(w + at, sfnt + rd32(r + 8), len);
        if (len > big) { big = len; big_at = at; }
        at += (len + 3) & ~3u;
    }
    for (UINT32 i = 0; i < extra; i++) {
        BYTE *e = w + dir + 12 + 16 * (m + i);
        e[0] = 'z'; e[1] = 'z'; e[2] = (BYTE)(i >> 8); e[3] = (BYTE)i;
        wr32(e + 8, big_at); wr32(e + 12, big);
    }
    *size = total;
    return w;
}
static IDWriteFontFace *face_named(IDWriteFontCollection *c, const WCHAR *family) { return face_of(c, family, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, NULL); }
static void en_name(IDWriteLocalizedStrings *s, WCHAR *out, UINT32 cap) {
    UINT32 i = 0; BOOL ex = FALSE;
    out[0] = 0;
    if (!s) return;
    IDWriteLocalizedStrings_FindLocaleName(s, L"en-us", &i, &ex);
    IDWriteLocalizedStrings_GetString(s, ex ? i : 0, out, cap);
}
static UINT32 count_fonts(IDWriteFontCollection *c, const WCHAR *family) {
    UINT32 i; BOOL ex = FALSE; IDWriteFontFamily *fam = NULL;
    if (FAILED(IDWriteFontCollection_FindFamilyName(c, family, &i, &ex)) || !ex || FAILED(IDWriteFontCollection_GetFontFamily(c, i, &fam))) return 0;
    UINT32 n = IDWriteFontFamily_GetFontCount(fam);
    IDWriteFontFamily_Release(fam);
    return n;
}

static void newer_versions(IDWriteFactory *f, IDWriteFontCollection *sys, IDWriteFontFace *face, IDWriteFont *font, const UINT16 *gi, const BYTE *bytes, UINT64 nbytes) {
    // Every factory version is the shared factory.
    IDWriteFactory7 *f7 = NULL;
    CHECK(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory7, (IUnknown **)&f7)) && (void *)f7 == (void *)f);
    const GUID *versions[] = { &IID_IDWriteFactory3, &IID_IDWriteFactory4, &IID_IDWriteFactory5, &IID_IDWriteFactory6 };
    for (int i = 0; i < 4; i++) { void *x = NULL; CHECK(SUCCEEDED(IDWriteFactory_QueryInterface(f, versions[i], &x)) && x == (void *)f); if (x) IUnknown_Release((IUnknown *)x); }
    IDWriteFontFace5 *f5 = NULL;
    CHECK(SUCCEEDED(IDWriteFontFace_QueryInterface(face, &IID_IDWriteFontFace5, (void **)&f5)) && f5);
    if (!f7 || !f5) { if (f7) IDWriteFactory7_Release(f7); if (f5) IDWriteFontFace5_Release(f5); return; }
    IDWriteFactory3 *f3 = (IDWriteFactory3 *)f7;
    WCHAR nm[64];

    // IDWriteFontFace3: properties, names, locality, rendering modes
    CHECK(IDWriteFontFace5_GetWeight(f5) == IDWriteFont_GetWeight(font) && IDWriteFontFace5_GetStyle(f5) == DWRITE_FONT_STYLE_NORMAL &&
          IDWriteFontFace5_GetStretch(f5) == DWRITE_FONT_STRETCH_NORMAL);
    IDWriteLocalizedStrings *names = NULL;
    CHECK(SUCCEEDED(IDWriteFontFace5_GetFamilyNames(f5, &names)) && names);
    en_name(names, nm, 64);
    CHECK(!wcscmp(nm, L"Helvetica"));
    if (names) IDWriteLocalizedStrings_Release(names);
    names = NULL;
    CHECK(SUCCEEDED(IDWriteFontFace5_GetFaceNames(f5, &names)) && names && IDWriteLocalizedStrings_GetCount(names) >= 1);
    if (names) IDWriteLocalizedStrings_Release(names);
    BOOL exists = FALSE;
    names = NULL;
    CHECK(SUCCEEDED(IDWriteFontFace5_GetInformationalStrings(f5, DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME, &names, &exists)) && exists && names);
    en_name(names, nm, 64);
    CHECK(!wcscmp(nm, L"Helvetica"));
    if (names) IDWriteLocalizedStrings_Release(names);
    CHECK(IDWriteFontFace5_HasCharacter(f5, 'H') && !IDWriteFontFace5_HasCharacter(f5, 0x0378));
    CHECK(IDWriteFontFace5_IsCharacterLocal(f5, 'H') && IDWriteFontFace5_IsGlyphLocal(f5, gi[0]));
    BOOL local = FALSE;
    CHECK(SUCCEEDED(IDWriteFontFace5_AreCharactersLocal(f5, L"Hello", 5, FALSE, &local)) && local);
    local = FALSE;
    CHECK(SUCCEEDED(IDWriteFontFace5_AreGlyphsLocal(f5, gi, 5, FALSE, &local)) && local);
    DWRITE_PANOSE p1, p2;
    memset(&p1, 0xEE, sizeof p1); memset(&p2, 0, sizeof p2);
    IDWriteFontFace5_GetPanose(f5, &p1);
    IDWriteFont1 *font1 = NULL;
    if (SUCCEEDED(IDWriteFont_QueryInterface(font, &IID_IDWriteFont1, (void **)&font1)) && font1) { IDWriteFont1_GetPanose(font1, &p2); IDWriteFont1_Release(font1); }
    CHECK(!memcmp(&p1, &p2, sizeof p1));
    DWRITE_RENDERING_MODE1 mode1 = 99; DWRITE_GRID_FIT_MODE grid = 99;
    CHECK(SUCCEEDED(VCALL(f5, IDWriteFontFace3_GetRecommendedRenderingMode)(f5, 300.0f, 96.0f, 96.0f, NULL, FALSE, DWRITE_OUTLINE_THRESHOLD_ANTIALIASED,
                                                                DWRITE_MEASURING_MODE_NATURAL, NULL, &mode1, &grid)) && mode1 == DWRITE_RENDERING_MODE1_OUTLINE && grid == DWRITE_GRID_FIT_MODE_DISABLED);
    IDWriteRenderingParams3 *rp3 = NULL;
    CHECK(SUCCEEDED(VCALL(f7, IDWriteFactory3_CreateCustomRenderingParams)(f7, 1.8f, 0.5f, 1.0f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT, DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC_DOWNSAMPLED,
                                                               DWRITE_GRID_FIT_MODE_DISABLED, &rp3)) && rp3);
    if (rp3) {
        CHECK(IDWriteRenderingParams3_GetRenderingMode1(rp3) == DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC_DOWNSAMPLED);
        CHECK(IDWriteRenderingParams3_GetRenderingMode(rp3) == DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC);
        CHECK(SUCCEEDED(VCALL(f5, IDWriteFontFace3_GetRecommendedRenderingMode)(f5, 12.0f, 96.0f, 96.0f, NULL, FALSE, DWRITE_OUTLINE_THRESHOLD_ANTIALIASED,
                                                                    DWRITE_MEASURING_MODE_NATURAL, (IDWriteRenderingParams *)rp3, &mode1, &grid)) && mode1 == DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC_DOWNSAMPLED);
        DWRITE_RENDERING_MODE mode0 = 99;
        CHECK(SUCCEEDED(IDWriteFontFace_GetRecommendedRenderingMode(face, 12.0f, 1.0f, DWRITE_MEASURING_MODE_NATURAL, (IDWriteRenderingParams *)rp3, &mode0)) &&
              mode0 == DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC);
        IDWriteRenderingParams3_Release(rp3);
    }
    CHECK(VCALL(f7, IDWriteFactory3_CreateCustomRenderingParams)(f7, 1.8f, 0.5f, 1.0f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT, 8, DWRITE_GRID_FIT_MODE_DISABLED, &rp3) == E_INVALIDARG);
    DWRITE_GLYPH_RUN run = { face, 24.0f, 5, gi, NULL, NULL, FALSE, 0 };
    IDWriteGlyphRunAnalysis *an = NULL;
    CHECK(SUCCEEDED(VCALL(f7, IDWriteFactory3_CreateGlyphRunAnalysis)(f7, &run, NULL, DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC_DOWNSAMPLED, DWRITE_MEASURING_MODE_NATURAL,
                                                          DWRITE_GRID_FIT_MODE_DISABLED, DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0, 20, &an)) && an);
    if (an) { RECT r; CHECK(SUCCEEDED(IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(an, DWRITE_TEXTURE_ALIASED_1x1, &r)) && r.right - r.left > 40); IDWriteGlyphRunAnalysis_Release(an); }

    // IDWriteFontFace4: outline images only; IDWriteFontFace5: no axes
    DWRITE_GLYPH_IMAGE_FORMATS formats = IDWriteFontFace5_GetGlyphImageFormats(f5), gformats = 0;
    CHECK((formats & (DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE | DWRITE_GLYPH_IMAGE_FORMATS_CFF)) && !(formats & ~3u));
    CHECK(SUCCEEDED(IDWriteFontFace5_GetGlyphImageFormats_(f5, gi[0], 0, 100, &gformats)) && gformats == formats);
    DWRITE_GLYPH_IMAGE_DATA img; void *img_ctx = (void *)1;
    CHECK(FAILED(IDWriteFontFace5_GetGlyphImageData(f5, gi[0], 32, DWRITE_GLYPH_IMAGE_FORMATS_PNG, &img, &img_ctx)) && !img_ctx && !img.imageData);
    CHECK(IDWriteFontFace5_GetFontAxisValueCount(f5) == 0 && !IDWriteFontFace5_HasVariations(f5));
    CHECK(SUCCEEDED(IDWriteFontFace5_GetFontAxisValues(f5, NULL, 0)));
    CHECK(IDWriteFontFace5_IsColorFont(f5) == FALSE && IDWriteFontFace5_GetColorPaletteCount(f5) == 0);
    IDWriteColorGlyphRunEnumerator1 *layers = (IDWriteColorGlyphRunEnumerator1 *)1;
    D2D1_POINT_2F origin = { 10.0f, 30.0f };
    CHECK(VCALL(f7, IDWriteFactory4_TranslateColorGlyphRun)(f7, origin, &run, NULL, DWRITE_GLYPH_IMAGE_FORMATS_COLR | DWRITE_GLYPH_IMAGE_FORMATS_PNG, DWRITE_MEASURING_MODE_NATURAL,
                                                NULL, 0, &layers) == DWRITE_E_NOCOLOR && !layers);

    // Its resource and reference: the same file and face again
    IDWriteFontResource *res = NULL;
    CHECK(SUCCEEDED(IDWriteFontFace5_GetFontResource(f5, &res)) && res);
    if (res) {
        CHECK(IDWriteFontResource_GetFontFaceIndex(res) == IDWriteFontFace_GetIndex(face) && IDWriteFontResource_GetFontAxisCount(res) == 0 && !IDWriteFontResource_HasVariations(res));
        IDWriteFontFace5 *again = NULL;
        CHECK(SUCCEEDED(IDWriteFontResource_CreateFontFace(res, DWRITE_FONT_SIMULATIONS_NONE, NULL, 0, &again)) && again);
        if (again) { CHECK(IDWriteFontFace5_Equals(f5, (IDWriteFontFace *)again)); IDWriteFontFace5_Release(again); }
        CHECK(SUCCEEDED(IDWriteFontResource_CreateFontFace(res, DWRITE_FONT_SIMULATIONS_BOLD, NULL, 0, &again)) && again);
        if (again) { CHECK(!IDWriteFontFace5_Equals(f5, (IDWriteFontFace *)again) && IDWriteFontFace5_GetSimulations(again) == DWRITE_FONT_SIMULATIONS_BOLD); IDWriteFontFace5_Release(again); }
        IDWriteLocalizedStrings *axis_names = (IDWriteLocalizedStrings *)1;
        CHECK(IDWriteFontResource_GetAxisNames(res, 0, &axis_names) == E_INVALIDARG && !axis_names);
        IDWriteFontResource_Release(res);
    }
    IDWriteFontFaceReference *ref = NULL;
    CHECK(SUCCEEDED(IDWriteFontFace5_GetFontFaceReference(f5, &ref)) && ref);
    if (ref) {
        CHECK(IDWriteFontFaceReference_GetFontFaceIndex(ref) == IDWriteFontFace_GetIndex(face) && IDWriteFontFaceReference_GetSimulations(ref) == 0);
        CHECK(IDWriteFontFaceReference_GetFileSize(ref) == nbytes && IDWriteFontFaceReference_GetLocalFileSize(ref) == nbytes);
        CHECK(IDWriteFontFaceReference_GetLocality(ref) == DWRITE_LOCALITY_LOCAL && SUCCEEDED(IDWriteFontFaceReference_EnqueueFontDownloadRequest(ref)));
        FILETIME ft = { 0, 0 };
        CHECK(SUCCEEDED(IDWriteFontFaceReference_GetFileTime(ref, &ft)) && (ft.dwHighDateTime || ft.dwLowDateTime));
        IDWriteFontFace3 *rf = NULL;
        CHECK(SUCCEEDED(IDWriteFontFaceReference_CreateFontFace(ref, &rf)) && (void *)rf == (void *)face);
        if (rf) IDWriteFontFace3_Release(rf);
        rf = NULL;
        CHECK(SUCCEEDED(IDWriteFontFaceReference_CreateFontFaceWithSimulations(ref, DWRITE_FONT_SIMULATIONS_OBLIQUE, &rf)) && rf &&
              IDWriteFontFace3_GetSimulations(rf) == DWRITE_FONT_SIMULATIONS_OBLIQUE && IDWriteFontFace3_GetStyle(rf) == DWRITE_FONT_STYLE_OBLIQUE);
        if (rf) IDWriteFontFace3_Release(rf);
        // the same face of the same file, by its path
        IDWriteFontFile *file = NULL; UINT32 one = 1;
        IDWriteFontFace_GetFiles(face, &one, &file);
        const void *key = NULL; UINT32 ks = 0;
        if (file) IDWriteFontFile_GetReferenceKey(file, &key, &ks);
        IDWriteFontFaceReference *by_path = NULL, *by_file = NULL;
        if (key && ks > 8) {
            CHECK(SUCCEEDED(IDWriteFactory3_CreateFontFaceReference(f3, (const WCHAR *)((const BYTE *)key + 8), NULL, IDWriteFontFace_GetIndex(face), DWRITE_FONT_SIMULATIONS_NONE, &by_path)) && by_path);
            if (by_path) { CHECK(IDWriteFontFaceReference_Equals(ref, by_path)); IDWriteFontFaceReference_Release(by_path); }
        }
        if (file) {
            CHECK(SUCCEEDED(IDWriteFactory3_CreateFontFaceReference_(f3, file, IDWriteFontFace_GetIndex(face), DWRITE_FONT_SIMULATIONS_BOLD, &by_file)) && by_file);
            if (by_file) { CHECK(!IDWriteFontFaceReference_Equals(ref, by_file)); IDWriteFontFaceReference_Release(by_file); }
            IDWriteFontFile_Release(file);
        }
        IDWriteFontFaceReference_Release(ref);
    }

    // IDWriteFont3, IDWriteFontFamily2, IDWriteFontList2, IDWriteFontCollection3
    IDWriteFontCollection3 *c3 = NULL;
    CHECK(SUCCEEDED(IDWriteFontCollection_QueryInterface(sys, &IID_IDWriteFontCollection3, (void **)&c3)) && c3);
    UINT32 helvetica_fonts = count_fonts(sys, L"Helvetica"), all_fonts = 0;
    if (c3) {
        CHECK(IDWriteFontCollection3_GetFontFamilyModel(c3) == DWRITE_FONT_FAMILY_MODEL_WEIGHT_STRETCH_STYLE);
        HANDLE expiry = IDWriteFontCollection3_GetExpirationEvent(c3);
        CHECK(expiry != NULL && WaitForSingleObject(expiry, 0) == WAIT_TIMEOUT);
        for (UINT32 i = 0; i < IDWriteFontCollection3_GetFontFamilyCount(c3); i++) {
            IDWriteFontFamily2 *fam = NULL;
            if (SUCCEEDED(VCALL(c3, IDWriteFontCollection2_GetFontFamily)(c3, i, &fam)) && fam) { all_fonts += IDWriteFontFamily2_GetFontCount(fam); IDWriteFontFamily2_Release(fam); }
        }
        DWRITE_FONT_AXIS_VALUE bold = { DWRITE_FONT_AXIS_TAG_WEIGHT, 700.0f };
        IDWriteFontList2 *list = NULL;
        CHECK(SUCCEEDED(IDWriteFontCollection3_GetMatchingFonts(c3, L"helvetica", &bold, 1, &list)) && list);
        if (list) {
            CHECK(IDWriteFontList2_GetFontCount(list) == helvetica_fonts && helvetica_fonts >= 2);
            IDWriteFont3 *first = NULL;
            CHECK(SUCCEEDED(VCALL(list, IDWriteFontList1_GetFont)(list, 0, &first)) && first && IDWriteFont3_GetWeight(first) == DWRITE_FONT_WEIGHT_BOLD);
            CHECK(IDWriteFontList2_GetFontLocality(list, 0) == DWRITE_LOCALITY_LOCAL);
            if (first) {
                CHECK(IDWriteFont3_Equals(first, (IDWriteFont *)first) && !IDWriteFont3_Equals(first, font));
                CHECK(IDWriteFont3_HasCharacter(first, 'Q') && !IDWriteFont3_HasCharacter(first, 0x0378) && IDWriteFont3_GetLocality(first) == DWRITE_LOCALITY_LOCAL);
                IDWriteFontFaceReference *fr = NULL;
                CHECK(SUCCEEDED(IDWriteFont3_GetFontFaceReference(first, &fr)) && fr);
                if (fr) {
                    IDWriteFontFace3 *ff = NULL, *ff2 = NULL;
                    CHECK(SUCCEEDED(IDWriteFontFaceReference_CreateFontFace(fr, &ff)) && ff && SUCCEEDED(IDWriteFont3_CreateFontFace(first, &ff2)) && ff == ff2);
                    if (ff) IDWriteFontFace3_Release(ff);
                    if (ff2) IDWriteFontFace3_Release(ff2);
                    IDWriteFontFaceReference_Release(fr);
                }
                IDWriteFont3_Release(first);
            }
            IDWriteFontSet1 *ls = NULL;
            CHECK(SUCCEEDED(IDWriteFontList2_GetFontSet(list, &ls)) && ls && IDWriteFontSet1_GetFontCount(ls) == helvetica_fonts);
            if (ls) IDWriteFontSet1_Release(ls);
            IDWriteFontList2_Release(list);
        }
        CHECK(SUCCEEDED(IDWriteFontCollection3_GetMatchingFonts(c3, L"No Such Family Xq", NULL, 0, &list)) && list && IDWriteFontList2_GetFontCount(list) == 0);
        if (list) IDWriteFontList2_Release(list);
        UINT32 hidx = 0; BOOL hex = FALSE; IDWriteFontFamily2 *hfam = NULL;
        IDWriteFontCollection3_FindFamilyName(c3, L"Helvetica", &hidx, &hex);
        if (hex && SUCCEEDED(VCALL(c3, IDWriteFontCollection2_GetFontFamily)(c3, hidx, &hfam)) && hfam) {
            DWRITE_FONT_AXIS_VALUE italic = { DWRITE_FONT_AXIS_TAG_ITALIC, 1.0f };
            CHECK(SUCCEEDED(IDWriteFontFamily2_GetMatchingFonts(hfam, &italic, 1, &list)) && list);
            IDWriteFont3 *first = NULL;
            if (list && SUCCEEDED(VCALL(list, IDWriteFontList1_GetFont)(list, 0, &first)) && first) {
                CHECK(IDWriteFont3_GetStyle(first) != DWRITE_FONT_STYLE_NORMAL && IDWriteFont3_GetWeight(first) == DWRITE_FONT_WEIGHT_NORMAL);
                IDWriteFont3_Release(first);
            }
            if (list) IDWriteFontList2_Release(list);
            CHECK(IDWriteFontFamily2_GetFontLocality(hfam, 0) == DWRITE_LOCALITY_LOCAL);
            IDWriteFontFaceReference *fr = NULL;
            CHECK(SUCCEEDED(IDWriteFontFamily2_GetFontFaceReference(hfam, 0, &fr)) && fr);
            if (fr) IDWriteFontFaceReference_Release(fr);
            IDWriteFontSet1 *fs = NULL;
            CHECK(SUCCEEDED(IDWriteFontFamily2_GetFontSet(hfam, &fs)) && fs && IDWriteFontSet1_GetFontCount(fs) == helvetica_fonts);
            if (fs) IDWriteFontSet1_Release(fs);
            IDWriteFontFamily2_Release(hfam);
        }
    }

    // The system font set: the system collection's fonts
    IDWriteFontSet3 *set = NULL;
    CHECK(SUCCEEDED(IDWriteFactory7_GetSystemFontSet(f7, FALSE, (IDWriteFontSet2 **)&set)) && set);
    IDWriteFontSet *set0 = (IDWriteFontSet *)set;
    if (set) {
        CHECK(IDWriteFontSet3_GetFontCount(set) == all_fonts && all_fonts > 100);
        HANDLE expiry = IDWriteFontSet3_GetExpirationEvent(set);
        CHECK(expiry != NULL && WaitForSingleObject(expiry, 0) == WAIT_TIMEOUT);
        UINT32 at = UINT32_MAX; BOOL found = FALSE;
        CHECK(SUCCEEDED(IDWriteFontSet3_FindFontFace(set, face, &at, &found)) && found && at < all_fonts);
        if (found) {
            IDWriteFontFace5 *sf = NULL;
            CHECK(SUCCEEDED(IDWriteFontSet3_CreateFontFace(set, at, &sf)) && (void *)sf == (void *)face);
            if (sf) IDWriteFontFace5_Release(sf);
            IDWriteFontFaceReference1 *sr = NULL;
            CHECK(SUCCEEDED(VCALL(set, IDWriteFontSet1_GetFontFaceReference)(set, at, &sr)) && sr);
            if (sr) {
                UINT32 at2 = UINT32_MAX; BOOL found2 = FALSE;
                CHECK(SUCCEEDED(IDWriteFontSet3_FindFontFaceReference(set, (IDWriteFontFaceReference *)sr, &at2, &found2)) && found2 && at2 == at);
                CHECK(IDWriteFontFaceReference1_GetFontAxisValueCount(sr) == 0);
                IDWriteFontFaceReference1_Release(sr);
            }
            IDWriteLocalizedStrings *ps = NULL; BOOL has = FALSE;
            CHECK(SUCCEEDED(IDWriteFontSet3_GetPropertyValues(set, at, DWRITE_FONT_PROPERTY_ID_POSTSCRIPT_NAME, &has, &ps)) && has && ps);
            en_name(ps, nm, 64);
            CHECK(!wcscmp(nm, L"Helvetica"));
            if (ps) IDWriteLocalizedStrings_Release(ps);
            CHECK(SUCCEEDED(IDWriteFontSet3_GetPropertyValues(set, at, DWRITE_FONT_PROPERTY_ID_WEIGHT, &has, &ps)) && has && ps);
            en_name(ps, nm, 64);
            CHECK(!wcscmp(nm, L"400"));
            if (ps) IDWriteLocalizedStrings_Release(ps);
            CHECK(SUCCEEDED(IDWriteFontSet3_GetPropertyValues(set, at, DWRITE_FONT_PROPERTY_ID_SEMANTIC_TAG, &has, &ps)) && !has && !ps);
            CHECK(IDWriteFontSet3_GetFontSourceType(set, at) == DWRITE_FONT_SOURCE_TYPE_PER_MACHINE && IDWriteFontSet3_GetFontSourceNameLength(set, at) == 0);
            CHECK(IDWriteFontSet3_GetFontLocality(set, at) == DWRITE_LOCALITY_LOCAL);
            IDWriteFontResource *sres = NULL;
            CHECK(SUCCEEDED(IDWriteFontSet3_CreateFontResource(set, at, &sres)) && sres);
            if (sres) IDWriteFontResource_Release(sres);
            DWRITE_FONT_AXIS_RANGE ranges[4]; UINT32 nranges = 99;
            CHECK(SUCCEEDED(IDWriteFontSet3_GetFontAxisRanges_(set, at, ranges, 4, &nranges)) && nranges == 0);
        }
        // a family's fonts, nearest a weight first
        IDWriteFontSet *hs = NULL;
        CHECK(SUCCEEDED(IDWriteFontSet_GetMatchingFonts_(set0, L"Helvetica", DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &hs)) && hs);
        if (hs) {
            CHECK(IDWriteFontSet_GetFontCount(hs) == helvetica_fonts);
            IDWriteLocalizedStrings *w = NULL; BOOL has = FALSE;
            CHECK(SUCCEEDED(IDWriteFontSet_GetPropertyValues(hs, 0, DWRITE_FONT_PROPERTY_ID_WEIGHT, &has, &w)) && has);
            en_name(w, nm, 64);
            CHECK(!wcscmp(nm, L"700"));
            if (w) IDWriteLocalizedStrings_Release(w);
            IDWriteStringList *families = NULL;
            CHECK(SUCCEEDED(IDWriteFontSet_GetPropertyValues__(hs, DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FAMILY_NAME, &families)) && families);
            if (families) {
                UINT32 len = 0;
                CHECK(IDWriteStringList_GetCount(families) == 1 && SUCCEEDED(IDWriteStringList_GetString(families, 0, nm, 64)) && !wcscmp(nm, L"Helvetica"));
                CHECK(SUCCEEDED(IDWriteStringList_GetStringLength(families, 0, &len)) && len == 9 && IDWriteStringList_GetString(families, 0, nm, 9) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
                CHECK(SUCCEEDED(IDWriteStringList_GetLocaleName(families, 0, nm, 64)) && !wcscmp(nm, L"en-us"));
                IDWriteStringList_Release(families);
            }
            IDWriteFontSet_Release(hs);
        }
        DWRITE_FONT_PROPERTY fam = { DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FAMILY_NAME, L"HELVETICA", NULL };
        DWRITE_FONT_PROPERTY both[2] = { fam, { DWRITE_FONT_PROPERTY_ID_WEIGHT, L"700", L"" } };
        UINT32 occ = 0;
        CHECK(SUCCEEDED(IDWriteFontSet_GetPropertyOccurrenceCount(set0, &fam, &occ)) && occ == helvetica_fonts);
        CHECK(SUCCEEDED(IDWriteFontSet_GetMatchingFonts(set0, both, 2, &hs)) && hs && IDWriteFontSet_GetFontCount(hs) >= 1 && IDWriteFontSet_GetFontCount(hs) < helvetica_fonts);
        if (hs) IDWriteFontSet_Release(hs);
        UINT32 idx[64], nidx = 0;
        CHECK(SUCCEEDED(IDWriteFontSet3_GetFilteredFontIndices(set, &fam, 1, FALSE, idx, 64, &nidx)) && nidx == helvetica_fonts);
        CHECK(IDWriteFontSet3_GetFilteredFontIndices(set, &fam, 1, FALSE, idx, 1, &nidx) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && nidx == helvetica_fonts);
        IDWriteFontSet1 *sub = NULL;
        CHECK(SUCCEEDED(IDWriteFontSet3_GetFilteredFonts__(set, idx, 2, &sub)) && sub && IDWriteFontSet1_GetFontCount(sub) == 2);
        if (sub) IDWriteFontSet1_Release(sub);
        DWRITE_FONT_AXIS_RANGE wr = { DWRITE_FONT_AXIS_TAG_WEIGHT, 100.0f, 900.0f };
        CHECK(SUCCEEDED(IDWriteFontSet3_GetFilteredFonts_(set, &wr, 1, FALSE, &sub)) && sub && IDWriteFontSet1_GetFontCount(sub) == 0);   // (no font has axes)
        if (sub) IDWriteFontSet1_Release(sub);
        DWRITE_FONT_AXIS_VALUE wv = { DWRITE_FONT_AXIS_TAG_WEIGHT, 700.0f };
        CHECK(SUCCEEDED(VCALL(set, IDWriteFontSet1_GetMatchingFonts)(set, &fam, &wv, 1, &sub)) && sub && IDWriteFontSet1_GetFontCount(sub) == helvetica_fonts);
        if (sub) {
            IDWriteLocalizedStrings *w = NULL; BOOL has = FALSE;
            IDWriteFontSet1_GetPropertyValues(sub, 0, DWRITE_FONT_PROPERTY_ID_WEIGHT, &has, &w);
            en_name(w, nm, 64);
            CHECK(has && !wcscmp(nm, L"700"));
            if (w) IDWriteLocalizedStrings_Release(w);
            IDWriteFontSet1_Release(sub);
        }
        // (fewer: the named instances of a variable font are its one face)
        CHECK(SUCCEEDED(IDWriteFontSet3_GetFirstFontResources(set, &sub)) && sub && IDWriteFontSet1_GetFontCount(sub) >= all_fonts / 2 && IDWriteFontSet1_GetFontCount(sub) <= all_fonts);
        if (sub) IDWriteFontSet1_Release(sub);
        IDWriteFontCollection1 *from_set = NULL;
        CHECK(SUCCEEDED(IDWriteFactory3_CreateFontCollectionFromFontSet(f3, set0, &from_set)) && (void *)from_set == (void *)sys);
        if (from_set) IDWriteFontCollection1_Release(from_set);
        // GDI interop by the set: a LOGFONT's fonts
        IDWriteGdiInterop1 *gdi1 = NULL;
        IDWriteFactory_GetGdiInterop(f, (IDWriteGdiInterop **)&gdi1);
        LOGFONTW lf = { 0 };
        lf.lfWeight = FW_BOLD; wcscpy(lf.lfFaceName, L"Helvetica");
        if (gdi1) {
            IDWriteFontSet *byl = NULL;
            CHECK(SUCCEEDED(IDWriteGdiInterop1_GetMatchingFontsByLOGFONT(gdi1, &lf, set0, &byl)) && byl && IDWriteFontSet_GetFontCount(byl) == helvetica_fonts);
            if (byl) {
                IDWriteLocalizedStrings *w = NULL; BOOL has = FALSE;
                IDWriteFontSet_GetPropertyValues(byl, 0, DWRITE_FONT_PROPERTY_ID_WEIGHT, &has, &w);
                en_name(w, nm, 64);
                CHECK(has && !wcscmp(nm, L"700"));
                if (w) IDWriteLocalizedStrings_Release(w);
                IDWriteFontSet_Release(byl);
            }
            FONTSIGNATURE sig, sig2;
            memset(&sig, 0xEE, sizeof sig); memset(&sig2, 0, sizeof sig2);
            CHECK(SUCCEEDED(IDWriteGdiInterop1_GetFontSignature(gdi1, font, &sig)) && (sig.fsUsb[0] & 1));     // Basic Latin
            CHECK(SUCCEEDED(IDWriteGdiInterop1_GetFontSignature_(gdi1, face, &sig2)) && !memcmp(&sig, &sig2, sizeof sig));
            IDWriteFont *lfont = NULL;
            CHECK(SUCCEEDED(IDWriteGdiInterop1_CreateFontFromLOGFONT(gdi1, &lf, NULL, &lfont)) && lfont && IDWriteFont_GetWeight(lfont) == DWRITE_FONT_WEIGHT_BOLD);
            if (lfont) IDWriteFont_Release(lfont);
            // the face of the font a DC has selected
            HDC dc = CreateCompatibleDC(NULL);
            HFONT hf = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, 0, 0, L"Helvetica");
            HGDIOBJ old = dc && hf ? SelectObject(dc, hf) : NULL;
            IDWriteFontFace *dcface = NULL;
            CHECK(dc && hf && SUCCEEDED(IDWriteGdiInterop1_CreateFontFaceFromHdc(gdi1, dc, &dcface)) && dcface == face);
            if (dcface) IDWriteFontFace_Release(dcface);
            if (old) SelectObject(dc, old);
            if (hf) DeleteObject(hf);
            if (dc) DeleteDC(dc);
            IDWriteGdiInterop1_Release(gdi1);
        }
    }

    // Kerning pairs from 'kern' (Helvetica: Apple's version; Arial: OpenType's), none where there is no such table
    static const WCHAR *const kerned[] = { L"Helvetica", L"Arial" };
    int kern_checked = 0;
    for (int k = 0; k < 2; k++) {
        IDWriteFontFace *kf = face_named(sys, kerned[k]);
        IDWriteFontFace1 *kf1 = NULL;
        if (kf && SUCCEEDED(IDWriteFontFace_QueryInterface(kf, &IID_IDWriteFontFace1, (void **)&kf1)) && IDWriteFontFace1_HasKerningPairs(kf1)) {
            UINT32 cps[4] = { 'A', 'V', 'A', 'x' }; UINT16 g[4]; INT32 adj[4] = { 7, 7, 7, 7 };
            IDWriteFontFace1_GetGlyphIndices(kf1, cps, 4, g);
            CHECK(SUCCEEDED(IDWriteFontFace1_GetKerningPairAdjustments(kf1, 4, g, adj)));
            CHECK(adj[0] < 0 && adj[3] == 0);                        // A V closer; the last glyph has nothing after it
            DWRITE_FONT_METRICS1 km;
            IDWriteFontFace1_GetMetrics(kf1, &km);
            printf("dwrite: %ls kerns A-V by %d of %u units per em\n", kerned[k], (int)adj[0], km.designUnitsPerEm);
            kern_checked++;
        }
        if (kf1) IDWriteFontFace1_Release(kf1);
        if (kf) IDWriteFontFace_Release(kf);
    }
    CHECK(kern_checked == 2);
    // Vertical glyph variants from GSUB 'vert' (a font with them: a corner bracket turns; Latin stays)
    static const WCHAR *const vertical[] = { L"Arial Unicode MS", L"Hiragino Sans GB", L"Apple SD Gothic Neo" };
    int vert_checked = 0;
    for (int v = 0; v < 3 && !vert_checked; v++) {
        IDWriteFontFace *vf = face_named(sys, vertical[v]);
        IDWriteFontFace1 *vf1 = NULL;
        if (vf && SUCCEEDED(IDWriteFontFace_QueryInterface(vf, &IID_IDWriteFontFace1, (void **)&vf1)) && IDWriteFontFace1_HasVerticalGlyphVariants(vf1)) {
            UINT32 cps[2] = { 0x300C, 'A' }; UINT16 g[2], vg[2];
            IDWriteFontFace1_GetGlyphIndices(vf1, cps, 2, g);
            CHECK(g[0] && SUCCEEDED(IDWriteFontFace1_GetVerticalGlyphVariants(vf1, 2, g, vg)) && vg[0] != g[0] && vg[1] == g[1]);
            vert_checked = 1;
        }
        if (vf1) IDWriteFontFace1_Release(vf1);
        if (vf) IDWriteFontFace_Release(vf);
    }
    CHECK(vert_checked);
    IDWriteFontFace1 *hf1 = NULL;
    if (SUCCEEDED(IDWriteFontFace_QueryInterface(face, &IID_IDWriteFontFace1, (void **)&hf1))) {
        UINT16 vg[5];
        CHECK(!IDWriteFontFace1_HasVerticalGlyphVariants(hf1) && SUCCEEDED(IDWriteFontFace1_GetVerticalGlyphVariants(hf1, 5, gi, vg)) && !memcmp(vg, gi, sizeof vg));
        IDWriteFontFace1_Release(hf1);
    }

    // Glyph origins: by the advances, offsets added; leftwards right to left
    D2D1_POINT_2F org[5];
    DWRITE_GLYPH_METRICS gm[5];
    DWRITE_FONT_METRICS fm;
    IDWriteFontFace_GetMetrics(face, &fm);
    IDWriteFontFace_GetDesignGlyphMetrics(face, gi, 5, gm, FALSE);
    CHECK(SUCCEEDED(IDWriteFactory7_ComputeGlyphOrigins_(f7, &run, origin, org)));
    float x = 10.0f, k = 24.0f / fm.designUnitsPerEm;
    int ok = 1;
    for (int i = 0; i < 5; i++) { ok &= fabsf(org[i].x - x) < 0.01f && org[i].y == 30.0f; x += gm[i].advanceWidth * k; }
    CHECK(ok);
    DWRITE_GLYPH_OFFSET offs[5] = { { 0, 0 }, { 2.0f, 3.0f }, { 0, 0 }, { 0, 0 }, { 0, 0 } };
    FLOAT advs[5] = { 10, 10, 10, 10, 10 };
    DWRITE_GLYPH_RUN rtl = { face, 24.0f, 5, gi, advs, offs, FALSE, 1 };
    CHECK(SUCCEEDED(IDWriteFactory7_ComputeGlyphOrigins(f7, &rtl, DWRITE_MEASURING_MODE_NATURAL, origin, NULL, org)));
    CHECK(org[0].x == 0.0f && org[1].x == -12.0f && org[1].y == 27.0f && org[4].x == -40.0f);

    // A builder: a reference with a family name of the set's own, and the faces of a font file in memory
    IDWriteFontSetBuilder2 *b = NULL;
    CHECK(SUCCEEDED(VCALL(f7, IDWriteFactory6_CreateFontSetBuilder)(f7, &b)) && b);
    IDWriteInMemoryFontFileLoader *mem = NULL;
    CHECK(SUCCEEDED(IDWriteFactory7_CreateInMemoryFontFileLoader(f7, &mem)) && mem);
    counted owner = { { &cu_vtbl }, 1 };
    if (b && mem) {
        CHECK(SUCCEEDED(IDWriteFactory_RegisterFontFileLoader(f, (IDWriteFontFileLoader *)mem)));
        IDWriteFontFile *mf = NULL, *mf2 = NULL;
        CHECK(SUCCEEDED(IDWriteInMemoryFontFileLoader_CreateInMemoryFontFileReference(mem, f, bytes, (UINT32)nbytes, &owner.iface, &mf)) && mf && owner.refs == 2);
        CHECK(IDWriteInMemoryFontFileLoader_GetFileCount(mem) == 1);
        UINT32 key0 = 0;
        CHECK(SUCCEEDED(IDWriteFactory_CreateCustomFontFileReference(f, &key0, 4, (IDWriteFontFileLoader *)mem, &mf2)) && mf2);
        UINT32 key9 = 9;
        IDWriteFontFile *none = (IDWriteFontFile *)1;
        CHECK(IDWriteFactory_CreateCustomFontFileReference(f, &key9, 4, (IDWriteFontFileLoader *)mem, &none) == E_INVALIDARG && !none);
        if (mf && mf2) {
            BOOL sup = FALSE; DWRITE_FONT_FILE_TYPE ft; DWRITE_FONT_FACE_TYPE fct; UINT32 nfaces = 0;
            CHECK(SUCCEEDED(IDWriteFontFile_Analyze(mf2, &sup, &ft, &fct, &nfaces)) && sup && nfaces >= 1);
            IDWriteFontFileLoader *back = NULL;
            CHECK(SUCCEEDED(IDWriteFontFile_GetLoader(mf, &back)) && (void *)back == (void *)mem);
            if (back) IDWriteFontFileLoader_Release(back);
            IDWriteFontFace *mface = NULL;
            CHECK(SUCCEEDED(IDWriteFactory_CreateFontFace(f, fct, 1, &mf, IDWriteFontFace_GetIndex(face), DWRITE_FONT_SIMULATIONS_NONE, &mface)) && mface);
            if (mface) { CHECK(glyph(mface, 'H') == gi[0]); IDWriteFontFace_Release(mface); }
            IDWriteFontFaceReference *href = NULL;
            IDWriteFontFace5_GetFontFaceReference(f5, &href);
            DWRITE_FONT_PROPERTY own = { DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FAMILY_NAME, L"Quuxian Sans", L"en-us" };
            CHECK(href && SUCCEEDED(IDWriteFontSetBuilder2_AddFontFaceReference_(b, href, &own, 1)));
            CHECK(SUCCEEDED(b->lpVtbl->AddFontFile(b, mf2)));          // (IDWriteFontSetBuilder1's, by the file)
            if (href) IDWriteFontFaceReference_Release(href);
            IDWriteFontSet *bs = NULL;
            CHECK(SUCCEEDED(IDWriteFontSetBuilder2_CreateFontSet(b, &bs)) && bs && IDWriteFontSet_GetFontCount(bs) == 1 + nfaces);
            if (bs) {
                UINT32 occ = 0;
                DWRITE_FONT_PROPERTY q = { DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FAMILY_NAME, L"quuxian sans", NULL };
                CHECK(SUCCEEDED(IDWriteFontSet_GetPropertyOccurrenceCount(bs, &q, &occ)) && occ == 1);
                IDWriteFontCollection1 *bc = NULL;
                CHECK(SUCCEEDED(IDWriteFactory3_CreateFontCollectionFromFontSet(f3, bs, &bc)) && bc && (void *)bc != (void *)sys);
                if (bc) {
                    UINT32 at; BOOL ex = FALSE;
                    CHECK(SUCCEEDED(IDWriteFontCollection1_FindFamilyName(bc, L"Quuxian Sans", &at, &ex)) && ex);
                    IDWriteFontFace *qf = face_of((IDWriteFontCollection *)bc, L"Quuxian Sans", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, NULL);
                    IDWriteFontFace5 *qf5 = NULL;
                    if (qf) IDWriteFontFace_QueryInterface(qf, &IID_IDWriteFontFace5, (void **)&qf5);
                    CHECK(qf5 && IDWriteFontFace5_Equals(qf5, face));               // Helvetica's face, by another name
                    if (qf5) IDWriteFontFace5_Release(qf5);
                    if (qf) IDWriteFontFace_Release(qf);
                    CHECK(SUCCEEDED(IDWriteFontCollection1_FindFamilyName(bc, L"Helvetica", &at, &ex)) && ex);   // (the file in memory's faces)
                    IDWriteFontSet *cs = NULL;
                    CHECK(SUCCEEDED(IDWriteFontCollection1_GetFontSet(bc, &cs)) && cs && IDWriteFontSet_GetFontCount(cs) == 1 + nfaces);
                    if (cs) IDWriteFontSet_Release(cs);
                    IDWriteFontCollection1_Release(bc);
                }
                IDWriteFontSet_Release(bs);
            }
        }
        if (mf2) IDWriteFontFile_Release(mf2);
        if (mf) IDWriteFontFile_Release(mf);
        IDWriteFontSetBuilder2_Release(b); b = NULL;
        CHECK(SUCCEEDED(IDWriteFactory_UnregisterFontFileLoader(f, (IDWriteFontFileLoader *)mem)));
        IDWriteInMemoryFontFileLoader_Release(mem); mem = NULL;
        CHECK(owner.refs == 1);                                       // (the loader and its files gone: the owner let go)
    }
    if (b) IDWriteFontSetBuilder2_Release(b);
    if (mem) IDWriteInMemoryFontFileLoader_Release(mem);

    // WOFF: a single-face font's tables stored, one made-up table compressed; unpacked to the font again
    BYTE woff2[8] = { 'w', 'O', 'F', '2', 0, 0, 0, 0 };
    CHECK(IDWriteFactory7_AnalyzeContainerType(f7, woff2, 8) == DWRITE_CONTAINER_TYPE_WOFF2 && IDWriteFactory7_AnalyzeContainerType(f7, bytes, 8) == DWRITE_CONTAINER_TYPE_UNKNOWN);
    IDWriteFontFace *arial = face_named(sys, L"Arial");
    UINT32 asize = 0; DWRITE_FONT_FILE_TYPE atype = DWRITE_FONT_FILE_TYPE_UNKNOWN;
    BYTE *abytes = arial ? face_bytes(arial, &asize, &atype) : NULL;
    if (abytes && atype == DWRITE_FONT_FILE_TYPE_TRUETYPE) {
        UINT32 wsize = 0;
        BYTE *woff = make_woff(abytes, &wsize);
        CHECK(IDWriteFactory7_AnalyzeContainerType(f7, woff, wsize) == DWRITE_CONTAINER_TYPE_WOFF);
        IDWriteFontFileStream *st = NULL;
        CHECK(IDWriteFactory7_UnpackFontFile(f7, DWRITE_CONTAINER_TYPE_WOFF2, woff, wsize, &st) == DWRITE_E_FILEFORMAT && !st);
        CHECK(SUCCEEDED(IDWriteFactory7_UnpackFontFile(f7, DWRITE_CONTAINER_TYPE_WOFF, woff, wsize, &st)) && st);
        if (st) {
            UINT64 n = 0; const void *frag = NULL; void *ctx = NULL;
            CHECK(SUCCEEDED(IDWriteFontFileStream_GetFileSize(st, &n)) && n > asize && SUCCEEDED(IDWriteFontFileStream_ReadFileFragment(st, &frag, 0, n, &ctx)) && frag);
            if (frag) {
                const BYTE *u = frag;
                UINT32 hl = 0, hl2 = 0, zl = 0;
                const BYTE *h = table_of(u, (UINT32)n, "head", &hl), *h2 = table_of(abytes, asize, "head", &hl2), *z = table_of(u, (UINT32)n, "zqzq", &zl);
                CHECK(rd32(u) == rd32(abytes) && rd16(u + 4) == rd16(abytes + 4) + 1);
                CHECK(h && h2 && hl == hl2 && !memcmp(h, h2, hl));
                int qs = z && zl == 775;
                for (UINT32 i = 0; qs && i < zl; i++) qs = z[i] == 'Q';
                CHECK(qs);
                // the unpacked font is a font: its face through the in-memory loader maps 'H' as Arial does
                IDWriteInMemoryFontFileLoader *ml = NULL; IDWriteFontFile *uf = NULL; IDWriteFontFace *uface = NULL;
                if (SUCCEEDED(IDWriteFactory7_CreateInMemoryFontFileLoader(f7, &ml)) && ml && SUCCEEDED(IDWriteFactory_RegisterFontFileLoader(f, (IDWriteFontFileLoader *)ml)) &&
                    SUCCEEDED(IDWriteInMemoryFontFileLoader_CreateInMemoryFontFileReference(ml, f, frag, (UINT32)n, NULL, &uf)))
                    IDWriteFactory_CreateFontFace(f, DWRITE_FONT_FACE_TYPE_TRUETYPE, 1, &uf, 0, DWRITE_FONT_SIMULATIONS_NONE, &uface);
                CHECK(uface && glyph(uface, 'H') == glyph(arial, 'H') && glyph(uface, 'H') != 0);
                if (uface) IDWriteFontFace_Release(uface);
                if (uf) IDWriteFontFile_Release(uf);
                if (ml) { IDWriteFactory_UnregisterFontFileLoader(f, (IDWriteFontFileLoader *)ml); IDWriteInMemoryFontFileLoader_Release(ml); }
            }
            IDWriteFontFileStream_ReleaseFileFragment(st, ctx);
            IDWriteFontFileStream_Release(st);
        }
        // what is not a WOFF file: its header's length not the file's, totalSfntSize not what the tables add up to (or
        // more than any font needs), the reserved field set, a tag twice, a table's data in the directory
        UINT32 n0 = rd16(abytes + 4) + 1, dir_end = 44 + 20 * n0;
        CHECK(unpack_changed(f7, woff, wsize, 8, wsize + 4, 1) == DWRITE_E_FILEFORMAT);
        CHECK(unpack_changed(f7, woff, wsize, 8, wsize - 4, 1) == DWRITE_E_FILEFORMAT);
        CHECK(unpack_changed(f7, woff, wsize, 16, rd32(woff + 16) + 4, 1) == DWRITE_E_FILEFORMAT);
        CHECK(unpack_changed(f7, woff, wsize, 16, rd32(woff + 16) - 4, 1) == DWRITE_E_FILEFORMAT);
        CHECK(unpack_changed(f7, woff, wsize, 16, 0x7FFFFFFC, 1) == DWRITE_E_FILEFORMAT);
        CHECK(unpack_changed(f7, woff, wsize, 14, 1, 0) == DWRITE_E_FILEFORMAT);
        CHECK(unpack_changed(f7, woff, wsize, 44 + 20, rd32(woff + 44), 1) == DWRITE_E_FILEFORMAT);
        CHECK(unpack_changed(f7, woff, wsize, 44 + 4, dir_end - 20, 1) == DWRITE_E_FILEFORMAT);
        CHECK(unpack_changed(f7, woff, wsize, 0, rd32(woff), 1) == S_OK);     // (the copy unchanged: a font)
        woff[44 + 20 * rd16(abytes + 4) + 8] ^= 0xFF;          // the compressed table's length made wrong: not a font
        CHECK(IDWriteFactory7_UnpackFontFile(f7, DWRITE_CONTAINER_TYPE_WOFF, woff, wsize, &st) == DWRITE_E_FILEFORMAT && !st);
        free(woff);

        // a collection whose face lists one table's bytes 64 times more: not a font (it would grow without bound)
        IDWriteInMemoryFontFileLoader *ml = NULL;
        if (SUCCEEDED(IDWriteFactory7_CreateInMemoryFontFileLoader(f7, &ml)) && ml && SUCCEEDED(IDWriteFactory_RegisterFontFileLoader(f, (IDWriteFontFileLoader *)ml))) {
            for (int extra = 0; extra <= 64; extra += 64) {
                UINT32 tsize = 0;
                BYTE *ttc = make_ttc(abytes, (UINT32)extra, &tsize);
                IDWriteFontFile *tf = NULL; IDWriteFontFace *tface = NULL;
                HRESULT hr = E_FAIL;
                if (SUCCEEDED(IDWriteInMemoryFontFileLoader_CreateInMemoryFontFileReference(ml, f, ttc, tsize, NULL, &tf)))
                    hr = IDWriteFactory_CreateFontFace(f, DWRITE_FONT_FACE_TYPE_TRUETYPE_COLLECTION, 1, &tf, 0, DWRITE_FONT_SIMULATIONS_NONE, &tface);
                if (!extra) CHECK(SUCCEEDED(hr) && tface && glyph(tface, 'H') == glyph(arial, 'H'));
                else CHECK(hr == DWRITE_E_FILEFORMAT && !tface);
                if (tface) IDWriteFontFace_Release(tface);
                if (tf) IDWriteFontFile_Release(tf);
                free(ttc);
            }
            IDWriteFactory_UnregisterFontFileLoader(f, (IDWriteFontFileLoader *)ml);
        } else CHECK(!"no in-memory loader for a made-up collection");
        if (ml) IDWriteInMemoryFontFileLoader_Release(ml);
    } else CHECK(!"no single-face Arial to make a WOFF file of");

    // made-up WOFF files of runs of 'Q': three tables with a copy of the stream each are a font; three or a thousand tables
    // sharing one stream are not (a stream inflated once per table that names it); 65535 tables sharing a stream that inflates to
    // 16 MB (a terabyte unpacked), with totalSfntSize as if there were one such table or the most there can be: not a
    // font, and said at once (nothing inflated)
    {
        UINT32 rsize = 0;
        BYTE *runs = make_runs_woff(3, 3, 0, &rsize);
        IDWriteFontFileStream *st = NULL;
        CHECK(SUCCEEDED(IDWriteFactory7_UnpackFontFile(f7, DWRITE_CONTAINER_TYPE_WOFF, runs, rsize, &st)) && st);
        if (st) {
            UINT64 n = 0; const void *frag = NULL; void *ctx = NULL;
            CHECK(SUCCEEDED(IDWriteFontFileStream_GetFileSize(st, &n)) && n == 12 + 16 * 3 + 3 * 776);
            if (n == 12 + 16 * 3 + 3 * 776 && SUCCEEDED(IDWriteFontFileStream_ReadFileFragment(st, &frag, 0, n, &ctx)) && frag) {
                const BYTE *u = frag;
                UINT32 ql = 0;
                const BYTE *q = table_of(u, (UINT32)n, "qz\0\2", &ql);
                int qs = rd16(u + 4) == 3 && q && ql == 775;
                for (UINT32 i = 0; qs && i < ql; i++) qs = q[i] == 'Q';
                CHECK(qs);
                IDWriteFontFileStream_ReleaseFileFragment(st, ctx);
            }
            IDWriteFontFileStream_Release(st);
        }
        free(runs);
        runs = make_runs_woff(3, 3, 1, &rsize);
        st = NULL;
        CHECK(IDWriteFactory7_UnpackFontFile(f7, DWRITE_CONTAINER_TYPE_WOFF, runs, rsize, &st) == DWRITE_E_FILEFORMAT && !st);
        free(runs);
        runs = make_runs_woff(1000, 255, 1, &rsize);           // (64 KB each, 64 MB in all: within what a font may be)
        CHECK(rd32(runs + 16) == 12 + 16 * 1000 + 1000 * 65792);
        CHECK(unpack_changed(f7, runs, rsize, 0, rd32(runs), 1) == DWRITE_E_FILEFORMAT);
        free(runs);
        runs = make_runs_woff(65535, 65536, 1, &rsize);
        UINT32 one = 12 + 16 * 65535 + ((1 + 258 * 65536 + 3) & ~3u);
        ULONGLONG t0 = GetTickCount64();
        CHECK(unpack_changed(f7, runs, rsize, 16, one, 1) == DWRITE_E_FILEFORMAT);
        CHECK(unpack_changed(f7, runs, rsize, 16, rd32(runs + 16), 1) == DWRITE_E_FILEFORMAT);
        ULONGLONG ms = GetTickCount64() - t0;
        CHECK(ms < 10000);
        if (ms >= 10000) printf("a WOFF file of 65535 tables sharing one stream took %llu ms to turn down\n", ms);
        free(runs);
    }
    free(abytes);
    if (arial) IDWriteFontFace_Release(arial);

    // Fallback: IDWriteFontFallback1 by axis values, and a fallback of the program's own
    IDWriteFontFallback1 *fb1 = NULL;
    IDWriteFactory7_GetSystemFontFallback(f7, (IDWriteFontFallback **)&fb1);
    source jp = { { &src_vtbl }, L"ひらがな", 4 }, latin = { { &src_vtbl }, L"Quiz", 4 }, han = { { &src_vtbl }, L"世", 1 };
    DWRITE_FONT_AXIS_VALUE normal = { DWRITE_FONT_AXIS_TAG_WEIGHT, 400.0f };
    if (fb1) {
        UINT32 mapped = 0; FLOAT scale = 0; IDWriteFontFace5 *mf5 = NULL;
        CHECK(SUCCEEDED(IDWriteFontFallback1_MapCharacters(fb1, &jp.iface, 0, 4, (IDWriteFontCollection *)sys, L"Helvetica", &normal, 1, &mapped, &scale, &mf5)) &&
              mf5 && mapped == 4 && scale == 1.0f && glyph((IDWriteFontFace *)mf5, 0x3072));
        if (mf5) IDWriteFontFace5_Release(mf5);
        IDWriteFontFallback1_Release(fb1);
    }
    IDWriteFontFallbackBuilder *fbb = NULL;
    CHECK(SUCCEEDED(IDWriteFactory7_CreateFontFallbackBuilder(f7, &fbb)) && fbb);
    UINT32 hidx = 0; BOOL hex = FALSE;
    IDWriteFontCollection_FindFamilyName(sys, L"Hiragino Sans", &hidx, &hex);
    if (fbb && hex) {
        DWRITE_UNICODE_RANGE kana = { 0x3040, 0x30FF };
        const WCHAR *targets[2] = { L"No Such Family Xq", L"Hiragino Sans" };
        CHECK(SUCCEEDED(IDWriteFontFallbackBuilder_AddMapping(fbb, &kana, 1, targets, 2, NULL, NULL, NULL, 1.5f)));
        CHECK(IDWriteFontFallbackBuilder_AddMapping(fbb, &kana, 1, targets, 2, NULL, NULL, NULL, 0.0f) == E_INVALIDARG);
        IDWriteFontFallback *own = NULL;
        CHECK(SUCCEEDED(IDWriteFontFallbackBuilder_CreateFontFallback(fbb, &own)) && own);
        if (own) {
            UINT32 mapped = 0; IDWriteFont *mf = NULL; FLOAT scale = 0;
            CHECK(SUCCEEDED(IDWriteFontFallback_MapCharacters(own, &jp.iface, 0, 4, sys, L"Helvetica", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                             DWRITE_FONT_STRETCH_NORMAL, &mapped, &mf, &scale)) && mf && mapped == 4 && scale == 1.5f);
            if (mf) {
                IDWriteFontFamily *fam = NULL; WCHAR name[64];
                IDWriteFont_GetFontFamily(mf, &fam);
                family_name(fam, name, 64);
                CHECK(!wcscmp(name, L"Hiragino Sans"));
                if (fam) IDWriteFontFamily_Release(fam);
                IDWriteFont_Release(mf); mf = NULL;
            }
            CHECK(SUCCEEDED(IDWriteFontFallback_MapCharacters(own, &latin.iface, 0, 4, sys, L"Helvetica", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                             DWRITE_FONT_STRETCH_NORMAL, &mapped, &mf, &scale)) && mf && mapped == 4 && scale == 1.0f);     // the base font
            if (mf) { IDWriteFont_Release(mf); mf = NULL; }
            CHECK(SUCCEEDED(IDWriteFontFallback_MapCharacters(own, &han.iface, 0, 1, sys, L"Helvetica", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                             DWRITE_FONT_STRETCH_NORMAL, &mapped, &mf, &scale)) && !mf && mapped == 1);     // no mapping has it
            IDWriteFontFallback *sysfb = NULL;
            IDWriteFactory7_GetSystemFontFallback(f7, &sysfb);
            IDWriteFontFallbackBuilder *fbb2 = NULL;
            IDWriteFontFallback *own2 = NULL;
            if (sysfb && SUCCEEDED(IDWriteFactory7_CreateFontFallbackBuilder(f7, &fbb2)) && fbb2 && SUCCEEDED(IDWriteFontFallbackBuilder_AddMappings(fbb2, own)) &&
                SUCCEEDED(IDWriteFontFallbackBuilder_AddMappings(fbb2, sysfb)) && SUCCEEDED(IDWriteFontFallbackBuilder_CreateFontFallback(fbb2, &own2)) && own2) {
                CHECK(SUCCEEDED(IDWriteFontFallback_MapCharacters(own2, &han.iface, 0, 1, sys, L"Helvetica", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                                 DWRITE_FONT_STRETCH_NORMAL, &mapped, &mf, &scale)) && mf && mapped == 1);     // then the system's
                if (mf) IDWriteFont_Release(mf);
            } else CHECK(!"a fallback with the system's after its own");
            if (own2) IDWriteFontFallback_Release(own2);
            if (fbb2) IDWriteFontFallbackBuilder_Release(fbb2);
            if (sysfb) IDWriteFontFallback_Release(sysfb);
            IDWriteFontFallback_Release(own);
        }
    }
    if (fbb) IDWriteFontFallbackBuilder_Release(fbb);

    // The download queue: always empty; listeners held until removed
    IDWriteFontDownloadQueue *q = NULL;
    CHECK(SUCCEEDED(IDWriteFactory7_GetFontDownloadQueue(f7, &q)) && q);
    if (q) {
        counted listener = { { &cu_vtbl }, 1 };
        UINT32 token = 0;
        CHECK(IDWriteFontDownloadQueue_IsEmpty(q) && IDWriteFontDownloadQueue_BeginDownload(q, NULL) == S_FALSE && IDWriteFontDownloadQueue_GetGenerationCount(q) == 0);
        CHECK(SUCCEEDED(IDWriteFontDownloadQueue_AddListener(q, (IDWriteFontDownloadListener *)&listener.iface, &token)) && listener.refs == 2);
        CHECK(SUCCEEDED(IDWriteFontDownloadQueue_RemoveListener(q, token)) && listener.refs == 1 && IDWriteFontDownloadQueue_RemoveListener(q, token) == E_INVALIDARG);
        IDWriteFontDownloadQueue_Release(q);
    }
    IDWriteNumberSubstitution *ns = NULL;
    CHECK(SUCCEEDED(IDWriteFactory7_CreateNumberSubstitution(f7, DWRITE_NUMBER_SUBSTITUTION_METHOD_NONE, L"en-us", FALSE, &ns)) && ns);
    if (ns) { IUnknown *u = NULL; CHECK(SUCCEEDED(IDWriteNumberSubstitution_QueryInterface(ns, &IID_IDWriteNumberSubstitution, (void **)&u)) && u); if (u) IUnknown_Release(u); IDWriteNumberSubstitution_Release(ns); }

    // A program's file read for a font set builder without DirectWrite's lock held
    if (bytes) {
        mem_loader pl = { { &ml_vtbl }, 1, bytes, nbytes, 0, 1, 0 };
        IDWriteFontFile *pf = NULL;
        IDWriteFontSetBuilder2 *pb = NULL;
        if (SUCCEEDED(IDWriteFactory_RegisterFontFileLoader(f, &pl.iface)) && SUCCEEDED(IDWriteFactory_CreateCustomFontFileReference(f, "MEM1", 4, &pl.iface, &pf)) && pf &&
            SUCCEEDED(VCALL(f7, IDWriteFactory6_CreateFontSetBuilder)(f7, &pb)) && pb) {
            probe_calls = probe_stuck = 0;
            CHECK(SUCCEEDED(pb->lpVtbl->AddFontFile(pb, pf)));
            CHECK(probe_calls >= 3 && probe_stuck == 0);
            IDWriteFontFaceReference *pr = NULL;
            CHECK(SUCCEEDED(IDWriteFactory3_CreateFontFaceReference_(f3, pf, 0, DWRITE_FONT_SIMULATIONS_NONE, &pr)) && pr);
            if (pr) {
                FILETIME t;
                probe_calls = probe_stuck = 0;
                CHECK(IDWriteFontFaceReference_GetFileTime(pr, &t) == E_NOTIMPL && probe_calls >= 1 && probe_stuck == 0);   // (the test's stream has no time)
                IDWriteFontFaceReference_Release(pr);
            }
        } else CHECK(!"a builder and a program's file");
        if (pb) IDWriteFontSetBuilder2_Release(pb);
        if (pf) IDWriteFontFile_Release(pf);
        IDWriteFactory_UnregisterFontFileLoader(f, &pl.iface);
        probe_finish();
    }

    printf("dwrite: newer versions: system font set of %u fonts (Helvetica %u), A-V kerned in %d fonts, vertical variants %s\n",
           all_fonts, helvetica_fonts, kern_checked, vert_checked ? "found" : "not found");
    if (c3) IDWriteFontCollection3_Release(c3);
    if (set) IDWriteFontSet3_Release(set);
    IDWriteFontFace5_Release(f5);
    IDWriteFactory7_Release(f7);
}

int main(void) {
    HANDLE threads[4];
    for (int i = 0; i < 4; i++) threads[i] = CreateThread(NULL, 0, worker, (void *)(INT_PTR)i, 0, NULL);
    WaitForMultipleObjects(4, threads, TRUE, 120000);
    for (int i = 0; i < 4; i++) CloseHandle(threads[i]);
    CHECK(thread_failures == 0 && thread_glyphs > 50);
    IDWriteFactory *f = NULL;
    IUnknown *unk = NULL;
    CHECK(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory8, &unk) == E_NOINTERFACE && !unk);
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
    CHECK(IDWriteFactory_QueryInterface(f, &IID_IDWriteFactory8, (void **)&f3) == E_NOINTERFACE && !f3);

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
    probe_coll = sys; probe_face = face;
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
    CHECK(IDWriteFontFace_QueryInterface(face, &IID_IDWriteFontFace6, (void **)&face5) == E_NOINTERFACE && !face5);
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

        // The loader called without DirectWrite's lock: new files read for Analyze, CreateFontFace and a custom
        // collection (CreateStreamFromKey, GetFileSize, ReadFileFragment, ReleaseFileFragment, the stream's Release),
        // and the loader's Release when a collection that held the last references to its file goes.
        mem_loader pl = { { &ml_vtbl }, 1, bytes, nbytes, 0, 1, 0 };
        IDWriteFontFile *pf[3] = { NULL, NULL, NULL };
        CHECK(SUCCEEDED(IDWriteFactory_RegisterFontFileLoader(f, &pl.iface)));
        for (int i = 0; i < 3; i++) CHECK(SUCCEEDED(IDWriteFactory_CreateCustomFontFileReference(f, "MEM1", 4, &pl.iface, &pf[i])) && pf[i]);
        if (pf[0] && pf[1] && pf[2]) {
            BOOL supported = FALSE; DWRITE_FONT_FILE_TYPE ftype; DWRITE_FONT_FACE_TYPE fctype; UINT32 nfaces = 0;
            probe_calls = probe_stuck = 0;
            CHECK(SUCCEEDED(IDWriteFontFile_Analyze(pf[0], &supported, &ftype, &fctype, &nfaces)) && supported && nfaces >= 1);
            CHECK(probe_calls >= 5 && probe_stuck == 0);
            IDWriteFontFace *pface = NULL;
            probe_calls = probe_stuck = 0;
            CHECK(SUCCEEDED(IDWriteFactory_CreateFontFace(f, fctype, 1, &pf[1], IDWriteFontFace_GetIndex(face), DWRITE_FONT_SIMULATIONS_NONE, &pface)) && pface);
            CHECK(probe_calls >= 5 && probe_stuck == 0);
            if (pface) { CHECK(glyph(pface, 'H') == gi[0]); IDWriteFontFace_Release(pface); }
            one_loader pcl = { { &ol_vtbl }, pf[2] };
            IDWriteFontCollection *pc = NULL;
            CHECK(SUCCEEDED(IDWriteFactory_RegisterFontCollectionLoader(f, &pcl.iface)));
            probe_calls = probe_stuck = 0;
            CHECK(SUCCEEDED(IDWriteFactory_CreateCustomFontCollection(f, &pcl.iface, "p", 1, &pc)) && pc);
            CHECK(probe_calls >= 5 && probe_stuck == 0);
            IDWriteFactory_UnregisterFontCollectionLoader(f, &pcl.iface);
            IDWriteFontFile_Release(pf[2]);            // the collection's faces hold the file now
            pf[2] = NULL;
            if (pc) {
                CHECK(IDWriteFontCollection_GetFontFamilyCount(pc) >= 1);
                LONG before = pl.refs;
                pl.probe_release = 1; probe_calls = probe_stuck = 0;
                IDWriteFontCollection_Release(pc);
                pl.probe_release = 0;
                CHECK(pl.refs == before - 1 && probe_calls >= 1 && probe_stuck == 0);
            }
        }
        for (int i = 0; i < 3; i++) if (pf[i]) IDWriteFontFile_Release(pf[i]);
        CHECK(SUCCEEDED(IDWriteFactory_UnregisterFontFileLoader(f, &pl.iface)));
        CHECK(pl.refs == 1 && fragments_out == 0);
        probe_finish();
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
    double cleartype_ink = raster_ink;
    int cleartype_colour = raster_colour;
    CHECK(cov > 50 && !bilevel && raster_colour > 20);              // ClearType: three coverage values per pixel
    CHECK(r.left >= 9 && r.left <= 13 && r.bottom >= 39 && r.bottom <= 42 && r.top < 40 - 14 && r.top > 40 - 24 && r.right > 10 + 40 && r.right < 10 + 24 * 4);
    RECT none;
    CHECK(raster(f2, f, face, gi, 5, 24.0f, DWRITE_RENDERING_MODE_NATURAL, 0, DWRITE_TEXTURE_ALIASED_1x1, &none, &bilevel) == 0 && none.right == none.left);
    RECT ra;
    CHECK(raster(f2, f, face, gi, 5, 24.0f, DWRITE_RENDERING_MODE_ALIASED, 0, DWRITE_TEXTURE_ALIASED_1x1, &ra, &bilevel) > 50 && bilevel);
    CHECK(abs(ra.left - r.left) <= 1 && abs(ra.top - r.top) <= 1 && abs(ra.right - r.right) <= 1 && abs(ra.bottom - r.bottom) <= 1);
    CHECK(raster(f2, f, face, gi, 5, 24.0f, DWRITE_RENDERING_MODE_ALIASED, 0, DWRITE_TEXTURE_CLEARTYPE_3x1, &none, &bilevel) == 0 && none.right == none.left);
    RECT rg;
    CHECK(raster(f2, f, face, gi, 5, 24.0f, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, 1, DWRITE_TEXTURE_ALIASED_1x1, &rg, &bilevel) > 50 && !bilevel);
    CHECK(abs(rg.left - r.left) <= 1 && rg.top == r.top && abs(rg.right - r.right) <= 1 && rg.bottom == r.bottom);
    CHECK(fabs(cleartype_ink - raster_ink) < 0.05 * raster_ink);   // the same ink, spread over subpixels
    double gray_ink = raster_ink;
    RECT rg3;
    CHECK(raster(f2, f, face, gi, 5, 24.0f, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, 1, DWRITE_TEXTURE_CLEARTYPE_3x1, &rg3, &bilevel) > 50 && !bilevel);
    CHECK(rg3.left == rg.left && rg3.right == rg.right && rg3.top == rg.top && rg3.bottom == rg.bottom);
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
            CHECK(r.no_offsets == 0 && r.moved == 0);
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
            // a drawing effect: the layout holds it (its AddRef called without DirectWrite's lock) until it goes
            effect fx = { { &fx_vtbl }, 1, 1 };
            probe_calls = probe_stuck = 0;
            CHECK(SUCCEEDED(IDWriteTextLayout_SetDrawingEffect(lay, &fx.iface, first5)));
            CHECK(fx.refs > 1 && probe_calls >= 1 && probe_stuck == 0);
            fx.probing = 0;
            IUnknown *back_fx = NULL;
            CHECK(SUCCEEDED(IDWriteTextLayout_GetDrawingEffect(lay, 2, &back_fx, &got)) && back_fx == &fx.iface && got.startPosition == 0 && got.length == 5);
            if (back_fx) IUnknown_Release(back_fx);
            CHECK(SUCCEEDED(IDWriteTextLayout_GetDrawingEffect(lay, 7, &back_fx, &got)) && !back_fx);
            renderer fr = { { &renderer_vtbl } };
            CHECK(SUCCEEDED(IDWriteTextLayout_Draw(lay, NULL, &fr.iface, 0.0f, 0.0f)) && fr.glyphs == 11);
            IDWriteTextLayout_Release(lay);
            CHECK(fx.refs == 1);
            probe_finish();
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

    newer_versions(f, sys, face, font, gi, bytes, nbytes);
    free(bytes);
    if (file) IDWriteFontFile_Release(file);
    IDWriteFontFace_Release(face);
    IDWriteFont_Release(font);
    IDWriteFontCollection_Release(sys);
    IDWriteFactory2_Release(f2);
    IDWriteFactory_Release(f);
    printf("dwrite: %d families; Hello at 24 DIPs covers %ld x %ld pixels (ClearType: %d pixels of subpixels apart, ink %.0f vs %.0f grayscale) -> %s\n",
           (int)nfam, r.right - r.left, r.bottom - r.top, cleartype_colour, cleartype_ink, gray_ink, failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
