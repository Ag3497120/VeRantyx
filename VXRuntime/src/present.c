// From GDI's pixels to the screen: the pixel transfers between surfaces (BitBlt, StretchBlt, SetDIBitsToDevice,
// StretchDIBits, GetDIBits, SetDIBits, AlphaBlend, TransparentBlt), painting (update regions, BeginPaint, EndPaint,
// RedrawWindow), scrolling (ScrollWindowEx, ScrollDC), layered windows (UpdateLayeredWindow,
// SetLayeredWindowAttributes) and DwmFlush. From Microsoft's documentation of each function.
//
// Pixels move in C, row by row, with no Core Graphics: a same-size SRCCOPY is a memcpy per row, other raster
// operations follow the ROP3 truth table, stretching samples the nearest pixel (or blends four in HALFTONE mode).
// Chromium with software compositing (--disable-gpu-compositing) shows its frames that way: a DIB section over shared
// memory, BitBlt to the window's DC (UpdateLayeredWindow for translucent windows), once per frame.
//
// What the screen shows of a window: GDI draws into its client bitmap (ui.h). When a picture is finished (a DC of the
// window is given back outside BeginPaint/EndPaint, EndPaint, UpdateLayeredWindow, DwmFlush) the drawn part is copied
// into the window's front buffer, and the main thread hands that to Core Animation (cocoa.m) at most once a frame:
// a frame drawn by several BitBlts is never shown half done. Drawing nobody commits (a DC kept for good) is committed
// after 30 ms. Child windows are layers in their parent's, so their pictures are composited by Core Animation.
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dispatch/dispatch.h>
#include <ImageIO/ImageIO.h>
#include "ui.h"

enum { ERROR_RGN = 0, NULLREGION = 1, SIMPLEREGION = 2, COMPLEXREGION = 3 };
enum { RGN_AND = 1, RGN_OR = 2, RGN_XOR = 3, RGN_DIFF = 4, RGN_COPY = 5 };
enum { ERROR_INVALID_WINDOW_HANDLE = 1400, ERROR_INVALID_PARAMETER = 87, ERROR_INVALID_HANDLE = 6 };
#define WM_ERASEBKGND_ 0x0014
#define WM_PAINT_ 0x000F
typedef struct { LONG cx, cy; } SIZE_;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec / 1e9; }
static int empty(RECT r) { return r.right <= r.left || r.bottom <= r.top; }
static RECT and_rect(RECT a, RECT b) {
    RECT r = { a.left > b.left ? a.left : b.left, a.top > b.top ? a.top : b.top, a.right < b.right ? a.right : b.right, a.bottom < b.bottom ? a.bottom : b.bottom };
    return empty(r) ? (RECT){ 0, 0, 0, 0 } : r;
}
static RECT or_rect(RECT a, RECT b) {
    if (empty(a)) return b;
    if (empty(b)) return a;
    return (RECT){ a.left < b.left ? a.left : b.left, a.top < b.top ? a.top : b.top, a.right > b.right ? a.right : b.right, a.bottom > b.bottom ? a.bottom : b.bottom };
}
static vx_window *window_of(HANDLE hwnd) {
    vx_window *w = vx_window_get(hwnd);
    if (!w) vx_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
    return w;
}

// ---- surfaces -----------------------------------------------------------------------------------------------------------
static uint32_t *row_of(const vx_surface *s, int y) { return s->px + (size_t)(s->bottom_up ? s->h - 1 - y : y) * s->w; }
// The i-th piece of r (device coordinates) that may be drawn: inside the surface and the i-th clip rectangle.
static int pieces(const vx_surface *s) { return s->nclip < 0 ? 1 : s->nclip; }
static RECT piece(const vx_surface *s, RECT r, int i) {
    r = and_rect(r, (RECT){ 0, 0, s->w, s->h });
    return s->nclip < 0 ? r : and_rect(r, s->clip[i]);
}

// A 32-bpp source for the pixel loops: rows in device order, and the temporary copy it may be (freed by src_done).
typedef struct { const uint32_t *px; int w, h, bottom_up; uint32_t *owned; } pixsrc;
static const uint32_t *src_row(const pixsrc *s, int y) { return s->px + (size_t)(s->bottom_up ? s->h - 1 - y : y) * s->w; }
static void src_done(pixsrc *s) { free(s->owned); s->owned = NULL; }
// The source rectangle `want` (device coordinates of the source surface) of a locked surface as a pixsrc. A window
// (which another thread may draw into, and whose lock is not held over the blit) and a surface of another depth are
// copied; a memory bitmap is used as it is. *origin: where (0, 0) of the pixsrc is in the source's device coordinates.
static int source_pixels(vx_surface *s, RECT want, pixsrc *out, POINT *origin) {
    memset(out, 0, sizeof *out);
    *origin = (POINT){ 0, 0 };
    if (s->px && !s->win) { *out = (pixsrc){ s->px, s->w, s->h, s->bottom_up, NULL }; return 1; }
    if (!s->px) {                                                   // another depth: the whole bitmap converted
        vx_bmih bi = { 40, s->w, s->bottom_up ? s->h : -s->h, 1, (uint16_t)s->bpp, 0, 0, 0, 0, 0, 0 };
        int w, h;
        uint32_t *px = vx_dib_to_bgra(&bi, s->bits, s->palette, 0, &w, &h);
        if (!px) return 0;
        *out = (pixsrc){ px, w, h, 0, px };
        return 1;
    }
    RECT r = and_rect(want, (RECT){ 0, 0, s->w, s->h });            // a window: the part wanted, copied
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return 0;
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) return 0;
    for (int y = 0; y < h; y++) memcpy(px + (size_t)y * w, row_of(s, r.top + y) + r.left, (size_t)w * 4);
    *out = (pixsrc){ px, w, h, 0, px };
    *origin = (POINT){ r.left, r.top };
    return 1;
}

// ---- pixel operations -----------------------------------------------------------------------------------------------------
enum { OP_COPY, OP_ROP, OP_ALPHA, OP_KEY };
typedef struct { int kind; uint8_t rop3; uint32_t pattern; uint8_t sca; int src_alpha; uint32_t key; } blit_op;
// A ternary raster operation from its truth table (the ROP code's third byte): bit (P << 2 | S << 1 | D) of the table
// is the result for those three bits, applied to all 32 bits at once.
static inline uint32_t rop3(uint8_t t, uint32_t p, uint32_t s, uint32_t d) {
    uint32_t r = 0;
    if (t & 0x01) r |= ~p & ~s & ~d;
    if (t & 0x02) r |= ~p & ~s & d;
    if (t & 0x04) r |= ~p & s & ~d;
    if (t & 0x08) r |= ~p & s & d;
    if (t & 0x10) r |= p & ~s & ~d;
    if (t & 0x20) r |= p & ~s & d;
    if (t & 0x40) r |= p & s & ~d;
    if (t & 0x80) r |= p & s & d;
    return r;
}
static inline uint32_t mul255(uint32_t a, uint32_t b) { uint32_t t = a * b + 128; return (t + (t >> 8)) >> 8; }
// AlphaBlend's formula: with per-pixel alpha (AC_SRC_ALPHA: premultiplied source) dst = src' + (1 - src'.a) dst, where
// src' is the source scaled by the constant alpha; without it, dst = sca src + (1 - sca) dst.
static inline uint32_t blend(const blit_op *op, uint32_t s, uint32_t d) {
    uint32_t sca = op->sca, out = 0;
    if (!op->src_alpha) {
        for (int sh = 0; sh < 32; sh += 8) out |= (mul255((s >> sh) & 0xff, sca) + mul255((d >> sh) & 0xff, 255 - sca)) << sh;
        return out;
    }
    if (sca != 255) {
        uint32_t t = 0;
        for (int sh = 0; sh < 32; sh += 8) t |= mul255((s >> sh) & 0xff, sca) << sh;
        s = t;
    }
    uint32_t inv = 255 - (s >> 24);
    for (int sh = 0; sh < 32; sh += 8) {
        uint32_t v = ((s >> sh) & 0xff) + mul255((d >> sh) & 0xff, inv);
        out |= (v > 255 ? 255 : v) << sh;
    }
    return out;
}
static inline int apply(const blit_op *op, uint32_t s, uint32_t *d) {
    switch (op->kind) {
    case OP_COPY: *d = s; return 1;
    case OP_ROP: *d = rop3(op->rop3, op->pattern, s, *d); return 1;
    case OP_ALPHA: *d = blend(op, s, *d); return 1;
    default: if ((s & 0xffffff) == op->key) return 0; *d = s; return 1;   // OP_KEY: the transparent color is left out
    }
}
// The four-pixel blend of HALFTONE stretching at (fx, fy) in 16.16 fixed point.
static uint32_t bilinear(const pixsrc *s, int64_t fx, int64_t fy) {
    int x0 = (int)(fx >> 16), y0 = (int)(fy >> 16);
    uint32_t ax = (uint32_t)(fx & 0xffff) >> 8, ay = (uint32_t)(fy & 0xffff) >> 8;
    int x1 = x0 + 1 < s->w ? x0 + 1 : x0, y1 = y0 + 1 < s->h ? y0 + 1 : y0;
    if (x0 < 0) x0 = x1 = 0;
    if (y0 < 0) y0 = y1 = 0;
    const uint32_t *r0 = src_row(s, y0), *r1 = src_row(s, y1);
    uint32_t a = r0[x0], b = r0[x1], c = r1[x0], d = r1[x1], out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        uint32_t top = ((a >> sh) & 0xff) * (256 - ax) + ((b >> sh) & 0xff) * ax;
        uint32_t bot = ((c >> sh) & 0xff) * (256 - ax) + ((d >> sh) & 0xff) * ax;
        out |= (((top * (256 - ay) + bot * ay) >> 16) & 0xff) << sh;
    }
    return out;
}

// Moves the source rectangle sr (pixsrc coordinates) to the destination rectangle dr (device coordinates), both
// normalised (positive sizes), mirrored by fx / fy, inside the clip region. *changed: what of the destination was
// touched. Source pixels outside the source are left out.
static void blit_pixels(vx_surface *d, RECT dr, int fx, int fy, const pixsrc *s, RECT sr, const blit_op *op, RECT *changed) {
    int dw = dr.right - dr.left, dh = dr.bottom - dr.top, sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    int same = sw == dw && sh == dh, halftone = !same && d->stretch_mode == 4 && op->kind != OP_KEY;
    int *xmap = NULL;
    for (int i = 0; i < pieces(d); i++) {
        RECT p = piece(d, dr, i);
        if (empty(p)) continue;
        *changed = or_rect(*changed, p);
        int n = p.right - p.left;
        if (!same || fx) {
            free(xmap);
            xmap = malloc(sizeof *xmap * (size_t)n);
            for (int k = 0; k < n; k++) {
                int64_t j = p.left + k - dr.left;                  // destination column in the rectangle
                int64_t sx = ((2 * j + 1) * sw) / (2 * (int64_t)dw);
                xmap[k] = sr.left + (int)(fx ? sw - 1 - sx : sx);
            }
        }
        for (int y = p.top; y < p.bottom; y++) {
            int64_t j = y - dr.top;
            int64_t syo = same ? j : ((2 * j + 1) * sh) / (2 * (int64_t)dh);
            int sy = sr.top + (int)(fy ? sh - 1 - syo : syo);
            if (sy < 0 || sy >= s->h) continue;
            uint32_t *drow = row_of(d, y);
            const uint32_t *srow = src_row(s, sy);
            if (same && !fx) {
                int sx0 = sr.left + (p.left - dr.left), x0 = p.left, x1 = p.right;
                if (sx0 < 0) { x0 -= sx0; sx0 = 0; }
                if (sx0 + (x1 - x0) > s->w) x1 = x0 + (s->w - sx0);
                if (x1 <= x0) continue;
                if (op->kind == OP_COPY) { memmove(drow + x0, srow + sx0, (size_t)(x1 - x0) * 4); continue; }
                for (int x = x0; x < x1; x++) apply(op, srow[sx0 + x - x0], &drow[x]);
                continue;
            }
            if (halftone) {
                int64_t fyy = (((2 * (fy ? (int64_t)dh - 1 - j : j) + 1) * sh << 16) / (2 * (int64_t)dh)) - 32768 + ((int64_t)sr.top << 16);
                for (int k = 0; k < n; k++) {
                    int64_t jx = p.left + k - dr.left;
                    if (fx) jx = dw - 1 - jx;
                    int64_t fxx = (((2 * jx + 1) * sw << 16) / (2 * (int64_t)dw)) - 32768 + ((int64_t)sr.left << 16);
                    if (fxx < 0) fxx = 0;
                    if (fyy < 0) fyy = 0;
                    if ((fxx >> 16) >= s->w || (fyy >> 16) >= s->h) continue;
                    apply(op, bilinear(s, fxx, fyy), &drow[p.left + k]);
                }
                continue;
            }
            for (int k = 0; k < n; k++) {
                int sx = xmap ? xmap[k] : sr.left + (p.left + k - dr.left);
                if (sx < 0 || sx >= s->w) continue;
                apply(op, srow[sx], &drow[p.left + k]);
            }
        }
    }
    free(xmap);
}

// A rectangle given as origin and signed size: normalised, with the mirroring a negative size asks for.
static RECT norm(int x, int y, int w, int h, int *flipx, int *flipy) {
    *flipx = w < 0; *flipy = h < 0;
    if (w < 0) { x += w + 1; w = -w; }
    if (h < 0) { y += h + 1; h = -h; }
    return (RECT){ x, y, x + w, y + h };
}

// One transfer between two DCs (logical coordinates of each).
static BOOL dc_blit(HANDLE dst, int xd, int yd, int wd, int hd, HANDLE src, int xs, int ys, int ws, int hs, const blit_op *op) {
    vx_surface s, d;
    if (!vx_surface_lock(src, &s)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    int dfx, dfy, sfx, sfy;
    RECT sr = norm(xs + s.ox, ys + s.oy, ws, hs, &sfx, &sfy);
    pixsrc ps; POINT org;
    int ok = source_pixels(&s, sr, &ps, &org);
    vx_surface_unlock(&s, NULL);                                    // (copied if it was a window)
    if (!ok) return 0;
    sr = (RECT){ sr.left - org.x, sr.top - org.y, sr.right - org.x, sr.bottom - org.y };
    // The screen's DC and a bitmap of another depth have nothing to draw on: done, as GDI here drew there before.
    if (!vx_surface_lock(dst, &d)) { src_done(&ps); if (vx_dc_valid(dst)) return 1; vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    if (!d.px) { vx_surface_unlock(&d, NULL); src_done(&ps); return 1; }
    RECT dr = norm(xd + d.ox, yd + d.oy, wd, hd, &dfx, &dfy);
    if (ps.px == d.px && !ps.owned) {                               // within one bitmap: from a copy of the source part
        size_t n = (size_t)ps.w * ps.h;
        uint32_t *copy = malloc(n * 4);
        if (copy) { memcpy(copy, ps.px, n * 4); ps.px = copy; ps.owned = copy; }
    }
    RECT changed = { 0, 0, 0, 0 };
    blit_pixels(&d, dr, dfx != sfx, dfy != sfy, &ps, sr, op, &changed);
    vx_surface_unlock(&d, empty(changed) ? NULL : &changed);
    src_done(&ps);
    if (op->kind != OP_ROP || op->rop3 == 0xCC) vx_gdi_copy_texts(dst, xd, yd, wd, hd, src, xs, ys, ws, hs);
    return 1;
}

// Whether a ROP3 code uses the source / the pattern (its truth table differs between S = 0 and 1 / P = 0 and 1).
static int rop_uses_source(uint8_t t) { return ((t >> 2) & 0x33) != (t & 0x33); }
static int rop_uses_pattern(uint8_t t) { return ((t >> 4) & 0x0f) != (t & 0x0f); }
static BOOL (WINAPI *g_patblt)(HANDLE, int, int, int, int, DWORD);
static WINAPI BOOL StretchBlt(HANDLE dst, int xd, int yd, int wd, int hd, HANDLE src, int xs, int ys, int ws, int hs, DWORD rop) {
    uint8_t t = (uint8_t)(rop >> 16);
    if (!rop_uses_source(t)) {
        if (!g_patblt) g_patblt = (BOOL (WINAPI *)(HANDLE, int, int, int, int, DWORD))vx_find_export("gdi32.dll", "PatBlt");
        return g_patblt ? g_patblt(dst, xd, yd, wd, hd, rop) : 0;
    }
    blit_op op = { t == 0xCC ? OP_COPY : OP_ROP, t, 0, 255, 0, 0 };
    if (rop_uses_pattern(t) && !vx_dc_brush_bgrx(dst, &op.pattern)) op.pattern = 0;
    return dc_blit(dst, xd, yd, wd, hd, src, xs, ys, ws, hs, &op);
}
static WINAPI BOOL BitBlt(HANDLE dst, int x, int y, int w, int h, HANDLE src, int xs, int ys, DWORD rop) {
    return StretchBlt(dst, x, y, w, h, src, xs, ys, w, h, rop);
}
// AlphaBlend / GdiAlphaBlend. BLENDFUNCTION (passed by value): BlendOp (AC_SRC_OVER), BlendFlags, SourceConstantAlpha,
// AlphaFormat (AC_SRC_ALPHA: the source has premultiplied per-pixel alpha). Sizes must be positive and the source
// rectangle inside the source surface.
static WINAPI BOOL GdiAlphaBlend(HANDLE dst, int xd, int yd, int wd, int hd, HANDLE src, int xs, int ys, int ws, int hs, uint32_t bf) {
    vx_surface s;
    if (wd < 0 || hd < 0 || ws < 0 || hs < 0 || (bf & 0xff) != 0 /* AC_SRC_OVER */) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (!vx_surface_lock(src, &s)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    int inside = xs + s.ox >= 0 && ys + s.oy >= 0 && xs + s.ox + ws <= s.w && ys + s.oy + hs <= s.h;
    vx_surface_unlock(&s, NULL);
    if (!inside) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    blit_op op = { OP_ALPHA, 0, 0, (uint8_t)(bf >> 16), ((bf >> 24) & 1) != 0, 0 };
    return dc_blit(dst, xd, yd, wd, hd, src, xs, ys, ws, hs, &op);
}
// TransparentBlt / GdiTransparentBlt: a copy (stretched) that leaves out the source pixels of one color.
static WINAPI BOOL GdiTransparentBlt(HANDLE dst, int xd, int yd, int wd, int hd, HANDLE src, int xs, int ys, int ws, int hs, uint32_t key) {
    if (wd < 0 || hd < 0 || ws < 0 || hs < 0) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    blit_op op = { OP_KEY, 0, 0, 255, 0, ((key & 0xff) << 16) | (key & 0xff00) | ((key >> 16) & 0xff) };
    return dc_blit(dst, xd, yd, wd, hd, src, xs, ys, ws, hs, &op);
}

// ---- DIBs to and from DCs and bitmaps ----------------------------------------------------------------------------------------
// A DIB's rows as a pixsrc: 32-bpp rows used in place, other depths converted. `lines` rows from `bits`.
static int dib_source(const vx_bmih *bi, const void *bits, int lines, pixsrc *out) {
    memset(out, 0, sizeof *out);
    if (!bi || !bits || bi->width <= 0 || lines <= 0) return 0;
    int bottom_up = bi->height > 0;
    if (bi->bpp == 32 && (bi->compression == 0 || bi->compression == 3)) {
        *out = (pixsrc){ bits, bi->width, lines, bottom_up, NULL };
        return 1;
    }
    vx_bmih band = *bi;
    band.height = bottom_up ? lines : -lines;
    int w, h;
    uint32_t *px = vx_dib_to_bgra(&band, bits, (const uint32_t *)((const uint8_t *)bi + bi->size), 0, &w, &h);
    if (!px) return 0;
    *out = (pixsrc){ px, w, h, 0, px };
    return 1;
}
static BOOL dib_to_dc(HANDLE h, RECT dr, int fx, int fy, pixsrc *src, RECT sr, DWORD rop) {
    vx_surface d;
    uint8_t t = (uint8_t)(rop >> 16);
    blit_op op = { t == 0xCC ? OP_COPY : OP_ROP, t, 0, 255, 0, 0 };
    if (rop_uses_pattern(t) && !vx_dc_brush_bgrx(h, &op.pattern)) op.pattern = 0;
    if (!vx_surface_lock(h, &d)) return vx_dc_valid(h);           // (the screen's DC: nothing to draw on)
    if (!d.px) { vx_surface_unlock(&d, NULL); return 1; }
    dr = (RECT){ dr.left + d.ox, dr.top + d.oy, dr.right + d.ox, dr.bottom + d.oy };
    RECT changed = { 0, 0, 0, 0 };
    blit_pixels(&d, dr, fx, fy, src, sr, &op, &changed);
    vx_surface_unlock(&d, empty(changed) ? NULL : &changed);
    if (!empty(changed)) vx_gdi_forget_texts(h, changed);
    return 1;
}
// StretchDIBits: the source rectangle of a bottom-up DIB is measured from its bottom row (ySrc is the bottom
// edge), a top-down DIB's from its top. Negative sizes mirror. Returns the scan lines copied.
static WINAPI int StretchDIBits(HANDLE h, int xd, int yd, int wd, int hd, int xs, int ys, int ws, int hs, const void *bits, const vx_bmih *bi, uint32_t usage, DWORD rop) {
    (void)usage;
    if (!bits || !bi || bi->width <= 0 || !bi->height) return 0;
    int height = bi->height < 0 ? -bi->height : bi->height;
    pixsrc s;
    if (!dib_source(bi, bits, height, &s)) return 0;
    int sfx, sfy, dfx, dfy;
    RECT sr = norm(xs, ys, ws, hs, &sfx, &sfy), dr = norm(xd, yd, wd, hd, &dfx, &dfy);
    if (bi->height > 0) sr = (RECT){ sr.left, height - sr.bottom, sr.right, height - sr.top };
    int ok = dib_to_dc(h, dr, dfx != sfx, dfy != sfy, &s, sr, rop);
    src_done(&s);
    return ok ? (hs < 0 ? -hs : hs) : 0;
}
// SetDIBitsToDevice: `lines` scan lines of the DIB starting at scan line `start` (counted from the bottom of a
// bottom-up DIB, from the top of a top-down one) are in bits; the part of the w x h source rectangle at (xs, ys)
// (its lower-left corner in a bottom-up DIB, its upper-left in a top-down one) that they hold goes to (xd, yd).
// Returns the scan lines set.
static WINAPI int SetDIBitsToDevice(HANDLE h, int xd, int yd, DWORD w, DWORD hgt, int xs, int ys, uint32_t start, uint32_t lines, const void *bits, const vx_bmih *bi, uint32_t usage) {
    (void)usage;
    if (!bits || !bi || bi->width <= 0 || !bi->height || !lines) return 0;
    int height = bi->height < 0 ? -bi->height : bi->height;
    if (start >= (uint32_t)height) return 0;
    if (start + lines > (uint32_t)height) lines = (uint32_t)height - start;
    pixsrc s;
    if (!dib_source(bi, bits, (int)lines, &s)) return 0;
    // In the band's own rows (top-down): which of them the source rectangle covers, and where they go.
    int top, bottom;                                                // band rows, top-down
    if (bi->height > 0) {                                           // band row r (from its top) is scan line start + lines - 1 - r
        int lo = ys, hi = ys + (int)hgt;                            // scan lines covered, from the bottom
        if (lo < (int)start) lo = (int)start;
        if (hi > (int)(start + lines)) hi = (int)(start + lines);
        if (hi <= lo) { src_done(&s); return (int)lines; }
        top = (int)(start + lines) - hi; bottom = (int)(start + lines) - lo;
        yd += (ys + (int)hgt) - hi;                                 // (the rectangle's top row is scan line ys + hgt - 1)
    } else {
        int lo = ys, hi = ys + (int)hgt;
        if (lo < (int)start) lo = (int)start;
        if (hi > (int)(start + lines)) hi = (int)(start + lines);
        if (hi <= lo) { src_done(&s); return (int)lines; }
        top = lo - (int)start; bottom = hi - (int)start;
        yd += lo - ys;
    }
    RECT sr = { xs, top, xs + (int)w, bottom }, dr = { xd, yd, xd + (int)w, yd + (bottom - top) };
    int ok = dib_to_dc(h, dr, 0, 0, &s, sr, 0x00CC0020);           // (src_row counts the band's rows from its top)
    src_done(&s);
    return ok ? (int)lines : 0;
}
// GetDIBits: a bitmap's scan lines (start..start + lines, counted as the BITMAPINFO's orientation says) in the format
// asked for (32, 24 or 16 bpp, or the bitmap's own), or, with no bits, the format (biBitCount 0) and sizes.
static void put_row(uint8_t *out, const uint32_t *px, int w, int bpp) {
    switch (bpp) {
    case 32: memcpy(out, px, (size_t)w * 4); break;
    case 24: for (int x = 0; x < w; x++) { out[3 * x] = (uint8_t)px[x]; out[3 * x + 1] = (uint8_t)(px[x] >> 8); out[3 * x + 2] = (uint8_t)(px[x] >> 16); } break;
    case 16: for (int x = 0; x < w; x++) ((uint16_t *)out)[x] = (uint16_t)(((px[x] >> 19) & 31) << 10 | ((px[x] >> 11) & 31) << 5 | ((px[x] >> 3) & 31)); break;
    }
}
static WINAPI int GetDIBits(HANDLE dc, HANDLE bitmap, uint32_t start, uint32_t lines, void *bits, vx_bmih *bi, uint32_t usage) {
    (void)dc; (void)usage;
    vx_surface b;
    if (!bi || !vx_bitmap_surface(bitmap, &b)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (!bi->bpp) {                                                 // the bitmap's format
        bi->width = b.w; bi->height = b.h; bi->planes = 1; bi->bpp = (uint16_t)b.bpp; bi->compression = 0;
        bi->image_size = (DWORD)(b.stride * (size_t)b.h);
        return 1;
    }
    int bpp = bi->bpp, height = bi->height < 0 ? -bi->height : bi->height, w = bi->width > 0 ? bi->width : b.w;
    size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
    if (!bits) {
        bi->image_size = (DWORD)(stride * (size_t)(height ? height : b.h));
        if (bpp <= 8 && b.palette) memcpy((uint8_t *)bi + bi->size, b.palette, sizeof(uint32_t) << bpp);
        return 1;
    }
    if ((bpp != 32 && bpp != 24 && bpp != 16) || (bi->compression != 0 && !(bi->compression == 3 && bpp == 32))) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    vx_surface tmp = b; uint32_t *conv = NULL;
    if (!b.px) {
        vx_bmih own = { 40, b.w, b.bottom_up ? b.h : -b.h, 1, (uint16_t)b.bpp, 0, 0, 0, 0, 0, 0 };
        int cw, ch;
        if (!(conv = vx_dib_to_bgra(&own, b.bits, b.palette, 0, &cw, &ch))) return 0;
        tmp.px = conv; tmp.bottom_up = 0;
    }
    int bottom_up = bi->height > 0, n = 0;
    if (!height) height = b.h;
    uint32_t *row = malloc((size_t)(w > 0 ? w : 1) * 4);
    for (uint32_t i = 0; i < lines && start + i < (uint32_t)height && start + i < (uint32_t)b.h; i++, n++) {
        uint32_t scan = start + i;                                  // scan line, counted from the bottom (bottom-up) or the top
        int y = bottom_up ? b.h - 1 - (int)scan : (int)scan;
        memset(row, 0, (size_t)w * 4);
        memcpy(row, row_of(&tmp, y), (size_t)(w < b.w ? w : b.w) * 4);
        put_row((uint8_t *)bits + stride * i, row, w, bpp);
    }
    free(row); free(conv);
    bi->image_size = (DWORD)(stride * (size_t)height);
    return n;
}
// SetDIBits: scan lines of a DIB into a bitmap (32-bpp bitmaps).
static WINAPI int SetDIBits(HANDLE dc, HANDLE bitmap, uint32_t start, uint32_t lines, const void *bits, const vx_bmih *bi, uint32_t usage) {
    (void)dc; (void)usage;
    vx_surface b;
    if (!bi || !bits || !vx_bitmap_surface(bitmap, &b) || !b.px) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    int height = bi->height < 0 ? -bi->height : bi->height;
    if (start >= (uint32_t)height) return 0;
    if (start + lines > (uint32_t)height) lines = (uint32_t)height - start;
    pixsrc s;
    if (!dib_source(bi, bits, (int)lines, &s)) return 0;
    int n = 0;
    for (uint32_t i = 0; i < lines; i++, n++) {
        uint32_t scan = start + i;
        int y = bi->height > 0 ? b.h - 1 - (int)scan : (int)scan; // the bitmap's row (top-down)
        int sy = bi->height > 0 ? (int)(lines - 1 - i) : (int)i;    // the band's row from its top, as src_row counts
        if (y < 0 || y >= b.h) continue;
        int w = s.w < b.w ? s.w : b.w;
        memcpy(row_of(&b, y), src_row(&s, sy), (size_t)w * 4);
    }
    src_done(&s);
    return n;
}

// ---- committing pictures ------------------------------------------------------------------------------------------------------
// How the front buffer holds the window: 0 opaque, 1 premultiplied alpha as drawn, 2 alpha from the color key.
static int front_mode(vx_window *w) {
    if (!(w->exstyle & WS_EX_LAYERED)) return 0;
    if (w->layered == 2) {
        if (w->layer_flags & 4 /* ULW_OPAQUE */) return 0;
        if (w->layer_flags & 1 /* ULW_COLORKEY */) return 2;
        return (w->layer_flags & 2 /* ULW_ALPHA */) && (w->layer_flags & 0x100 /* AC_SRC_ALPHA */) ? 1 : 0;
    }
    return w->layered == 1 && (w->layer_flags & 1 /* LWA_COLORKEY */) ? 2 : 0;
}
static void commit_locked(vx_window *w) {
    if (!w->pixels) { w->changed = 0; return; }
    int mode = front_mode(w), full = 0;
    if (!w->front || w->fw != w->bw || w->fh != w->bh || (mode != 0) != w->front_alpha || mode == 2) {
        if (!w->front || w->fw != w->bw || w->fh != w->bh) {
            free(w->front);
            w->front = malloc((size_t)w->bw * w->bh * 4);
            w->fw = w->bw; w->fh = w->bh;
            if (!w->front) { w->fw = w->fh = 0; w->changed = 0; return; }
        }
        full = 1;
    }
    RECT all = { 0, 0, w->bw, w->bh }, r = full || !w->changed ? all : and_rect(w->dirty, all);
    uint32_t key = ((w->layer_key & 0xff) << 16) | (w->layer_key & 0xff00) | ((w->layer_key >> 16) & 0xff);
    for (int y = r.top; y < r.bottom; y++) {
        const uint32_t *from = w->pixels + (size_t)y * w->bw + r.left;
        uint32_t *to = w->front + (size_t)y * w->fw + r.left;
        if (mode != 2) { memcpy(to, from, (size_t)(r.right - r.left) * 4); continue; }
        for (int x = 0; x < r.right - r.left; x++) to[x] = (from[x] & 0xffffff) == key ? 0 : from[x] | 0xff000000u;
    }
    w->front_alpha = mode != 0;
    w->changed = 0;
    w->committed++;
}
void vx_present_commit(vx_window *w) {
    if (!w || w->destroyed) return;
    pthread_mutex_lock(&w->lock);
    int any = w->changed || !w->front;
    if (any) commit_locked(w);
    pthread_mutex_unlock(&w->lock);
    if (any) vx_cocoa_redraw(vx_window_top(w));
}
void vx_present_dc_released(vx_window *w) {
    if (w->paint_depth > 0 || !w->changed) return;
    vx_present_commit(w);
}
// Drawing nobody commits (a DC kept for good, a program that never gives its DC back) still reaches the screen,
// 30 ms after it began.
#define LATE_COMMIT 0.030
static void late_commit(void *p) {
    vx_window *w = p;
    pthread_mutex_lock(&w->lock);
    w->fallback_pending = 0;
    int now = 0;
    if (!w->destroyed && w->changed) {
        double age = now_s() - w->changed_at;
        if (age >= LATE_COMMIT) { commit_locked(w); now = 1; }
        else {
            w->fallback_pending = 1;
            dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((LATE_COMMIT - age) * 1e9) + 1000000), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), w, late_commit);
        }
    }
    pthread_mutex_unlock(&w->lock);
    if (now) vx_cocoa_redraw(vx_window_top(w));
}
void vx_present_changed(vx_window *w, const RECT *r) {
    RECT b = r ? and_rect(*r, (RECT){ 0, 0, w->bw, w->bh }) : (RECT){ 0, 0, w->bw, w->bh };
    if (empty(b)) return;
    if (!w->changed) { w->changed = 1; w->dirty = b; w->changed_at = now_s(); }
    else w->dirty = or_rect(w->dirty, b);
    if (!w->fallback_pending) {
        w->fallback_pending = 1;
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(LATE_COMMIT * 1e9) + 1000000), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), w, late_commit);
    }
}
static void release_copy(void *info, const void *data, size_t size) { (void)info; (void)size; free((void *)data); }
// The committed picture as an image of its own (main thread): what Core Animation gets.
CGImageRef vx_window_front_image(vx_window *w, int *alpha) {
    pthread_mutex_lock(&w->lock);
    if (!w->front || !w->fw || !w->fh) { pthread_mutex_unlock(&w->lock); return NULL; }
    size_t n = (size_t)w->fw * w->fh * 4;
    void *copy = malloc(n);
    if (!copy) { pthread_mutex_unlock(&w->lock); return NULL; }
    memcpy(copy, w->front, n);
    int fw = w->fw, fh = w->fh, a = w->front_alpha;
    w->presented = w->committed;
    pthread_mutex_unlock(&w->lock);
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, copy, n, release_copy);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef img = CGImageCreate((size_t)fw, (size_t)fh, 8, 32, (size_t)fw * 4, cs,
                                   (a ? kCGImageAlphaPremultipliedFirst : kCGImageAlphaNoneSkipFirst) | kCGBitmapByteOrder32Little, dp, NULL, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs); CGDataProviderRelease(dp);
    if (alpha) *alpha = a;
    return img;
}
int vx_png_write(CGImageRef img, const char *png) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)png, (CFIndex)strlen(png), false);
    CGImageDestinationRef d = url ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
    int ok = 0;
    if (d) { CGImageDestinationAddImage(d, img, NULL); ok = CGImageDestinationFinalize(d); CFRelease(d); }
    if (url) CFRelease(url);
    return ok;
}
void vx_present_window_gone(vx_window *w) {
    pthread_mutex_lock(&w->lock);
    free(w->front); w->front = NULL; w->fw = w->fh = 0;
    w->changed = 0;
    vx_region_free(w->update); w->update = NULL;
    pthread_mutex_unlock(&w->lock);
    vx_cocoa_window_gone(w);
}
int vx_ui_snapshot_view(HANDLE hwnd, const char *png, char *info, size_t cap) {
    vx_window *w = vx_window_get(hwnd);
    return w && vx_cocoa_snapshot_view(w, png, info, cap);
}

// ---- the update region and painting -----------------------------------------------------------------------------------------
// A window's update region is w->update (client coordinates) while w->invalid; NULL then means the whole client area.
static vx_region *update_copy(vx_window *w) {                       // (lock held) what is invalid, as a region
    vx_region *g = vx_region_new();
    if (!w->invalid) return g;
    if (w->update) vx_region_copy(g, w->update);
    else vx_region_set_rect(g, 0, 0, w->client_w, w->client_h);
    return g;
}
static void set_update(vx_window *w, vx_region *g) {               // (lock held) g taken
    RECT client = { 0, 0, w->client_w, w->client_h };
    vx_region *c = vx_region_new();
    vx_region_set_rect(c, client.left, client.top, client.right, client.bottom);
    vx_region_combine(g, g, c, RGN_AND);
    vx_region_free(c);
    vx_region_free(w->update);
    w->update = NULL;
    w->invalid = g->n > 0;
    if (!w->invalid) { vx_region_free(g); w->erase = 0; return; }
    RECT b = vx_region_box(g);
    if (g->n == 1 && b.left <= 0 && b.top <= 0 && b.right >= client.right && b.bottom >= client.bottom) vx_region_free(g);
    else w->update = g;
}
static void invalidate_region(vx_window *w, const vx_region *r, int erase) {
    pthread_mutex_lock(&w->lock);
    if (!r) { vx_region_free(w->update); w->update = NULL; w->invalid = w->client_w > 0 && w->client_h > 0; }
    else if (r->n) {
        vx_region *g = update_copy(w);
        vx_region_combine(g, g, r, RGN_OR);
        set_update(w, g);
    }
    if (erase && w->invalid) w->erase = 1;
    pthread_mutex_unlock(&w->lock);
    vx_window_wake(w);
}
static void validate_region(vx_window *w, const vx_region *r) {
    pthread_mutex_lock(&w->lock);
    if (!r) { vx_region_free(w->update); w->update = NULL; w->invalid = 0; w->erase = 0; }
    else if (w->invalid) {
        vx_region *g = update_copy(w);
        vx_region_combine(g, g, r, RGN_DIFF);
        set_update(w, g);
    }
    pthread_mutex_unlock(&w->lock);
}
void vx_window_invalidate(vx_window *w, const RECT *r, int erase) {
    if (!r) { invalidate_region(w, NULL, erase); return; }
    vx_region *g = vx_region_new();
    vx_region_set_rect(g, r->left, r->top, r->right, r->bottom);
    invalidate_region(w, g, erase);
    vx_region_free(g);
}
void vx_window_validate_all(vx_window *w) { validate_region(w, NULL); }

// PAINTSTRUCT: hdc, fErase, rcPaint, fRestore, fIncUpdate, rgbReserved[32].
typedef struct { HANDLE hdc; BOOL erase; RECT paint; BOOL restore, inc_update; uint8_t reserved[32]; } PAINTSTRUCT;
// BeginPaint: a DC clipped to the update region, which is then validated; WM_ERASEBKGND first when the region was
// invalidated with erasing (fErase tells whether the window procedure left the erasing to the caller). Until
// EndPaint, DCs of the window given back do not commit its picture: the background alone is never shown.
static WINAPI HANDLE BeginPaint(HANDLE h, PAINTSTRUCT *ps) {
    vx_window *w = window_of(h);
    if (!w || !ps) return NULL;
    pthread_mutex_lock(&w->lock);
    vx_region *g = update_copy(w);
    int erase = w->erase;
    vx_region_free(w->update); w->update = NULL; w->invalid = 0; w->erase = 0;
    pthread_mutex_unlock(&w->lock);
    w->paint_depth++;
    memset(ps, 0, sizeof *ps);
    ps->hdc = vx_dc_for_window(w);
    vx_dc_set_clip(ps->hdc, g);
    ps->paint = g->n ? vx_region_box(g) : (RECT){ 0, 0, 0, 0 };
    vx_region_free(g);
    ps->erase = erase && !vx_send_message(h, WM_ERASEBKGND_, (WPARAM)ps->hdc, 0);
    return ps->hdc;
}
static WINAPI BOOL EndPaint(HANDLE h, const PAINTSTRUCT *ps) {
    vx_window *w = vx_window_get(h);
    if (w && w->paint_depth > 0) w->paint_depth--;
    if (ps) vx_dc_release(ps->hdc);
    if (w && !w->paint_depth) vx_present_commit(w);
    return 1;
}
LRESULT vx_default_paint(HANDLE hwnd) { PAINTSTRUCT ps; if (BeginPaint(hwnd, &ps)) EndPaint(hwnd, &ps); return 0; }

static WINAPI BOOL InvalidateRect(HANDLE h, const RECT *r, BOOL erase) {
    if (!h) {                                                       // every window (of this program)
        vx_window *all[1024];
        int n = vx_window_all(all, 1024);
        for (int i = 0; i < n; i++) vx_window_invalidate(all[i], NULL, erase);
        return 1;
    }
    vx_window *w = window_of(h);
    if (!w) return 0;
    vx_window_invalidate(w, r, erase);
    return 1;
}
static WINAPI BOOL InvalidateRgn(HANDLE h, HANDLE rgn, BOOL erase) {
    vx_window *w = window_of(h);
    if (!w) return 0;
    vx_region *g = rgn ? vx_gdi_region(rgn) : NULL;
    if (rgn && !g) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    invalidate_region(w, g, erase);
    return 1;
}
static WINAPI BOOL ValidateRect(HANDLE h, const RECT *r) {
    vx_window *w = window_of(h);
    if (!w) return 0;
    if (!r) { validate_region(w, NULL); return 1; }
    vx_region *g = vx_region_new();
    vx_region_set_rect(g, r->left, r->top, r->right, r->bottom);
    validate_region(w, g);
    vx_region_free(g);
    return 1;
}
static WINAPI BOOL ValidateRgn(HANDLE h, HANDLE rgn) {
    vx_window *w = window_of(h);
    if (!w) return 0;
    vx_region *g = rgn ? vx_gdi_region(rgn) : NULL;
    if (rgn && !g) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    validate_region(w, g);
    return 1;
}
// GetUpdateRect: the smallest rectangle around the update region (empty: 0); bErase sends the pending WM_ERASEBKGND.
static WINAPI BOOL GetUpdateRect(HANDLE h, RECT *r, BOOL erase) {
    vx_window *w = window_of(h);
    if (!w) return 0;
    pthread_mutex_lock(&w->lock);
    vx_region *g = update_copy(w);
    int pending = w->erase;
    if (erase && pending) w->erase = 0;
    pthread_mutex_unlock(&w->lock);
    RECT b = g->n ? vx_region_box(g) : (RECT){ 0, 0, 0, 0 };
    int any = g->n > 0;
    vx_region_free(g);
    if (r) *r = b;
    if (any && erase && pending) {
        HANDLE dc = vx_dc_for_window(w);
        w->paint_depth++;
        vx_send_message(h, WM_ERASEBKGND_, (WPARAM)dc, 0);
        w->paint_depth--;
        vx_dc_release(dc);
    }
    return any;
}
static WINAPI int GetUpdateRgn(HANDLE h, HANDLE rgn, BOOL erase) {
    vx_window *w = window_of(h);
    vx_region *out = vx_gdi_region(rgn);
    if (!w || !out) { if (w) vx_set_last_error(ERROR_INVALID_HANDLE); return ERROR_RGN; }
    pthread_mutex_lock(&w->lock);
    vx_region *g = update_copy(w);
    pthread_mutex_unlock(&w->lock);
    vx_region_copy(out, g);
    vx_region_free(g);
    if (erase && out->n) { RECT dummy; GetUpdateRect(h, &dummy, 1); }
    return vx_region_type(out);
}
static WINAPI BOOL UpdateWindow(HANDLE h) {
    vx_window *w = window_of(h);
    if (!w) return 0;
    if (w->invalid && w->visible && vx_window_wants_paint(w)) vx_send_message(h, WM_PAINT_, 0, 0);
    return 1;
}
// RedrawWindow: RDW_INVALIDATE / RDW_VALIDATE the rectangle or region given (else the client area), RDW_ERASE /
// RDW_NOERASE, RDW_ALLCHILDREN (children too), RDW_UPDATENOW / RDW_ERASENOW (painted before it returns).
#define RDW_INVALIDATE 0x1
#define RDW_ERASE 0x4
#define RDW_VALIDATE 0x8
#define RDW_NOERASE 0x20
#define RDW_ALLCHILDREN 0x80
#define RDW_UPDATENOW 0x100
#define RDW_ERASENOW 0x200
static void redraw(vx_window *w, const vx_region *g, uint32_t flags, int depth) {
    if (flags & RDW_INVALIDATE) invalidate_region(w, g, (flags & RDW_ERASE) && !(flags & RDW_NOERASE));
    else if (flags & RDW_VALIDATE) {
        validate_region(w, g);
        if (flags & RDW_NOERASE) { pthread_mutex_lock(&w->lock); w->erase = 0; pthread_mutex_unlock(&w->lock); }
    }
    if ((flags & RDW_ALLCHILDREN) && depth < 32) {
        vx_window *kids[256];
        int n = vx_window_children(w, kids, 256, 0);
        for (int i = 0; i < n; i++) {
            vx_region *kg = NULL;
            if (g) {                                                // (the part of the region over the child, in its coordinates)
                kg = vx_region_new();
                vx_region_copy(kg, g);
                vx_region_offset(kg, -kids[i]->rect.left, -kids[i]->rect.top);
            }
            redraw(kids[i], kg, flags, depth + 1);
            vx_region_free(kg);
        }
    }
    if (flags & (RDW_UPDATENOW | RDW_ERASENOW)) UpdateWindow(w->hwnd);
}
static WINAPI BOOL RedrawWindow(HANDLE h, const RECT *r, HANDLE rgn, uint32_t flags) {
    vx_window *w = window_of(h);
    if (!w) return 0;
    vx_region *g = NULL;
    if (rgn) { vx_region *src = vx_gdi_region(rgn); if (src) { g = vx_region_new(); vx_region_copy(g, src); } }
    else if (r) { g = vx_region_new(); vx_region_set_rect(g, r->left, r->top, r->right, r->bottom); }
    redraw(w, g, flags, 0);
    vx_region_free(g);
    return 1;
}
static WINAPI BOOL LockWindowUpdate(HANDLE h) { (void)h; return 1; }

// ---- scrolling ---------------------------------------------------------------------------------------------------------------
// Moves the pixels of `scroll` by (dx, dy) on a locked surface: those landing inside `clip` (and the clip region) are
// drawn; *uncovered gets what of scroll & clip no moved pixel covers. Device coordinates.
static void scroll_pixels(vx_surface *s, int dx, int dy, RECT scroll, RECT clip, vx_region *uncovered, RECT *changed) {
    RECT bounds = { 0, 0, s->w, s->h };
    RECT area = and_rect(and_rect(scroll, clip), bounds);
    RECT to = and_rect((RECT){ scroll.left + dx, scroll.top + dy, scroll.right + dx, scroll.bottom + dy }, and_rect(clip, bounds));
    RECT from = { to.left - dx, to.top - dy, to.right - dx, to.bottom - dy };
    from = and_rect(from, bounds);
    to = (RECT){ from.left + dx, from.top + dy, from.right + dx, from.bottom + dy };
    vx_region_set_rect(uncovered, area.left, area.top, area.right, area.bottom);
    if (empty(to) || !s->px) return;
    vx_region *moved = vx_region_new();
    vx_region_set_rect(moved, to.left, to.top, to.right, to.bottom);
    vx_region_combine(uncovered, uncovered, moved, RGN_DIFF);
    vx_region_free(moved);
    for (int i = 0; i < pieces(s); i++) {
        RECT p = piece(s, to, i);
        if (empty(p)) continue;
        *changed = or_rect(*changed, p);
        int n = p.right - p.left;
        if (dy > 0) for (int y = p.bottom - 1; y >= p.top; y--) memmove(row_of(s, y) + p.left, row_of(s, y - dy) + p.left - dx, (size_t)n * 4);
        else for (int y = p.top; y < p.bottom; y++) memmove(row_of(s, y) + p.left, row_of(s, y - dy) + p.left - dx, (size_t)n * 4);
    }
}
#define SW_SCROLLCHILDREN 0x1
#define SW_INVALIDATE 0x2
#define SW_ERASE 0x4
static WINAPI int ScrollWindowEx(HANDLE h, int dx, int dy, const RECT *scroll, const RECT *clip, HANDLE rgn_update, RECT *rect_update, uint32_t flags) {
    vx_window *w = window_of(h);
    if (!w) return ERROR_RGN;
    RECT client = { 0, 0, w->client_w, w->client_h };
    RECT sr = scroll ? *scroll : client, cr = clip ? *clip : client;
    vx_region *uncovered = vx_region_new();
    HANDLE dc = vx_dc_for_window(w);
    vx_surface s;
    RECT changed = { 0, 0, 0, 0 };
    if (vx_surface_lock(dc, &s)) {
        scroll_pixels(&s, dx, dy, sr, cr, uncovered, &changed);
        vx_surface_unlock(&s, empty(changed) ? NULL : &changed);
    }
    vx_dc_release(dc);
    // what was invalid moves along with the pixels (and stays invalid where it was)
    pthread_mutex_lock(&w->lock);
    if (w->invalid) {
        vx_region *g = update_copy(w), *in = vx_region_new(), *area = vx_region_new();
        vx_region_set_rect(area, sr.left, sr.top, sr.right, sr.bottom);
        vx_region_combine(in, g, area, RGN_AND);
        vx_region_offset(in, dx, dy);
        vx_region_set_rect(area, cr.left, cr.top, cr.right, cr.bottom);
        vx_region_combine(in, in, area, RGN_AND);
        vx_region_combine(g, g, in, RGN_OR);
        set_update(w, g);
        vx_region_free(in); vx_region_free(area);
    }
    pthread_mutex_unlock(&w->lock);
    if (flags & SW_INVALIDATE) invalidate_region(w, uncovered, (flags & SW_ERASE) != 0);
    if (flags & SW_SCROLLCHILDREN) {
        BOOL (WINAPI *set_pos)(HANDLE, HANDLE, int, int, int, int, uint32_t) = (BOOL (WINAPI *)(HANDLE, HANDLE, int, int, int, int, uint32_t))vx_find_export("user32.dll", "SetWindowPos");
        vx_window *kids[256];
        int n = vx_window_children(w, kids, 256, 0);
        for (int i = 0; i < n && set_pos; i++) {
            RECT k = kids[i]->rect;
            if (scroll && empty(and_rect(k, sr))) continue;
            set_pos(kids[i]->hwnd, NULL, k.left + dx, k.top + dy, 0, 0, 0x1 | 0x4 | 0x10);   // SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
        }
    }
    vx_region *out = rgn_update ? vx_gdi_region(rgn_update) : NULL;
    if (out) vx_region_copy(out, uncovered);
    if (rect_update) *rect_update = uncovered->n ? vx_region_box(uncovered) : (RECT){ 0, 0, 0, 0 };
    int type = vx_region_type(uncovered);
    vx_region_free(uncovered);
    return type;
}
static WINAPI BOOL ScrollWindow(HANDLE h, int dx, int dy, const RECT *scroll, const RECT *clip) {
    return ScrollWindowEx(h, dx, dy, scroll, clip, NULL, NULL, SW_INVALIDATE | SW_ERASE | (scroll ? 0 : SW_SCROLLCHILDREN)) != ERROR_RGN;
}
// ScrollDC: the same on a DC's surface (logical coordinates, the DC's clip region too); nothing is invalidated.
static WINAPI BOOL ScrollDC(HANDLE h, int dx, int dy, const RECT *scroll, const RECT *clip, HANDLE rgn_update, RECT *rect_update) {
    vx_surface s;
    if (!vx_surface_lock(h, &s)) return 0;
    RECT bounds = { -s.ox, -s.oy, s.w - s.ox, s.h - s.oy };
    RECT sr = scroll ? *scroll : bounds, cr = clip ? *clip : bounds;
    sr = (RECT){ sr.left + s.ox, sr.top + s.oy, sr.right + s.ox, sr.bottom + s.oy };
    cr = (RECT){ cr.left + s.ox, cr.top + s.oy, cr.right + s.ox, cr.bottom + s.oy };
    vx_region *uncovered = vx_region_new();
    RECT changed = { 0, 0, 0, 0 };
    scroll_pixels(&s, dx, dy, sr, cr, uncovered, &changed);
    vx_surface_unlock(&s, empty(changed) ? NULL : &changed);
    vx_region_offset(uncovered, -s.ox, -s.oy);
    vx_region *out = rgn_update ? vx_gdi_region(rgn_update) : NULL;
    if (out) vx_region_copy(out, uncovered);
    if (rect_update) *rect_update = uncovered->n ? vx_region_box(uncovered) : (RECT){ 0, 0, 0, 0 };
    vx_region_free(uncovered);
    return 1;
}

// ---- layered windows -------------------------------------------------------------------------------------------------------
#define ULW_COLORKEY 1
#define ULW_ALPHA 2
#define ULW_OPAQUE 4
#define ULW_EX_NORESIZE 8
#define LWA_COLORKEY 1
#define LWA_ALPHA 2
typedef struct { uint8_t op, flags, sca, format; } BLENDFUNCTION;
// UpdateLayeredWindow(Indirect): the window becomes the picture in hdcSrc at pptSrc (psize, the window's new size;
// prcDirty: only that part changes), at pptDst, blended by pblend (ULW_ALPHA: its constant alpha, and AC_SRC_ALPHA:
// the picture's premultiplied per-pixel alpha), or with crKey transparent (ULW_COLORKEY), or opaque (ULW_OPAQUE).
// Fails for a window that is not WS_EX_LAYERED, or was set up with SetLayeredWindowAttributes.
static BOOL update_layered(HANDLE h, const POINT *dst, const SIZE_ *size, HANDLE src, const POINT *at, uint32_t key, const BLENDFUNCTION *blend, DWORD flags, const RECT *dirty);
static WINAPI BOOL UpdateLayeredWindow(HANDLE h, HANDLE dc_dst, const POINT *dst, const SIZE_ *size, HANDLE src, const POINT *at, uint32_t key, const BLENDFUNCTION *blend, DWORD flags) {
    (void)dc_dst;
    if (flags & ~(DWORD)(ULW_COLORKEY | ULW_ALPHA | ULW_OPAQUE)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    return update_layered(h, dst, size, src, at, key, blend, flags, NULL);
}
// UPDATELAYEREDWINDOWINFO: cbSize, hdcDst, pptDst, psize, hdcSrc, pptSrc, crKey, pblend, dwFlags, prcDirty.
typedef struct { DWORD size; HANDLE dc_dst; const POINT *dst; const SIZE_ *psize; HANDLE src; const POINT *at; uint32_t key; const BLENDFUNCTION *blend; DWORD flags; const RECT *dirty; } ULWINFO;
static WINAPI BOOL UpdateLayeredWindowIndirect(HANDLE h, const ULWINFO *info) {
    if (!info || info->size != sizeof *info || (info->flags & ~(DWORD)(ULW_COLORKEY | ULW_ALPHA | ULW_OPAQUE | ULW_EX_NORESIZE))) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    vx_window *w = window_of(h);
    if (!w) return 0;
    if ((info->flags & ULW_EX_NORESIZE) && info->psize) {
        RECT r = w->rect;
        if (info->psize->cx != r.right - r.left || info->psize->cy != r.bottom - r.top) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    }
    return update_layered(h, info->dst, info->psize, info->src, info->at, info->key, info->blend, info->flags & ~(DWORD)ULW_EX_NORESIZE, info->dirty);
}
static BOOL update_layered(HANDLE h, const POINT *dst, const SIZE_ *size, HANDLE src, const POINT *at, uint32_t key, const BLENDFUNCTION *blend, DWORD flags, const RECT *dirty) {
    vx_window *w = window_of(h);
    if (!w) return 0;
    if (!(w->exstyle & WS_EX_LAYERED) || w->layered == 1 || (w->style & WS_CHILD)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    RECT r = w->rect;
    int cx = size ? size->cx : r.right - r.left, cy = size ? size->cy : r.bottom - r.top;
    if (cx <= 0 || cy <= 0) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if ((dst && (dst->x != r.left || dst->y != r.top)) || cx != r.right - r.left || cy != r.bottom - r.top) {
        BOOL (WINAPI *set_pos)(HANDLE, HANDLE, int, int, int, int, uint32_t) = (BOOL (WINAPI *)(HANDLE, HANDLE, int, int, int, int, uint32_t))vx_find_export("user32.dll", "SetWindowPos");
        if (set_pos) set_pos(h, NULL, dst ? dst->x : r.left, dst ? dst->y : r.top, cx, cy, 0x4 | 0x10 | (dst ? 0 : 0x2));   // SWP_NOZORDER | SWP_NOACTIVATE
    }
    int was = w->layered;
    DWORD before = w->layer_flags;
    uint8_t alpha_before = w->layer_alpha;
    w->layered = 2;
    w->layer_flags = flags | (blend && (blend->format & 1) ? 0x100 : 0);
    w->layer_alpha = (flags & ULW_ALPHA) && blend ? blend->sca : 255;
    w->layer_key = key;
    if (was != 2 || before != w->layer_flags || alpha_before != w->layer_alpha) vx_cocoa_set_layered(w);
    if (src) {
        HANDLE wdc = vx_dc_for_window(w);
        POINT o = at ? *at : (POINT){ 0, 0 };
        RECT part = dirty ? *dirty : (RECT){ 0, 0, cx, cy };
        blit_op op = { OP_COPY, 0xCC, 0, 255, 0, 0 };
        w->paint_depth++;                                           // (one commit, below)
        dc_blit(wdc, part.left, part.top, part.right - part.left, part.bottom - part.top, src, o.x + part.left, o.y + part.top,
                part.right - part.left, part.bottom - part.top, &op);
        vx_dc_release(wdc);
        w->paint_depth--;
    }
    pthread_mutex_lock(&w->lock);
    if (front_mode(w) == 2 || !w->front) vx_present_changed(w, NULL);   // (the color key applies to all of it)
    pthread_mutex_unlock(&w->lock);
    vx_present_commit(w);
    return 1;
}
static WINAPI BOOL SetLayeredWindowAttributes(HANDLE h, uint32_t key, uint8_t alpha, DWORD flags) {
    vx_window *w = window_of(h);
    if (!w) return 0;
    if (!(w->exstyle & WS_EX_LAYERED) || (flags & ~(DWORD)(LWA_COLORKEY | LWA_ALPHA))) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    int was = w->layered;
    w->layered = 1; w->layer_key = key; w->layer_alpha = flags & LWA_ALPHA ? alpha : 255; w->layer_flags = flags;
    vx_cocoa_set_layered(w);
    if (was == 2) vx_window_invalidate(w, NULL, 1);                // (painted by WM_PAINT from now on)
    pthread_mutex_lock(&w->lock);
    vx_present_changed(w, NULL);                                    // (the color key and alpha apply to all of it)
    pthread_mutex_unlock(&w->lock);
    vx_present_commit(w);
    return 1;
}
static WINAPI BOOL GetLayeredWindowAttributes(HANDLE h, uint32_t *key, uint8_t *alpha, DWORD *flags) {
    vx_window *w = window_of(h);
    if (!w) return 0;
    if (!(w->exstyle & WS_EX_LAYERED) || w->layered != 1) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (key) *key = w->layer_key;
    if (alpha) *alpha = w->layer_alpha;
    if (flags) *flags = w->layer_flags;
    return 1;
}
// WS_EX_LAYERED set anew: the window waits for SetLayeredWindowAttributes or UpdateLayeredWindow (invisible until
// then, as on Windows); cleared: an ordinary window again, repainted.
void vx_present_exstyle(vx_window *w, DWORD before) {
    DWORD changed = before ^ w->exstyle;
    if (!(changed & (WS_EX_LAYERED | WS_EX_TRANSPARENT))) return;
    if (changed & WS_EX_LAYERED) {
        w->layered = 0; w->layer_flags = 0; w->layer_alpha = 255;
        if (!(w->exstyle & WS_EX_LAYERED)) vx_window_invalidate(w, NULL, 1);
    }
    vx_cocoa_set_layered(w);
    if (changed & WS_EX_LAYERED) {
        pthread_mutex_lock(&w->lock);
        vx_present_changed(w, NULL);
        pthread_mutex_unlock(&w->lock);
        vx_present_commit(w);
    }
}

// PrintWindow: the window's client area drawn into a DC (PW_CLIENTONLY or not: windows here have no frame).
static WINAPI BOOL PrintWindow(HANDLE h, HANDLE dc, uint32_t flags) {
    (void)flags;
    vx_window *w = window_of(h);
    if (!w) return 0;
    HANDLE wdc = vx_dc_for_window(w);
    blit_op op = { OP_COPY, 0xCC, 0, 255, 0, 0 };
    BOOL ok = dc_blit(dc, 0, 0, w->client_w, w->client_h, wdc, 0, 0, w->client_w, w->client_h, &op);
    w->paint_depth++; vx_dc_release(wdc); w->paint_depth--;
    return ok;
}

// ---- DwmFlush --------------------------------------------------------------------------------------------------------------
// DwmFlush: what was drawn so far reaches the screen, and the call returns at the next frame of the display.
static WINAPI int32_t DwmFlush(void) {
    vx_window *all[1024];
    int n = vx_window_all(all, 1024);
    for (int i = 0; i < n; i++) if (all[i]->changed) vx_present_commit(all[i]);
    vx_cocoa_present_now();
    double period = vx_cocoa_refresh_period(), t = now_s();
    if (period <= 0.001) period = 1.0 / 60;
    double next = (double)((int64_t)(t / period) + 1) * period;
    struct timespec ts = { (time_t)(next - t), (long)((next - t - (double)(time_t)(next - t)) * 1e9) };
    nanosleep(&ts, NULL);
    return 0;                                                       // S_OK
}

static const vx_export gdi32[] = {
    VX_EXPORT(BitBlt), VX_EXPORT(StretchBlt), VX_EXPORT(StretchDIBits), VX_EXPORT(SetDIBitsToDevice), VX_EXPORT(GetDIBits),
    VX_EXPORT(SetDIBits), VX_EXPORT(GdiAlphaBlend), VX_EXPORT(GdiTransparentBlt),
};
static const vx_export msimg32[] = { { "AlphaBlend", (void *)GdiAlphaBlend }, { "TransparentBlt", (void *)GdiTransparentBlt } };
static const vx_export user32[] = {
    VX_EXPORT(BeginPaint), VX_EXPORT(EndPaint), VX_EXPORT(InvalidateRect), VX_EXPORT(InvalidateRgn), VX_EXPORT(ValidateRect),
    VX_EXPORT(ValidateRgn), VX_EXPORT(GetUpdateRect), VX_EXPORT(GetUpdateRgn), VX_EXPORT(UpdateWindow), VX_EXPORT(RedrawWindow),
    VX_EXPORT(LockWindowUpdate), VX_EXPORT(ScrollWindowEx), VX_EXPORT(ScrollWindow), VX_EXPORT(ScrollDC),
    VX_EXPORT(UpdateLayeredWindow), VX_EXPORT(UpdateLayeredWindowIndirect), VX_EXPORT(SetLayeredWindowAttributes),
    VX_EXPORT(GetLayeredWindowAttributes), VX_EXPORT(PrintWindow),
};
static const vx_export dwmapi[] = { VX_EXPORT(DwmFlush) };

void vx_present_init(void) {
    vx_register_module("gdi32.dll", gdi32, sizeof gdi32 / sizeof gdi32[0]);
    vx_register_module("msimg32.dll", msimg32, sizeof msimg32 / sizeof msimg32[0]);
    vx_register_module("user32.dll", user32, sizeof user32 / sizeof user32[0]);
    vx_register_module("dwmapi.dll", dwmapi, sizeof dwmapi / sizeof dwmapi[0]);
}

#ifdef __aarch64__
#include "thunks/present.c.inc"
#endif
