// GPU time per guest frame, read from Metal itself.
//
// A PC VR game sizes its rendering from the GPU time the compositor reports
// back (SteamVR's Compositor_FrameTiming::m_flTotalRenderGpuMs: Source 2's
// auto-fidelity keeps a ring of it and drops or raises vr_fidelity_level
// against the frame budget). MoltenVK has no timestamp queries worth the name
// on Apple GPUs, but every MTLCommandBuffer knows when the GPU started and
// finished it. So every command buffer committed on a queue the guest submits
// to gets a completed handler, and a frame's GPU time is the UNION of its
// buffers' [GPUStartTime, GPUEndTime] intervals: overlap between neighbouring
// buffers is counted once and the idle gaps between them not at all.
//
// A buffer belongs to the frame that was open when it was committed; the frame
// closes at kl_gpu_time_frame_end (the OpenVR shim calls it on the game's thread
// when both eyes are submitted). A frame is final once every buffer committed
// in it has completed.
//
// The commit hook is a method swizzle of the command buffer class's -commit,
// installed once from the first watched queue's own buffer class; buffers of
// any other queue (the compositor's, ANGLE's) pass straight through.
// KL_GPU_TIME=0 leaves -commit alone.
#include <os/lock.h>
#include <stdio.h>
#include <stdatomic.h>
#include <execinfo.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mach/mach_time.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>

#include "kl_gpu_time.h"
#include "kl_glfb.h"
#include <math.h>

enum { kRing = 16, kMaxQueues = 8 };
// Role 0 is the guest's queue(s), role 1 our compositor's (on the device it
// draws every panel refresh, so twice per game frame at 120 Hz; SteamVR's
// "total render" time includes the compositor's GPU work too).
typedef struct {
    uint32_t frame;          // which frame this slot holds
    uint32_t committed, completed;
    double busy[2];          // seconds of union GPU time so far, per role
    double last_end[2];      // end of the latest interval already counted, per role
} slot;

static os_unfair_lock g_lk = OS_UNFAIR_LOCK_INIT;
static void *g_queues[kMaxQueues];
static int g_roles[kMaxQueues];
static int g_nqueues;
static uint32_t g_frame = 1;     // the open frame (0 means "none" in a slot)
static slot g_slots[kRing];
static IMP g_commit;             // the original -commit

static slot *slot_of(uint32_t f) {
    slot *s = &g_slots[f % kRing];
    if (s->frame != f) { memset(s, 0, sizeof *s); s->frame = f; }
    return s;
}

static void on_complete(uint32_t f, int role, double start, double end) {
    os_unfair_lock_lock(&g_lk);
    slot *s = &g_slots[f % kRing];
    if (s->frame == f) {
        if (end > start && start > 0) {
            double from = start > s->last_end[role] ? start : s->last_end[role];
            if (end > from) s->busy[role] += end - from;
            if (end > s->last_end[role]) s->last_end[role] = end;
        }
        s->completed++;
    }
    os_unfair_lock_unlock(&g_lk);
}

// The queue's role, or -1 when it is not watched.
static int watched(void *q) {
    for (int i = 0; i < g_nqueues; i++) if (g_queues[i] == q) return g_roles[i];
    return -1;
}

// ---- Snapshot of the guest's output image (KL_GPU_SNAP=<frame>:<file.ppm>) -------
// Layer 0 (the left eye) of the first 8-bit RGBA render target at least 1024 wide that the guest
// draws in that frame (the tonemapped eye image), copied out when its command buffer commits:
// the A/B check that a pass change (KL_VK_MERGE) leaves the picture alone.
static uint32_t g_snap_at, g_snap_n = 1, g_snap_every = 1, g_snap_taken;
static char g_snap_path[512];
static void *g_snap_tex, *g_snap_cb;
static int g_snap_done;
static void snap_note(id<MTLCommandBuffer> cb, MTLRenderPassDescriptor *d, uint32_t f) {
    if (!g_snap_at || g_snap_done || g_snap_tex || f != g_snap_at) return;
    id<MTLTexture> t = d.colorAttachments[0].texture;
    // Eye-sized only (asymmetric L0 is 1917 wide). KL_GPU_SNAP_MINW raises the floor: at 3360 wide the game's
    // half-resolution RGBA8 target (1680) passes 1500 and comes first in the frame.
    static int minw = -1;
    if (minw < 0) { const char *e = getenv("KL_GPU_SNAP_MINW"); minw = e && atoi(e) > 0 ? atoi(e) : 1500; }
    if (!t || t.pixelFormat != MTLPixelFormatRGBA8Unorm || (int)t.width < minw || t.height < 1200 || t.sampleCount != 1) return;
    g_snap_tex = (void *)CFBridgingRetain(t);
    g_snap_cb = (__bridge void *)cb;
}
static void snap_take(id<MTLCommandBuffer> cb) {
    if (!g_snap_tex || g_snap_done || (__bridge void *)cb != g_snap_cb) return;
    g_snap_done = 1;
    // A sequence (KL_GPU_SNAP_N frames, KL_GPU_SNAP_EVERY apart): <path>, then <path>.1, <path>.2, ...
    char path[560];
    if (g_snap_taken) snprintf(path, sizeof path, "%s.%u", g_snap_path, g_snap_taken);
    else snprintf(path, sizeof path, "%s", g_snap_path);
    uint32_t at = g_snap_at;
    id<MTLTexture> t = (__bridge id<MTLTexture>)g_snap_tex;
    NSUInteger x0 = 0, y0 = 0, w = t.width, h = t.height;
    // KL_GPU_SNAP_CROP=x,y,w,h: write only that region, so a sequence can take every frame (a flicker hunt)
    const char *cr = getenv("KL_GPU_SNAP_CROP");
    unsigned cx, cy, cw, ch;
    if (cr && sscanf(cr, "%u,%u,%u,%u", &cx, &cy, &cw, &ch) == 4 && cx + cw <= t.width && cy + ch <= t.height && cw && ch) {
        x0 = cx; y0 = cy; w = cw; h = ch;
    }
    NSUInteger bpr = w * 4;
    id<MTLBuffer> buf = [cb.device newBufferWithLength:bpr * h options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x0, y0, 0) sourceSize:MTLSizeMake(w, h, 1)
              toBuffer:buf destinationOffset:0 destinationBytesPerRow:bpr destinationBytesPerImage:bpr * h];
    [b endEncoding];
    NSString *where = [NSString stringWithUTF8String:path];
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        FILE *fp = fopen(where.UTF8String, "wb");
        if (!fp) return;
        fprintf(fp, "P6\n%lu %lu\n255\n", (unsigned long)w, (unsigned long)h);
        const uint8_t *px = buf.contents;
        for (NSUInteger i = 0; i < w * h; i++) fwrite(px + 4 * i, 1, 3, fp);
        fclose(fp);
        fprintf(stderr, "[snap] frame %u: %lux%lu written to %s\n", at, (unsigned long)w, (unsigned long)h, where.UTF8String);
    }];
    CFRelease(g_snap_tex);
    g_snap_tex = NULL;
    if (++g_snap_taken < g_snap_n) { g_snap_at += g_snap_every; g_snap_done = 0; }
}

static void mg_flush(void *cb);
static void kl_commit(id self, SEL sel) {
    mg_flush((__bridge void *)self);
    if (g_snap_tex) snap_take((id<MTLCommandBuffer>)self);
    id<MTLCommandBuffer> cb = (id<MTLCommandBuffer>)self;
    void *q = (__bridge void *)[cb commandQueue];
    os_unfair_lock_lock(&g_lk);
    int role = watched(q);
    uint32_t f = g_frame;
    if (role >= 0) slot_of(f)->committed++;
    os_unfair_lock_unlock(&g_lk);
    if (role >= 0) {
        [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
            on_complete(f, role, done.GPUStartTime, done.GPUEndTime);
        }];
    }
    ((void (*)(id, SEL))g_commit)(self, sel);
}

// ---- Foveation spike (KL_VRR_SPIKE; a Mac measurement, off by default) -----
// Tier 2 foveation means shading the scene passes at a lower rate away from the
// gaze with Metal's variable rasterization rate (VRR). Before building it, this
// measures what it would buy, through the same command buffer class:
//   KL_VRR_SPIKE=census   once, in frame KL_VRR_SPIKE_AT (default 900), log every
//                         render pass the guest opens: attachments, sizes,
//                         formats, samples, layers, load/store actions.
//   KL_VRR_SPIKE=inject[:q]  attach a centred 5x5-zone rate map (rates q, (1+q)/2,
//                         1, (1+q)/2, q per axis; q default 0.25) to every guest
//                         pass whose first attachment has 4 samples and is at
//                         least 1024 wide: the scene passes. Nothing unwarps the
//                         result, so the picture is wrong; [gputime] shows what
//                         the passes would save.
static int g_vrr_mode = -1;          // 0 off, 1 census, 2 inject
static float g_vrr_q = 0.25f;
static uint32_t g_vrr_at = 900;
static double g_vrr_toggle;
static IMP g_rce;                    // the original -renderCommandEncoderWithDescriptor:
static unsigned g_vrr_injected;
static struct { NSUInteger w, h, layers; void *map; } g_vrr_maps[8];

static void vrr_census(MTLRenderPassDescriptor *d, uint32_t f) {
    static unsigned n;
    char line[1024];
    int u = snprintf(line, sizeof line, "[vrr] frame %u pass %u: rt %lux%lu layers %lu", f, n++,
                     (unsigned long)d.renderTargetWidth, (unsigned long)d.renderTargetHeight,
                     (unsigned long)d.renderTargetArrayLength);
    for (int i = 0; i < 8 && u < (int)sizeof line - 128; i++) {
        MTLRenderPassColorAttachmentDescriptor *a = d.colorAttachments[i];
        if (!a.texture) continue;
        u += snprintf(line + u, sizeof line - u, " | c%d %lux%lu fmt %lu s%lu t%lu a%lu L%lu S%lu%s", i,
                      (unsigned long)a.texture.width, (unsigned long)a.texture.height,
                      (unsigned long)a.texture.pixelFormat, (unsigned long)a.texture.sampleCount,
                      (unsigned long)a.texture.textureType, (unsigned long)a.texture.arrayLength,
                      (unsigned long)a.loadAction, (unsigned long)a.storeAction, a.resolveTexture ? " +resolve" : "");
    }
    id<MTLTexture> z = d.depthAttachment.texture;
    if (z && u < (int)sizeof line - 96)
        u += snprintf(line + u, sizeof line - u, " | z %lux%lu fmt %lu s%lu L%lu S%lu%s", (unsigned long)z.width,
                      (unsigned long)z.height, (unsigned long)z.pixelFormat, (unsigned long)z.sampleCount,
                      (unsigned long)d.depthAttachment.loadAction, (unsigned long)d.depthAttachment.storeAction,
                      d.depthAttachment.resolveTexture ? " +resolve" : "");
    fprintf(stderr, "%s\n", line);
}

static id<MTLRasterizationRateMap> vrr_map(id<MTLDevice> dev, NSUInteger w, NSUInteger h, NSUInteger layers) {
    for (int i = 0; i < 8; i++)
        if (g_vrr_maps[i].map && g_vrr_maps[i].w == w && g_vrr_maps[i].h == h && g_vrr_maps[i].layers == layers)
            return (__bridge id<MTLRasterizationRateMap>)g_vrr_maps[i].map;
    // KL_VRR_UNIFORM=1: every zone at q (a plain downscale in physical space; tests whether the scene's
    // shaders look anything up by screen position, which a rate map would misplace).
    static int uniform = -1;
    if (uniform < 0) { const char *u = getenv("KL_VRR_UNIFORM"); uniform = u && *u == '1'; }
    float q = g_vrr_q, m = uniform ? q : (1 + q) / 2, r[5] = { q, m, uniform ? q : 1, m, q };
    MTLRasterizationRateLayerDescriptor *l =
        [[MTLRasterizationRateLayerDescriptor alloc] initWithSampleCount:MTLSizeMake(5, 5, 0) horizontal:r vertical:r];
    MTLRasterizationRateLayerDescriptor *ls[8];
    for (NSUInteger i = 0; i < layers && i < 8; i++) ls[i] = l;
    MTLRasterizationRateMapDescriptor *md = [MTLRasterizationRateMapDescriptor
        rasterizationRateMapDescriptorWithScreenSize:MTLSizeMake(w, h, 0) layerCount:layers layers:ls];
    id<MTLRasterizationRateMap> map = [dev newRasterizationRateMapWithDescriptor:md];
    if (!map) return nil;
    for (int i = 0; i < 8; i++)
        if (!g_vrr_maps[i].map) {
            g_vrr_maps[i].w = w; g_vrr_maps[i].h = h; g_vrr_maps[i].layers = layers;
            g_vrr_maps[i].map = (void *)CFBridgingRetain(map);
            // KL_VRR_DUMP=<file>: the map's logical->physical mapping along each axis (it is separable), for an
            // offline unwarp of a snapshot: "x <logical> <physical>" and "y <logical> <physical>" lines.
            const char *dump = getenv("KL_VRR_DUMP");
            if (dump && *dump) {
                FILE *df = fopen(dump, "w");
                if (df) {
                    fprintf(df, "screen %lu %lu\n", (unsigned long)w, (unsigned long)h);
                    for (NSUInteger x = 0; x <= w; x++) {
                        MTLCoordinate2D c = [map mapScreenToPhysicalCoordinates:MTLCoordinate2DMake((float)x, (float)h / 2) forLayer:0];
                        fprintf(df, "x %lu %.4f\n", (unsigned long)x, c.x);
                    }
                    for (NSUInteger y = 0; y <= h; y++) {
                        MTLCoordinate2D c = [map mapScreenToPhysicalCoordinates:MTLCoordinate2DMake((float)w / 2, (float)y) forLayer:0];
                        fprintf(df, "y %lu %.4f\n", (unsigned long)y, c.y);
                    }
                    fclose(df);
                }
            }
            MTLSize ps = [map physicalSizeForLayer:0];
            fprintf(stderr, "[vrr] rate map %lux%lu x%lu layers, q %.2f: physical %lux%lu (%.0f%% of the pixels)\n",
                    (unsigned long)w, (unsigned long)h, (unsigned long)layers, q, (unsigned long)ps.width,
                    (unsigned long)ps.height, 100.0 * ps.width * ps.height / ((double)w * h));
            break;
        }
    return map;
}

// ---- Per-pass GPU time (KL_GPU_PASSES=<first frame>; a Mac/diagnostic tool) --
// For KL_GPU_PASSES_N frames (default 60) from frame KL_GPU_PASSES, every render,
// compute and blit encoder on the guest's queue gets Metal stage-boundary
// timestamps (a counter sample buffer on its pass descriptor). Afterwards the
// passes are grouped by what they draw into (the census signature: attachment
// sizes, formats, samples, load/store actions) and listed by GPU ms per frame.
// Encoders overlap on the GPU, so the rows can add up to more than the frame.
enum { kProfPerBuf = 4096, kProfBufs = 8, kProfSamples = kProfPerBuf * kProfBufs };   // Metal: 32 KB per buffer
static int g_prof = -1;
static uint32_t g_prof_from, g_prof_n = 60;
static id<MTLCounterSampleBuffer> g_prof_buf, g_prof_bufs[kProfBufs];
static unsigned g_prof_used;                       // sample pairs handed out
static struct { uint32_t frame; char sig[200]; } *g_prof_meta;
static MTLTimestamp g_prof_cpu0, g_prof_gpu0;
static int g_prof_done;
// KL_GPU_PASSES may list several windows, "4000,t300,t337": a plain number is a guest frame, tN is N seconds after the
// first guest frame (for a scene reached at a known time, e.g. on the headset). Each window reports on its own.
enum { kProfWins = 8 };
static double g_prof_at[kProfWins];               // seconds after the first frame, or -1 for a frame number
static uint32_t g_prof_fr[kProfWins];
static int g_prof_nwin, g_prof_win;
static double g_prof_t0;                          // the first guest frame (CACurrentMediaTime)

static void prof_sig_render(MTLRenderPassDescriptor *d, char *out, size_t n) {
    int u = snprintf(out, n, "R %lux%lu L%lu", (unsigned long)d.renderTargetWidth, (unsigned long)d.renderTargetHeight,
                     (unsigned long)d.renderTargetArrayLength);
    for (int i = 0; i < 8 && u < (int)n - 40; i++) {
        MTLRenderPassColorAttachmentDescriptor *a = d.colorAttachments[i];
        if (!a.texture) continue;
        u += snprintf(out + u, n - u, " c%d %lux%lu f%lu s%lu ld%lu st%lu%s", i, (unsigned long)a.texture.width,
                      (unsigned long)a.texture.height, (unsigned long)a.texture.pixelFormat,
                      (unsigned long)a.texture.sampleCount, (unsigned long)a.loadAction, (unsigned long)a.storeAction,
                      a.resolveTexture ? "+r" : "");
    }
    id<MTLTexture> z = d.depthAttachment.texture;
    if (z && u < (int)n - 40)
        snprintf(out + u, n - u, " z %lux%lu f%lu s%lu ld%lu st%lu%s", (unsigned long)z.width, (unsigned long)z.height,
                 (unsigned long)z.pixelFormat, (unsigned long)z.sampleCount, (unsigned long)d.depthAttachment.loadAction,
                 (unsigned long)d.depthAttachment.storeAction, d.depthAttachment.resolveTexture ? "+r" : "");
}

// A sample pair for an encoder in guest frame f, or -1 (not profiling it).
static int prof_pair(id<MTLCommandBuffer> cb, uint32_t f) {
    if (g_prof <= 0 || g_prof_done || f < g_prof_from || f >= g_prof_from + g_prof_n) return -1;
    if (!g_prof_buf) {
        id<MTLDevice> dev = cb.device;
        if (![dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
            fprintf(stderr, "[passes] no stage-boundary counter sampling on this GPU\n"); g_prof = 0; return -1;
        }
        id<MTLCounterSet> ts = nil;
        for (id<MTLCounterSet> c in dev.counterSets) if ([c.name isEqualToString:MTLCommonCounterSetTimestamp]) ts = c;
        MTLCounterSampleBufferDescriptor *bd = [MTLCounterSampleBufferDescriptor new];
        bd.counterSet = ts; bd.storageMode = MTLStorageModeShared; bd.sampleCount = kProfPerBuf;
        for (int i = 0; i < kProfBufs; i++) {
            NSError *err = nil;
            g_prof_bufs[i] = [dev newCounterSampleBufferWithDescriptor:bd error:&err];
            if (!g_prof_bufs[i]) { fprintf(stderr, "[passes] no counter sample buffer: %s\n", err.localizedDescription.UTF8String); g_prof = 0; return -1; }
        }
        g_prof_buf = g_prof_bufs[0];
        g_prof_meta = calloc(kProfSamples / 2, sizeof *g_prof_meta);
        [dev sampleTimestamps:&g_prof_cpu0 gpuTimestamp:&g_prof_gpu0];
        fprintf(stderr, "[passes] profiling guest frames %u..%u\n", g_prof_from, g_prof_from + g_prof_n - 1);
    }
    if (g_prof_used >= kProfSamples / 2) return -1;
    int k = (int)g_prof_used++;
    g_prof_meta[k].frame = f;
    return k;
}
// Pair k lives in buffer k / (kProfPerBuf / 2), at samples 2 * (k % (kProfPerBuf / 2)) and the one after.
static id<MTLCounterSampleBuffer> prof_buf(int k) { return g_prof_bufs[k / (kProfPerBuf / 2)]; }
static NSUInteger prof_idx(int k) { return 2 * (NSUInteger)(k % (kProfPerBuf / 2)); }

static id (*g_cce)(id, SEL, MTLDispatchType);    // -computeCommandEncoderWithDispatchType:
static id (*g_bce)(id, SEL);                     // -blitCommandEncoder
static uint32_t prof_frame_of(id<MTLCommandBuffer> cb, int *role) {
    void *q = (__bridge void *)[cb commandQueue];
    os_unfair_lock_lock(&g_lk);
    *role = watched(q);
    uint32_t f = g_frame;
    os_unfair_lock_unlock(&g_lk);
    return f;
}
static id kl_cce(id self, SEL sel, MTLDispatchType dt) {
    int role; uint32_t f = prof_frame_of((id<MTLCommandBuffer>)self, &role);
    int k = role == 0 ? prof_pair((id<MTLCommandBuffer>)self, f) : -1;
    if (k < 0) return g_cce(self, sel, dt);
    snprintf(g_prof_meta[k].sig, sizeof g_prof_meta[k].sig, "C compute");
    MTLComputePassDescriptor *d = [MTLComputePassDescriptor computePassDescriptor];
    d.dispatchType = dt;
    d.sampleBufferAttachments[0].sampleBuffer = prof_buf(k);
    d.sampleBufferAttachments[0].startOfEncoderSampleIndex = prof_idx(k);
    d.sampleBufferAttachments[0].endOfEncoderSampleIndex = prof_idx(k) + 1;
    return [(id<MTLCommandBuffer>)self computeCommandEncoderWithDescriptor:d];
}
static id kl_bce(id self, SEL sel) {
    int role; uint32_t f = prof_frame_of((id<MTLCommandBuffer>)self, &role);
    int k = role == 0 ? prof_pair((id<MTLCommandBuffer>)self, f) : -1;
    if (k < 0) return g_bce(self, sel);
    snprintf(g_prof_meta[k].sig, sizeof g_prof_meta[k].sig, "B blit");
    MTLBlitPassDescriptor *d = [MTLBlitPassDescriptor blitPassDescriptor];
    d.sampleBufferAttachments[0].sampleBuffer = prof_buf(k);
    d.sampleBufferAttachments[0].startOfEncoderSampleIndex = prof_idx(k);
    d.sampleBufferAttachments[0].endOfEncoderSampleIndex = prof_idx(k) + 1;
    return [(id<MTLCommandBuffer>)self blitCommandEncoderWithDescriptor:d];
}

// After the window's frames are final: resolve, group, print.
static void prof_report(void) {
    if (g_prof <= 0 || g_prof_done || !g_prof_buf || g_prof_from == UINT32_MAX || g_frame < g_prof_from + g_prof_n + 3) return;
    g_prof_done = 1;
    unsigned pairs = g_prof_used;
    // Resolve every buffer that was handed pairs into one array, pair k at [2k], [2k+1].
    MTLCounterResultTimestamp *r = calloc(2 * (size_t)pairs + 2, sizeof *r);
    NSData *data = r ? [NSData dataWithBytesNoCopy:r length:(2 * (size_t)pairs + 2) * sizeof *r freeWhenDone:YES] : nil;
    for (unsigned k0 = 0; r && k0 < pairs; k0 += kProfPerBuf / 2) {
        unsigned nk = pairs - k0 < kProfPerBuf / 2 ? pairs - k0 : kProfPerBuf / 2;
        NSData *part = [prof_buf((int)k0) resolveCounterRange:NSMakeRange(0, 2 * nk)];
        if (part) memcpy(r + 2 * k0, part.bytes, part.length < 2 * nk * sizeof *r ? part.length : 2 * nk * sizeof *r);
    }
    MTLTimestamp cpu1, gpu1;
    [g_prof_buf.device sampleTimestamps:&cpu1 gpuTimestamp:&gpu1];
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double cpu_ns = (double)(cpu1 - g_prof_cpu0) * tb.numer / tb.denom;
    double ns_per_tick = gpu1 > g_prof_gpu0 && cpu_ns > 0 ? cpu_ns / (double)(gpu1 - g_prof_gpu0) : 1.0;
    // Exclusive time: stage-boundary spans include waiting behind earlier passes, so each pass is
    // credited only with how far it pushes the frame's end past everything encoded before it
    // (the passes of a frame, in encoding order). The sum is the frame's busy time.
    enum { kMax = 256 };
    static struct { char sig[200]; double t; unsigned n; } row[kMax];
    int rows = 0; unsigned bad = 0;
    double total = 0;
    uint32_t cur = UINT32_MAX; uint64_t hi = 0;
    // One frame in encoding order (the window's second frame): what each encoder draws into, its span
    // and its exclusive time, to see what sits between the scene passes.
    {
        uint32_t fd = g_prof_from + 1; uint64_t h2 = 0, f0 = 0; int idx = 0;
        for (unsigned k = 0; k < pairs && data; k++) {
            if (g_prof_meta[k].frame != fd) continue;
            uint64_t a = r[2 * k].timestamp, b = r[2 * k + 1].timestamp;
            if (a == MTLCounterErrorValue || b == MTLCounterErrorValue || !a || b < a) {
                fprintf(stderr, "[passes] f%u #%02d  (no timestamps)  %s\n", fd, idx++, g_prof_meta[k].sig); continue;
            }
            if (!f0) f0 = a;
            uint64_t from = a > h2 ? a : h2;
            fprintf(stderr, "[passes] f%u #%02d at %7.3f span %6.3f excl %6.3f ms  %s\n", fd, idx++, (a - f0) / 1e6,
                    (b - a) / 1e6, b > from ? (b - from) / 1e6 : 0.0, g_prof_meta[k].sig);
            if (b > h2) h2 = b;
        }
    }
    static double vtx[kMax], frg[kMax];
    memset(vtx, 0, sizeof vtx); memset(frg, 0, sizeof frg);
    for (unsigned k = 0; k < pairs && data; k++) {
        uint64_t a = r[2 * k].timestamp, b = r[2 * k + 1].timestamp;
        if (g_prof_meta[k].sig[0] == '~') continue;
        if (g_prof_meta[k].frame != cur) { cur = g_prof_meta[k].frame; hi = 0; }
        if (a == MTLCounterErrorValue || b == MTLCounterErrorValue || !a || b < a) { bad++; continue; }
        // the stages pair right after a render pass: vertex end, fragment start
        double vs = 0, fs = 0;
        if (k + 1 < pairs && g_prof_meta[k + 1].sig[0] == '~') {
            uint64_t ve = r[2 * (k + 1)].timestamp, fb = r[2 * (k + 1) + 1].timestamp;
            if (ve != MTLCounterErrorValue && ve >= a && ve) vs = (double)(ve - a);
            if (fb != MTLCounterErrorValue && fb && b >= fb) fs = (double)(b - fb);
        }
        uint64_t from = a > hi ? a : hi;
        double t = b > from ? (double)(b - from) : 0;
        if (b > hi) hi = b;
        total += t;
        int j = 0;
        while (j < rows && strcmp(row[j].sig, g_prof_meta[k].sig)) j++;
        if (j == rows) { if (rows == kMax) continue; strcpy(row[rows].sig, g_prof_meta[k].sig); row[rows].t = 0; row[rows].n = 0; rows++; }
        row[j].t += t; row[j].n++;
        vtx[j] += vs; frg[j] += fs;
    }
    // Units: Apple GPUs report these counters in ns, although sampleTimestamps' GPU clock ticks at
    // 24 MHz; take whichever puts the frame's busy time in a sane range (0.2..40 ms).
    double per_frame = total / g_prof_n, scale = 1e-6;          // raw ns -> ms
    if (per_frame * scale < 0.2 || per_frame * scale > 40) scale = ns_per_tick * 1e-6;
    for (int i = 0; i < rows; i++) for (int j = i + 1; j < rows; j++)
        if (row[j].t > row[i].t) {
            __typeof__(row[0]) tmp = row[i]; row[i] = row[j]; row[j] = tmp;
            double x = vtx[i]; vtx[i] = vtx[j]; vtx[j] = x; x = frg[i]; frg[i] = frg[j]; frg[j] = x;
        }
    fprintf(stderr, "[passes] %u encoders over %u frames (%u without timestamps): %.2f GPU ms per frame busy "
            "(units: %s); exclusive time by what they draw into:\n", pairs, g_prof_n, bad, per_frame * scale,
            scale == 1e-6 ? "ns" : "GPU ticks");
    for (int i = 0; i < rows; i++)
        fprintf(stderr, "[passes] %6.3f ms/frame %5.1f%%  x%-5.1f vtx %6.3f frag %6.3f  %s\n", row[i].t * scale / g_prof_n,
                total > 0 ? 100.0 * row[i].t / total : 0, (double)row[i].n / g_prof_n, vtx[i] * scale / g_prof_n,
                frg[i] * scale / g_prof_n, row[i].sig);
    if (++g_prof_win < g_prof_nwin) {             // the next window reuses the sample buffers
        g_prof_used = 0;
        g_prof_from = g_prof_at[g_prof_win] >= 0 ? UINT32_MAX : g_prof_fr[g_prof_win];
        g_prof_done = 0;
        if (g_prof_from != UINT32_MAX) fprintf(stderr, "[passes] profiling guest frames %u..%u\n", g_prof_from, g_prof_from + g_prof_n - 1);
    }
}


// ---- Merging suspended/resumed scene passes (KL_VK_MERGE=1; experimental) -----
// Source 2 records its stereo scene pass as one Vulkan render pass instance split across command
// buffers with dynamic-rendering suspend/resume. MoltenVK ends the Metal encoder at every
// suspension, storing and resolving the full MSAA colour and depth, and loads them again at the
// resume. Per Vulkan the chunks are one render pass instance (an intermediate resolve is not
// observable), so here the Metal encoder is kept open across them: MoltenVK's endEncoding on an MSAA
// encoder is deferred, and when the next render pass on the same command buffer loads exactly the
// same attachments the same encoder is handed back. Its store actions are cached and applied at the
// real end, which comes before any other encoder, event or commit on that command buffer.
typedef struct {
    void *enc, *cb;                      // retained encoder; its command buffer (not retained)
    void *tex[10], *res[10];             // colour 0..7, depth, stencil attachments and resolve targets
    NSUInteger slice[10], level[10], w, h, layers;
    void *vis;                           // visibility result buffer (occlusion queries); must be the same
    int held;                            // MoltenVK has ended it; the real end is pending
    int st_set[10]; MTLStoreAction st[10];
    int opt_set[10]; MTLStoreActionOptions opt[10];
} kl_merge_t;
enum { kMergeMax = 16 };
static kl_merge_t g_mg[kMergeMax];
static os_unfair_lock g_mg_lk = OS_UNFAIR_LOCK_INIT;
static int g_merge = -1;
static unsigned g_merged, g_flushed;
static IMP g_end, g_scsa, g_sdsa, g_sssa, g_scso, g_sdso, g_ssso;
static IMP g_m_cce, g_m_bce, g_m_bced, g_m_cced, g_m_sig, g_m_wait;

static void mg_fill(kl_merge_t *m, MTLRenderPassDescriptor *d) {
    for (int i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *a = d.colorAttachments[i];
        m->tex[i] = (__bridge void *)a.texture; m->res[i] = (__bridge void *)a.resolveTexture;
        m->slice[i] = a.slice; m->level[i] = a.level;
    }
    m->tex[8] = (__bridge void *)d.depthAttachment.texture; m->res[8] = (__bridge void *)d.depthAttachment.resolveTexture;
    m->slice[8] = d.depthAttachment.slice; m->level[8] = d.depthAttachment.level;
    m->tex[9] = (__bridge void *)d.stencilAttachment.texture; m->res[9] = (__bridge void *)d.stencilAttachment.resolveTexture;
    m->slice[9] = d.stencilAttachment.slice; m->level[9] = d.stencilAttachment.level;
    m->w = d.renderTargetWidth; m->h = d.renderTargetHeight; m->layers = d.renderTargetArrayLength;
    m->vis = (__bridge void *)d.visibilityResultBuffer;
}
// KL_VK_MSAA_MEMORYLESS (kl_vulkan.c): a memoryless attachment lives only inside one encoder, so no encoder may load
// or store one. A resume chunk that did not merge into the held encoder would load it: that is counted ("unmerged
// load", the number that must stay zero) and made legal (DontCare), and a store is turned into DontCare or a plain
// resolve, so Metal never sees an illegal action. [ml] lines report the counts every 2000 memoryless encoders.
static _Atomic unsigned g_ml_enc, g_ml_loads, g_ml_stores;
static int ml_is(void *tex) { return tex && ((__bridge id<MTLTexture>)tex).storageMode == MTLStorageModeMemoryless; }
static MTLStoreAction ml_store(MTLStoreAction a) {
    if (a == MTLStoreActionStore) { atomic_fetch_add(&g_ml_stores, 1); return MTLStoreActionDontCare; }
    if (a == MTLStoreActionStoreAndMultisampleResolve) { atomic_fetch_add(&g_ml_stores, 1); return MTLStoreActionMultisampleResolve; }
    return a;
}
// A pass that loads any attachment is a resume (MoltenVK turned the memoryless ones' Load into DontCare).
static int ml_resume(MTLRenderPassDescriptor *d) {
    for (int i = 0; i < 8; i++) if (d.colorAttachments[i].texture && d.colorAttachments[i].loadAction == MTLLoadActionLoad) return 1;
    if (d.depthAttachment.texture && d.depthAttachment.loadAction == MTLLoadActionLoad) return 1;
    return 0;
}
static void ml_guard(MTLRenderPassDescriptor *d) {
    if (!d) return;
    int any = 0;
    for (int i = 0; i < 10; i++) {
        MTLRenderPassAttachmentDescriptor *a = i < 8 ? (MTLRenderPassAttachmentDescriptor *)d.colorAttachments[i]
                                             : i == 8 ? (MTLRenderPassAttachmentDescriptor *)d.depthAttachment
                                                      : (MTLRenderPassAttachmentDescriptor *)d.stencilAttachment;
        if (!ml_is((__bridge void *)a.texture)) continue;
        any = 1;
        if (a.loadAction == MTLLoadActionLoad || (a.loadAction == MTLLoadActionDontCare && ml_resume(d))) {
            unsigned n = atomic_fetch_add(&g_ml_loads, 1) + 1;
            if (n <= 8) fprintf(stderr, "[ml] UNMERGED load of memoryless attachment %d (%lux%lu) -> DontCare (#%u)\n", i,
                                (unsigned long)a.texture.width, (unsigned long)a.texture.height, n);
            a.loadAction = MTLLoadActionDontCare;
        }
        a.storeAction = ml_store(a.storeAction);
    }
    if (any) {
        unsigned n = atomic_fetch_add(&g_ml_enc, 1) + 1;
        if (n == 1 || n % 2000 == 0)
            fprintf(stderr, "[ml] %u memoryless MSAA encoders, %u unmerged loads, %u stores dropped\n", n,
                    atomic_load(&g_ml_loads), atomic_load(&g_ml_stores));
    }
}

// A pass that only continues the held one: the same attachments, every one of them loaded.
static int mg_continues(const kl_merge_t *m, MTLRenderPassDescriptor *d) {
    kl_merge_t n; memset(&n, 0, sizeof n); mg_fill(&n, d);
    for (int i = 0; i < 10; i++)
        if (n.tex[i] != m->tex[i] || n.res[i] != m->res[i] || n.slice[i] != m->slice[i] || n.level[i] != m->level[i]) return 0;
    if (n.w != m->w || n.h != m->h || n.layers != m->layers || n.vis != m->vis || d.rasterizationRateMap) return 0;
    // A memoryless attachment (KL_VK_MSAA_MEMORYLESS) is never loaded: MoltenVK turns a resume's Load into DontCare
    // for it, so DontCare there also continues the held pass (a Clear still starts a new one).
    #define MG_LOADS(a) ((a).loadAction == MTLLoadActionLoad || \
                         ((a).loadAction == MTLLoadActionDontCare && ml_is((__bridge void *)(a).texture)))
    for (int i = 0; i < 8; i++) if (d.colorAttachments[i].texture && !MG_LOADS(d.colorAttachments[i])) return 0;
    if (d.depthAttachment.texture && !MG_LOADS(d.depthAttachment)) return 0;
    if (d.stencilAttachment.texture && !MG_LOADS(d.stencilAttachment)) return 0;
    #undef MG_LOADS
    return 1;
}
static kl_merge_t *mg_of_enc(void *e) { for (int i = 0; i < kMergeMax; i++) if (g_mg[i].enc == e) return &g_mg[i]; return NULL; }
static kl_merge_t *mg_held_on(void *cb) { for (int i = 0; i < kMergeMax; i++) if (g_mg[i].enc && g_mg[i].cb == cb && g_mg[i].held) return &g_mg[i]; return NULL; }

// The real end of a held encoder: its latest store actions, then endEncoding. Call without g_mg_lk.
static void mg_finish(kl_merge_t c) {
    id enc = (__bridge id)c.enc;
    for (int i = 0; i < 10; i++) if (c.st_set[i] && ml_is(c.tex[i])) c.st[i] = ml_store(c.st[i]);
    for (int i = 0; i < 8; i++) {
        if (c.st_set[i]) ((void (*)(id, SEL, MTLStoreAction, NSUInteger))g_scsa)(enc, @selector(setColorStoreAction:atIndex:), c.st[i], (NSUInteger)i);
        if (c.opt_set[i] && g_scso) ((void (*)(id, SEL, MTLStoreActionOptions, NSUInteger))g_scso)(enc, @selector(setColorStoreActionOptions:atIndex:), c.opt[i], (NSUInteger)i);
    }
    if (c.st_set[8]) ((void (*)(id, SEL, MTLStoreAction))g_sdsa)(enc, @selector(setDepthStoreAction:), c.st[8]);
    if (c.opt_set[8] && g_sdso) ((void (*)(id, SEL, MTLStoreActionOptions))g_sdso)(enc, @selector(setDepthStoreActionOptions:), c.opt[8]);
    if (c.st_set[9]) ((void (*)(id, SEL, MTLStoreAction))g_sssa)(enc, @selector(setStencilStoreAction:), c.st[9]);
    if (c.opt_set[9] && g_ssso) ((void (*)(id, SEL, MTLStoreActionOptions))g_ssso)(enc, @selector(setStencilStoreActionOptions:), c.opt[9]);
    ((void (*)(id, SEL))g_end)(enc, @selector(endEncoding));
    CFRelease(c.enc);
}
// End whatever is held on this command buffer (before any other use of it). `why` names the call that needs it ended;
// for a held pass with a memoryless attachment the first few are logged ([ml] flush), because a resume after one of
// these cannot continue that pass.
static void mg_flush_why(void *cb, const char *why) {
    if (g_merge <= 0) return;
    os_unfair_lock_lock(&g_mg_lk);
    kl_merge_t *m = mg_held_on(cb), c;
    if (m) { c = *m; memset(m, 0, sizeof *m); g_flushed++; }
    os_unfair_lock_unlock(&g_mg_lk);
    if (m) {
        int ml = 0; for (int i = 0; i < 10; i++) if (ml_is(c.tex[i])) ml = 1;
        static _Atomic unsigned said;
        if (ml && why && atomic_fetch_add(&said, 1) < 12) {
            fprintf(stderr, "[ml] flush of a held memoryless pass by %s\n", why);
            static _Atomic unsigned bt;
            if (atomic_fetch_add(&bt, 1) < 4) {   // who asked for it (MoltenVK's command), the first few
                void *fr[24]; int n = backtrace(fr, 24); char **sy = backtrace_symbols(fr, n);
                for (int k = 1; k < n && sy; k++) fprintf(stderr, "[ml]   %s\n", sy[k]);
                free(sy);
            }
        }
        mg_finish(c);
    }
}
static void mg_flush(void *cb) { mg_flush_why(cb, NULL); }   // commit: the pass's normal end
// For the guest-stall dump (kl_app.c): how many merged scene encoders are held open right now, and the counters.
int kl_merge_held(unsigned *merged, unsigned *flushed) {
    int n = 0;
    os_unfair_lock_lock(&g_mg_lk);
    for (int i = 0; i < kMergeMax; i++) if (g_mg[i].enc && g_mg[i].held) n++;
    os_unfair_lock_unlock(&g_mg_lk);
    if (merged) *merged = g_merged;
    if (flushed) *flushed = g_flushed;
    return n;
}

static void kl_end(id self, SEL sel) {
    if (g_merge > 0) {
        os_unfair_lock_lock(&g_mg_lk);
        kl_merge_t *m = mg_of_enc((__bridge void *)self);
        if (m && !m->held) { m->held = 1; os_unfair_lock_unlock(&g_mg_lk); return; }
        os_unfair_lock_unlock(&g_mg_lk);
    }
    ((void (*)(id, SEL))g_end)(self, sel);
}
#define MG_CACHE(idx, field, flag, val) do { \
    if (g_merge > 0) { \
        os_unfair_lock_lock(&g_mg_lk); \
        kl_merge_t *m = mg_of_enc((__bridge void *)self); \
        if (m) { m->field[idx] = (val); m->flag[idx] = 1; os_unfair_lock_unlock(&g_mg_lk); return; } \
        os_unfair_lock_unlock(&g_mg_lk); \
    } } while (0)
static void kl_scsa(id self, SEL sel, MTLStoreAction a, NSUInteger i) {
    if (i < 8) MG_CACHE(i, st, st_set, a);
    ((void (*)(id, SEL, MTLStoreAction, NSUInteger))g_scsa)(self, sel, a, i);
}
static void kl_sdsa(id self, SEL sel, MTLStoreAction a) { MG_CACHE(8, st, st_set, a); ((void (*)(id, SEL, MTLStoreAction))g_sdsa)(self, sel, a); }
static void kl_sssa(id self, SEL sel, MTLStoreAction a) { MG_CACHE(9, st, st_set, a); ((void (*)(id, SEL, MTLStoreAction))g_sssa)(self, sel, a); }
static void kl_scso(id self, SEL sel, MTLStoreActionOptions o, NSUInteger i) {
    if (i < 8) MG_CACHE(i, opt, opt_set, o);
    ((void (*)(id, SEL, MTLStoreActionOptions, NSUInteger))g_scso)(self, sel, o, i);
}
static void kl_sdso(id self, SEL sel, MTLStoreActionOptions o) { MG_CACHE(8, opt, opt_set, o); ((void (*)(id, SEL, MTLStoreActionOptions))g_sdso)(self, sel, o); }
static void kl_ssso(id self, SEL sel, MTLStoreActionOptions o) { MG_CACHE(9, opt, opt_set, o); ((void (*)(id, SEL, MTLStoreActionOptions))g_ssso)(self, sel, o); }

// Everything else a command buffer can do ends the held encoder first.
static id kl_m_cce(id self, SEL sel, MTLDispatchType t) { mg_flush_why((__bridge void *)self, "computeCommandEncoder"); return ((id (*)(id, SEL, MTLDispatchType))g_m_cce)(self, sel, t); }
static id kl_m_bce(id self, SEL sel) { mg_flush_why((__bridge void *)self, "blitCommandEncoder"); return ((id (*)(id, SEL))g_m_bce)(self, sel); }
static id kl_m_bced(id self, SEL sel, id d) { mg_flush_why((__bridge void *)self, "blitCommandEncoderWithDescriptor"); return ((id (*)(id, SEL, id))g_m_bced)(self, sel, d); }
static id kl_m_cced(id self, SEL sel, id d) { mg_flush_why((__bridge void *)self, "computeCommandEncoderWithDescriptor"); return ((id (*)(id, SEL, id))g_m_cced)(self, sel, d); }
static void kl_m_sig(id self, SEL sel, id ev, uint64_t v) { mg_flush_why((__bridge void *)self, "encodeSignalEvent"); ((void (*)(id, SEL, id, uint64_t))g_m_sig)(self, sel, ev, v); }
static void kl_m_wait(id self, SEL sel, id ev, uint64_t v) { mg_flush_why((__bridge void *)self, "encodeWaitForEvent"); ((void (*)(id, SEL, id, uint64_t))g_m_wait)(self, sel, ev, v); }

static void hook(Class c, SEL s, IMP mine, IMP *orig);
// ---- Foveated scene passes (KL_VRR=1) ------------------------------------------
// The spike above, made the real thing. Measured on the Mac (2026-09-29): a rate map on the scene
// passes alone is enough, because Source 2's post chain (bloom, the output pass) simply carries the
// squeezed ("physical") layout through to the eye image, and the compositor unwarps it with the same
// map (kl_glfb_eye_rate_map -> the unwarp grid). Offline unwarp against a normal frame: 0.47/255 mean
// difference; the lighting is placed right because nothing in the scene shaders keys on screen position.
// Each eye gets its own layer, full rate around its OWN optical axis (the frustum is asymmetric, so the
// axis is off-centre: ~63% across for the left eye, ~37% for the right), falling off to KL_VRR_EDGE
// (default 0.35) at the far edge. KL_VRR_INNER / KL_VRR_OUTER: where the fall-off starts and ends, as
// fractions of the image width from the axis (defaults 0.18 and 0.50).
enum { kVrrZones = 16 };
extern void kl_ovrp_eye_view(int eye, float *px, float *py, float *pz, float *qx, float *qy, float *qz, float *qw, float tan4[4]);
static int g_vrr_on = -1;
static float vrr_env(const char *n, float d) { const char *e = getenv(n); return e && *e ? (float)atof(e) : d; }
// Rate per zone along one axis: 1 within `in` (a fraction of the image) of the axis, falling linearly to `edge` at `out`.
static void vrr_rates(float centre, float in, float outr, float edge, float *out) {
    for (int i = 0; i < kVrrZones; i++) {
        float p = ((float)i + 0.5f) / kVrrZones, t = fabsf(p - centre);
        out[i] = t <= in ? 1.0f : t >= outr ? edge : 1.0f - (1.0f - edge) * (t - in) / (outr - in);
    }
}
// KL_VRR_MAP: "centre" (default) keeps full rate within +-20 degrees of each eye's axis (about Apple's own fixed
// foveation region), converted to image fractions from the eye's tangents, then falls to 0.25 over the next 12%
// of the image; "soft" is the first map (full rate within 18% of the width, 0.35 from 50% out). KL_VRR_EDGE,
// KL_VRR_INNER, KL_VRR_OUTER override either.
static int vrr_soft(void) { const char *m = getenv("KL_VRR_MAP"); return m && !strcmp(m, "soft"); }
static id<MTLRasterizationRateMap> vrr_fov_map(id<MTLDevice> dev, NSUInteger w, NSUInteger h, NSUInteger layers) {
    for (int i = 0; i < 8; i++)
        if (g_vrr_maps[i].map && g_vrr_maps[i].w == w && g_vrr_maps[i].h == h && g_vrr_maps[i].layers == layers)
            return (__bridge id<MTLRasterizationRateMap>)g_vrr_maps[i].map;
    MTLRasterizationRateLayerDescriptor *ls[2];
    float cx[2] = { 0.5f, 0.5f }, cy = 0.5f, span_x = 2.732f, span_y = 2.192f;   // headset-like defaults
    for (int e = 0; e < 2; e++) {
        float p[3], q[4], t[4] = { 0, 0, 0, 0 };   // tangents: left, right, up, down
        kl_ovrp_eye_view(e, &p[0], &p[1], &p[2], &q[0], &q[1], &q[2], &q[3], t);
        if (t[0] > 0 && t[1] > 0) { cx[e] = t[0] / (t[0] + t[1]); span_x = t[0] + t[1]; }
        if (e == 0 && t[2] > 0 && t[3] > 0) { cy = t[2] / (t[2] + t[3]); span_y = t[2] + t[3]; }
    }
    int soft = vrr_soft();
    float tan20 = 0.364f;
    float in_x = vrr_env("KL_VRR_INNER", soft ? 0.18f : tan20 / span_x);
    float in_y = vrr_env("KL_VRR_INNER", soft ? 0.18f : tan20 / span_y);
    float out_x = vrr_env("KL_VRR_OUTER", soft ? 0.50f : in_x + 0.12f);
    float out_y = vrr_env("KL_VRR_OUTER", soft ? 0.50f : in_y + 0.12f);
    float edge = vrr_env("KL_VRR_EDGE", soft ? 0.35f : 0.25f);
    float ry[kVrrZones];
    vrr_rates(cy, in_y, out_y, edge, ry);
    for (NSUInteger l = 0; l < layers && l < 2; l++) {
        float rx[kVrrZones];
        vrr_rates(cx[layers > 1 ? l : 0], in_x, out_x, edge, rx);
        ls[l] = [[MTLRasterizationRateLayerDescriptor alloc] initWithSampleCount:MTLSizeMake(kVrrZones, kVrrZones, 0)
                                                                     horizontal:rx vertical:ry];
    }
    MTLRasterizationRateMapDescriptor *md = [MTLRasterizationRateMapDescriptor
        rasterizationRateMapDescriptorWithScreenSize:MTLSizeMake(w, h, 0) layerCount:(layers > 1 ? 2 : 1) layers:ls];
    id<MTLRasterizationRateMap> map = [dev newRasterizationRateMapWithDescriptor:md];
    if (!map) return nil;
    for (int i = 0; i < 8; i++)
        if (!g_vrr_maps[i].map) {
            g_vrr_maps[i].w = w; g_vrr_maps[i].h = h; g_vrr_maps[i].layers = layers;
            g_vrr_maps[i].map = (void *)CFBridgingRetain(map);
            break;
        }
    MTLSize p0 = [map physicalSizeForLayer:0];
    fprintf(stderr, "[vrr] foveated map (%s) %lux%lu: axis at %.0f%% / %.0f%% across, %.0f%% down; full rate within %.0f%%/%.0f%%, "
            "edge %.2f; physical %lux%lu (%.0f%% of the pixels)\n", soft ? "soft" : "centre",
            (unsigned long)w, (unsigned long)h, 100 * cx[0], 100 * cx[1], 100 * cy, 100 * in_x, 100 * in_y, edge, (unsigned long)p0.width,
            (unsigned long)p0.height, 100.0 * p0.width * p0.height / ((double)w * h));
    const char *dump = getenv("KL_VRR_DUMP");
    if (dump && *dump) {
        FILE *df = fopen(dump, "w");
        if (df) {
            fprintf(df, "screen %lu %lu\n", (unsigned long)w, (unsigned long)h);
            for (NSUInteger l = 0; l < (layers > 1 ? 2 : 1); l++) {
                for (NSUInteger x = 0; x <= w; x++)
                    fprintf(df, "x%lu %lu %.4f\n", (unsigned long)l, (unsigned long)x,
                            [map mapScreenToPhysicalCoordinates:MTLCoordinate2DMake((float)x, (float)h / 2) forLayer:l].x);
                for (NSUInteger y = 0; y <= h; y++)
                    fprintf(df, "y%lu %lu %.4f\n", (unsigned long)l, (unsigned long)y,
                            [map mapScreenToPhysicalCoordinates:MTLCoordinate2DMake((float)w / 2, (float)y) forLayer:l].y);
            }
            fclose(df);
        }
    }
    return map;
}

// For the compositor's unwarp grid (KleptonCompositor.unwarpGrid): screen -> physical for one eye of the
// published map, for an eye image stored TOP-DOWN (Vulkan). The grid is built bottom-up and the shader flips
// v afterwards (kl_reproject_v, flipY), so the screen y is flipped on the way in and the physical y
// pre-compensated on the way out: the shader's flip then lands on the physical texel.
typedef struct { void *map; unsigned layer; float scr_h, tex_h; } kl_vrr_ctx;
static kl_vrr_ctx g_vrr_ctx[2];
void kl_vrr_s2p(void *ctx, float sx, float sy, float *px, float *py) {
    kl_vrr_ctx *c = (kl_vrr_ctx *)ctx;
    if (!c || !c->map) { *px = sx; *py = sy; return; }
    id<MTLRasterizationRateMap> m = (__bridge id<MTLRasterizationRateMap>)c->map;
    MTLCoordinate2D p = [m mapScreenToPhysicalCoordinates:MTLCoordinate2DMake(sx, c->scr_h - sy) forLayer:c->layer];
    *px = p.x;
    *py = c->tex_h - p.y;
}
// The foveated map an eye image of this size was rasterized through (NULL if none): the compositor picks by the
// size of the image in hand, so a frame rendered at the previous fidelity level is unwarped with ITS map.
void *kl_vrr_map_for(int w, int h) {
    if (g_vrr_on <= 0) return NULL;
    for (int i = 0; i < 8; i++)
        if (g_vrr_maps[i].map && (int)g_vrr_maps[i].w == w && (int)g_vrr_maps[i].h == h) return g_vrr_maps[i].map;
    return NULL;
}
void *kl_vrr_grid_ctx(int eye, void *map, float tex_h) {
    if (g_vrr_on < 0) { const char *v = getenv("KL_VRR"); g_vrr_on = v && *v == '1'; }
    if (g_vrr_on <= 0 || !map || eye < 0 || eye > 1) return NULL;
    id<MTLRasterizationRateMap> m = (__bridge id<MTLRasterizationRateMap>)map;
    g_vrr_ctx[eye].map = map;
    g_vrr_ctx[eye].layer = m.layerCount > 1 ? (unsigned)eye : 0;
    g_vrr_ctx[eye].scr_h = (float)m.screenSize.height;
    g_vrr_ctx[eye].tex_h = tex_h;
    return &g_vrr_ctx[eye];
}

// KL_VK_MERGE_VIS=1 (with KL_VK_MERGE): the scene's first chunk issues no occlusion queries and so gets no
// visibility-result buffer, while the chunk that resumes it does; Metal fixes that buffer per encoder, so the two
// could not merge and the first had to store its 4x samples for the second to load back. The buffer a resumed MSAA
// scene chunk brought (retained) is attached to the next first chunk on the same attachments up front: with no
// visibility mode set, the first chunk writes nothing to it. A different buffer next time just means no merge.
static int g_merge_vis = -1;
static void *g_vis_tex0, *g_vis_buf;
static unsigned g_vis_given, g_vis_seen;

static id kl_rce(id self, SEL sel, MTLRenderPassDescriptor *d) {
    if (g_merge > 0 && d) {
        if (g_merge_vis < 0) { const char *e = getenv("KL_VK_MERGE_VIS"); g_merge_vis = e && *e == '1';
                               if (g_merge_vis) fprintf(stderr, "[merge] carrying the scene's visibility buffer into its first chunk (KL_VK_MERGE_VIS)\n"); }
        if (g_merge_vis > 0 && !d.visibilityResultBuffer && g_vis_buf && d.colorAttachments[0].texture &&
            d.colorAttachments[0].texture.sampleCount > 1 && (__bridge void *)d.colorAttachments[0].texture == g_vis_tex0) {
            d.visibilityResultBuffer = (__bridge id<MTLBuffer>)g_vis_buf;
            if (g_vis_given++ % 2000 == 0) fprintf(stderr, "[merge] visibility buffer given to %u first chunks (%u seen)\n", g_vis_given, g_vis_seen);
        }
        void *cb = (__bridge void *)self;
        os_unfair_lock_lock(&g_mg_lk);
        kl_merge_t *m = mg_held_on(cb);
        if (m && mg_continues(m, d)) {
            m->held = 0;
            g_merged++;
            id enc = (__bridge id)m->enc;
            os_unfair_lock_unlock(&g_mg_lk);
            if (g_merged % 2000 == 1) fprintf(stderr, "[merge] %u resumed passes merged, %u real ends\n", g_merged, g_flushed);
            return enc;
        }
        if (g_merge_vis > 0 && m && d.visibilityResultBuffer && m->vis != (__bridge void *)d.visibilityResultBuffer &&
            d.colorAttachments[0].texture && d.colorAttachments[0].texture.sampleCount > 1 &&
            (__bridge void *)d.colorAttachments[0].texture == m->tex[0]) {
            void *nb = (__bridge void *)d.visibilityResultBuffer;
            g_vis_seen++;
            if (nb != g_vis_buf) {   // remember it for the next frame's first chunk
                if (g_vis_buf) CFRelease(g_vis_buf);
                g_vis_buf = (void *)CFBridgingRetain(d.visibilityResultBuffer);
                g_vis_tex0 = m->tex[0];
                if (g_vis_seen < 8) fprintf(stderr, "[merge] scene visibility buffer %p (seen %u)\n", nb, g_vis_seen);
            }
        }
        {   // why not (the first few): a held pass on another command buffer, or a different descriptor
            static int said;
            if (said < 6 && d.colorAttachments[0].texture && d.colorAttachments[0].texture.sampleCount > 1) {
                int other = 0;
                for (int i = 0; i < kMergeMax; i++) if (g_mg[i].enc && g_mg[i].held && g_mg[i].cb != cb) other = 1;
                if (m || other) {
                    said++;
                    char why[160] = "";
                    if (m) {
                        kl_merge_t n; memset(&n, 0, sizeof n); mg_fill(&n, d);
                        for (int i = 0; i < 10 && !why[0]; i++)
                            if (n.tex[i] != m->tex[i] || n.res[i] != m->res[i] || n.slice[i] != m->slice[i] || n.level[i] != m->level[i])
                                snprintf(why, sizeof why, "attachment %d: tex %p/%p res %p/%p slice %lu/%lu", i, m->tex[i], n.tex[i],
                                         m->res[i], n.res[i], (unsigned long)m->slice[i], (unsigned long)n.slice[i]);
                        if (!why[0]) snprintf(why, sizeof why, "size %lux%lu/%lux%lu layers %lu/%lu vis %p/%p rrm %p",
                                              (unsigned long)m->w, (unsigned long)m->h, (unsigned long)n.w, (unsigned long)n.h,
                                              (unsigned long)m->layers, (unsigned long)n.layers, m->vis,
                                              (__bridge void *)d.visibilityResultBuffer, (__bridge void *)d.rasterizationRateMap);
                    }
                    fprintf(stderr, "[merge] not merged: %s (load c0 %lu z %lu) %s\n",
                            m ? "held pass on this command buffer does not match" : "held pass is on another command buffer",
                            (unsigned long)d.colorAttachments[0].loadAction, (unsigned long)d.depthAttachment.loadAction, why);
                }
            }
        }
        os_unfair_lock_unlock(&g_mg_lk);
        mg_flush_why(cb, "a render pass that does not continue it");
    }
    if ((g_vrr_mode > 0 || g_vrr_on > 0) && d) {
        id<MTLCommandBuffer> cb = (id<MTLCommandBuffer>)self;
        void *q = (__bridge void *)[cb commandQueue];
        os_unfair_lock_lock(&g_lk);
        int role = watched(q);
        uint32_t f = g_frame;
        os_unfair_lock_unlock(&g_lk);
        if (role == 0 && g_vrr_mode == 1 && f == g_vrr_at) vrr_census(d, f);
        // KL_VRR_SPIKE_TOGGLE=s: inject for s seconds, then leave the passes alone for s seconds, and so
        // on, so one run gives both sides of the A/B on the same scene ([vrr] logs each switch).
        int on = 1;
        if (g_vrr_mode == 2 && g_vrr_toggle > 0) {
            on = ((long)(CACurrentMediaTime() / g_vrr_toggle)) % 2 == 0;
            static int was = -1;
            if (on != was) {
                was = on;
                time_t tt = time(NULL);
                struct tm lt;
                localtime_r(&tt, &lt);
                fprintf(stderr, "[vrr] %02d:%02d:%02d inject %s\n", lt.tm_hour, lt.tm_min, lt.tm_sec, on ? "on" : "off");
            }
        }
        if (role == 0 && g_vrr_on > 0 && !d.rasterizationRateMap) {
            id<MTLTexture> t = d.colorAttachments[0].texture ? d.colorAttachments[0].texture : d.depthAttachment.texture;
            if (t && t.sampleCount >= 4 && t.width >= 1024) {
                NSUInteger layers = d.renderTargetArrayLength > 1 ? d.renderTargetArrayLength : 1;
                NSUInteger w = d.renderTargetWidth ? d.renderTargetWidth : t.width;
                NSUInteger h = d.renderTargetHeight ? d.renderTargetHeight : t.height;
                id<MTLRasterizationRateMap> map = vrr_fov_map(cb.device, w, h, layers);
                if (map) {
                    d.rasterizationRateMap = map;
                    static void *published;
                    if ((__bridge void *)map != published) {   // the compositor unwarps the eye image with it
                        published = (__bridge void *)map;
                        kl_glfb_publish_eye_rate_map(published, kVrrZones, kVrrZones);
                    }
                }
            }
        }
        if (role == 0 && g_vrr_mode == 2 && on && !d.rasterizationRateMap) {
            id<MTLTexture> t = d.colorAttachments[0].texture ? d.colorAttachments[0].texture : d.depthAttachment.texture;
            if (t && t.sampleCount >= 4 && t.width >= 1024) {
                NSUInteger layers = d.renderTargetArrayLength > 1 ? d.renderTargetArrayLength : 1;
                // The map covers the render area (the eye textures are allocated at the largest
                // fidelity level's size and rendered in their top-left corner).
                NSUInteger w = d.renderTargetWidth ? d.renderTargetWidth : t.width;
                NSUInteger h = d.renderTargetHeight ? d.renderTargetHeight : t.height;
                id<MTLRasterizationRateMap> map = vrr_map(cb.device, w, h, layers);
                if (map) {
                    d.rasterizationRateMap = map;
                    if (g_vrr_injected++ % 5000 == 0)
                        fprintf(stderr, "[vrr] %u scene passes given the rate map so far\n", g_vrr_injected);
                }
            }
        }
    }
    if (g_prof > 0 && d) {
        int role; uint32_t f = prof_frame_of((id<MTLCommandBuffer>)self, &role);
        int k = role == 0 ? prof_pair((id<MTLCommandBuffer>)self, f) : -1;
        // A second pair, in the same sample buffer, for the vertex stage's end and the fragment stage's start.
        int k2 = k >= 0 ? prof_pair((id<MTLCommandBuffer>)self, f) : -1;
        if (k >= 0 && k2 >= 0 && prof_buf(k2) != prof_buf(k)) k2 = -1;
        if (k >= 0) {
            prof_sig_render(d, g_prof_meta[k].sig, sizeof g_prof_meta[k].sig);
            d.sampleBufferAttachments[0].sampleBuffer = prof_buf(k);
            d.sampleBufferAttachments[0].startOfVertexSampleIndex = prof_idx(k);
            d.sampleBufferAttachments[0].endOfVertexSampleIndex = k2 >= 0 ? prof_idx(k2) : MTLCounterDontSample;
            d.sampleBufferAttachments[0].startOfFragmentSampleIndex = k2 >= 0 ? prof_idx(k2) + 1 : MTLCounterDontSample;
            d.sampleBufferAttachments[0].endOfFragmentSampleIndex = prof_idx(k) + 1;
            if (k2 >= 0) snprintf(g_prof_meta[k2].sig, sizeof g_prof_meta[k2].sig, "~stages of %d", k);
        }
    }
    ml_guard(d);
    id enc = ((id (*)(id, SEL, MTLRenderPassDescriptor *))g_rce)(self, sel, d);
    if (g_snap_at && !g_snap_done && enc && d) {
        int role; uint32_t f = prof_frame_of((id<MTLCommandBuffer>)self, &role);
        if (role == 0) snap_note((id<MTLCommandBuffer>)self, d, f);
    }
    if (g_merge > 0 && enc && d && d.colorAttachments[0].texture && d.colorAttachments[0].texture.sampleCount > 1) {
        int role; (void)prof_frame_of((id<MTLCommandBuffer>)self, &role);
        if (role == 0) {
            static int hooked;
            if (!hooked) {   // the render encoder class: endEncoding and the store actions
                hooked = 1;
                Class ec = object_getClass(enc);
                hook(ec, @selector(endEncoding), (IMP)kl_end, &g_end);
                hook(ec, @selector(setColorStoreAction:atIndex:), (IMP)kl_scsa, &g_scsa);
                hook(ec, @selector(setDepthStoreAction:), (IMP)kl_sdsa, &g_sdsa);
                hook(ec, @selector(setStencilStoreAction:), (IMP)kl_sssa, &g_sssa);
                hook(ec, @selector(setColorStoreActionOptions:atIndex:), (IMP)kl_scso, &g_scso);
                hook(ec, @selector(setDepthStoreActionOptions:), (IMP)kl_sdso, &g_sdso);
                hook(ec, @selector(setStencilStoreActionOptions:), (IMP)kl_ssso, &g_ssso);
                fprintf(stderr, "[merge] deferring the end of MSAA scene encoders (%s)\n", class_getName(ec));
            }
            os_unfair_lock_lock(&g_mg_lk);
            for (int i = 0; i < kMergeMax; i++)
                if (!g_mg[i].enc) {
                    memset(&g_mg[i], 0, sizeof g_mg[i]);
                    g_mg[i].enc = (void *)CFBridgingRetain(enc);
                    g_mg[i].cb = (__bridge void *)self;
                    mg_fill(&g_mg[i], d);
                    break;
                }
            os_unfair_lock_unlock(&g_mg_lk);
        }
    }
    return enc;
}

static void hook(Class c, SEL s, IMP mine, IMP *orig) {
    Method m = class_getInstanceMethod(c, s);
    if (!m) { fprintf(stderr, "[gputime] %s has no %s\n", class_getName(c), sel_getName(s)); return; }
    *orig = method_getImplementation(m);
    if (!class_addMethod(c, s, mine, method_getTypeEncoding(m))) method_setImplementation(m, mine);
}

static void vrr_install(Class c) {
    const char *vr = getenv("KL_VRR");
    g_vrr_on = vr && *vr == '1';
    if (g_vrr_on) fprintf(stderr, "[vrr] foveated scene passes on (KL_VRR=1)\n");
    const char *sn = getenv("KL_GPU_SNAP");
    if (sn && *sn) {
        g_snap_at = (uint32_t)atoi(sn);
        const char *colon = strchr(sn, ':');
        snprintf(g_snap_path, sizeof g_snap_path, "%s", colon ? colon + 1 : "kl-snap.ppm");
        const char *sn_n = getenv("KL_GPU_SNAP_N"), *sn_e = getenv("KL_GPU_SNAP_EVERY");
        if (sn_n && atoi(sn_n) > 0) g_snap_n = (uint32_t)atoi(sn_n);
        if (sn_e && atoi(sn_e) > 0) g_snap_every = (uint32_t)atoi(sn_e);
    }
    const char *mg = getenv("KL_VK_MERGE");
    g_merge = mg && *mg == '1';
    if (g_merge) {
        hook(c, @selector(computeCommandEncoderWithDispatchType:), (IMP)kl_m_cce, &g_m_cce);
        hook(c, @selector(blitCommandEncoder), (IMP)kl_m_bce, &g_m_bce);
        hook(c, @selector(blitCommandEncoderWithDescriptor:), (IMP)kl_m_bced, &g_m_bced);
        hook(c, @selector(computeCommandEncoderWithDescriptor:), (IMP)kl_m_cced, &g_m_cced);
        hook(c, @selector(encodeSignalEvent:value:), (IMP)kl_m_sig, &g_m_sig);
        hook(c, @selector(encodeWaitForEvent:value:), (IMP)kl_m_wait, &g_m_wait);
        fprintf(stderr, "[merge] on (KL_VK_MERGE)\n");
    }
    const char *pp = getenv("KL_GPU_PASSES");
    g_prof = pp && *pp ? 1 : 0;
    if (g_prof) {
        for (const char *q = pp; *q && g_prof_nwin < kProfWins; ) {
            if (*q == 't') { g_prof_at[g_prof_nwin] = atof(q + 1); g_prof_fr[g_prof_nwin] = 0; }
            else { g_prof_at[g_prof_nwin] = -1; g_prof_fr[g_prof_nwin] = (uint32_t)atoi(q); }
            g_prof_nwin++;
            q = strchr(q, ','); if (!q) break; q++;
        }
        g_prof_from = g_prof_nwin && g_prof_at[0] >= 0 ? UINT32_MAX : g_prof_fr[0];
        const char *pn = getenv("KL_GPU_PASSES_N");
        if (pn && atoi(pn) > 0) g_prof_n = (uint32_t)atoi(pn);
        hook(c, @selector(computeCommandEncoderWithDispatchType:), (IMP)kl_cce, (IMP *)&g_cce);
        hook(c, @selector(blitCommandEncoder), (IMP)kl_bce, (IMP *)&g_bce);
    }
    const char *e = getenv("KL_VRR_SPIKE");
    g_vrr_mode = !e || !*e ? 0 : !strncmp(e, "census", 6) ? 1 : !strncmp(e, "inject", 6) ? 2 : 0;
    if (!g_vrr_mode && !g_prof && !g_merge && !g_snap_at && !g_vrr_on) return;
    if (!g_vrr_mode) { hook(c, @selector(renderCommandEncoderWithDescriptor:), (IMP)kl_rce, &g_rce); return; }
    if (g_vrr_mode == 2 && e[6] == ':') g_vrr_q = (float)atof(e + 7);
    const char *at = getenv("KL_VRR_SPIKE_AT");
    if (at && *at) g_vrr_at = (uint32_t)atoi(at);
    const char *tg = getenv("KL_VRR_SPIKE_TOGGLE");
    if (tg && *tg) g_vrr_toggle = atof(tg);
    SEL s = @selector(renderCommandEncoderWithDescriptor:);
    Method m = class_getInstanceMethod(c, s);
    if (!m) { fprintf(stderr, "[vrr] %s has no -renderCommandEncoderWithDescriptor:\n", class_getName(c)); return; }
    g_rce = method_getImplementation(m);
    if (!class_addMethod(c, s, (IMP)kl_rce, method_getTypeEncoding(m))) method_setImplementation(m, (IMP)kl_rce);
    fprintf(stderr, "[vrr] spike: %s\n", g_vrr_mode == 1 ? "census" : "inject");
}

static void watch(void *mtl_queue, int role) {
    if (!mtl_queue) return;
    const char *e = getenv("KL_GPU_TIME");
    if (e && *e == '0') return;
    os_unfair_lock_lock(&g_lk);
    int have = watched(mtl_queue) >= 0;
    if (!have && g_nqueues < kMaxQueues) { g_roles[g_nqueues] = role; g_queues[g_nqueues++] = mtl_queue; }
    int first = !g_commit;
    os_unfair_lock_unlock(&g_lk);
    if (have || !first) return;
    // The class that actually implements -commit for this device's buffers.
    id<MTLCommandQueue> q = (__bridge id<MTLCommandQueue>)mtl_queue;
    Class c;
    @autoreleasepool { c = object_getClass([q commandBuffer]); }
    Method m = class_getInstanceMethod(c, @selector(commit));
    if (!m) { fprintf(stderr, "[gputime] %s has no -commit; GPU time stays 0\n", class_getName(c)); return; }
    // Add the hook on the concrete class itself (so a superclass shared with
    // other command buffer kinds is left alone), keeping the inherited IMP.
    IMP orig = method_getImplementation(m);
    os_unfair_lock_lock(&g_lk);
    g_commit = orig;
    os_unfair_lock_unlock(&g_lk);
    if (!class_addMethod(c, @selector(commit), (IMP)kl_commit, method_getTypeEncoding(m)))
        method_setImplementation(m, (IMP)kl_commit);
    fprintf(stderr, "[gputime] timing %s -commit (first queue: %s)\n", class_getName(c),
            role ? "the compositor's" : "the guest's");
    vrr_install(c);
}

unsigned kl_gpu_time_frame(void) { return g_commit ? g_frame : 0; }
void kl_gpu_time_watch(void *mtl_queue) { watch(mtl_queue, 0); }
void kl_gpu_time_watch_compositor(void *mtl_queue) { watch(mtl_queue, 1); }

void kl_gpu_time_frame_end(void) {
    os_unfair_lock_lock(&g_lk);
    g_frame++;
    slot_of(g_frame);
    int on = g_commit != NULL;
    os_unfair_lock_unlock(&g_lk);
    if (!on) return;
    if (g_prof > 0) {
        double now = CACurrentMediaTime();
        if (!g_prof_t0) g_prof_t0 = now;
        if (!g_prof_done && g_prof_from == UINT32_MAX && g_prof_win < g_prof_nwin && now - g_prof_t0 >= g_prof_at[g_prof_win]) {
            g_prof_from = g_frame + 1;
            fprintf(stderr, "[passes] window %d at %.1f s after the first guest frame: profiling guest frames %u..%u\n",
                    g_prof_win + 1, now - g_prof_t0, g_prof_from, g_prof_from + g_prof_n - 1);
        }
    }
    if (g_prof > 0 && !g_prof_done) prof_report();
    // Every few seconds: the finished frames' GPU time.
    static unsigned last_f, n;
    static double sum, csum, worst, since;
    unsigned f;
    float comp;
    float ms = kl_gpu_time_frame_ms(0, &comp, &f);
    if (f && f != last_f) { last_f = f; sum += ms; csum += comp; n++; if (ms > worst) worst = ms; }
    // KL_GPU_TIME_LOG: the report interval in seconds (default 5).
    static double every;
    if (!every) { const char *e = getenv("KL_GPU_TIME_LOG"); every = e && atof(e) > 0 ? atof(e) : 5.0; }
    double now = CACurrentMediaTime();
    if (!since) since = now;
    if (now - since >= every) {
        if (n) {
            time_t t = time(NULL);
            struct tm lt;
            localtime_r(&t, &lt);
            fprintf(stderr, "[gputime] %02d:%02d:%02d GPU time per frame: game avg %.2f / worst %.2f ms, compositor avg "
                    "%.2f ms, over %u frames\n", lt.tm_hour, lt.tm_min, lt.tm_sec, sum / n, worst, csum / n, n);
        }
        sum = csum = worst = 0; n = 0; since = now;
    }
}

// The guest's GPU ms (and the compositor's in *compositor) of the latest final
// frame at least `frames_ago` closed frames back (0 = the frame closed most
// recently, or the one before it while its buffers are still in flight).
// Returns 0 with *frame = 0 when nothing is known.
float kl_gpu_time_frame_ms(unsigned frames_ago, float *compositor, unsigned *frame) {
    float ms = 0, cms = 0;
    unsigned got = 0;
    os_unfair_lock_lock(&g_lk);
    for (unsigned back = 1 + frames_ago; back < kRing - 1 && back < g_frame; back++) {
        uint32_t f = g_frame - back;
        slot *s = &g_slots[f % kRing];
        if (s->frame != f) break;
        if (s->committed && s->completed == s->committed) {
            ms = (float)(s->busy[0] * 1e3); cms = (float)(s->busy[1] * 1e3); got = f; break;
        }
    }
    os_unfair_lock_unlock(&g_lk);
    if (frame) *frame = got;
    if (compositor) *compositor = cms;
    return ms;
}

// KL_CP_DEPTH: an R32Float texture VIEW over a linear buffer (the depth staging buffer), so the compositor samples the
// game's depth where the copy left it instead of paying a second copy into an image. Returned retained (+1).
void *kl_mtl_buffer_texture_r32f(void *mtl_buffer, unsigned w, unsigned h, unsigned pitch_bytes) {
    id<MTLBuffer> b = (__bridge id<MTLBuffer>)mtl_buffer;
    if (!b || !w || !h || (NSUInteger)pitch_bytes * h > b.length) return NULL;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                                                                   width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = b.storageMode;
    d.cpuCacheMode = b.cpuCacheMode;
    d.hazardTrackingMode = b.hazardTrackingMode;
    NSUInteger align = [b.device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatR32Float];
    if (align && pitch_bytes % align) return NULL;
    id<MTLTexture> t = [b newTextureWithDescriptor:d offset:0 bytesPerRow:pitch_bytes];
    return t ? (void *)CFBridgingRetain(t) : NULL;
}
void kl_mtl_release(void *obj) { if (obj) CFRelease(obj); }
