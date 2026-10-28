// vxrt-npu: the frame interpolator (and optical flow, for extrapolation) of VXRuntime's NPU mode (VXRT_NPU_INTERP=1, src/npu_interp.m), a native arm64
// process of its own because VideoToolbox's VTLowLatencyFrameInterpolation (macOS 26, on the Neural Engine) reports
// itself unsupported in an x86-64 process under Rosetta, which is what vxrt is.
//
// It makes the frames' buffers (420v IOSurfaces, global so that the runtime can find them by ID) and one frame
// processor session per tile (the processor takes at most about 0.92 Mpx a frame on an M1 Max: larger pictures go in
// tiles), and answers requests on descriptor 3, a socket from the runtime (fixed-size messages, below). It ends when
// the runtime closes the socket.
//
//   vxrt-npu --check [flow]   exit 0 when frame interpolation (optical flow) is supported here
//   vxrt-npu --selftest   interpolates two synthetic frames, checks the middle one
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <CoreVideo/CoreVideo.h>
#include <fcntl.h>
#include <unistd.h>
#include "../src/npu_proto.h"

static int read_all(int fd, void *p, size_t n) {
    for (size_t got = 0; got < n;) {
        ssize_t r = read(fd, (char *)p + got, n - got);
        if (r <= 0) return 0;
        got += (size_t)r;
    }
    return 1;
}
static int write_all(int fd, const void *p, size_t n) {
    for (size_t put = 0; put < n;) {
        ssize_t r = write(fd, (const char *)p + put, n - put);
        if (r <= 0) return 0;
        put += (size_t)r;
    }
    return 1;
}

static CVPixelBufferRef make_buffer(uint32_t w, uint32_t h, OSType format) API_AVAILABLE(macos(26.0)) {
    CVPixelBufferRef pb = NULL;
    // (IOSurfaceIsGlobal, deprecated but the one way to share a surface by ID without a Mach port exchange)
    NSDictionary *attrs = @{ (id)kCVPixelBufferIOSurfacePropertiesKey: @{ @"IOSurfaceIsGlobal": @YES },
                             (id)kCVPixelBufferMetalCompatibilityKey: @YES };
    CVPixelBufferCreate(NULL, w, h, format, (__bridge CFDictionaryRef)attrs, &pb);
    return pb;
}

// (our own messages go to the runtime's log; VideoToolbox's chatter on stderr does not)
static FILE *g_err;

static double now_ms(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }

API_AVAILABLE(macos(26.0))
@interface NpuTiles : NSObject {
@public
    uint32_t tw, th, nt, nsrc, ndst, mode;
    VTFrameProcessor *proc[NPU_MAX_TILES];
    CVPixelBufferRef src[NPU_MAX_SLOTS][NPU_MAX_TILES], dst[NPU_MAX_SLOTS][NPU_MAX_TILES], bwd[NPU_MAX_SLOTS];
}
@end
@implementation NpuTiles
- (void)dealloc {
    for (uint32_t t = 0; t < NPU_MAX_TILES; t++) [proc[t] endSession];
    for (uint32_t s = 0; s < NPU_MAX_SLOTS; s++)
        for (uint32_t t = 0; t < NPU_MAX_TILES; t++) {
            if (src[s][t]) CVPixelBufferRelease(src[s][t]);
            if (dst[s][t]) CVPixelBufferRelease(dst[s][t]);
        }
    for (uint32_t s = 0; s < NPU_MAX_SLOTS; s++) if (bwd[s]) CVPixelBufferRelease(bwd[s]);
}
@end

// The sessions start one after another (about 1.1 s for the first on an M1 Max, ~0.1 s for the next ones).
static NpuTiles *tiles_create(const npu_init_req *q, npu_init_reply *r) API_AVAILABLE(macos(26.0)) {
    NpuTiles *T = [NpuTiles new];
    T->tw = q->tile_w; T->th = q->tile_h; T->nt = q->tiles; T->nsrc = q->src_slots; T->ndst = q->dst_slots;
    double t0 = now_ms();
    for (uint32_t t = 0; t < T->nt; t++) {
        VTLowLatencyFrameInterpolationConfiguration *cfg =
            [[VTLowLatencyFrameInterpolationConfiguration alloc] initWithFrameWidth:T->tw frameHeight:T->th numberOfInterpolatedFrames:1];
        T->proc[t] = [VTFrameProcessor new];
        NSError *err = nil;
        if (!cfg || ![T->proc[t] startSessionWithConfiguration:cfg error:&err]) {
            fprintf(g_err, "vxrt-npu: the frame interpolation session (%ux%u) did not start: %s\n", T->tw, T->th,
                    err.localizedDescription.UTF8String ?: "no configuration");
            T->proc[t] = nil;
            return nil;
        }
    }
    r->start_ms = (float)(now_ms() - t0);
    uint32_t k = 0;
    for (uint32_t s = 0; s < T->nsrc; s++)
        for (uint32_t t = 0; t < T->nt; t++) {
            T->src[s][t] = make_buffer(T->tw, T->th, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
            if (!T->src[s][t] || !CVPixelBufferGetIOSurface(T->src[s][t])) return nil;
            r->ids[k++] = IOSurfaceGetID(CVPixelBufferGetIOSurface(T->src[s][t]));
        }
    for (uint32_t s = 0; s < T->ndst; s++)
        for (uint32_t t = 0; t < T->nt; t++) {
            T->dst[s][t] = make_buffer(T->tw, T->th, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
            if (!T->dst[s][t] || !CVPixelBufferGetIOSurface(T->dst[s][t])) return nil;
            r->ids[k++] = IOSurfaceGetID(CVPixelBufferGetIOSurface(T->dst[s][t]));
        }
    return T;
}

static VTFrameProcessorFrame *frame(CVPixelBufferRef pb, int64_t t) API_AVAILABLE(macos(26.0)) {
    return [[VTFrameProcessorFrame alloc] initWithBuffer:pb presentationTimeStamp:CMTimeMake(t, 1000)];
}

static int flow_run(NpuTiles *T, uint32_t a, uint32_t b, uint32_t d, int64_t pts) API_AVAILABLE(macos(26.0)) {
    VTFrameProcessorOpticalFlow *of = [[VTFrameProcessorOpticalFlow alloc] initWithForwardFlow:T->dst[d][0] backwardFlow:T->bwd[d]];
    VTOpticalFlowParameters *p = of ? [[VTOpticalFlowParameters alloc] initWithSourceFrame:frame(T->src[a][0], pts) nextFrame:frame(T->src[b][0], pts + 1)
                                                                       submissionMode:VTOpticalFlowParametersSubmissionModeSequential
                                                               destinationOpticalFlow:of] : nil;
    if (!p) return -1;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block int failed = 0;
    [T->proc[0] processWithParameters:p completionHandler:^(id<VTFrameProcessorParameters> r, NSError *err) {
        (void)r;
        if (err) { failed = 1; fprintf(g_err, "vxrt-npu: optical flow failed: %s\n", err.localizedDescription.UTF8String); }
        dispatch_semaphore_signal(done);
    }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    return failed ? -2 : 0;
}

// Random 8x8 blocks, moved by (dx, dy): to measure what a flow vector's unit is in frame pixels (it depends on the
// frame size: a quarter pixel up to ~1000 pixels wide, an eighth at 1512).
static void fill_blocks(CVPixelBufferRef pb, int dx, int dy) {
    CVPixelBufferLockBaseAddress(pb, 0);
    __fp16 *b = CVPixelBufferGetBaseAddress(pb);
    size_t stride = CVPixelBufferGetBytesPerRow(pb) / 2, w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb);
    for (size_t y = 0; y < h; y++)
        for (size_t x = 0; x < w; x++) {
            int xs = (int)x - dx, ys = (int)y - dy;
            uint32_t k = (uint32_t)((xs / 8 + 1000) * 73856093u) ^ (uint32_t)((ys / 8 + 1000) * 19349663u);
            float v = 0.1f + 0.8f * (float)((k >> 7) % 1000) / 1000.0f;
            for (int c = 0; c < 4; c++) b[y * stride + 4 * x + c] = (__fp16)(c == 3 ? 1.0f : v);
        }
    CVPixelBufferUnlockBaseAddress(pb, 0);
}

static NpuTiles *flow_create(const npu_init_req *q, npu_init_reply *r) API_AVAILABLE(macos(26.0)) {
    NpuTiles *T = [NpuTiles new];
    T->tw = q->tile_w; T->th = q->tile_h; T->nt = 1; T->nsrc = q->src_slots; T->ndst = q->dst_slots; T->mode = q->mode;
    double t0 = now_ms();
    VTOpticalFlowConfiguration *cfg = [[VTOpticalFlowConfiguration alloc] initWithFrameWidth:T->tw frameHeight:T->th
        qualityPrioritization:VTOpticalFlowConfigurationQualityPrioritizationNormal revision:VTOpticalFlowConfiguration.defaultRevision];
    T->proc[0] = [VTFrameProcessor new];
    NSError *err = nil;
    if (!cfg || ![T->proc[0] startSessionWithConfiguration:cfg error:&err]) {
        fprintf(g_err, "vxrt-npu: the optical flow session (%ux%u) did not start: %s\n", T->tw, T->th, err.localizedDescription.UTF8String ?: "no configuration");
        T->proc[0] = nil;
        return nil;
    }
    NSDictionary *da = cfg.destinationPixelBufferAttributes;
    id f = da[(id)kCVPixelBufferPixelFormatTypeKey];
    OSType ff = [f isKindOfClass:[NSArray class]] ? [f[0] unsignedIntValue] : [f unsignedIntValue];
    r->flow_w = [da[(id)kCVPixelBufferWidthKey] unsignedIntValue];
    r->flow_h = [da[(id)kCVPixelBufferHeightKey] unsignedIntValue];
    if (ff != kCVPixelFormatType_TwoComponent16Half || !r->flow_w || !r->flow_h) {
        fprintf(g_err, "vxrt-npu: unexpected optical flow buffers (%08x %ux%u)\n", (unsigned)ff, r->flow_w, r->flow_h);
        return nil;
    }
    uint32_t k = 0;
    for (uint32_t s = 0; s < T->nsrc; s++) {
        if (!(T->src[s][0] = make_buffer(T->tw, T->th, kCVPixelFormatType_64RGBAHalf))) return nil;
        r->ids[k++] = IOSurfaceGetID(CVPixelBufferGetIOSurface(T->src[s][0]));
    }
    for (uint32_t s = 0; s < T->ndst; s++) {
        if (!(T->dst[s][0] = make_buffer(r->flow_w, r->flow_h, ff))) return nil;
        r->ids[k++] = IOSurfaceGetID(CVPixelBufferGetIOSurface(T->dst[s][0]));
    }
    for (uint32_t s = 0; s < T->ndst; s++) {
        if (!(T->bwd[s] = make_buffer(r->flow_w, r->flow_h, ff))) return nil;
        r->ids[k++] = IOSurfaceGetID(CVPixelBufferGetIOSurface(T->bwd[s]));
    }
    // the unit: a 16 x 8 pixel move, the mean flow in the middle of the frame
    fill_blocks(T->src[0][0], 0, 0);
    fill_blocks(T->src[1][0], 16, 8);
    r->flow_scale = 0;
    if (!flow_run(T, 0, 1, 0, 0)) {
        CVPixelBufferRef fb = T->dst[0][0];
        CVPixelBufferLockBaseAddress(fb, kCVPixelBufferLock_ReadOnly);
        const uint8_t *base = CVPixelBufferGetBaseAddress(fb);
        size_t stride = CVPixelBufferGetBytesPerRow(fb);
        double sum = 0; int n = 0;
        for (uint32_t y = r->flow_h / 4; y < r->flow_h * 3 / 4; y++)
            for (uint32_t x = r->flow_w / 4; x < r->flow_w * 3 / 4; x++) { sum += ((const __fp16 *)(base + y * stride))[2 * x]; n++; }
        CVPixelBufferUnlockBaseAddress(fb, kCVPixelBufferLock_ReadOnly);
        if (n && sum / n > 0.5) r->flow_scale = (float)(16.0 / (sum / n));
    }
    if (!r->flow_scale) { fprintf(g_err, "vxrt-npu: the optical flow's unit could not be measured\n"); return nil; }
    r->start_ms = (float)(now_ms() - t0);
    return T;
}

// The midpoint (phase 0.5) between src slot prev and src slot cur into dst slot dst, every tile; the tiles are handed
// to the processor together (it runs them one after another on the one engine).
static int tiles_process(NpuTiles *T, const npu_proc_req *q) API_AVAILABLE(macos(26.0)) {
    if (q->prev >= T->nsrc || q->cur >= T->nsrc || q->dst >= T->ndst) return -1;
    if (T->mode == NPU_MODE_FLOW) return flow_run(T, q->prev, q->cur, q->dst, (int64_t)q->seq * 2);
    dispatch_group_t g = dispatch_group_create();
    __block int failed = 0;
    int64_t pts = (int64_t)q->seq * 2;
    for (uint32_t t = 0; t < T->nt; t++) {
        VTLowLatencyFrameInterpolationParameters *p =
            [[VTLowLatencyFrameInterpolationParameters alloc] initWithSourceFrame:frame(T->src[q->cur][t], pts + 2)
                                                                  previousFrame:frame(T->src[q->prev][t], pts)
                                                             interpolationPhase:@[ @0.5f ]
                                                              destinationFrames:@[ frame(T->dst[q->dst][t], pts + 1) ]];
        if (!p) { failed = 1; continue; }
        dispatch_group_enter(g);
        // (the completion handler form: the async overload returned at once with nothing written, in Swift)
        [T->proc[t] processWithParameters:p completionHandler:^(id<VTFrameProcessorParameters> done, NSError *err) {
            (void)done;
            if (err) { failed = 1; fprintf(g_err, "vxrt-npu: interpolation failed: %s\n", err.localizedDescription.UTF8String); }
            dispatch_group_leave(g);
        }];
    }
    dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
    return failed ? -2 : 0;
}

static int selftest(void) API_AVAILABLE(macos(26.0)) {
    npu_init_req q = { NPU_OP_INIT, 256, 128, 1, 2, 1, NPU_MODE_INTERPOLATE };
    npu_init_reply r = { 0 };
    NpuTiles *T = tiles_create(&q, &r);
    if (!T) { printf("vxrt-npu selftest: no session\n"); return 1; }
    // two frames of a vertical edge, 16 pixels apart: the middle one should have it near halfway
    for (uint32_t s = 0; s < 2; s++) {
        CVPixelBufferRef pb = T->src[s][0];
        CVPixelBufferLockBaseAddress(pb, 0);
        for (int pl = 0; pl < 2; pl++) {
            uint8_t *b = CVPixelBufferGetBaseAddressOfPlane(pb, pl);
            size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pb, pl), h = CVPixelBufferGetHeightOfPlane(pb, pl),
                   w = CVPixelBufferGetWidthOfPlane(pb, pl) * (pl ? 2 : 1);
            for (size_t y = 0; y < h; y++)
                for (size_t x = 0; x < w; x++) b[y * stride + x] = pl ? 128 : (x < 100 + 16 * s ? 40 : 200);
        }
        CVPixelBufferUnlockBaseAddress(pb, 0);
    }
    npu_proc_req p = { NPU_OP_PROC, 1, 0, 1, 0 };
    if (tiles_process(T, &p)) { printf("vxrt-npu selftest: process failed\n"); return 1; }
    CVPixelBufferRef d = T->dst[0][0];
    CVPixelBufferLockBaseAddress(d, kCVPixelBufferLock_ReadOnly);
    const uint8_t *y = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(d, 0) + 64 * CVPixelBufferGetBytesPerRowOfPlane(d, 0);
    int edge = -1;
    for (int x = 0; x < 256 && edge < 0; x++) if (y[x] >= 120) edge = x;
    CVPixelBufferUnlockBaseAddress(d, kCVPixelBufferLock_ReadOnly);
    printf("vxrt-npu selftest: edge at %d in the middle frame (100 and 116 in the frames around it)\n", edge);
    if (edge < 100 || edge > 116) return 1;
    if (!VTOpticalFlowConfiguration.isSupported) return 0;
    npu_init_req fq = { NPU_OP_INIT, 640, 416, 1, 2, 1, NPU_MODE_FLOW };
    npu_init_reply fr = { 0 };
    if (!flow_create(&fq, &fr)) { printf("vxrt-npu selftest: no optical flow\n"); return 1; }
    printf("vxrt-npu selftest: optical flow %ux%u for 640x416, %.2f pixels a unit\n", fr.flow_w, fr.flow_h, fr.flow_scale);
    return 0;
}

int main(int argc, char **argv) {
    g_err = stderr;
    if (argc < 2) {
        int fd = dup(2), null = open("/dev/null", O_WRONLY);
        if (fd >= 0 && null >= 0 && (g_err = fdopen(fd, "w"))) { setvbuf(g_err, NULL, _IOLBF, 0); dup2(null, 2); }
        else g_err = stderr;
        if (null >= 0) close(null);
    }
    @autoreleasepool {
        if (@available(macOS 26.0, *)) {
            int flow = argc > 2 && !strcmp(argv[2], "flow");
            if (flow ? !VTOpticalFlowConfiguration.isSupported : !VTLowLatencyFrameInterpolationConfiguration.isSupported) {
                if (argc > 1 && !strcmp(argv[1], "--selftest")) { printf("vxrt-npu selftest: skipped (not supported here)\n"); return 0; }
                return 2;
            }
            if (argc > 1 && !strcmp(argv[1], "--check")) return 0;
            if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest();
            NpuTiles *T = nil;
            for (;;) {
                uint32_t op;
                if (!read_all(NPU_FD, &op, sizeof op)) return 0;
                if (op == NPU_OP_INIT) {
                    npu_init_req q = { .op = op };
                    if (!read_all(NPU_FD, (char *)&q + sizeof op, sizeof q - sizeof op)) return 0;
                    npu_init_reply r = { 0 };
                    T = nil;
                    if (q.tiles < 1 || q.tiles > NPU_MAX_TILES || q.src_slots > NPU_MAX_SLOTS || q.dst_slots > NPU_MAX_SLOTS ||
                        !q.tile_w || !q.tile_h) r.status = -1;
                    else if (!(T = q.mode == NPU_MODE_FLOW ? flow_create(&q, &r) : tiles_create(&q, &r))) r.status = -2;
                    if (!write_all(NPU_FD, &r, sizeof r)) return 0;
                } else if (op == NPU_OP_PROC) {
                    npu_proc_req q = { .op = op };
                    if (!read_all(NPU_FD, (char *)&q + sizeof op, sizeof q - sizeof op)) return 0;
                    double t0 = now_ms();
                    npu_proc_reply r = { q.seq, T ? tiles_process(T, &q) : -1, 0 };
                    r.ms = (float)(now_ms() - t0);
                    if (!write_all(NPU_FD, &r, sizeof r)) return 0;
                } else return 1;
            }
        }
        if (argc > 1 && !strcmp(argv[1], "--selftest")) { printf("vxrt-npu selftest: skipped (macOS 26 needed)\n"); return 0; }
        return 2;
    }
}
