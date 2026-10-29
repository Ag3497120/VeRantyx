// The warp core (src/warp_core.m) on synthetic frames: B's pixels hold their own coordinates (RGBA32Float: x/w, y/h,
// layer, 1), so every output pixel says where in B it came from. Checks: a camera rotation (pose), a sideways step
// (parallax: depth order, the uncovered background), 1-5 predictions of a moving square (the game's motion vectors:
// the square where it will be, the uncovered strip background - never the square twice, never black), a stereo eye
// (stereo.m's disparity), the HUD held over a panning scene; then the GPU time at 1920x1080 (--bench for more).
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/warp_core.h"

enum { W = 320, H = 192, RW = 160, RH = 96 };
static const float ZA = 0.1f, ZBG = 50, ZFG = 5, TY = 0.6f;
static const int FX0 = 120, FX1 = 200, FY0 = 64, FY1 = 128;      // the square (frame pixels)
static int failures;

static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;

static id<MTLTexture> tex(MTLPixelFormat f, uint32_t w, uint32_t h, const void *data, size_t bpp) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    if (data) [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:data bytesPerRow:w * bpp];
    return t;
}
static int in_square(float x, float y, float dx) { return x >= FX0 + dx && x < FX1 + dx && y >= FY0 && y < FY1; }

// B (coordinates), its depth and motion vectors at render size (the square moving by (mx, 0) frame pixels an
// interval, or everything by (pan, 0)), A (the previous frame: B moved back), the HUD rectangle the same in both
static id<MTLTexture> B, A, DEPTH, MV;
static void scene(float mx, float pan, int hud) {
    float *b = malloc(W * H * 16), *a = malloc(W * H * 16), *d = malloc(RW * RH * 4), *mv = malloc(RW * RH * 8);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float *p = b + 4 * (y * W + x);
            p[0] = (x + 0.5f) / W; p[1] = (y + 0.5f) / H; p[2] = in_square(x, y, 0); p[3] = 1;
        }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {                              // (A(x) = B(x + m): B's pixel was at x - m)
            int sx = x + (int)lroundf(pan);
            sx = sx < 0 ? 0 : sx >= W ? W - 1 : sx;
            int is_hud = hud && x >= 20 && x < 80 && y >= 20 && y < 60;
            memcpy(a + 4 * (y * W + x), b + 4 * (y * W + (is_hud ? x : sx)), 16);
        }
    for (int y = 0; y < RH; y++)
        for (int x = 0; x < RW; x++) {
            float fx = (x + 0.5f) * W / RW, fy = (y + 0.5f) * H / RH;
            int sq = in_square(fx, fy, 0);
            d[y * RW + x] = ZA / (sq ? ZFG : ZBG);                 // (reversed, infinite: d = near / z)
            float m = pan + (sq ? mx : 0);
            mv[2 * (y * RW + x)] = -m / W;                         // (UV units: B's pixel was at uv + mv)
            mv[2 * (y * RW + x) + 1] = 0;
        }
    B = tex(MTLPixelFormatRGBA32Float, W, H, b, 16);
    A = tex(MTLPixelFormatRGBA32Float, W, H, a, 16);
    DEPTH = tex(MTLPixelFormatR32Float, RW, RH, d, 4);
    MV = tex(MTLPixelFormatRG32Float, RW, RH, mv, 8);
    free(b); free(a); free(d); free(mv);
}

static vxw_params base(void) {
    vxw_params p = { 0 };
    p.w = W; p.h = H; p.mv_sx = W; p.mv_sy = H;
    p.tan_y = TY; p.tan_x = TY * W / H;
    p.z_a = ZA; p.z_b = 0;
    p.rot[0] = p.rot[4] = p.rot[8] = 1;
    p.thr = 1.0f / 255;
    return p;
}

static float *run(vxw_core *c, const vxw_params *p, int use_a) {
    id<MTLTexture> out = tex(MTLPixelFormatRGBA32Float, W, H, NULL, 16);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    if (!vxw_encode(c, (__bridge void *)cb, p, (__bridge void *)B, use_a ? (__bridge void *)A : NULL, (__bridge void *)MV,
                    (__bridge void *)DEPTH, (__bridge void *)out)) { printf("FAIL: nothing encoded\n"); failures++; return NULL; }
    [cb commit];
    [cb waitUntilCompleted];
    float *o = malloc(W * H * 16);
    [out getBytes:o bytesPerRow:W * 16 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    return o;
}

static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
// the error of where pixels came from against where they should have, over the pixels where want() says
typedef int (*want_fn)(int x, int y, float *sx, float *sy, float *layer, void *ctx);
static void check(const char *name, const float *o, want_fn want, void *ctx, float med_max, float p95_max, float layer_ok) {
    static float err[W * H];
    int n = 0, bad_layer = 0, black = 0, total = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const float *p = o + 4 * (y * W + x);
            total++;
            if (!(p[3] > 0.5f) || !isfinite(p[0]) || !isfinite(p[1]) || (p[0] == 0 && p[1] == 0)) black++;
            float sx, sy, layer;
            if (!want(x, y, &sx, &sy, &layer, ctx)) continue;
            float gx = p[0] * W, gy = p[1] * H;
            err[n++] = hypotf(gx - sx, gy - sy);
            if (layer >= 0 && fabsf(p[2] - layer) > 0.5f) bad_layer++;
        }
    qsort(err, n, sizeof *err, cmpf);
    float med = n ? err[n / 2] : 0, p95 = n ? err[(int)(n * 0.95f)] : 0;
    int ok = n > 0 && med <= med_max && p95 <= p95_max && bad_layer <= layer_ok * n && black == 0;
    printf("%s %s: %d pixels checked, source error median %.2f px, p95 %.2f px; wrong layer %d; black/garbage %d of %d\n",
           ok ? "ok  " : "FAIL", name, n, med, p95, bad_layer, black, total);
    if (!ok) failures++;
}

// T1: a yaw of 2 degrees: every pixel's source is its ray turned back (depth does not matter)
static float ang = 2.0f * (float)M_PI / 180;
static int want_rot(int x, int y, float *sx, float *sy, float *layer, void *ctx) {
    const vxw_params *p = ctx;
    float nx = (x + 0.5f) / W * 2 - 1, ny = 1 - (y + 0.5f) / H * 2;
    float dx = nx * p->tan_x, dy = ny * p->tan_y, dz = 1;
    // X' = R X: X = R^T X' (R a yaw: rows (c 0 s) (0 1 0) (-s 0 c))
    float c = cosf(ang), s = sinf(ang);
    float ox = c * dx - s * dz, oz = s * dx + c * dz, oy = dy;
    *sx = (ox / (oz * p->tan_x) + 1) * 0.5f * W;
    *sy = (1 - oy / (oz * p->tan_y)) * 0.5f * H;
    *layer = -1;
    // (inside the frame, away from the square's edges: a block straddling one lands with either side)
    int sq = in_square(*sx, *sy, 0), near_edge = in_square(*sx + 3, *sy, 0) != sq || in_square(*sx - 3, *sy, 0) != sq ||
             in_square(*sx, *sy + 3, 0) != sq || in_square(*sx, *sy - 3, 0) != sq;
    return *sx > 3 && *sx < W - 3 && *sy > 3 && *sy < H - 3 && !near_edge;
}
// T2: the camera 0.3 to the right: the square (z 5) moves left 9.6 px, the background (z 50) 0.96
static float step_x = 0.3f;
static float shift_of(float z, const vxw_params *p) { return -(step_x / z) / p->tan_x * 0.5f * W; }
static int want_step(int x, int y, float *sx, float *sy, float *layer, void *ctx) {
    const vxw_params *p = ctx;
    float fg = shift_of(ZFG, p), bg = shift_of(ZBG, p);
    int on_sq = in_square(x + 0.5f - fg, y + 0.5f, 0);
    // (2 px away from the square's new edges and its old right edge: the uncovered strip is checked as background)
    if (in_square(x + 2.5f - fg, y + 0.5f, 0) != on_sq || in_square(x - 1.5f - fg, y + 0.5f, 0) != on_sq ||
        in_square(x + 0.5f - fg, y + 2.5f, 0) != on_sq || in_square(x + 0.5f - fg, y - 1.5f, 0) != on_sq) return 0;
    if (!on_sq && in_square(x + 0.5f - bg, y + 0.5f, 0)) {        // (uncovered: background, from beside the hole)
        *layer = 0; *sx = x + 0.5f; *sy = y + 0.5f;
        return 2;
    }
    *layer = on_sq; *sx = x + 0.5f - (on_sq ? fg : bg); *sy = y + 0.5f;
    return *sx > 2 && *sx < W - 2;
}
static int want_step_pos(int x, int y, float *sx, float *sy, float *layer, void *ctx) {
    return want_step(x, y, sx, sy, layer, ctx) == 1;
}
static int want_step_hole(int x, int y, float *sx, float *sy, float *layer, void *ctx) {
    if (want_step(x, y, sx, sy, layer, ctx) != 2) return 0;
    return 1;                                                      // (the layer is what matters; its source: beside the hole)
}
// T3: the square moving 6 px an interval, t intervals on
typedef struct { float t, m; int part; } mv_ctx;
static int want_mv(int x, int y, float *sx, float *sy, float *layer, void *ctx_) {
    mv_ctx *c = ctx_;
    float dx = c->t * c->m, X = x + 0.5f, Y = y + 0.5f;
    int on = in_square(X, Y, dx), edge = in_square(X + 2, Y, dx) != on || in_square(X - 2, Y, dx) != on ||
             in_square(X, Y + 2, dx) != on || in_square(X, Y - 2, dx) != on;
    if (edge) return 0;
    int uncovered = !on && in_square(X, Y, 0);
    if (c->part == 0 && !uncovered) { *layer = on; *sx = X - (on ? dx : 0); *sy = Y; return 1; }
    if (c->part == 1 && uncovered && !(in_square(X + 2, Y, 0) != 1 || in_square(X - 2, Y, 0) != 1)) {
        *layer = 0; *sx = X; *sy = Y; return 1;                    // (the layer is what matters: background)
    }
    return 0;
}
// T4: the right eye, S 8 px, screen plane at z 10: the background 3.2 px right, the square 3.2 left (clamped pop-out)
static int want_eye(int x, int y, float *sx, float *sy, float *layer, void *ctx) {
    (void)ctx;
    float X = x + 0.5f, Y = y + 0.5f;
    float bg = 0.5f * 8 * (1 - 10.0f / ZBG), fg = 0.5f * fmaxf(8 * (1 - 10.0f / ZFG), -2 * 0.4f * 8);
    int on = in_square(X - fg, Y, 0);
    if (in_square(X - fg + 2.5f, Y, 0) != on || in_square(X - fg - 2.5f, Y, 0) != on || in_square(X - fg, Y + 2, 0) != on ||
        in_square(X - fg, Y - 2, 0) != on || in_square(X - bg + 3, Y, 0) != in_square(X - bg - 3, Y, 0)) return 0;
    if (!on && in_square(X - bg, Y, 0)) return 0;                  // (the uncovered strip: checked in T2's way)
    *layer = on; *sx = X - (on ? fg : bg); *sy = Y;
    return *sx > 2 && *sx < W - 2;
}
// T5: everything panning 5 px an interval, the HUD (20..80 x 20..60) the same in A and B: held
static int want_hud(int x, int y, float *sx, float *sy, float *layer, void *ctx) {
    float t = *(float *)ctx, X = x + 0.5f, Y = y + 0.5f;
    int hud = X >= 20 && X < 80 && Y >= 20 && Y < 60, near = X >= 16 && X < 84 && Y >= 16 && Y < 64;
    if (near && !hud) return 0;
    if (hud) { *sx = X; *sy = Y; *layer = 0; return X > 22 && X < 78 && Y > 22 && Y < 58; }
    float sxx = X - 5 * t;
    if (in_square(sxx + 3, Y, 0) != in_square(sxx - 3, Y, 0) || in_square(sxx, Y + 3, 0) != in_square(sxx, Y - 3, 0)) return 0;
    *sx = sxx; *sy = Y; *layer = -1;
    return sxx > 2;
}

// T6: an eye as CompositorServices gives it: its own texture size (OW x OH), an asymmetric frustum, no motion, and the
// depth out (dst_near / distance); colour through compute and depth R32Float, then a render pass with Depth32Float
enum { OW = 240, OH = 200 };
static const float DT[4] = { 0.7f, 0.9f, 0.5f, 0.4f }, DNEAR = 0.05f;
static void eye_src(int x, int y, float *sx, float *sy, const vxw_params *p) {
    float rx = -DT[0] + (x + 0.5f) / OW * (DT[0] + DT[1]), ry = DT[2] - (y + 0.5f) / OH * (DT[2] + DT[3]);
    *sx = (rx / p->tan_x + 1) * 0.5f * W; *sy = (1 - ry / p->tan_y) * 0.5f * H;
}
static void eye_test(vxw_core *c, int render) {
    vxw_params p = base();
    p.flags = VXW_POSE;
    memcpy(p.dst_tan, DT, sizeof DT);
    p.dst_near = DNEAR;
    id<MTLTexture> out = tex(MTLPixelFormatRGBA32Float, OW, OH, NULL, 16), od;
    if (render) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:OW height:OH mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        od = [dev newTextureWithDescriptor:td];
    } else od = tex(MTLPixelFormatR32Float, OW, OH, NULL, 4);
    id<MTLBuffer> db = [dev newBufferWithLength:OW * OH * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    if (!vxw_encode_eye(c, (__bridge void *)cb, &p, (__bridge void *)B, NULL, (__bridge void *)MV, (__bridge void *)DEPTH,
                        (__bridge void *)out, (__bridge void *)od)) { printf("FAIL: eye: nothing encoded\n"); failures++; return; }
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:od sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(OW, OH, 1)
               toBuffer:db destinationOffset:0 destinationBytesPerRow:OW * 4 destinationBytesPerImage:OW * OH * 4];
    [bl endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    float *o = malloc(OW * OH * 16);
    [out getBytes:o bytesPerRow:OW * 16 fromRegion:MTLRegionMake2D(0, 0, OW, OH) mipmapLevel:0];
    const float *d = db.contents;
    int n = 0, bad = 0, nd = 0, badd = 0, black = 0;
    float worst = 0;
    for (int y = 0; y < OH; y++)
        for (int x = 0; x < OW; x++) {
            const float *q = o + 4 * (y * OW + x);
            if (!(q[3] > 0.5f) || !isfinite(q[0])) { black++; continue; }
            float sx, sy;
            eye_src(x, y, &sx, &sy, &p);
            int sq = in_square(sx, sy, 0);
            if (in_square(sx + 3, sy, 0) != sq || in_square(sx - 3, sy, 0) != sq || in_square(sx, sy + 3, 0) != sq ||
                in_square(sx, sy - 3, 0) != sq || sx < 3 || sx > W - 3 || sy < 3 || sy > H - 3) continue;
            float e = hypotf(q[0] * W - sx, q[1] * H - sy);
            n++;
            if (e > 1.5f) bad++;
            if (e > worst) worst = e;
            float want = DNEAR / (sq ? ZFG : ZBG);
            nd++;
            if (fabsf(d[y * OW + x] - want) > 0.02f * want) badd++;
        }
    // (equal-depth blocks landing on one output block: the higher index wins, a neighbour's displacement - a few
    //  pixels at most, 1-2 B pixels off; none further)
    int ok = n > 1000 && bad * 1000 <= n && worst < 3.5f && badd == 0 && black == 0;
    printf("%s eye %dx%d, asymmetric frustum, depth out (%s): %d pixels: %d off by > 1.5 px (worst %.2f); depth %d of %d wrong; "
           "black %d\n", ok ? "ok  " : "FAIL", OW, OH, render ? "render pass, Depth32Float" : "compute, R32Float", n, bad, worst, badd, nd, black);
    if (!ok) failures++;
    free(o);
}

static void bench(vxw_core *c, uint32_t w, uint32_t h, int n, int eyes) {
    id<MTLTexture> b = tex(MTLPixelFormatBGRA8Unorm, w, h, NULL, 4), mv = tex(MTLPixelFormatRG16Float, w / 2, h / 2, NULL, 4),
                   d = tex(MTLPixelFormatR32Float, w / 2, h / 2, NULL, 4), o = tex(MTLPixelFormatBGRA8Unorm, w, h, NULL, 4);
    vxw_params p = base();
    p.w = w; p.h = h; p.t = 0.5f; p.flags = VXW_STILL | (eyes > 1 ? VXW_POSE | VXW_STEREO : 0);
    p.S = 0.02f * w; p.pop = 0.4f; p.zc = 10; p.eye = 0.5f;
    double sum = 0;
    float v[1024];
    for (int i = 0; i < n + 3; i++) {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        for (int e = 0; e < eyes; e++) { p.eye = e ? 0.5f : -0.5f; vxw_encode(c, (__bridge void *)cb, &p, (__bridge void *)b, (__bridge void *)b, (__bridge void *)mv, (__bridge void *)d, (__bridge void *)o); }
        [cb commit];
        [cb waitUntilCompleted];
        if (i >= 3) { v[i - 3] = (float)((cb.GPUEndTime - cb.GPUStartTime) * 1000); sum += v[i - 3]; }
    }
    qsort(v, n, sizeof *v, cmpf);
    printf("bench %ux%u, %d eye%s: GPU %.3f ms median, %.3f ms mean (%d runs)\n", w, h, eyes, eyes > 1 ? "s" : "", v[n / 2], sum / n, n);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        dev = MTLCreateSystemDefaultDevice();
        queue = [dev newCommandQueue];
        vxw_core *c = vxw_create((__bridge void *)dev);
        if (!c) { printf("FAIL: no core\n"); return 1; }
        if (argc > 1 && !strcmp(argv[1], "--bench")) {
            bench(c, 1280, 720, 200, 1); bench(c, 1920, 1080, 200, 1); bench(c, 1920, 1080, 200, 2); bench(c, 2560, 1440, 100, 2);
            return 0;
        }
        scene(6, 0, 0);
        vxw_params p = base();
        p.flags = VXW_POSE;
        float c_ = cosf(ang), s_ = sinf(ang);
        float rot[9] = { c_, 0, s_, 0, 1, 0, -s_, 0, c_ };
        memcpy(p.rot, rot, sizeof rot);
        float *o = run(c, &p, 0);
        check("rotation 2 deg (pose, no motion)", o, want_rot, &p, 0.35f, 1.0f, 1);
        free(o);

        p = base();
        p.flags = VXW_POSE;
        p.pos[0] = -step_x;                                        // (the camera right: points left of it)
        o = run(c, &p, 0);
        check("step 0.3 right: parallax", o, want_step_pos, &p, 0.35f, 1.0f, 0.001f);
        check("step 0.3 right: uncovered strip is background", o, want_step_hole, &p, 1e9f, 1e9f, 0.02f);
        free(o);

        for (int t = 1; t <= 5; t++) {
            p = base();
            p.t = (float)t;
            o = run(c, &p, 0);
            char name[96];
            mv_ctx m = { (float)t, 6, 0 };
            snprintf(name, sizeof name, "motion vectors, t %d: the square %d px on, background still", t, 6 * t);
            check(name, o, want_mv, &m, 0.35f, 1.0f, 0.001f);
            m.part = 1;
            snprintf(name, sizeof name, "motion vectors, t %d: the uncovered %d px strip is background", t, 6 * t);
            check(name, o, want_mv, &m, 1e9f, 1e9f, 0.02f);
            free(o);
        }

        p = base();
        p.flags = VXW_STEREO;
        p.eye = 0.5f; p.S = 8; p.pop = 0.4f; p.zc = 10; p.conv = 1;
        o = run(c, &p, 0);
        check("stereo right eye (S 8 px, screen plane z 10)", o, want_eye, &p, 0.35f, 1.0f, 0.001f);
        free(o);
        // the same eye 3 intervals on: one warp for both
        p.t = 3;
        o = run(c, &p, 0);
        {
            int n = 0, bad = 0;
            float fg = 0.5f * fmaxf(8 * (1 - 10.0f / ZFG), -2 * 0.4f * 8);
            for (int y = FY0 + 4; y < FY1 - 4; y++)
                for (int x = FX0 + 18 + 8; x < FX1 + 18 - 8; x++) {
                    const float *q = o + 4 * (y * W + x);
                    n++;
                    if (q[2] < 0.5f || fabsf(q[0] * W - (x + 0.5f - 18 - fg)) > 1) bad++;
                }
            printf("%s stereo right eye 3 intervals on: the square %d px on and %.1f px over: %d of %d pixels wrong\n",
                   bad ? "FAIL" : "ok  ", 18, fg, bad, n);
            if (bad) failures++;
        }
        free(o);

        scene(0, 5, 1);
        for (int t = 1; t <= 3; t += 2) {
            p = base();
            p.t = (float)t;
            p.flags = VXW_STILL;
            o = run(c, &p, 1);
            float tt = (float)t;
            char name[96];
            snprintf(name, sizeof name, "HUD held over a 5 px pan, t %d", t);
            check(name, o, want_hud, &tt, 0.35f, 1.0f, 1);
            free(o);
        }
        scene(6, 0, 0);
        eye_test(c, 0);
        eye_test(c, 1);
        bench(c, 1920, 1080, 50, 1);
        bench(c, 1920, 1080, 50, 2);
        vxw_destroy(c);
    }
    printf(failures ? "warp core: %d FAILED\n" : "warp core: all ok\n", failures);
    return failures != 0;
}
