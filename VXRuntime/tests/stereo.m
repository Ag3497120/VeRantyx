// stereo.m's DIBR pass on the GPU, without a game (build/stereo_test):
//   stereo_test                      a made-up scene (striped background far, a square near): checks the flat
//                                    fallback, that each eye moves the background the right way by the right amount
//                                    and the screen-plane square not at all, and that no pixel is left unfilled
//   stereo_test <color.png> <depth.f32> <dw> <dh> <out dir> [frames]
//                                    a dumped game frame (VXRT_D3D12_STEREO_DUMP, reversed Z) through the pass:
//                                    <out dir>/0001-sbs.png, -anaglyph.png and the pass's GPU time
// Both go through the shared frame file (VXRT_STEREO_STREAM) as the encoder would read it.
#import <Metal/Metal.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include "stereo.h"

static int g_verbose = 1;
void vx_log(const char *fmt, ...) {
    if (!g_verbose) return;
    va_list a; va_start(a, fmt); vfprintf(stdout, fmt, a); va_end(a); fputc('\n', stdout);
}

static id<MTLDevice> dev;
static id<MTLCommandQueue> q;
static uint8_t *map;

static id<MTLTexture> depth_texture(const float *d, uint32_t w, uint32_t h) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    id<MTLBuffer> b = [dev newBufferWithBytes:d length:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromBuffer:b sourceOffset:0 sourceBytesPerRow:w * 4 sourceBytesPerImage:(NSUInteger)w * h * 4 sourceSize:MTLSizeMake(w, h, 1)
            toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return t;
}
static id<MTLTexture> color_texture(const uint8_t *rgba, uint32_t w, uint32_t h) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:rgba bytesPerRow:w * 4];
    return t;
}
// one frame: a pass that drew the depth (when given), then the present; waits for the GPU
static void frame(id<MTLTexture> color, id<MTLTexture> depth, uint32_t w, uint32_t h) {
    if (depth) {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        vx_stereo_depth_pass((__bridge void *)cb, (__bridge void *)depth, 100, 100, 1u << 5);   // (GREATER: reversed Z)
        [cb commit];
    }
    vx_stereo_present((__bridge void *)q, (__bridge void *)color, w, h);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    [cb commit];
    [cb waitUntilCompleted];
    usleep(20000);                                                   // (the completion handler publishes)
}
// the last published side-by-side frame (BGRA8): left eye at x < w
static const uint8_t *published(uint32_t *w2, uint32_t *h) {
    int32_t latest = *(volatile int32_t *)map;
    const uint8_t *f = map + 64 + (size_t)(88 + 4096u * 4096u * 8u) * (latest & 1);
    *w2 = ((const uint32_t *)f)[1]; *h = ((const uint32_t *)f)[2];
    return f + 88;
}

static int synthetic(void) {
    const uint32_t W = 640, H = 400, DW = 320, DH = 200;             // (depth at half size, as an upscaler renders)
    uint8_t *rgba = malloc(W * H * 4);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint8_t *p = rgba + (y * W + x) * 4;
            int sq = x >= W / 4 && x < 3 * W / 4 && y >= H / 4 && y < 3 * H / 4;
            if (sq) { p[0] = 40; p[1] = 200; p[2] = 40; }                       // the near square: green
            else { int stripe = (x / 16) & 1; p[0] = stripe ? 220 : 60; p[1] = 60; p[2] = stripe ? 60 : 220; }   // red/blue stripes
            p[3] = 255;
        }
    float *d = malloc(DW * DH * 4);
    for (uint32_t y = 0; y < DH; y++)
        for (uint32_t x = 0; x < DW; x++)
            d[y * DW + x] = x >= DW / 4 && x < 3 * DW / 4 && y >= DH / 4 && y < 3 * DH / 4 ? 0.05f : 0.01f;
    id<MTLTexture> color = color_texture(rgba, W, H), depth = depth_texture(d, DW, DH);
    int fails = 0;
    uint32_t w2, h;
    // frame 1: no depth seen yet: flat
    frame(color, depth, W, H);
    const uint8_t *px = published(&w2, &h);
    if (w2 != 2 * W || h != H) { printf("FAIL: published %ux%u, expected %ux%u\n", w2, h, 2 * W, H); return 1; }
    int diff = 0;
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
            for (int e = 0; e < 2; e++) {
                const uint8_t *s = px + ((size_t)y * 2 * W + e * W + x) * 4, *o = rgba + (y * W + x) * 4;
                if (s[2] != o[0] || s[1] != o[1] || s[0] != o[2]) diff++;
            }
    printf("%s: flat frame (the depth not chosen yet): %d pixels differ from the picture\n", diff ? "FAIL" : "ok", diff);
    fails += diff != 0;
    // frames 2..: stereo (the depth range fit: the square, a quarter of the picture, is the near end: -POP*S)
    for (int k = 0; k < 40; k++) frame(color, depth, W, H);
    px = published(&w2, &h);
    // the background's stripe edges on the row above the square: where does the left eye's red->blue edge near
    // x = 64 land, and the right eye's? expected: disparity S*(1 - 0.01*1.4/0.05) = 0.72 * 2 % * 640 = 9.2 px, half each way
    float S = 0.02f * W, expect = 0.5f * S * (1 - 0.01f * 1.4f / 0.05f), sq_shift = 0.5f * 0.4f * S;
    uint32_t y = H / 8;
    int edge[2] = { -1, -1 };
    for (int e = 0; e < 2; e++)
        for (uint32_t x = 40; x < 90 && edge[e] < 0; x++) {
            const uint8_t *a = px + ((size_t)y * 2 * W + e * W + x) * 4;
            if (a[2] < 140 && a[-4 + 2] >= 140 && x > 40) edge[e] = (int)x;
        }
    int orig = 64;                                                   // (x / 16 odd is red: 48..63 red, 64 blue)
    printf("background edge: picture %d, left eye %d (expected %.1f), right eye %d (expected %.1f)\n", orig, edge[0], orig - expect,
           edge[1], orig + expect);
    if (edge[0] < 0 || edge[1] < 0 || fabsf(edge[0] - (orig - expect)) > 1.5f || fabsf(edge[1] - (orig + expect)) > 1.5f) {
        printf("FAIL: background disparity\n"); fails++;
    } else printf("ok: the background is behind the screen (left eye left, right eye right)\n");
    // the square's left edge (the near end: in front of the screen, -0.4*S): the left eye's to the right, the right's left
    y = H / 2;
    int sq[2] = { -1, -1 };
    for (int e = 0; e < 2; e++)
        for (uint32_t x = W / 4 - 20; x < W / 4 + 20 && sq[e] < 0; x++) {
            const uint8_t *a = px + ((size_t)y * 2 * W + e * W + x) * 4;
            if (a[1] > 150) sq[e] = (int)x;
        }
    int sq_ok = fabsf(sq[0] - (W / 4 + sq_shift)) <= 1.5f && fabsf(sq[1] - (W / 4 - sq_shift)) <= 1.5f;
    printf("%s: square's edge at %d / %d (expected %.1f / %.1f: in front of the screen)\n", sq_ok ? "ok" : "FAIL", sq[0], sq[1],
           W / 4 + sq_shift, W / 4 - sq_shift);
    fails += !sq_ok;
    // every pixel filled with colour from the picture (no black from an unfilled hole)
    int black = 0;
    for (uint32_t k = 0; k < 2 * W * H; k++) { const uint8_t *a = px + k * 4; if (a[0] < 30 && a[1] < 30 && a[2] < 30) black++; }
    printf("%s: %d unfilled pixels\n", black ? "FAIL" : "ok", black);
    fails += black != 0;
    return fails;
}


// A tilted plane (a floor seen from above it): d = n/z is affine in screen space for any plane, so its disparity must
// be an exact linear ramp - any curve in the mapping (a clamp, a soft compression, linear z) makes it bulge. Depth at
// a third of the size (an upscaler's Ultra Performance), d rising from 0.002 at the top to 0.032 at the bottom (and a
// little across). The picture is a sawtooth (red = 8 x mod 256) whose value tells each eye pixel's source position;
// each row's shift is the mean over the row, disparity = right eye's shift - left eye's.
static int plane(void) {
    const uint32_t W = 640, H = 400, DW = 213, DH = 133;
    uint8_t *rgba = malloc(W * H * 4);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) { uint8_t *p = rgba + (y * W + x) * 4; p[0] = (uint8_t)(x * 8); p[1] = 128; p[2] = 128; p[3] = 255; }
    float *d = malloc(DW * DH * 4);
    for (uint32_t y = 0; y < DH; y++)
        for (uint32_t x = 0; x < DW; x++) d[y * DW + x] = 0.002f + 0.030f * y / (DH - 1) + 0.004f * x / (DW - 1);
    id<MTLTexture> color = color_texture(rgba, W, H), depth = depth_texture(d, DW, DH);
    for (int k = 0; k < 40; k++) frame(color, depth, W, H);
    uint32_t w2, h;
    const uint8_t *px = published(&w2, &h);
    double sy = 0, syy = 0, sv = 0, syv = 0, n = 0, disp[400];
    for (uint32_t y = 0; y < H; y++) {
        double sh[2] = { 0, 0 };
        for (int e = 0; e < 2; e++) {
            double sum = 0; int cnt = 0;
            for (uint32_t x = 40; x < W - 40; x++) {
                const uint8_t *a = px + ((size_t)y * 2 * W + e * W + x) * 4;
                double src = a[2] / 8.0;                              // (source x mod 32)
                double shift = fmod(x - src + 64.0 + 16.0, 32.0) - 16.0;
                if (fabs(shift) < 12) { sum += shift; cnt++; }
            }
            sh[e] = cnt ? sum / cnt : 0;
        }
        disp[y] = sh[1] - sh[0];
        if (y >= 8 && y < H - 16) { sy += y; syy += (double)y * y; sv += disp[y]; syv += y * disp[y]; n++; }
    }
    double b = (n * syv - sy * sv) / (n * syy - sy * sy), a = (sv - b * sy) / n, worst = 0;
    int at = 0;
    for (uint32_t y = 8; y < H - 16; y++) if (fabs(disp[y] - (a + b * y)) > worst) { worst = fabs(disp[y] - (a + b * y)); at = (int)y; }
    printf("tilted plane: disparity %.2f px at the top, %.2f at the bottom; off a straight line by %.3f px at most (row %d)\n",
           disp[8], disp[H - 17], worst, at);
    int ok = worst < 0.2 && b < 0;
    printf("%s: the plane stays planar (disparity linear in d)\n", ok ? "ok" : "FAIL");
    return !ok;
}

static int dumped(int argc, char **argv) {
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:@(argv[1])], NULL);
    CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
    if (!img) { printf("cannot read %s\n", argv[1]); return 1; }
    uint32_t W = (uint32_t)CGImageGetWidth(img), H = (uint32_t)CGImageGetHeight(img);
    uint8_t *rgba = calloc(W * H, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(rgba, W, H, 8, W * 4, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(ctx, CGRectMake(0, 0, W, H), img);
    uint32_t DW = (uint32_t)atoi(argv[3]), DH = (uint32_t)atoi(argv[4]);
    FILE *f = fopen(argv[2], "rb");
    float *d = malloc((size_t)DW * DH * 4);
    if (!f || fread(d, 4, (size_t)DW * DH, f) != (size_t)DW * DH) { printf("cannot read %s\n", argv[2]); return 1; }
    fclose(f);
    int frames = argc > 6 ? atoi(argv[6]) : 120;
    id<MTLTexture> color = color_texture(rgba, W, H), depth = depth_texture(d, DW, DH);
    char path[1100];
    snprintf(path, sizeof path, "%s/now", argv[5]);
    for (int k = 0; k < frames; k++) {
        if (k == frames - 1) { FILE *n = fopen(path, "w"); if (n) { fputs("1\n", n); fclose(n); } }
        frame(color, depth, W, H);
    }
    sleep(1);
    return 0;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        // (VXRT_STEREO_STREAM given: that file, kept - to feed a running MacEncoder/HardwareEncoder)
        char stream[1024] = "/tmp/vxrt_stereo_test.XXXXXX";
        const char *given = getenv("VXRT_STEREO_STREAM");
        int fd;
        if (given && *given) snprintf(stream, sizeof stream, "%s", given);
        else {
            if ((fd = mkstemp(stream)) < 0) return 1;
            close(fd);
        }
        setenv("VXRT_STEREO", "1", 1);
        setenv("VXRT_STEREO_STREAM", stream, 1);
        setenv("VXRT_STEREO_STATS", "40", 1);
        if (argc >= 6) setenv("VXRT_STEREO_SHOT", argv[5], 1);
        dev = MTLCreateSystemDefaultDevice();
        q = [dev newCommandQueue];
        if (!dev || !q || !vx_stereo_on()) { printf("no Metal device\n"); return 1; }
        // (stereo.m maps it on its first publish; the test reads it the same way)
        fd = open(stream, O_RDWR);
        ftruncate(fd, 64 + 2 * (88 + 4096ull * 4096 * 8) + 312 + 1024);
        map = mmap(NULL, 64 + 2 * (88 + 4096ull * 4096 * 8) + 312 + 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        int r = argc >= 6 ? dumped(argc, argv) : synthetic() + plane();
        if (!given || !*given) unlink(stream);
        printf("%s\n", r ? "stereo_test: FAILED" : "stereo_test: ok");
        return r != 0;
    }
}
