// GDI regions (gdi32): areas made of rectangles, combined with AND, OR, XOR and DIFF, compared, measured and listed
// (GetRegionData). Chromium (the Windows Steam client's steamwebhelper) works out its windows' regions with them and
// ends when one is missing. From Microsoft's documentation of each function.
//
// A region is kept the way Windows describes it in RGNDATA: rectangles that do not overlap, sorted top to bottom and
// left to right, in bands (rectangles of a band share their top and bottom), with neighbouring bands that have the
// same horizontal spans merged. That form is unique, so two regions are equal when their lists are. The region
// objects live in gdi.c's handle table (vx_gdi_region_new / vx_gdi_region); a DC's clip region (gdi.c) and a window's
// update region (present.c) are regions too.
#include <stdlib.h>
#include <string.h>
#include "ui.h"

enum { ERROR_RGN = 0, NULLREGION = 1, SIMPLEREGION = 2, COMPLEXREGION = 3 };
enum { RGN_AND = 1, RGN_OR = 2, RGN_XOR = 3, RGN_DIFF = 4, RGN_COPY = 5 };
enum { ERROR_INVALID_HANDLE = 6, ERROR_INVALID_PARAMETER = 87 };


static void add(vx_region *g, LONG l, LONG t, LONG r, LONG b) {
    if (g->n == g->cap) { g->cap = g->cap ? g->cap * 2 : 8; g->r = realloc(g->r, (size_t)g->cap * sizeof *g->r); }
    g->r[g->n++] = (RECT){ l, t, r, b };
}
vx_region *vx_region_new(void) { return calloc(1, sizeof(vx_region)); }
void vx_region_free(vx_region *g) { if (g) { free(g->r); free(g); } }
static void set_rect(vx_region *g, LONG l, LONG t, LONG r, LONG b) {
    if (l > r) { LONG x = l; l = r; r = x; }                  // (coordinates are put in order)
    if (t > b) { LONG y = t; t = b; b = y; }
    g->n = 0;
    if (l < r && t < b) add(g, l, t, r, b);
}
void vx_region_copy(vx_region *dst, const vx_region *src) {
    if (dst == src) return;
    dst->n = 0;
    for (int i = 0; i < src->n; i++) add(dst, src->r[i].left, src->r[i].top, src->r[i].right, src->r[i].bottom);
}
static int type_of(const vx_region *g) { return !g->n ? NULLREGION : g->n == 1 ? SIMPLEREGION : COMPLEXREGION; }
static RECT box_of(const vx_region *g) {
    RECT b = { 0, 0, 0, 0 };
    for (int i = 0; i < g->n; i++) {
        if (!i) { b = g->r[0]; continue; }
        if (g->r[i].left < b.left) b.left = g->r[i].left;
        if (g->r[i].top < b.top) b.top = g->r[i].top;
        if (g->r[i].right > b.right) b.right = g->r[i].right;
        if (g->r[i].bottom > b.bottom) b.bottom = g->r[i].bottom;
    }
    return b;
}

// The horizontal spans of g between y and y + 1 (rectangles of a band cover the whole band), into xs as pairs.
static int spans(const vx_region *g, LONG y, LONG *xs) {
    int n = 0;
    for (int i = 0; i < g->n; i++) if (g->r[i].top <= y && y < g->r[i].bottom) { xs[n++] = g->r[i].left; xs[n++] = g->r[i].right; }
    return n;
}
static int in_spans(const LONG *xs, int n, LONG x) { for (int i = 0; i < n; i += 2) if (xs[i] <= x && x < xs[i + 1]) return 1; return 0; }
static int cmp_long(const void *a, const void *b) { LONG x = *(const LONG *)a, y = *(const LONG *)b; return x < y ? -1 : x > y; }
static int keep(int mode, int a, int b) {
    switch (mode) { case RGN_AND: return a && b; case RGN_OR: return a || b; case RGN_XOR: return a != b; default: return a && !b; }
}
// a OP b, band by band: the bands are cut at every top and bottom of either region; in each, the spans are cut at
// every left and right, kept by the operation and joined; a band with the same spans as the one just above it
// (touching it) extends that one.
static void combine(vx_region *out, const vx_region *a, const vx_region *b, int mode) {
    int ny = 0;
    LONG *ys = malloc(sizeof(LONG) * (size_t)(2 * (a->n + b->n) + 1));
    for (int i = 0; i < a->n; i++) { ys[ny++] = a->r[i].top; ys[ny++] = a->r[i].bottom; }
    for (int i = 0; i < b->n; i++) { ys[ny++] = b->r[i].top; ys[ny++] = b->r[i].bottom; }
    qsort(ys, (size_t)ny, sizeof *ys, cmp_long);
    LONG *sa = malloc(sizeof(LONG) * (size_t)(2 * a->n + 1)), *sb = malloc(sizeof(LONG) * (size_t)(2 * b->n + 1));
    LONG *xs = malloc(sizeof(LONG) * (size_t)(2 * (a->n + b->n) + 1));
    vx_region r = { 0 };
    int prev_start = -1, prev_count = 0;                        // the band above, in r
    for (int k = 0; k + 1 < ny; k++) {
        LONG y0 = ys[k], y1 = ys[k + 1];
        if (y0 == y1) continue;
        int na = spans(a, y0, sa), nb = spans(b, y0, sb), nx = 0;
        for (int i = 0; i < na; i++) xs[nx++] = sa[i];
        for (int i = 0; i < nb; i++) xs[nx++] = sb[i];
        qsort(xs, (size_t)nx, sizeof *xs, cmp_long);
        int start = r.n;
        LONG open = 0; int inside = 0;
        for (int i = 0; i + 1 < nx; i++) {
            if (xs[i] == xs[i + 1]) continue;
            int in = keep(mode, in_spans(sa, na, xs[i]), in_spans(sb, nb, xs[i]));
            if (in && !inside) { open = xs[i]; inside = 1; }
            if (!in && inside) { add(&r, open, y0, xs[i], y1); inside = 0; }
        }
        if (inside) add(&r, open, y0, xs[nx - 1], y1);
        int count = r.n - start;
        int same = prev_start >= 0 && count == prev_count && count && r.r[prev_start].bottom == y0;
        for (int i = 0; same && i < count; i++)
            same = r.r[prev_start + i].left == r.r[start + i].left && r.r[prev_start + i].right == r.r[start + i].right;
        if (same) { for (int i = 0; i < count; i++) r.r[prev_start + i].bottom = y1; r.n = start; }
        else if (count) { prev_start = start; prev_count = count; }
    }
    free(ys); free(sa); free(sb); free(xs);
    free(out->r);
    *out = r;
}

// For the rest of the runtime (ui.h): update regions, clip regions (gdi.c, present.c).
void vx_region_set_rect(vx_region *g, LONG l, LONG t, LONG r, LONG b) { set_rect(g, l, t, r, b); }
int vx_region_combine(vx_region *out, const vx_region *a, const vx_region *b, int mode) {
    if (mode == RGN_COPY) vx_region_copy(out, a); else combine(out, a, b, mode);
    return type_of(out);
}
void vx_region_offset(vx_region *g, int dx, int dy) {
    for (int i = 0; i < g->n; i++) { g->r[i].left += dx; g->r[i].right += dx; g->r[i].top += dy; g->r[i].bottom += dy; }
}
RECT vx_region_box(const vx_region *g) { return box_of(g); }
int vx_region_type(const vx_region *g) { return type_of(g); }

// ---- gdi32 -----------------------------------------------------------------------------------------------------------------
static vx_region *region_of(HANDLE h) {
    vx_region *g = vx_gdi_region(h);
    if (!g) vx_set_last_error(ERROR_INVALID_HANDLE);
    return g;
}
static WINAPI HANDLE CreateRectRgn(int l, int t, int r, int b) {
    vx_region *g = vx_region_new();
    set_rect(g, l, t, r, b);
    HANDLE h = vx_gdi_region_new(g);
    if (!h) vx_region_free(g);
    return h;
}
static WINAPI HANDLE CreateRectRgnIndirect(const RECT *r) {
    if (!r) { vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    return CreateRectRgn(r->left, r->top, r->right, r->bottom);
}
static WINAPI BOOL SetRectRgn(HANDLE h, int l, int t, int r, int b) {
    vx_region *g = region_of(h);
    if (!g) return 0;
    set_rect(g, l, t, r, b);
    return 1;
}
static WINAPI int CombineRgn(HANDLE dst, HANDLE src1, HANDLE src2, int mode) {
    vx_region *d = region_of(dst), *a = region_of(src1), *b = mode == RGN_COPY ? a : region_of(src2);
    if (!d || !a || !b || mode < RGN_AND || mode > RGN_COPY) { if (d && a && b) vx_set_last_error(ERROR_INVALID_PARAMETER); return ERROR_RGN; }
    if (mode == RGN_COPY) vx_region_copy(d, a);
    else combine(d, a, b, mode);
    return type_of(d);
}
static WINAPI BOOL EqualRgn(HANDLE h1, HANDLE h2) {
    vx_region *a = region_of(h1), *b = region_of(h2);
    if (!a || !b) return ERROR_RGN;
    return a->n == b->n && !memcmp(a->r, b->r, sizeof(RECT) * (size_t)a->n);
}
static WINAPI int GetRgnBox(HANDLE h, RECT *box) {
    vx_region *g = region_of(h);
    if (!g || !box) return ERROR_RGN;
    *box = box_of(g);
    return type_of(g);
}
static WINAPI int OffsetRgn(HANDLE h, int dx, int dy) {
    vx_region *g = region_of(h);
    if (!g) return ERROR_RGN;
    vx_region_offset(g, dx, dy);
    return type_of(g);
}
static WINAPI BOOL PtInRegion(HANDLE h, int x, int y) {
    vx_region *g = region_of(h);
    for (int i = 0; g && i < g->n; i++) if (g->r[i].left <= x && x < g->r[i].right && g->r[i].top <= y && y < g->r[i].bottom) return 1;
    return 0;
}
static WINAPI BOOL RectInRegion(HANDLE h, const RECT *r) {
    vx_region *g = region_of(h);
    if (!g || !r) return 0;
    LONG l = r->left < r->right ? r->left : r->right, rr = r->left < r->right ? r->right : r->left;
    LONG t = r->top < r->bottom ? r->top : r->bottom, b = r->top < r->bottom ? r->bottom : r->top;
    for (int i = 0; i < g->n; i++) if (g->r[i].left < rr && l < g->r[i].right && g->r[i].top < b && t < g->r[i].bottom) return 1;
    return 0;
}
// RGNDATA: RGNDATAHEADER { dwSize 32, iType RDH_RECTANGLES, nCount, nRgnSize, rcBound } and the rectangles.
static WINAPI DWORD GetRegionData(HANDLE h, DWORD size, uint8_t *data) {
    vx_region *g = region_of(h);
    if (!g) return 0;
    DWORD need = 32 + 16 * (DWORD)g->n;
    if (!data) return need;
    if (size < need) return 0;
    uint32_t *hd = (uint32_t *)data;
    hd[0] = 32; hd[1] = 1; hd[2] = (uint32_t)g->n; hd[3] = 16 * (uint32_t)g->n;
    RECT b = box_of(g);
    memcpy(data + 16, &b, 16);
    if (g->n) memcpy(data + 32, g->r, 16 * (size_t)g->n);
    return size;
}
// A region from RGNDATA's rectangles (in any order, overlapping or not), moved and scaled by an XFORM without
// rotation or shear (those Windows answers with a region this runtime cannot keep: refused).
static WINAPI HANDLE ExtCreateRegion(const float *xform, DWORD size, const uint8_t *data) {
    if (!data || size < 32) { vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    const uint32_t *hd = (const uint32_t *)data;
    if (hd[0] != 32 || hd[1] != 1 || size < 32 + 16 * (uint64_t)hd[2]) { vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    float m11 = 1, m22 = 1, dx = 0, dy = 0;
    if (xform) {                                                // XFORM: eM11, eM12, eM21, eM22, eDx, eDy
        if (xform[1] != 0 || xform[2] != 0) { vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
        m11 = xform[0]; m22 = xform[3]; dx = xform[4]; dy = xform[5];
    }
    vx_region *g = vx_region_new(), one = { 0 };
    for (uint32_t i = 0; i < hd[2]; i++) {
        RECT r;
        memcpy(&r, data + 32 + 16 * (size_t)i, 16);
        set_rect(&one, (LONG)(r.left * m11 + dx), (LONG)(r.top * m22 + dy), (LONG)(r.right * m11 + dx), (LONG)(r.bottom * m22 + dy));
        if (one.n) combine(g, g, &one, RGN_OR);
    }
    free(one.r);
    HANDLE h = vx_gdi_region_new(g);
    if (!h) vx_region_free(g);
    return h;
}

static const vx_export gdi32[] = {
    VX_EXPORT(CreateRectRgn), VX_EXPORT(CreateRectRgnIndirect), VX_EXPORT(SetRectRgn), VX_EXPORT(CombineRgn), VX_EXPORT(EqualRgn),
    VX_EXPORT(GetRgnBox), VX_EXPORT(OffsetRgn), VX_EXPORT(PtInRegion), VX_EXPORT(RectInRegion), VX_EXPORT(GetRegionData),
    VX_EXPORT(ExtCreateRegion),
};

void vx_region_init(void) {
    vx_register_module("gdi32.dll", gdi32, sizeof gdi32 / sizeof gdi32[0]);
}

#ifdef __aarch64__
#include "thunks/region.c.inc"
#endif
