#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreGraphics/CoreGraphics.h>
#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "macvr.h"

#pragma pack(push, 1)
typedef struct {
    uint8_t tracked;
    uint8_t pad0[3];
    float wrist[16];
    float thumbTip[3];
    float indexTip[3];
    float middleTip[3];
    float pinchDistance;
} TestSharedHandPose;

typedef struct {
    uint32_t version;
    uint64_t updatedAtMs;
    float head[16];
    TestSharedHandPose left;
    TestSharedHandPose right;
    double visionTimestamp;
    uint8_t hasTimestamp;
    uint8_t hasEyeTangents;
    uint8_t reserved[10];
    float eyeTangents[8];
    float ipd;
} TestSharedHands;
#pragma pack(pop)

static uint64_t cur_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
}

int main(void) {
    @autoreleasepool {
        printf("=== VeRantyx MacVR Unit & Integration Test ===\n");

        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        assert(dev != nil && "Metal device required");

        // 1. Test FNV-1a 64-bit hash
        const char *sample = "VerantyxMacVR";
        uint64_t h1 = macvr_fnv1a64(sample, strlen(sample));
        uint64_t h2 = macvr_fnv1a64(sample, strlen(sample));
        assert(h1 == h2 && h1 != 0xcbf29ce484222325ULL);
        printf("[PASS] FNV-1a 64-bit hash deterministic: %016llx\n", (unsigned long long)h1);

        // 2. Test Top-Level Argument Buffer -> Descriptor Table -> View/Projection CBV extraction
        id<MTLBuffer> cbvBuf = [dev newBufferWithLength:1024 options:MTLResourceStorageModeShared];
        id<MTLBuffer> tblBuf = [dev newBufferWithLength:128 options:MTLResourceStorageModeShared];
        macvr_test_register_buffer(cbvBuf);
        macvr_test_register_buffer(tblBuf);

        uint8_t *cbv = (uint8_t *)cbvBuf.contents;
        memset(cbv, 0, 1024);
        // Write projection matrix at byte 128: P[0] = 1/tan(50 deg) = 0.8391, P[5] = 1.4917
        float proj[16] = {0};
        proj[0] = 0.8390996f;
        proj[5] = 1.4917327f;
        memcpy(cbv + 128, proj, sizeof(proj));

        // Write orthonormal world-to-view rotation matrix at byte 320
        float view[16] = {
            1, 0, 0, 10.0f,
            0, 0, 1, 20.0f,
            0, -1, 0, 1.7f,
            0, 0, 0, 1.0f
        };
        memcpy(cbv + 320, view, sizeof(view));

        uint8_t *tbl = (uint8_t *)tblBuf.contents;
        memset(tbl, 0, 128);
        uint64_t cbvAddr = cbvBuf.gpuAddress;
        memcpy(tbl + 24, &cbvAddr, 8);

        uint64_t tlab[4] = { tblBuf.gpuAddress, 0, 0, 0 };
        float parsed_m[16] = {0};
        float tx = 0.0f, ty = 0.0f;
        int ok = macvr_test_parse_camera_from_tlab(tlab, sizeof(tlab), parsed_m, &tx, &ty);
        assert(ok == 1);
        assert(fabsf(tx - (1.0f / proj[0])) < 1e-4f);
        assert(fabsf(ty - (1.0f / proj[5])) < 1e-4f);

        float read_m[16] = {0};
        uint64_t seq = macvr_game_camera(read_m);
        assert(seq >= 1);
        assert(fabsf(read_m[3] - 10.0f) < 1e-4f && fabsf(read_m[10] - 0.0f) < 1e-4f);
        printf("[PASS] Top-level argument buffer camera & projection extraction (tx=%.3f, ty=%.3f, seq=%llu)\n",
               tx, ty, (unsigned long long)seq);

        // 3. Test SharedHands reading & 3D Viewmodel Hand Tracking
        const char *hands_path = "/tmp/verantyx_macvr_test_hands";
        setenv("VERANTYX_SHARED_HANDS", hands_path, 1);
        int hfd = open(hands_path, O_RDWR | O_CREAT | O_TRUNC, 0666);
        assert(hfd >= 0);
        TestSharedHands sh;
        memset(&sh, 0, sizeof(sh));
        sh.version = 2;
        sh.updatedAtMs = cur_ms();
        float ident[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,1.65f,0,1
        };
        memcpy(sh.head, ident, sizeof(ident));
        sh.hasTimestamp = 1;
        sh.visionTimestamp = 12345.678;
        sh.hasEyeTangents = 1;
        float et[8] = {1.05f, 0.85f, 0.88f, 1.02f, 0.85f, 1.05f, 0.88f, 1.02f};
        memcpy(sh.eyeTangents, et, sizeof(et));
        sh.ipd = 0.064f;
        sh.right.tracked = 1;
        memcpy(sh.right.wrist, ident, sizeof(ident));
        sh.right.wrist[12] = 0.22f;
        sh.right.wrist[13] = 1.45f;
        sh.right.wrist[14] = -0.40f;
        assert(write(hfd, &sh, sizeof(sh)) == sizeof(sh));
        close(hfd);

        float read_head[16] = {0}, read_et[8] = {0}, read_ipd = 0.0f;
        double read_ts = 0.0;
        int h_ok = macvr_read_head_tracking(read_head, &read_ts, read_et, &read_ipd, NULL);
        assert(h_ok == 1);
        assert(fabs(read_ts - 12345.678) < 1e-4);
        assert(fabsf(read_ipd - 0.064f) < 1e-4f);
        assert(fabsf(read_et[0] - 1.05f) < 1e-4f);

        float g_delta[3] = {0}, g_rot[3] = {0}, o_delta[3] = {0}, o_rot[3] = {0};
        int off_ok = 0;
        int vm_side = macvr_viewmodel_poses(g_delta, g_rot, o_delta, o_rot, &off_ok);
        assert(vm_side == 1);
        printf("[PASS] SharedHands tracking & 3D viewmodel pose (ipd=%.3fm, ts=%.3f, dx=%.3f, dy=%.3f, dz=%.3f)\n",
               read_ipd, read_ts, g_delta[0], g_delta[1], g_delta[2]);

        // 4. Test Stereo Depth Warp + Floating HUD + Shared Frame Output (VR mode = 2 & Screen mode = 3)
        const char *frame_path = "/tmp/verantyx_macvr_test_frame";
        setenv("VERANTYX_EYE_W", "640", 1);
        setenv("VERANTYX_EYE_H", "640", 1);
        int pres_ok = macvr_test_present_synthetic_frame(dev, frame_path, 1);
        assert(pres_ok == 1);

        int ffd = open(frame_path, O_RDONLY);
        assert(ffd >= 0);
        struct stat st;
        assert(fstat(ffd, &st) == 0 && st.st_size > 1024);
        const uint8_t *fmap = (const uint8_t *)mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, ffd, 0);
        assert(fmap != MAP_FAILED);

        int32_t latest_idx = *(const int32_t *)(fmap + 0);
        int32_t cap_mode = *(const int32_t *)(fmap + 16);
        assert(latest_idx == 0 || latest_idx == 1);
        assert(cap_mode == 2);

        size_t slot_stride = 88ULL + 4096ULL * 4096ULL * 8ULL;
        const uint8_t *slot = fmap + 64 + (size_t)latest_idx * slot_stride;
        uint32_t f_seq = *(const uint32_t *)(slot + 0);
        uint32_t f_w   = *(const uint32_t *)(slot + 4);
        uint32_t f_h   = *(const uint32_t *)(slot + 8);
        uint32_t f_fmt = *(const uint32_t *)(slot + 12);
        double   f_ts  = *(const double *)(slot + 80);
        assert(f_seq >= 1);
        assert(f_w == 1280 && f_h == 640);
        assert(f_fmt == 87);
        assert(fabs(f_ts - 12345.678) < 1e-4);

        // Verify center pixel of left eye is non-zero
        const uint8_t *px = slot + 88 + (320 * 1280 + 320) * 4;
        assert(px[0] > 0 || px[1] > 0 || px[2] > 0);
        printf("[PASS] Stereo VR frame published (seq=%u, %ux%u, mode=%d, center_bgra=%u,%u,%u,%u)\n",
               f_seq, f_w, f_h, cap_mode, px[0], px[1], px[2], px[3]);

        // Also test Screen mode (vrMode = 0 -> captureSourceMode = 3)
        pres_ok = macvr_test_present_synthetic_frame(dev, frame_path, 0);
        assert(pres_ok == 1);
        int32_t cap_mode_screen = *(const int32_t *)(fmap + 16);
        assert(cap_mode_screen == 3);
        printf("[PASS] Virtual Cinema Screen mode frame published (mode=%d)\n", cap_mode_screen);

        munmap((void *)fmap, (size_t)st.st_size);
        close(ffd);
        unlink(frame_path);
        unlink(hands_path);

        printf("=== ALL MACVR TESTS PASSED ===\n");
        return 0;
    }
}
