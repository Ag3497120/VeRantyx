// gdi32 (and the drawing parts of user32): device contexts, pens, brushes, fonts, bitmaps, text.
//
// Every drawing surface is 32-bit BGRX in memory, the layout of a Windows 32-bpp DIB, drawn with Core Graphics:
// a window's client area (top-down) or a memory bitmap (top-down, or bottom-up for a bottom-up DIB section).
// Contexts are set up so GDI coordinates (y down) work unchanged; images and text, which Core Graphics draws
// y-up, are flipped locally. Text goes through Core Text. Moving pixels between surfaces (BitBlt, StretchBlt, the DIB
// transfers, AlphaBlend) and getting them to the screen is present.c's.
//
// A DC keeps the state Microsoft documents for it: the viewport and window origins (MM_TEXT: a logical point lands
// at itself less the window origin plus the viewport origin), the clip region (device coordinates), the stretch
// mode, and SaveDC's stack of all that. Steam's vgui draws each panel of its windows at a viewport origin.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <CoreText/CoreText.h>
#include "ui.h"

enum { OBJ_PEN = 1, OBJ_BRUSH = 2, OBJ_DC = 3, OBJ_FONT = 6, OBJ_BITMAP = 7, OBJ_REGION = 8, OBJ_MEMDC = 10 };
enum { ERROR_RGN = 0, NULLREGION = 1, SIMPLEREGION = 2, COMPLEXREGION = 3 };
enum { RGN_AND = 1, RGN_OR = 2, RGN_XOR = 3, RGN_DIFF = 4, RGN_COPY = 5 };
#define GDI_BASE 0x40000
#define MAX_GDI 65536
#define TRANSPARENT_BK 1
#define OPAQUE_BK 2

// What SaveDC keeps and RestoreDC brings back.
typedef struct dc_state {
    HANDLE bitmap, brush, pen, font;
    uint32_t text_color, bk_color, dc_brush, dc_pen, text_align;
    int bk_mode, map_mode, stretch_mode, graphics_mode;
    POINT pos, vp_org, win_org;
    vx_region *clip;                                          // (a copy)
} dc_state;

typedef struct gdi_obj {
    int type, stock;
    uint32_t color; int style, width;                         // pen, brush
    int font_height, font_weight, italic; char face[64];      // font
    int bw, bh, bottom_up, bpp; uint8_t *bits; int own_bits;  // bitmap: bits in its own format
    uint32_t *pixels; CGContextRef ctx;                       //   and a BGRX surface for drawing (bpp 32: the same memory)
    uint32_t palette[256], masks[3];
    int dib; HANDLE section; DWORD offset; void *view;        //   a DIB section; its file mapping's view (unmapped with it)
    vx_window *w;                                             // DC: a window's client area,
    HANDLE bitmap, brush, pen, font;                          //   or the bitmap selected into a memory DC
    uint32_t text_color, bk_color, dc_brush, dc_pen, text_align;
    int bk_mode, map_mode, stretch_mode, graphics_mode;
    POINT pos, vp_org, win_org;
    vx_region *clip;                                          //   the clip region (device coordinates), NULL: none
    dc_state *saved; int nsaved;                              //   SaveDC's stack
    HANDLE owned_rgn;                                         //   GetDCEx's region, deleted with the DC
    vx_region *region;                                        // region: its rectangles (region.c)
    vx_texts texts;                                           // bitmap: text drawn on it (ui.h)
} gdi_obj;

static gdi_obj *g_objs[MAX_GDI];
static int g_obj_next;
static pthread_mutex_t g_gdi_lock = PTHREAD_MUTEX_INITIALIZER;
static HANDLE g_stock[20];
static HANDLE g_default_bitmap;          // what a new memory DC has selected: 1x1, as on Windows

static HANDLE new_obj(gdi_obj *o) {
    pthread_mutex_lock(&g_gdi_lock);
    int i = g_obj_next;
    for (int k = 0; k < MAX_GDI && g_objs[i]; k++) i = (i + 1) % MAX_GDI;
    HANDLE h = NULL;
    if (!g_objs[i]) { g_objs[i] = o; g_obj_next = (i + 1) % MAX_GDI; h = (HANDLE)(intptr_t)(GDI_BASE + i * 4); }
    pthread_mutex_unlock(&g_gdi_lock);
    if (!h) free(o);
    return h;
}
static gdi_obj *obj(HANDLE h) {
    intptr_t v = (intptr_t)h - GDI_BASE;
    return v >= 0 && v % 4 == 0 && v / 4 < MAX_GDI ? g_objs[v / 4] : NULL;
}
static gdi_obj *obj_of(HANDLE h, int type) { gdi_obj *o = obj(h); return o && o->type == type ? o : NULL; }
static gdi_obj *dc_of(HANDLE h) { gdi_obj *o = obj(h); return o && (o->type == OBJ_DC || o->type == OBJ_MEMDC) ? o : NULL; }

// ---- coordinates -------------------------------------------------------------------------------------------------------
static int dc_dx(const gdi_obj *dc) { return dc->vp_org.x - dc->win_org.x; }
static int dc_dy(const gdi_obj *dc) { return dc->vp_org.y - dc->win_org.y; }
static RECT to_device(const gdi_obj *dc, RECT r) {
    int dx = dc_dx(dc), dy = dc_dy(dc);
    return (RECT){ r.left + dx, r.top + dy, r.right + dx, r.bottom + dy };
}
// The surface's size, device coordinates (0x0 for the screen DC, which draws nothing).
static RECT surface_rect(const gdi_obj *dc) {
    if (dc->type == OBJ_DC) return dc->w ? (RECT){ 0, 0, dc->w->client_w, dc->w->client_h } : (RECT){ 0, 0, 0, 0 };
    gdi_obj *b = obj_of(dc->bitmap, OBJ_BITMAP);
    return b ? (RECT){ 0, 0, b->bw, b->bh } : (RECT){ 0, 0, 0, 0 };
}

// ---- text on surfaces (ui.h, for Command Clips) ---------------------------------------------------------------------
pthread_mutex_t vx_text_lock = PTHREAD_MUTEX_INITIALIZER;
static int overlaps(RECT a, RECT b) { return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom; }
static void texts_forget(vx_texts *t, RECT r) {                // (vx_text_lock held)
    int k = 0;
    for (int i = 0; i < t->n; i++) { if (overlaps(t->v[i].r, r)) free(t->v[i].s); else t->v[k++] = t->v[i]; }
    t->n = k;
}
static void texts_add(vx_texts *t, RECT r, char *s) {          // (held; s is taken)
    texts_forget(t, r);
    if (t->n == t->cap) { t->cap = t->cap ? 2 * t->cap : 16; t->v = realloc(t->v, (size_t)t->cap * sizeof *t->v); }
    t->v[t->n++] = (vx_text){ r, s };
}
void vx_texts_free(vx_texts *t) {
    pthread_mutex_lock(&vx_text_lock);
    for (int i = 0; i < t->n; i++) free(t->v[i].s);
    free(t->v); *t = (vx_texts){ 0 };
    pthread_mutex_unlock(&vx_text_lock);
}
static vx_texts *texts_of(gdi_obj *dc) {
    if (dc->type == OBJ_DC) return dc->w ? &dc->w->texts : NULL;
    gdi_obj *b = obj_of(dc->bitmap, OBJ_BITMAP);
    return b ? &b->texts : NULL;
}
static void texts_painted(gdi_obj *dc, RECT r) {               // r (logical) painted over: its text is gone
    vx_texts *t = texts_of(dc);
    if (!t || !t->n) return;
    pthread_mutex_lock(&vx_text_lock); texts_forget(t, to_device(dc, r)); pthread_mutex_unlock(&vx_text_lock);
}
void vx_gdi_forget_texts(HANDLE h, RECT r) {
    gdi_obj *dc = dc_of(h);
    vx_texts *t = dc ? texts_of(dc) : NULL;
    if (!t || !t->n) return;
    pthread_mutex_lock(&vx_text_lock); texts_forget(t, r); pthread_mutex_unlock(&vx_text_lock);
}
// A blit takes the text along with the pixels (scaled as they are); logical coordinates of each DC.
void vx_gdi_copy_texts(HANDLE dst, int xd, int yd, int wd, int hd, HANDLE src, int xs, int ys, int ws, int hs) {
    gdi_obj *d = dc_of(dst), *s = dc_of(src);
    vx_texts *dt = d ? texts_of(d) : NULL, *st = s ? texts_of(s) : NULL;
    if (!dt || dt == st || !ws || !hs) return;
    RECT to = to_device(d, (RECT){ xd, yd, xd + wd, yd + hd }), from = to_device(s, (RECT){ xs, ys, xs + ws, ys + hs });
    pthread_mutex_lock(&vx_text_lock);
    texts_forget(dt, to);
    for (int i = 0; st && i < st->n; i++) {
        RECT r = st->v[i].r;
        if (r.left < from.left || r.top < from.top || r.right > from.right || r.bottom > from.bottom) continue;
        RECT t = { to.left + (r.left - from.left) * wd / ws, to.top + (r.top - from.top) * hd / hs,
                   to.left + (r.right - from.left) * wd / ws, to.top + (r.bottom - from.top) * hd / hs };
        texts_add(dt, t, strdup(st->v[i].s));
    }
    pthread_mutex_unlock(&vx_text_lock);
}

// ---- colors ----------------------------------------------------------------------------------------------------------
uint32_t vx_sys_color(int i) {
    static const uint32_t colors[31] = {
        0xC8C8C8, 0x000000, 0xD1B499, 0xDBCDBF, 0xF0F0F0, 0xFFFFFF, 0x646464, 0x000000, 0x000000, 0x000000,
        0xB4B4B4, 0xFCF7F4, 0xABABAB, 0xD77800, 0xFFFFFF, 0xF0F0F0, 0xA0A0A0, 0x6D6D6D, 0x000000, 0x000000,
        0xFFFFFF, 0x696969, 0xE3E3E3, 0x000000, 0xE1FFFF, 0x000000, 0xCC6600, 0xEAD1B9, 0xF2E4D7, 0xD77800, 0xF0F0F0,
    };
    return i >= 0 && i < 31 ? colors[i] : 0;
}
static void set_fill(CGContextRef c, uint32_t cr) { CGContextSetRGBFillColor(c, (cr & 0xff) / 255.0, ((cr >> 8) & 0xff) / 255.0, ((cr >> 16) & 0xff) / 255.0, 1); }
static void set_stroke(CGContextRef c, uint32_t cr) { CGContextSetRGBStrokeColor(c, (cr & 0xff) / 255.0, ((cr >> 8) & 0xff) / 255.0, ((cr >> 16) & 0xff) / 255.0, 1); }
static uint32_t bgrx_to_colorref(uint32_t p) { return ((p >> 16) & 0xff) | (p & 0xff00) | ((p & 0xff) << 16); }
static uint32_t colorref_to_bgrx(uint32_t c) { return 0xff000000u | ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff); }

// A brush handle may also be a system color index + 1 (COLOR_WINDOW + 1 and so on).
static int brush_color(HANDLE h, uint32_t *color, gdi_obj *dc) {
    if ((uintptr_t)h > 0 && (uintptr_t)h <= 31) { *color = vx_sys_color((int)(uintptr_t)h - 1); return 1; }
    gdi_obj *b = obj_of(h, OBJ_BRUSH);
    if (!b || b->style == 1 /* BS_NULL */) return 0;
    *color = b->stock == 18 /* DC_BRUSH */ && dc ? dc->dc_brush : b->color;
    return 1;
}

// ---- surfaces -----------------------------------------------------------------------------------------------------------
static CGContextRef make_context(uint32_t *px, int w, int h, int bottom_up) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(px, (size_t)w, (size_t)h, 8, (size_t)w * 4, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (c && !bottom_up) { CGContextTranslateCTM(c, 0, h); CGContextScaleCTM(c, 1, -1); }
    return c;
}

// The surface a DC draws on: its context (clipped, and moved to the DC's origin until unlock_target), size and
// orientation. Window DCs lock the window; unlock_target ends it and tells present.c what was drawn.
typedef struct { CGContextRef ctx; int w, h, bottom_up; uint32_t *pixels; vx_window *win; int ox, oy; const RECT *clip; int nclip; } target;
static void clip_context(CGContextRef c, const vx_region *clip) {
    CGRect small[16], *rs = clip->n <= 16 ? small : malloc(sizeof(CGRect) * (size_t)clip->n);
    for (int i = 0; i < clip->n; i++) rs[i] = CGRectMake(clip->r[i].left, clip->r[i].top, clip->r[i].right - clip->r[i].left, clip->r[i].bottom - clip->r[i].top);
    if (clip->n) CGContextClipToRects(c, rs, (size_t)clip->n); else CGContextClipToRect(c, CGRectZero);
    if (rs != small) free(rs);
}
static int lock_target(gdi_obj *dc, target *t) {
    memset(t, 0, sizeof *t);
    if (dc->type == OBJ_DC) {
        if (!dc->w) return 0;
        pthread_mutex_lock(&dc->w->lock);
        if (!dc->w->ctx) { pthread_mutex_unlock(&dc->w->lock); return 0; }
        *t = (target){ dc->w->ctx, dc->w->bw, dc->w->bh, 0, dc->w->pixels, dc->w, 0, 0, NULL, -1 };
    } else {
        gdi_obj *b = obj_of(dc->bitmap, OBJ_BITMAP);
        if (!b || !b->ctx) return 0;
        *t = (target){ b->ctx, b->bw, b->bh, b->bottom_up, b->pixels, NULL, 0, 0, NULL, -1 };
    }
    t->ox = dc_dx(dc); t->oy = dc_dy(dc);
    if (dc->clip) { t->clip = dc->clip->r; t->nclip = dc->clip->n; }
    CGContextSaveGState(t->ctx);
    if (dc->clip) clip_context(t->ctx, dc->clip);
    if (t->ox || t->oy) CGContextTranslateCTM(t->ctx, t->ox, t->oy);
    return 1;
}
// drawn: the logical rectangle the operation touched (NULL: nothing).
static void unlock_target(target *t, const RECT *drawn) {
    CGContextRestoreGState(t->ctx);
    if (!t->win) return;
    if (drawn) {
        RECT d = { drawn->left + t->ox, drawn->top + t->oy, drawn->right + t->ox, drawn->bottom + t->oy };
        vx_present_changed(t->win, &d);
    }
    pthread_mutex_unlock(&t->win->lock);
}
// Whether a device point is inside the clip region (and the surface).
static int in_clip(const target *t, int x, int y) {
    if (x < 0 || y < 0 || x >= t->w || y >= t->h) return 0;
    if (t->nclip < 0) return 1;
    for (int i = 0; i < t->nclip; i++) if (t->clip[i].left <= x && x < t->clip[i].right && t->clip[i].top <= y && y < t->clip[i].bottom) return 1;
    return 0;
}

// ---- surfaces for present.c (ui.h) -------------------------------------------------------------------------------------------
static int bitmap_surface(gdi_obj *b, vx_surface *s) {
    memset(s, 0, sizeof *s);
    s->px = b->bpp == 32 ? b->pixels : NULL;
    s->w = b->bw; s->h = b->bh; s->bottom_up = b->bottom_up; s->bpp = b->bpp;
    s->bits = b->bits; s->palette = b->palette; s->stride = (((size_t)b->bw * b->bpp + 31) / 32) * 4;
    s->nclip = -1;
    return 1;
}
int vx_bitmap_surface(HANDLE h, vx_surface *s) { gdi_obj *b = obj_of(h, OBJ_BITMAP); return b ? bitmap_surface(b, s) : 0; }
int vx_surface_lock(HANDLE h, vx_surface *s) {
    gdi_obj *dc = dc_of(h);
    memset(s, 0, sizeof *s);
    if (!dc) return 0;
    if (dc->type == OBJ_DC) {
        if (!dc->w) return 0;
        pthread_mutex_lock(&dc->w->lock);
        if (!dc->w->pixels) { pthread_mutex_unlock(&dc->w->lock); return 0; }
        *s = (vx_surface){ dc->w->pixels, dc->w->bw, dc->w->bh, 0, 32, (uint8_t *)dc->w->pixels, NULL, (size_t)dc->w->bw * 4, dc->w, 0, 0, NULL, -1, 0, 1 };
    } else {
        gdi_obj *b = obj_of(dc->bitmap, OBJ_BITMAP);
        if (!b) return 0;
        bitmap_surface(b, s);
    }
    s->ox = dc_dx(dc); s->oy = dc_dy(dc);
    if (dc->clip) { s->clip = dc->clip->r; s->nclip = dc->clip->n; }
    s->stretch_mode = dc->stretch_mode;
    return 1;
}
void vx_surface_unlock(vx_surface *s, const RECT *drawn) {
    if (!s->win || !s->locked) return;
    if (drawn) vx_present_changed(s->win, drawn);
    s->locked = 0;
    pthread_mutex_unlock(&s->win->lock);
}
HANDLE vx_dc_bitmap(HANDLE h) { gdi_obj *dc = dc_of(h); return dc && dc->type == OBJ_MEMDC ? dc->bitmap : NULL; }
int vx_dc_brush_bgrx(HANDLE h, uint32_t *bgrx) {
    gdi_obj *dc = dc_of(h);
    uint32_t c;
    if (!dc || !brush_color(dc->brush, &c, dc)) return 0;
    *bgrx = colorref_to_bgrx(c);
    return 1;
}
int vx_dc_valid(HANDLE h) { return dc_of(h) != NULL; }
vx_window *vx_dc_window(HANDLE h) { gdi_obj *dc = dc_of(h); return dc && dc->type == OBJ_DC ? dc->w : NULL; }
int vx_dc_set_clip(HANDLE h, const vx_region *r) {
    gdi_obj *dc = dc_of(h);
    if (!dc) return 0;
    if (!r) { vx_region_free(dc->clip); dc->clip = NULL; return 1; }
    if (!dc->clip) dc->clip = vx_region_new();
    vx_region_copy(dc->clip, r);
    return 1;
}

// ---- device contexts ------------------------------------------------------------------------------------------------------
static gdi_obj *new_dc(int type, vx_window *w) {
    gdi_obj *d = calloc(1, sizeof *d);
    d->type = type; d->w = w;
    d->brush = g_stock[0]; d->pen = g_stock[7]; d->font = g_stock[13];
    d->text_color = 0; d->bk_color = 0xFFFFFF; d->bk_mode = OPAQUE_BK; d->dc_brush = 0xFFFFFF; d->dc_pen = 0;
    d->map_mode = 1 /* MM_TEXT */; d->stretch_mode = 1 /* BLACKONWHITE */; d->graphics_mode = 1 /* GM_COMPATIBLE */;
    return d;
}
HANDLE vx_dc_for_window(vx_window *w) { return new_obj(new_dc(OBJ_DC, w)); }
static WINAPI BOOL DeleteObject(HANDLE h);
static void free_obj(HANDLE h) {
    intptr_t v = ((intptr_t)h - GDI_BASE) / 4;
    pthread_mutex_lock(&g_gdi_lock);
    gdi_obj *o = g_objs[v];
    g_objs[v] = NULL;
    pthread_mutex_unlock(&g_gdi_lock);
    if (o && o->type == OBJ_BITMAP) {
        vx_texts_free(&o->texts);
        if (o->ctx) CGContextRelease(o->ctx);
        if (o->pixels && (void *)o->pixels != (void *)o->bits) free(o->pixels);
        if (o->own_bits) free(o->bits);
        if (o->view) {
            BOOL (WINAPI *unmap)(void *) = (BOOL (WINAPI *)(void *))vx_find_export("kernel32.dll", "UnmapViewOfFile");
            if (unmap) unmap(o->view);
        }
    }
    if (o && (o->type == OBJ_DC || o->type == OBJ_MEMDC)) {
        vx_region_free(o->clip);
        for (int i = 0; i < o->nsaved; i++) vx_region_free(o->saved[i].clip);
        free(o->saved);
        if (o->owned_rgn) DeleteObject(o->owned_rgn);
    }
    if (o && o->type == OBJ_REGION) vx_region_free(o->region);
    free(o);
}
// Region objects for region.c, which keeps their rectangles: a new one holding r, and the one behind a handle.
HANDLE vx_gdi_region_new(vx_region *r) {
    gdi_obj *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->type = OBJ_REGION; o->region = r;
    HANDLE h = new_obj(o);                                    // (frees o when the table is full)
    return h;
}
vx_region *vx_gdi_region(HANDLE h) { gdi_obj *o = obj_of(h, OBJ_REGION); return o ? o->region : NULL; }
// A window DC given back (ReleaseDC, EndPaint): what it drew may be a finished picture (present.c).
void vx_dc_release(HANDLE h) {
    gdi_obj *d = dc_of(h);
    if (!d || d->type != OBJ_DC) return;
    vx_window *w = d->w;
    free_obj(h);
    if (w) vx_present_dc_released(w);
}

static WINAPI HANDLE GetDC(HANDLE hwnd) {
    if (!hwnd) return new_obj(new_dc(OBJ_DC, NULL));          // the screen: answers questions, draws nothing
    vx_window *w = vx_window_get(hwnd);
    return w ? vx_dc_for_window(w) : NULL;
}
static WINAPI HANDLE GetWindowDC(HANDLE hwnd) { return GetDC(hwnd); }
// GetDCEx: a window DC whose clip region the flags shape: DCX_INTERSECTRGN / DCX_EXCLUDERGN with the region given (which
// becomes the system's, deleted with the DC), DCX_INTERSECTUPDATE / DCX_EXCLUDEUPDATE with the window's update region
// (DCX_VALIDATE then validates it). Windows here have no non-client area: DCX_WINDOW is the client area too.
#define DCX_EXCLUDERGN 0x40
#define DCX_INTERSECTRGN 0x80
#define DCX_EXCLUDEUPDATE 0x100
#define DCX_INTERSECTUPDATE 0x200
#define DCX_VALIDATE 0x200000
static WINAPI HANDLE GetDCEx(HANDLE hwnd, HANDLE rgn, DWORD flags) {
    HANDLE h = GetDC(hwnd);
    gdi_obj *dc = dc_of(h);
    vx_region *given = (uintptr_t)rgn > 1 ? vx_gdi_region(rgn) : NULL;
    if (!dc || !dc->w || !(flags & (DCX_EXCLUDERGN | DCX_INTERSECTRGN | DCX_EXCLUDEUPDATE | DCX_INTERSECTUPDATE))) {
        if (dc && given && (flags & (DCX_EXCLUDERGN | DCX_INTERSECTRGN))) dc->owned_rgn = rgn;
        return h;
    }
    vx_window *w = dc->w;
    vx_region *clip = vx_region_new(), *with = vx_region_new();
    vx_region_set_rect(clip, 0, 0, w->client_w, w->client_h);
    if (given && (flags & (DCX_EXCLUDERGN | DCX_INTERSECTRGN))) {
        vx_region_combine(clip, clip, given, flags & DCX_INTERSECTRGN ? RGN_AND : RGN_DIFF);
        dc->owned_rgn = rgn;
    }
    if (flags & (DCX_EXCLUDEUPDATE | DCX_INTERSECTUPDATE)) {
        pthread_mutex_lock(&w->lock);
        if (!w->invalid) with->n = 0;
        else if (w->update) vx_region_copy(with, w->update);
        else vx_region_set_rect(with, 0, 0, w->client_w, w->client_h);
        pthread_mutex_unlock(&w->lock);
        vx_region_combine(clip, clip, with, flags & DCX_INTERSECTUPDATE ? RGN_AND : RGN_DIFF);
        if ((flags & DCX_INTERSECTUPDATE) && (flags & DCX_VALIDATE)) vx_window_validate_all(w);
    }
    vx_region_free(with);
    dc->clip = clip;
    return h;
}
static WINAPI int ReleaseDC(HANDLE hwnd, HANDLE dc) { (void)hwnd; gdi_obj *d = dc_of(dc); if (!d || d->type != OBJ_DC) return 0; vx_dc_release(dc); return 1; }
static WINAPI HANDLE CreateCompatibleDC(HANDLE dc) {
    (void)dc;
    gdi_obj *d = new_dc(OBJ_MEMDC, NULL);
    d->bitmap = g_default_bitmap;
    return new_obj(d);
}
static WINAPI HANDLE CreateDCA(const char *drv, const char *dev, const char *out, const void *dm) { (void)drv; (void)dev; (void)out; (void)dm; return GetDC(NULL); }
static WINAPI HANDLE CreateDCW(const WCHAR *drv, const WCHAR *dev, const WCHAR *out, const void *dm) { (void)drv; (void)dev; (void)out; (void)dm; return GetDC(NULL); }
static WINAPI BOOL DeleteDC(HANDLE h) {
    gdi_obj *d = dc_of(h);
    if (!d) return 0;
    vx_window *w = d->type == OBJ_DC ? d->w : NULL;
    free_obj(h);
    if (w) vx_present_dc_released(w);
    return 1;
}
static WINAPI HANDLE WindowFromDC(HANDLE h) { gdi_obj *d = dc_of(h); return d && d->w ? d->w->hwnd : NULL; }

static WINAPI int GetDeviceCaps(HANDLE h, int index) {
    (void)h;
    RECT s; vx_virtual_screen(&s);
    switch (index) {
    case 2: return 1;                                            // TECHNOLOGY: DT_RASDISPLAY
    case 4: return s.right * 254 / 960;                          // HORZSIZE, millimetres at 96 dpi
    case 6: return s.bottom * 254 / 960;
    case 8: case 118: return s.right;                            // HORZRES, DESKTOPHORZRES
    case 10: case 117: return s.bottom;                          // VERTRES, DESKTOPVERTRES
    case 12: return 32;                                          // BITSPIXEL
    case 14: return 1;                                           // PLANES
    case 24: return -1;                                          // NUMCOLORS
    case 38: return 0x1 | 0x8 | 0x80 | 0x200 | 0x800 | 0x2000;   // RASTERCAPS: BITBLT, BITMAP64, DI_BITMAP, DIBTODEV, STRETCHBLT, STRETCHDIB
    case 88: case 90: return 96;                                 // LOGPIXELSX/Y
    case 104: return 0;                                          // SIZEPALETTE
    case 116: return 60;                                         // VREFRESH
    case 120: return 0x1 | 0x2 | 0x10;                           // SHADEBLENDCAPS: SB_CONST_ALPHA, SB_PIXEL_ALPHA, SB_PREMULT_ALPHA
    default: return 0;
    }
}

// ---- objects -------------------------------------------------------------------------------------------------------------
static HANDLE make_brush(int style, uint32_t color, int stock) {
    gdi_obj *b = calloc(1, sizeof *b);
    b->type = OBJ_BRUSH; b->style = style; b->color = color; b->stock = stock;
    return new_obj(b);
}
static HANDLE make_pen(int style, int width, uint32_t color, int stock) {
    gdi_obj *p = calloc(1, sizeof *p);
    p->type = OBJ_PEN; p->style = style; p->width = width < 1 ? 1 : width; p->color = color; p->stock = stock;
    return new_obj(p);
}
static HANDLE make_font(int height, int weight, int italic, const char *face, int stock) {
    gdi_obj *f = calloc(1, sizeof *f);
    f->type = OBJ_FONT; f->font_height = height; f->font_weight = weight; f->italic = italic; f->stock = stock;
    snprintf(f->face, sizeof f->face, "%s", face ? face : "");
    return new_obj(f);
}

static HANDLE make_bitmap(int w, int h, int bpp, int bottom_up, const void *init, uint8_t *external) {
    if (w <= 0 || h <= 0) return NULL;
    gdi_obj *b = calloc(1, sizeof *b);
    b->type = OBJ_BITMAP; b->bw = w; b->bh = h; b->bpp = bpp; b->bottom_up = bottom_up;
    size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
    b->bits = external ? external : calloc(1, stride * (size_t)h);
    if (!b->bits) { free(b); vx_set_last_error(8 /* ERROR_NOT_ENOUGH_MEMORY */); return NULL; }
    b->own_bits = !external;
    if (init) memcpy(b->bits, init, stride * (size_t)h);
    if (bpp == 32) {
        b->pixels = (uint32_t *)b->bits;
        b->ctx = make_context(b->pixels, w, h, bottom_up);
    }
    return new_obj(b);
}
static WINAPI HANDLE CreateCompatibleBitmap(HANDLE dc, int w, int h) { (void)dc; return make_bitmap(w, h, 32, 0, NULL, NULL); }
static WINAPI HANDLE CreateBitmap(int w, int h, uint32_t planes, uint32_t bpp, const void *bits) {
    (void)planes;
    if (bpp != 32) return make_bitmap(w, h, 32, 0, NULL, NULL);   // other depths start blank (converted surfaces later)
    return make_bitmap(w, h, 32, 0, bits, NULL);
}

// The masks of a 16- or 32-bpp BI_BITFIELDS DIB: in the header from BITMAPV2INFOHEADER on, after it before that.
static const uint32_t *dib_masks(const vx_bmih *bi) {
    return bi->size >= 52 ? (const uint32_t *)((const uint8_t *)bi + 40) : (const uint32_t *)((const uint8_t *)bi + bi->size);
}
// CreateDIBSection: a bitmap whose bits the program writes directly. With a file mapping (hSection), the bits are a
// view of it at dwOffset (a multiple of 4), which another view of the same mapping sees: Chromium's software output
// draws into shared memory that way and blits it from a DIB section made over it.
static WINAPI HANDLE CreateDIBSection(HANDLE dc, const vx_bmih *bi, uint32_t usage, void **bits, HANDLE section, DWORD offset) {
    (void)dc; (void)usage;
    if (bits) *bits = NULL;
    int bpp = bi ? bi->bpp : 0;
    if (!bi || bi->width <= 0 || !bi->height || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) ||
        (bi->compression != 0 /* BI_RGB */ && !(bi->compression == 3 /* BI_BITFIELDS */ && (bpp == 16 || bpp == 32)))) {
        vx_set_last_error(87 /* ERROR_INVALID_PARAMETER */);
        return NULL;
    }
    int h = bi->height < 0 ? -bi->height : bi->height;
    size_t size = (((size_t)bi->width * bpp + 31) / 32) * 4 * (size_t)h;
    uint8_t *mem = NULL; void *view = NULL;
    if (section) {
        if (offset % 4) { vx_set_last_error(87); return NULL; }
        void *(WINAPI *map)(HANDLE, DWORD, DWORD, DWORD, size_t) = (void *(WINAPI *)(HANDLE, DWORD, DWORD, DWORD, size_t))vx_find_export("kernel32.dll", "MapViewOfFile");
        size_t page = (size_t)getpagesize(), base = offset & ~(page - 1);
        view = map ? map(section, 0x6 /* FILE_MAP_READ | FILE_MAP_WRITE */, 0, (DWORD)base, offset - base + size) : NULL;
        if (!view) return NULL;
        mem = (uint8_t *)view + (offset - base);
    }
    HANDLE hb = make_bitmap(bi->width, h, bpp, bi->height > 0, NULL, mem);
    gdi_obj *b = obj(hb);
    if (!b) {
        if (view) { BOOL (WINAPI *unmap)(void *) = (BOOL (WINAPI *)(void *))vx_find_export("kernel32.dll", "UnmapViewOfFile"); if (unmap) unmap(view); }
        return NULL;
    }
    b->dib = 1; b->section = section; b->offset = offset; b->view = view;
    if (bpp <= 8) {
        DWORD n = bi->used && bi->used <= (1u << bpp) ? bi->used : 1u << bpp;
        memcpy(b->palette, (const uint8_t *)bi + bi->size, sizeof(uint32_t) * n);
    }
    if (bi->compression == 3) memcpy(b->masks, dib_masks(bi), sizeof b->masks);
    else if (bpp == 16) { b->masks[0] = 0x7c00; b->masks[1] = 0x03e0; b->masks[2] = 0x001f; }
    else if (bpp == 32) { b->masks[0] = 0xff0000; b->masks[1] = 0xff00; b->masks[2] = 0xff; }
    if (bits) *bits = b->bits;
    return hb;
}

static WINAPI HANDLE CreateSolidBrush(uint32_t color) { return make_brush(0, color, 0); }
static WINAPI HANDLE CreateBrushIndirect(const uint32_t *lb) { return make_brush((int)lb[0], lb[1], 0); }
static WINAPI HANDLE CreatePen(int style, int width, uint32_t color) { return make_pen(style, width, color, 0); }
static WINAPI HANDLE GetSysColorBrush(int i) { return make_brush(0, vx_sys_color(i), 0); }
static WINAPI DWORD GetSysColor(int i) { return vx_sys_color(i); }

// LOGFONT: height, width, escapement, orientation, weight, italic, underline, strike-out, charset, precisions,
// quality, pitch, face name (32 characters).
static WINAPI HANDLE CreateFontIndirectA(const uint8_t *lf) { return make_font(*(const LONG *)lf, *(const LONG *)(lf + 16), lf[20], (const char *)lf + 28, 0); }
static WINAPI HANDLE CreateFontIndirectW(const uint8_t *lf) {
    char face[64]; size_t n = vx_utf16_to_utf8((const WCHAR *)(lf + 28), vx_wcslen((const WCHAR *)(lf + 28)), face, sizeof face - 1);
    face[n < sizeof face ? n : sizeof face - 1] = 0;
    return make_font(*(const LONG *)lf, *(const LONG *)(lf + 16), lf[20], face, 0);
}
static WINAPI HANDLE CreateFontA(int h, int w, int esc, int orient, int weight, DWORD italic, DWORD ul, DWORD so, DWORD cs, DWORD op, DWORD cp, DWORD q, DWORD pf, const char *face) {
    (void)w; (void)esc; (void)orient; (void)ul; (void)so; (void)cs; (void)op; (void)cp; (void)q; (void)pf;
    return make_font(h, weight, (int)italic, face, 0);
}
static WINAPI HANDLE CreateFontW(int h, int w, int esc, int orient, int weight, DWORD italic, DWORD ul, DWORD so, DWORD cs, DWORD op, DWORD cp, DWORD q, DWORD pf, const WCHAR *face) {
    char *f = vx_utf8_dup(face);
    HANDLE r = CreateFontA(h, w, esc, orient, weight, italic, ul, so, cs, op, cp, q, pf, f);
    free(f);
    return r;
}

static WINAPI HANDLE GetStockObject(int i) { return i >= 0 && i < 20 ? g_stock[i] : NULL; }
static WINAPI BOOL DeleteObject(HANDLE h) {
    gdi_obj *o = obj(h);
    if (!o || o->type == OBJ_DC || o->type == OBJ_MEMDC) return 0;
    if (!o->stock) free_obj(h);
    return 1;
}
static WINAPI DWORD GetObjectType(HANDLE h) { gdi_obj *o = obj(h); return o ? (DWORD)o->type : 0; }

// ---- the clip region ---------------------------------------------------------------------------------------------------------
// The clip region's type, as the clip functions return it (no clip region: the whole surface, a simple region).
static int clip_type(const gdi_obj *dc) { return dc->clip ? vx_region_type(dc->clip) : SIMPLEREGION; }
// The DC's clip region combined with r (device coordinates) by mode; with no clip region yet, the whole surface is it.
static int clip_combine(gdi_obj *dc, const vx_region *r, int mode) {
    if (mode == RGN_COPY) {
        if (!r) { vx_region_free(dc->clip); dc->clip = NULL; return SIMPLEREGION; }
        if (!dc->clip) dc->clip = vx_region_new();
        vx_region_copy(dc->clip, r);
        return clip_type(dc);
    }
    if (!r) return ERROR_RGN;
    if (!dc->clip) {
        RECT s = surface_rect(dc);
        dc->clip = vx_region_new();
        vx_region_set_rect(dc->clip, s.left, s.top, s.right, s.bottom);
    }
    return vx_region_combine(dc->clip, dc->clip, r, mode);
}
static WINAPI int ExtSelectClipRgn(HANDLE h, HANDLE rgn, int mode) {
    gdi_obj *dc = dc_of(h);
    vx_region *r = rgn ? vx_gdi_region(rgn) : NULL;
    if (!dc || (rgn && !r) || mode < RGN_AND || mode > RGN_COPY || (!rgn && mode != RGN_COPY)) { vx_set_last_error(87); return ERROR_RGN; }
    return clip_combine(dc, r, mode);
}
static WINAPI int SelectClipRgn(HANDLE h, HANDLE rgn) { return ExtSelectClipRgn(h, rgn, RGN_COPY); }
static int clip_with_rect(HANDLE h, int l, int t, int r, int b, int mode) {
    gdi_obj *dc = dc_of(h);
    if (!dc) return ERROR_RGN;
    vx_region *g = vx_region_new();
    RECT d = to_device(dc, (RECT){ l, t, r, b });
    vx_region_set_rect(g, d.left, d.top, d.right, d.bottom);
    int type = clip_combine(dc, g, mode);
    vx_region_free(g);
    return type;
}
static WINAPI int IntersectClipRect(HANDLE dc, int l, int t, int r, int b) { return clip_with_rect(dc, l, t, r, b, RGN_AND); }
static WINAPI int ExcludeClipRect(HANDLE dc, int l, int t, int r, int b) { return clip_with_rect(dc, l, t, r, b, RGN_DIFF); }
static WINAPI int OffsetClipRgn(HANDLE h, int x, int y) {
    gdi_obj *dc = dc_of(h);
    if (!dc) return ERROR_RGN;
    if (dc->clip) vx_region_offset(dc->clip, x, y);
    return clip_type(dc);
}
// GetClipBox: the smallest rectangle around what can be drawn on (logical coordinates).
static WINAPI int GetClipBox(HANDLE h, RECT *out) {
    gdi_obj *dc = dc_of(h);
    if (!dc || !out) return ERROR_RGN;
    RECT s = surface_rect(dc), b = s;
    int type = SIMPLEREGION;
    if (dc->clip) {
        vx_region *g = vx_region_new();
        vx_region_set_rect(g, s.left, s.top, s.right, s.bottom);
        type = vx_region_combine(g, g, dc->clip, RGN_AND);
        b = vx_region_box(g);
        vx_region_free(g);
    }
    *out = (RECT){ b.left - dc_dx(dc), b.top - dc_dy(dc), b.right - dc_dx(dc), b.bottom - dc_dy(dc) };
    return type;
}
// GetClipRgn: 1 and a copy of the clip region (device coordinates), 0 when the DC has none, -1 on error.
static WINAPI int GetClipRgn(HANDLE h, HANDLE rgn) {
    gdi_obj *dc = dc_of(h);
    vx_region *r = vx_gdi_region(rgn);
    if (!dc || !r) return -1;
    if (!dc->clip) return 0;
    vx_region_copy(r, dc->clip);
    return 1;
}

static WINAPI HANDLE SelectObject(HANDLE dch, HANDLE h) {
    gdi_obj *dc = dc_of(dch), *o = obj(h);
    if (!dc || !o) return NULL;
    if (o->type == OBJ_REGION) return (HANDLE)(intptr_t)clip_combine(dc, o->region, RGN_COPY);   // as SelectClipRgn
    HANDLE *slot = o->type == OBJ_BRUSH ? &dc->brush : o->type == OBJ_PEN ? &dc->pen : o->type == OBJ_FONT ? &dc->font :
                   o->type == OBJ_BITMAP && dc->type == OBJ_MEMDC ? &dc->bitmap : NULL;
    if (!slot) return NULL;
    HANDLE old = *slot;
    *slot = h;
    return old;
}
static WINAPI HANDLE GetCurrentObject(HANDLE dch, uint32_t type) {
    gdi_obj *dc = dc_of(dch);
    if (!dc) return NULL;
    return type == OBJ_BRUSH ? dc->brush : type == OBJ_PEN ? dc->pen : type == OBJ_FONT ? dc->font : type == OBJ_BITMAP ? dc->bitmap : NULL;
}
// BITMAP: type, width, height, width in bytes, planes, bits per pixel, bits (a DIB section's only). DIBSECTION: that,
// then its BITMAPINFOHEADER, the three masks, the section and the offset.
static int get_object(HANDLE h, int size, void *out, int wide) {
    gdi_obj *o = obj(h);
    if (!o) return 0;
    if (o->type == OBJ_BITMAP) {
        int full = o->dib && size >= 104 ? 104 : 32;
        if (!out) return o->dib ? 104 : 32;
        uint8_t bm[104] = { 0 };
        LONG stride = (LONG)((((size_t)o->bw * o->bpp + 31) / 32) * 4);
        *(LONG *)(bm + 4) = o->bw; *(LONG *)(bm + 8) = o->bh; *(LONG *)(bm + 12) = stride;
        *(uint16_t *)(bm + 16) = 1; *(uint16_t *)(bm + 18) = (uint16_t)o->bpp; *(void **)(bm + 24) = o->dib ? o->bits : NULL;
        if (full == 104) {
            vx_bmih bi = { 40, o->bw, o->bottom_up ? o->bh : -o->bh, 1, (uint16_t)o->bpp, 0, (DWORD)(stride * o->bh), 0, 0, 0, 0 };
            memcpy(bm + 32, &bi, 40);
            memcpy(bm + 72, o->masks, 12);
            *(HANDLE *)(bm + 88) = o->section; *(DWORD *)(bm + 96) = o->offset;
        }
        memcpy(out, bm, size < full ? (size_t)size : (size_t)full);
        return size < full ? size : full;
    }
    if (o->type == OBJ_BRUSH) { if (!out) return 16; uint32_t lb[4] = { (uint32_t)o->style, o->color, 0, 0 }; memcpy(out, lb, size < 16 ? (size_t)size : 16); return 16; }
    if (o->type == OBJ_PEN) { if (!out) return 16; uint32_t lp[4] = { (uint32_t)o->style, (uint32_t)o->width, 0, o->color }; memcpy(out, lp, size < 16 ? (size_t)size : 16); return 16; }
    if (o->type == OBJ_FONT) {
        int full = wide ? 92 : 60;
        if (!out) return full;
        uint8_t lf[92] = { 0 };
        *(LONG *)lf = o->font_height; *(LONG *)(lf + 16) = o->font_weight; lf[20] = (uint8_t)o->italic;
        if (wide) vx_utf8_to_utf16(o->face, strlen(o->face), (WCHAR *)(lf + 28), 31); else snprintf((char *)lf + 28, 32, "%s", o->face);
        memcpy(out, lf, size < full ? (size_t)size : (size_t)full);
        return size < full ? size : full;
    }
    return 0;
}
static WINAPI int GetObjectA(HANDLE h, int size, void *out) { return get_object(h, size, out, 0); }
static WINAPI int GetObjectW(HANDLE h, int size, void *out) { return get_object(h, size, out, 1); }

static WINAPI uint32_t SetTextColor(HANDLE h, uint32_t c) { gdi_obj *d = dc_of(h); if (!d) return 0xffffffffu; uint32_t o = d->text_color; d->text_color = c; return o; }
static WINAPI uint32_t GetTextColor(HANDLE h) { gdi_obj *d = dc_of(h); return d ? d->text_color : 0; }
static WINAPI uint32_t SetBkColor(HANDLE h, uint32_t c) { gdi_obj *d = dc_of(h); if (!d) return 0xffffffffu; uint32_t o = d->bk_color; d->bk_color = c; return o; }
static WINAPI uint32_t GetBkColor(HANDLE h) { gdi_obj *d = dc_of(h); return d ? d->bk_color : 0; }
static WINAPI int SetBkMode(HANDLE h, int m) { gdi_obj *d = dc_of(h); if (!d) return 0; int o = d->bk_mode; d->bk_mode = m; return o; }
static WINAPI int GetBkMode(HANDLE h) { gdi_obj *d = dc_of(h); return d ? d->bk_mode : 0; }
static WINAPI uint32_t SetDCBrushColor(HANDLE h, uint32_t c) { gdi_obj *d = dc_of(h); if (!d) return 0xffffffffu; uint32_t o = d->dc_brush; d->dc_brush = c; return o; }
static WINAPI uint32_t SetDCPenColor(HANDLE h, uint32_t c) { gdi_obj *d = dc_of(h); if (!d) return 0xffffffffu; uint32_t o = d->dc_pen; d->dc_pen = c; return o; }
// Modes kept and reported; the stretch mode chooses how StretchBlt samples (present.c). Only MM_TEXT maps coordinates
// (one logical unit is one pixel); text alignment and the advanced graphics mode are not applied to drawing yet.
static WINAPI int SetStretchBltMode(HANDLE h, int m) { gdi_obj *d = dc_of(h); if (!d || m < 1 || m > 4) return 0; int o = d->stretch_mode; d->stretch_mode = m; return o; }
static WINAPI int GetStretchBltMode(HANDLE h) { gdi_obj *d = dc_of(h); return d ? d->stretch_mode : 0; }
static WINAPI int SetMapMode(HANDLE h, int m) { gdi_obj *d = dc_of(h); if (!d || m < 1 || m > 8) return 0; int o = d->map_mode; d->map_mode = m; return o; }
static WINAPI int GetMapMode(HANDLE h) { gdi_obj *d = dc_of(h); return d ? d->map_mode : 0; }
static WINAPI uint32_t SetTextAlign(HANDLE h, uint32_t a) { gdi_obj *d = dc_of(h); if (!d) return 0xffffffffu; uint32_t o = d->text_align; d->text_align = a; return o; }
static WINAPI uint32_t GetTextAlign(HANDLE h) { gdi_obj *d = dc_of(h); return d ? d->text_align : 0xffffffffu; }
static WINAPI int SetGraphicsMode(HANDLE h, int m) { gdi_obj *d = dc_of(h); if (!d || m < 1 || m > 2) return 0; int o = d->graphics_mode; d->graphics_mode = m; return o; }
static WINAPI int GetGraphicsMode(HANDLE h) { gdi_obj *d = dc_of(h); return d ? d->graphics_mode : 0; }

// Origins: the viewport origin is in device units, the window origin in logical ones.
static BOOL set_origin(HANDLE h, int window, int x, int y, POINT *old, int offset) {
    gdi_obj *d = dc_of(h);
    if (!d) return 0;
    POINT *p = window ? &d->win_org : &d->vp_org;
    if (old) *old = *p;
    if (offset) { p->x += x; p->y += y; } else *p = (POINT){ x, y };
    return 1;
}
static WINAPI BOOL SetViewportOrgEx(HANDLE h, int x, int y, POINT *old) { return set_origin(h, 0, x, y, old, 0); }
static WINAPI BOOL OffsetViewportOrgEx(HANDLE h, int x, int y, POINT *old) { return set_origin(h, 0, x, y, old, 1); }
static WINAPI BOOL GetViewportOrgEx(HANDLE h, POINT *p) { gdi_obj *d = dc_of(h); if (!d || !p) return 0; *p = d->vp_org; return 1; }
static WINAPI BOOL SetWindowOrgEx(HANDLE h, int x, int y, POINT *old) { return set_origin(h, 1, x, y, old, 0); }
static WINAPI BOOL OffsetWindowOrgEx(HANDLE h, int x, int y, POINT *old) { return set_origin(h, 1, x, y, old, 1); }
static WINAPI BOOL GetWindowOrgEx(HANDLE h, POINT *p) { gdi_obj *d = dc_of(h); if (!d || !p) return 0; *p = d->win_org; return 1; }
static WINAPI BOOL LPtoDP(HANDLE h, POINT *p, int n) { gdi_obj *d = dc_of(h); if (!d) return 0; for (int i = 0; i < n; i++) { p[i].x += dc_dx(d); p[i].y += dc_dy(d); } return 1; }
static WINAPI BOOL DPtoLP(HANDLE h, POINT *p, int n) { gdi_obj *d = dc_of(h); if (!d) return 0; for (int i = 0; i < n; i++) { p[i].x -= dc_dx(d); p[i].y -= dc_dy(d); } return 1; }
static WINAPI BOOL GetCurrentPositionEx(HANDLE h, POINT *p) { gdi_obj *d = dc_of(h); if (!d || !p) return 0; *p = d->pos; return 1; }

// SaveDC / RestoreDC: the state above (objects, colors, modes, origins, the clip region) on a stack. RestoreDC takes
// a level SaveDC returned, or -n for the n-th most recent; the states above it are dropped.
static WINAPI int SaveDC(HANDLE h) {
    gdi_obj *d = dc_of(h);
    if (!d) return 0;
    dc_state *s = realloc(d->saved, sizeof *s * (size_t)(d->nsaved + 1));
    if (!s) return 0;
    d->saved = s;
    s += d->nsaved;
    *s = (dc_state){ d->bitmap, d->brush, d->pen, d->font, d->text_color, d->bk_color, d->dc_brush, d->dc_pen, d->text_align,
                     d->bk_mode, d->map_mode, d->stretch_mode, d->graphics_mode, d->pos, d->vp_org, d->win_org, NULL };
    if (d->clip) { s->clip = vx_region_new(); vx_region_copy(s->clip, d->clip); }
    return ++d->nsaved;
}
static WINAPI BOOL RestoreDC(HANDLE h, int level) {
    gdi_obj *d = dc_of(h);
    if (!d) return 0;
    int i = level > 0 ? level - 1 : d->nsaved + level;
    if (!level || i < 0 || i >= d->nsaved) return 0;
    dc_state *s = &d->saved[i];
    d->bitmap = s->bitmap; d->brush = s->brush; d->pen = s->pen; d->font = s->font;
    d->text_color = s->text_color; d->bk_color = s->bk_color; d->dc_brush = s->dc_brush; d->dc_pen = s->dc_pen; d->text_align = s->text_align;
    d->bk_mode = s->bk_mode; d->map_mode = s->map_mode; d->stretch_mode = s->stretch_mode; d->graphics_mode = s->graphics_mode;
    d->pos = s->pos; d->vp_org = s->vp_org; d->win_org = s->win_org;
    vx_region_free(d->clip);
    d->clip = s->clip; s->clip = NULL;
    for (int k = i + 1; k < d->nsaved; k++) vx_region_free(d->saved[k].clip);
    d->nsaved = i;
    return 1;
}
static WINAPI BOOL GdiFlush(void) { return 1; }

// ---- drawing -----------------------------------------------------------------------------------------------------------------
static void fill(gdi_obj *dc, RECT r, uint32_t color) {
    target t;
    if (!lock_target(dc, &t)) return;
    set_fill(t.ctx, color);
    CGContextFillRect(t.ctx, CGRectMake(r.left, r.top, r.right - r.left, r.bottom - r.top));
    unlock_target(&t, &r);
    texts_painted(dc, r);
}
static WINAPI int FillRect(HANDLE h, const RECT *r, HANDLE brush) {
    gdi_obj *dc = dc_of(h);
    uint32_t color;
    if (!dc || !r || !brush_color(brush, &color, dc)) return 0;
    fill(dc, *r, color);
    return 1;
}
static WINAPI int FrameRect(HANDLE h, const RECT *r, HANDLE brush) {
    gdi_obj *dc = dc_of(h);
    uint32_t color;
    if (!dc || !brush_color(brush, &color, dc)) return 0;
    fill(dc, (RECT){ r->left, r->top, r->right, r->top + 1 }, color);
    fill(dc, (RECT){ r->left, r->bottom - 1, r->right, r->bottom }, color);
    fill(dc, (RECT){ r->left, r->top, r->left + 1, r->bottom }, color);
    fill(dc, (RECT){ r->right - 1, r->top, r->right, r->bottom }, color);
    return 1;
}
void vx_fill_client(vx_window *w, HANDLE brush) {
    HANDLE dc = vx_dc_for_window(w);
    RECT r = { 0, 0, w->client_w, w->client_h };
    FillRect(dc, &r, brush);
    vx_dc_release(dc);
}
// Pixels of a device rectangle (inside the clip region) XORed with a value: DSTINVERT, PATINVERT.
static void xor_rect(gdi_obj *dc, RECT r, uint32_t value) {
    target t;
    if (!lock_target(dc, &t)) return;
    RECT d = { r.left + t.ox, r.top + t.oy, r.right + t.ox, r.bottom + t.oy };
    if (d.left < 0) d.left = 0;
    if (d.top < 0) d.top = 0;
    if (d.right > t.w) d.right = t.w;
    if (d.bottom > t.h) d.bottom = t.h;
    for (int y = d.top; y < d.bottom; y++) {
        uint32_t *row = t.pixels + (size_t)(t.bottom_up ? t.h - 1 - y : y) * t.w;
        for (int x = d.left; x < d.right; x++) if (in_clip(&t, x, y)) row[x] ^= value;
    }
    unlock_target(&t, &r);
}
static WINAPI BOOL PatBlt(HANDLE h, int x, int y, int w, int hgt, DWORD rop) {
    gdi_obj *dc = dc_of(h);
    if (!dc) return 0;
    if (w < 0) { x += w; w = -w; }
    if (hgt < 0) { y += hgt; hgt = -hgt; }
    RECT r = { x, y, x + w, y + hgt };
    uint32_t color = 0;
    if (rop == 0x00550009) { xor_rect(dc, r, 0x00ffffff); return 1; }               // DSTINVERT
    if (rop == 0x005A0049) {                                                        // PATINVERT
        if (brush_color(dc->brush, &color, dc)) xor_rect(dc, r, colorref_to_bgrx(color) & 0x00ffffff);
        return 1;
    }
    if (rop == 0x00FF0062) color = 0xFFFFFF;                     // WHITENESS
    else if (rop != 0x00000042 && !brush_color(dc->brush, &color, dc)) return 1;   // PATCOPY and others: the brush (NULL: nothing)
    fill(dc, r, color);
    return 1;
}

// Shapes are outlined with the pen and filled with the brush (a NULL brush or pen skips that part).
static void shape(HANDLE h, RECT r, int ellipse) {
    gdi_obj *dc = dc_of(h);
    target t;
    if (!dc || !lock_target(dc, &t)) return;
    CGRect cr = CGRectMake(r.left + 0.5, r.top + 0.5, r.right - r.left - 1, r.bottom - r.top - 1);
    uint32_t color;
    if (brush_color(dc->brush, &color, dc)) {
        set_fill(t.ctx, color);
        if (ellipse) CGContextFillEllipseInRect(t.ctx, cr); else CGContextFillRect(t.ctx, cr);
    }
    gdi_obj *pen = obj_of(dc->pen, OBJ_PEN);
    int pw = 0;
    if (pen && pen->style != 5 /* PS_NULL */) {
        set_stroke(t.ctx, pen->stock == 19 ? dc->dc_pen : pen->color);
        CGContextSetLineWidth(t.ctx, pen->width);
        if (ellipse) CGContextStrokeEllipseInRect(t.ctx, cr); else CGContextStrokeRect(t.ctx, cr);
        pw = pen->width;
    }
    RECT drawn = { r.left - pw, r.top - pw, r.right + pw, r.bottom + pw };
    unlock_target(&t, &drawn);
}
static WINAPI BOOL Rectangle(HANDLE h, int l, int t, int r, int b) { shape(h, (RECT){ l, t, r, b }, 0); return 1; }
static WINAPI BOOL Ellipse(HANDLE h, int l, int t, int r, int b) { shape(h, (RECT){ l, t, r, b }, 1); return 1; }
static WINAPI BOOL MoveToEx(HANDLE h, int x, int y, POINT *old) {
    gdi_obj *dc = dc_of(h);
    if (!dc) return 0;
    if (old) *old = dc->pos;
    dc->pos = (POINT){ x, y };
    return 1;
}
static WINAPI BOOL LineTo(HANDLE h, int x, int y) {
    gdi_obj *dc = dc_of(h);
    target t;
    if (!dc) return 0;
    gdi_obj *pen = obj_of(dc->pen, OBJ_PEN);
    if (pen && pen->style != 5 && lock_target(dc, &t)) {
        set_stroke(t.ctx, pen->stock == 19 ? dc->dc_pen : pen->color);
        CGContextSetLineWidth(t.ctx, pen->width);
        CGContextMoveToPoint(t.ctx, dc->pos.x + 0.5, dc->pos.y + 0.5);
        CGContextAddLineToPoint(t.ctx, x + 0.5, y + 0.5);
        CGContextStrokePath(t.ctx);
        int pw = pen->width + 1;
        RECT drawn = { (dc->pos.x < x ? dc->pos.x : x) - pw, (dc->pos.y < y ? dc->pos.y : y) - pw,
                       (dc->pos.x > x ? dc->pos.x : x) + pw, (dc->pos.y > y ? dc->pos.y : y) + pw };
        unlock_target(&t, &drawn);
    }
    dc->pos = (POINT){ x, y };
    return 1;
}
static WINAPI uint32_t SetPixel(HANDLE h, int x, int y, uint32_t color) {
    gdi_obj *dc = dc_of(h);
    target t;
    if (!dc || !lock_target(dc, &t)) return 0xffffffffu;
    int dx = x + t.ox, dy = y + t.oy, set = in_clip(&t, dx, dy);
    if (set) t.pixels[(size_t)(t.bottom_up ? t.h - 1 - dy : dy) * t.w + dx] = colorref_to_bgrx(color);
    RECT drawn = { x, y, x + 1, y + 1 };
    unlock_target(&t, set ? &drawn : NULL);
    return set ? color : 0xffffffffu;
}
static WINAPI uint32_t GetPixel(HANDLE h, int x, int y) {
    gdi_obj *dc = dc_of(h);
    target t;
    if (!dc || !lock_target(dc, &t)) return 0xffffffffu;           // CLR_INVALID
    uint32_t c = 0xffffffffu;
    int dx = x + t.ox, dy = y + t.oy;
    if (in_clip(&t, dx, dy)) c = bgrx_to_colorref(t.pixels[(size_t)(t.bottom_up ? t.h - 1 - dy : dy) * t.w + dx]);
    unlock_target(&t, NULL);
    return c;
}

// ---- images ----------------------------------------------------------------------------------------------------------------------
// Converts DIB rows of any common format to a top-down BGRA buffer (malloc'd): 32 bpp keeps its fourth byte when
// `keep_alpha`, the other depths have none (0xff).
uint32_t *vx_dib_to_bgra(const vx_bmih *bi, const void *bits, const uint32_t *palette, int keep_alpha, int *w_out, int *h_out) {
    int w = bi->width, h = bi->height < 0 ? -bi->height : bi->height, bottom_up = bi->height > 0, bpp = bi->bpp;
    if (w <= 0 || h <= 0 || (bpp != 32 && bpp != 24 && bpp != 16 && bpp != 8 && bpp != 4 && bpp != 1)) return NULL;
    size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
    uint32_t *out = malloc((size_t)w * h * 4);
    if (!out) return NULL;
    const uint32_t *masks = dib_masks(bi);
    int is565 = bpp == 16 && bi->compression == 3 && masks[1] == 0x07E0;
    uint32_t alpha = keep_alpha ? 0 : 0xff000000u;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = (const uint8_t *)bits + stride * (size_t)(bottom_up ? h - 1 - y : y);
        uint32_t *o = out + (size_t)y * w;
        switch (bpp) {
        case 32: for (int x = 0; x < w; x++) o[x] = ((const uint32_t *)row)[x] | alpha; break;
        case 24: for (int x = 0; x < w; x++) o[x] = 0xff000000u | (uint32_t)row[x * 3 + 2] << 16 | (uint32_t)row[x * 3 + 1] << 8 | row[x * 3]; break;
        case 16:
            for (int x = 0; x < w; x++) {
                uint16_t p = ((const uint16_t *)row)[x];
                uint32_t r = is565 ? (p >> 11) & 31 : (p >> 10) & 31, g = is565 ? (p >> 5) & 63 : (p >> 5) & 31, b = p & 31;
                r = r * 255 / 31; g = is565 ? g * 255 / 63 : g * 255 / 31; b = b * 255 / 31;
                o[x] = 0xff000000u | r << 16 | g << 8 | b;
            }
            break;
        default: {
            int per = 8 / bpp, mask = (1 << bpp) - 1;
            for (int x = 0; x < w; x++) {
                int idx = (row[x / per] >> ((per - 1 - x % per) * bpp)) & mask;
                o[x] = palette[idx] | 0xff000000u;                 // RGBQUAD: B, G, R, reserved = BGRX already
            }
        }
        }
    }
    *w_out = w; *h_out = h;
    return out;
}

// ---- text ------------------------------------------------------------------------------------------------------------------------
// The DC's font at its em size in pixels (*em). LOGFONT's height, as Windows maps it: negative, the character (em)
// height; positive, the cell height (ascent + descent, which includes the internal leading), so the em is that
// scaled by the face's own em-to-cell ratio; 0, a default size. GetTextMetrics reports the same numbers back
// (character height = tmHeight - tmInternalLeading), which programs that derive a font of a wanted height rely on.
static CTFontRef ct_font_em(gdi_obj *dc, double *em);
static CTFontRef ct_font(gdi_obj *dc) { double em; return ct_font_em(dc, &em); }
static CTFontRef ct_font_em(gdi_obj *dc, double *em) {
    gdi_obj *f = obj_of(dc->font, OBJ_FONT);
    int height = f ? f->font_height : 0;
    double size = height < 0 ? -height : height > 0 ? height : 13;
    CTFontRef font = NULL;
    if (f && f->face[0]) {
        CFStringRef name = CFStringCreateWithCString(NULL, f->face, kCFStringEncodingUTF8);
        font = CTFontCreateWithName(name, size, NULL);
        CFRelease(name);
    }
    if (!font) font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, NULL);
    if (f && (f->font_weight >= 600 || f->italic)) {
        CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(font, size, NULL,
            (f->font_weight >= 600 ? kCTFontBoldTrait : 0) | (f->italic ? kCTFontItalicTrait : 0), kCTFontBoldTrait | kCTFontItalicTrait);
        if (styled) { CFRelease(font); font = styled; }
    }
    double cell = CTFontGetAscent(font) + CTFontGetDescent(font);
    if (height > 0 && cell > 0 && fabs(cell - size) > 1e-6) {          // the em whose cell is `height` pixels
        CTFontRef scaled = CTFontCreateCopyWithAttributes(font, size * size / cell, NULL, NULL);
        if (scaled) { CFRelease(font); font = scaled; size = CTFontGetSize(font); }
    }
    *em = size;
    return font;
}
static CTLineRef make_line(gdi_obj *dc, const uint16_t *text, int n, CTFontRef font) {
    CFStringRef s = CFStringCreateWithCharacters(NULL, text, n);
    uint32_t c = dc->text_color;
    CGColorRef color = CGColorCreateGenericRGB((c & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, ((c >> 16) & 0xff) / 255.0, 1);
    const void *keys[] = { kCTFontAttributeName, kCTForegroundColorAttributeName };
    const void *vals[] = { font, color };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(NULL, s, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    CFRelease(as); CFRelease(attrs); CGColorRelease(color); CFRelease(s);
    return line;
}
// Width and line height of a run of text in the DC's font.
static void measure(gdi_obj *dc, const uint16_t *text, int n, double *width, double *ascent, double *descent) {
    CTFontRef font = ct_font(dc);
    CTLineRef line = make_line(dc, text, n, font);
    double leading;
    *width = CTLineGetTypographicBounds(line, ascent, descent, &leading);
    CFRelease(line); CFRelease(font);
}
static void draw_text(gdi_obj *dc, int x, int y, const uint16_t *text, int n, const RECT *opaque) {
    target t;
    if (!lock_target(dc, &t)) return;
    CTFontRef font = ct_font(dc);
    CTLineRef line = make_line(dc, text, n, font);
    double ascent, descent, leading, width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    RECT box = { x, y, x + (LONG)(width + 0.5), y + (LONG)(ascent + descent + 0.5) }, drawn = box;
    if (opaque) {
        set_fill(t.ctx, dc->bk_color); CGContextFillRect(t.ctx, CGRectMake(opaque->left, opaque->top, opaque->right - opaque->left, opaque->bottom - opaque->top));
        drawn = (RECT){ opaque->left < box.left ? opaque->left : box.left, opaque->top < box.top ? opaque->top : box.top,
                        opaque->right > box.right ? opaque->right : box.right, opaque->bottom > box.bottom ? opaque->bottom : box.bottom };
    }
    else if (dc->bk_mode == OPAQUE_BK) { set_fill(t.ctx, dc->bk_color); CGContextFillRect(t.ctx, CGRectMake(x, y, width, ascent + descent)); }
    CGContextSaveGState(t.ctx);
    CGContextSetTextMatrix(t.ctx, CGAffineTransformMakeScale(1, -1));   // glyphs upright in y-down coordinates
    CGContextSetTextPosition(t.ctx, x, y + ascent);
    CTLineDraw(line, t.ctx);
    CGContextRestoreGState(t.ctx);
    CFRelease(line); CFRelease(font);
    drawn.left -= 2; drawn.right += 2;                              // (glyphs may overhang their advance a little)
    unlock_target(&t, &drawn);
    vx_texts *tx = texts_of(dc);
    if (tx && n > 0) {
        WCHAR *z = malloc(((size_t)n + 1) * sizeof *z);
        memcpy(z, text, (size_t)n * sizeof *z); z[n] = 0;
        char *u = vx_utf8_dup(z);
        free(z);
        pthread_mutex_lock(&vx_text_lock); texts_add(tx, to_device(dc, box), u); pthread_mutex_unlock(&vx_text_lock);
    }
}
static uint16_t *wide_from(const char *s, int n, int *len) {
    size_t l = n < 0 ? strlen(s) : (size_t)n;
    size_t wl = vx_utf8_to_utf16(s, l, NULL, 0);
    uint16_t *w = malloc((wl + 1) * 2);
    vx_utf8_to_utf16(s, l, w, wl);
    *len = (int)wl;
    return w;
}
static WINAPI BOOL TextOutW(HANDLE h, int x, int y, const uint16_t *s, int n) { gdi_obj *dc = dc_of(h); if (!dc) return 0; draw_text(dc, x, y, s, n, NULL); return 1; }
static WINAPI BOOL TextOutA(HANDLE h, int x, int y, const char *s, int n) { int l; uint16_t *w = wide_from(s, n, &l); BOOL r = TextOutW(h, x, y, w, l); free(w); return r; }
static WINAPI BOOL ExtTextOutW(HANDLE h, int x, int y, uint32_t opts, const RECT *r, const uint16_t *s, uint32_t n, const int *dx) {
    (void)dx;
    gdi_obj *dc = dc_of(h);
    if (!dc) return 0;
    if (s && n) draw_text(dc, x, y, s, (int)n, (opts & 2) && r ? r : NULL);   // ETO_OPAQUE
    else if ((opts & 2) && r) fill(dc, *r, dc->bk_color);
    return 1;
}
static WINAPI BOOL ExtTextOutA(HANDLE h, int x, int y, uint32_t opts, const RECT *r, const char *s, uint32_t n, const int *dx) {
    int l; uint16_t *w = wide_from(s ? s : "", s ? (int)n : 0, &l);
    BOOL ok = ExtTextOutW(h, x, y, opts, r, w, (uint32_t)l, dx);
    free(w);
    return ok;
}
static WINAPI BOOL GetTextExtentPoint32W(HANDLE h, const uint16_t *s, int n, POINT *size) {
    gdi_obj *dc = dc_of(h);
    if (!dc) return 0;
    double w, a, d;
    measure(dc, s, n, &w, &a, &d);
    size->x = (LONG)(w + 0.5); size->y = (LONG)(a + d + 0.5);
    return 1;
}
static WINAPI BOOL GetTextExtentPoint32A(HANDLE h, const char *s, int n, POINT *size) { int l; uint16_t *w = wide_from(s, n, &l); BOOL r = GetTextExtentPoint32W(h, w, l, size); free(w); return r; }
// TEXTMETRIC: eleven LONGs (height, ascent, descent, internal/external leading, average/max width, weight,
// overhang, aspect x/y), then the character fields.
// The heights are the font's ascent and descent; the internal leading is what of them lies above the em.
static BOOL text_metrics(HANDLE h, LONG *tm) {
    gdi_obj *dc = dc_of(h);
    if (!dc) return 0;
    double w, a, d, em;
    uint16_t x = 'x';
    measure(dc, &x, 1, &w, &a, &d);
    CTFontRef font = ct_font_em(dc, &em);
    double ascent = CTFontGetAscent(font), descent = CTFontGetDescent(font), leading = CTFontGetLeading(font);
    CFRelease(font);
    memset(tm, 0, 44);
    tm[0] = (LONG)lround(ascent + descent); tm[1] = (LONG)lround(ascent); tm[2] = tm[0] - tm[1];
    tm[3] = tm[0] > (LONG)lround(em) ? tm[0] - (LONG)lround(em) : 0;   // tmInternalLeading
    tm[4] = (LONG)lround(leading);                                       // tmExternalLeading
    tm[5] = (LONG)(w + 0.5); tm[6] = (LONG)(w * 2 + 0.5);
    gdi_obj *f = obj_of(dc->font, OBJ_FONT);
    tm[7] = f && f->font_weight ? f->font_weight : 400;
    tm[9] = tm[10] = 96;
    return 1;
}
static WINAPI BOOL GetTextMetricsA(HANDLE h, uint8_t *tm) { if (!text_metrics(h, (LONG *)tm)) return 0; tm[44] = 0x20; tm[45] = 0xff; tm[46] = '?'; tm[47] = ' '; return 1; }
static WINAPI BOOL GetTextMetricsW(HANDLE h, uint8_t *tm) {
    if (!text_metrics(h, (LONG *)tm)) return 0;
    *(uint16_t *)(tm + 44) = 0x20; *(uint16_t *)(tm + 46) = 0xfffd; *(uint16_t *)(tm + 48) = '?'; *(uint16_t *)(tm + 50) = ' ';
    return 1;
}

// DrawText: one line, aligned in the rectangle (DT_CENTER, DT_RIGHT, DT_VCENTER, DT_BOTTOM; DT_CALCRECT measures).
static WINAPI int DrawTextW(HANDLE h, const uint16_t *s, int n, RECT *r, uint32_t fmt) {
    gdi_obj *dc = dc_of(h);
    if (!dc) return 0;
    int len = n < 0 ? (int)vx_wcslen(s) : n;
    double w, a, d;
    measure(dc, s, len, &w, &a, &d);
    int th = (int)(a + d + 0.5);
    if (fmt & 0x400) { r->right = r->left + (LONG)(w + 0.5); r->bottom = r->top + th; return th; }
    int x = fmt & 1 ? (int)(r->left + (r->right - r->left - w) / 2) : fmt & 2 ? (int)(r->right - w) : r->left;
    int y = fmt & 4 ? r->top + (r->bottom - r->top - th) / 2 : fmt & 8 ? r->bottom - th : r->top;
    draw_text(dc, x, y, s, len, NULL);
    return th;
}
static WINAPI int DrawTextA(HANDLE h, const char *s, int n, RECT *r, uint32_t fmt) { int l; uint16_t *w = wide_from(s, n, &l); int res = DrawTextW(h, w, l, r, fmt); free(w); return res; }

// ---- bitmaps as plain pixels (icons.c: icons are made of bitmaps and give bitmaps back) ----------------------------------------
// A bitmap's pixels, top-down BGRA (malloc'd): a 32-bpp bitmap's fourth byte as stored, 0xff for other depths.
uint32_t *vx_gdi_bitmap_bgra(HANDLE h, int *w, int *hgt, int *bpp) {
    gdi_obj *b = obj_of(h, OBJ_BITMAP);
    if (!b) return NULL;
    *w = b->bw; *hgt = b->bh; *bpp = b->bpp;
    if (b->bpp == 32) {
        uint32_t *out = malloc((size_t)b->bw * b->bh * 4);
        for (int y = 0; y < b->bh; y++) memcpy(out + (size_t)y * b->bw, b->pixels + (size_t)(b->bottom_up ? b->bh - 1 - y : y) * b->bw, (size_t)b->bw * 4);
        return out;
    }
    vx_bmih bi = { 40, b->bw, b->bottom_up ? b->bh : -b->bh, 1, (uint16_t)b->bpp, 0, 0, 0, 0, 0, 0 };
    int iw, ih;
    return vx_dib_to_bgra(&bi, b->bits, b->palette, 0, &iw, &ih);
}
// A 32-bpp top-down bitmap holding these pixels, their fourth byte (alpha) kept.
HANDLE vx_gdi_bitmap_from_bgra(int w, int h, const uint32_t *px) { return make_bitmap(w, h, 32, 0, px, NULL); }
// A 1-bpp top-down bitmap (a mask: 1 is white) from rows padded to 4 bytes. Not drawable on, like other depths here.
HANDLE vx_gdi_bitmap_mono(int w, int h, const uint8_t *rows) {
    HANDLE hb = make_bitmap(w, h, 1, 0, rows, NULL);
    gdi_obj *b = obj(hb);
    if (b) { b->palette[0] = 0; b->palette[1] = 0xffffff; }
    return hb;
}
// Draws straight-alpha BGRA pixels (pw x ph, top-down) stretched to (x, y, w, h): blended over what is there, or
// copied with the alpha left out.
int vx_gdi_draw_bgra(HANDLE h, int x, int y, int w, int hgt, const uint32_t *px, int pw, int ph, int blend) {
    gdi_obj *dc = dc_of(h);
    target t;
    if (!dc || !px || pw <= 0 || ph <= 0) return 0;
    CFDataRef data = CFDataCreate(NULL, (const UInt8 *)px, (CFIndex)pw * ph * 4);
    CGDataProviderRef dp = CGDataProviderCreateWithCFData(data);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGImageRef img = CGImageCreate((size_t)pw, (size_t)ph, 8, 32, (size_t)pw * 4, cs,
                                   (blend ? kCGImageAlphaFirst : kCGImageAlphaNoneSkipFirst) | kCGBitmapByteOrder32Little, dp, NULL, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs); CGDataProviderRelease(dp); CFRelease(data);
    if (!img) return 0;
    if (lock_target(dc, &t)) {
        CGContextSaveGState(t.ctx);
        CGContextSetInterpolationQuality(t.ctx, kCGInterpolationHigh);
        CGContextSetBlendMode(t.ctx, blend ? kCGBlendModeNormal : kCGBlendModeCopy);
        CGContextTranslateCTM(t.ctx, 0, y + hgt);                  // (row 0 at the visual top)
        CGContextScaleCTM(t.ctx, 1, -1);
        CGContextDrawImage(t.ctx, CGRectMake(x, 0, w, hgt), img);
        CGContextRestoreGState(t.ctx);
        RECT drawn = { x, y, x + w, y + hgt };
        unlock_target(&t, &drawn);
    }
    CGImageRelease(img);
    texts_painted(dc, (RECT){ x, y, x + w, y + hgt });
    return 1;
}

// Pixel formats and OpenGL buffers arrive with opengl32; until then nothing matches.
// Pixel formats for OpenGL (opengl.c): there is one (RGBA8, depth 24, stencil 8, double-buffered).
static WINAPI int ChoosePixelFormat(HANDLE dc, const void *pfd) { (void)dc; (void)pfd; return 1; }
static WINAPI BOOL SetPixelFormat(HANDLE dc, int f, const void *pfd) { (void)dc; (void)pfd; return f == 1; }
static WINAPI int DescribePixelFormat(HANDLE dc, int f, uint32_t n, void *pfd) { (void)dc; (void)f; return vx_gl_describe_pixel_format(pfd, n); }
static WINAPI int GetPixelFormat(HANDLE dc) { (void)dc; return 1; }
static WINAPI BOOL SwapBuffers(HANDLE dc) { (void)dc; return vx_gl_swap(); }

static const vx_export gdi32[] = {
    VX_EXPORT(CreateCompatibleDC), VX_EXPORT(CreateDCA), VX_EXPORT(CreateDCW), VX_EXPORT(DeleteDC), VX_EXPORT(GetDeviceCaps),
    VX_EXPORT(CreateCompatibleBitmap), VX_EXPORT(CreateBitmap), VX_EXPORT(CreateDIBSection), VX_EXPORT(CreateSolidBrush),
    VX_EXPORT(CreateBrushIndirect), VX_EXPORT(CreatePen), VX_EXPORT(CreateFontIndirectA), VX_EXPORT(CreateFontIndirectW),
    VX_EXPORT(CreateFontA), VX_EXPORT(CreateFontW), VX_EXPORT(GetStockObject), VX_EXPORT(DeleteObject), VX_EXPORT(GetObjectType),
    VX_EXPORT(SelectObject), VX_EXPORT(GetCurrentObject), VX_EXPORT(GetObjectA), VX_EXPORT(GetObjectW),
    VX_EXPORT(SetTextColor), VX_EXPORT(GetTextColor), VX_EXPORT(SetBkColor), VX_EXPORT(GetBkColor), VX_EXPORT(SetBkMode), VX_EXPORT(GetBkMode),
    VX_EXPORT(SetDCBrushColor), VX_EXPORT(SetDCPenColor), VX_EXPORT(SetStretchBltMode), VX_EXPORT(GetStretchBltMode),
    VX_EXPORT(SetMapMode), VX_EXPORT(GetMapMode), VX_EXPORT(SetTextAlign), VX_EXPORT(GetTextAlign), VX_EXPORT(SetGraphicsMode), VX_EXPORT(GetGraphicsMode),
    VX_EXPORT(SaveDC), VX_EXPORT(RestoreDC), VX_EXPORT(SetViewportOrgEx), VX_EXPORT(OffsetViewportOrgEx), VX_EXPORT(GetViewportOrgEx),
    VX_EXPORT(SetWindowOrgEx), VX_EXPORT(OffsetWindowOrgEx), VX_EXPORT(GetWindowOrgEx), VX_EXPORT(LPtoDP), VX_EXPORT(DPtoLP),
    VX_EXPORT(GetCurrentPositionEx), VX_EXPORT(GdiFlush),
    VX_EXPORT(SelectClipRgn), VX_EXPORT(ExtSelectClipRgn), VX_EXPORT(IntersectClipRect), VX_EXPORT(ExcludeClipRect),
    VX_EXPORT(OffsetClipRgn), VX_EXPORT(GetClipBox), VX_EXPORT(GetClipRgn),
    VX_EXPORT(PatBlt), VX_EXPORT(Rectangle), VX_EXPORT(Ellipse), VX_EXPORT(MoveToEx), VX_EXPORT(LineTo), VX_EXPORT(SetPixel), VX_EXPORT(GetPixel),
    VX_EXPORT(TextOutA), VX_EXPORT(TextOutW), VX_EXPORT(ExtTextOutA), VX_EXPORT(ExtTextOutW),
    VX_EXPORT(GetTextExtentPoint32A), VX_EXPORT(GetTextExtentPoint32W), VX_EXPORT(GetTextMetricsA), VX_EXPORT(GetTextMetricsW),
    VX_EXPORT(ChoosePixelFormat), VX_EXPORT(SetPixelFormat), VX_EXPORT(DescribePixelFormat), VX_EXPORT(GetPixelFormat), VX_EXPORT(SwapBuffers),
};
static const vx_export user32_gdi[] = {
    VX_EXPORT(GetDC), VX_EXPORT(GetWindowDC), VX_EXPORT(GetDCEx), VX_EXPORT(ReleaseDC), VX_EXPORT(WindowFromDC),
    VX_EXPORT(FillRect), VX_EXPORT(FrameRect), VX_EXPORT(DrawTextA), VX_EXPORT(DrawTextW),
    VX_EXPORT(GetSysColor), VX_EXPORT(GetSysColorBrush),
};

void vx_gdi_init(void) {
    // Stock objects (GetStockObject indices).
    g_stock[0] = make_brush(0, 0xFFFFFF, 1); g_stock[1] = make_brush(0, 0xC0C0C0, 1); g_stock[2] = make_brush(0, 0x808080, 1);
    g_stock[3] = make_brush(0, 0x404040, 1); g_stock[4] = make_brush(0, 0x000000, 1); g_stock[5] = make_brush(1, 0, 1);
    g_stock[6] = make_pen(0, 1, 0xFFFFFF, 1); g_stock[7] = make_pen(0, 1, 0x000000, 1); g_stock[8] = make_pen(5, 1, 0, 1);
    for (int i = 10; i <= 17; i++) g_stock[i] = make_font(-13, 400, 0, "", 1);
    g_stock[18] = make_brush(0, 0xFFFFFF, 18); g_stock[19] = make_pen(0, 1, 0, 19);   // DC_BRUSH, DC_PEN
    g_default_bitmap = make_bitmap(1, 1, 32, 0, NULL, NULL);
    obj(g_default_bitmap)->stock = 1;
    vx_register_module("gdi32.dll", gdi32, sizeof gdi32 / sizeof gdi32[0]);
    vx_register_module("user32.dll", user32_gdi, sizeof user32_gdi / sizeof user32_gdi[0]);
}

#ifdef __aarch64__
#include "thunks/gdi.c.inc"
#endif
