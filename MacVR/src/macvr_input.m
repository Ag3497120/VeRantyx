#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "macvr.h"

// ============================================================================
// DYLD_INTERPOSE for CGAssociateMouseAndMouseCursorPosition (from vxcg.c)
// ============================================================================
static atomic_int g_mouse_look = 0;

int macvr_game_mouse_look(void) {
    return atomic_load(&g_mouse_look);
}

static CGError my_CGAssociateMouseAndMouseCursorPosition(boolean_t connected) {
    int ml = !connected;
    int prev = atomic_exchange(&g_mouse_look, ml);
    if (prev != ml) {
        macvr_log("CGAssociateMouseAndMouseCursorPosition(%d) -> mouse_look=%d", (int)connected, ml);
    }
    return CGAssociateMouseAndMouseCursorPosition(connected);
}

__attribute__((used, section("__DATA,__interpose")))
static struct {
    const void *replacement;
    const void *original;
} g_macvr_interpose[] = {
    { (const void *)my_CGAssociateMouseAndMouseCursorPosition, (const void *)CGAssociateMouseAndMouseCursorPosition }
};

// ============================================================================
// SharedHands Reader (matches MacEncoder/HardwareEncoder.swift)
// ============================================================================
#pragma pack(push, 1)
typedef struct {
    uint8_t tracked;
    uint8_t pad0[3];
    float wrist[16];
    float thumbTip[3];
    float indexTip[3];
    float middleTip[3];
    float pinchDistance;
} MacVRSharedHandPose; // 84 bytes

typedef struct {
    uint32_t version;           // offset 0
    uint64_t updatedAtMs;       // offset 4
    float head[16];             // offset 12 (column-major 4x4)
    MacVRSharedHandPose left;   // offset 76
    MacVRSharedHandPose right;  // offset 160
    double visionTimestamp;     // offset 244
    uint8_t hasTimestamp;       // offset 252
    uint8_t hasEyeTangents;     // offset 253
    uint8_t reserved[10];       // offset 254..263
    float eyeTangents[8];       // offset 264 (L, R, U, D positive tangents for left & right eyes)
    float ipd;                  // offset 320
} MacVRSharedHands;             // 324 bytes
#pragma pack(pop)

static int g_hands_fd = -1;
static const MacVRSharedHands *g_hands_map = NULL;
static size_t g_hands_map_len = 0;
static pthread_mutex_t g_hands_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
}

static const MacVRSharedHands *macvr_map_hands_locked(void) {
    if (g_hands_map) return g_hands_map;
    const char *path = getenv("VERANTYX_SHARED_HANDS");
    if (!path || !*path) path = "/tmp/verantyx_vr_hands";
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(MacVRSharedHands)) {
        close(fd);
        return NULL;
    }
    void *ptr = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    g_hands_fd = fd;
    g_hands_map = (const MacVRSharedHands *)ptr;
    g_hands_map_len = (size_t)st.st_size;
    macvr_log("Mapped SharedHands from %s (%zu bytes)", path, g_hands_map_len);
    return g_hands_map;
}

int macvr_read_head_tracking(float head_col_major[16],
                             double *out_vision_timestamp,
                             float out_eye_tangents[8],
                             float *out_ipd,
                             uint64_t *out_updated_ms) {
    pthread_mutex_lock(&g_hands_lock);
    const MacVRSharedHands *sh = macvr_map_hands_locked();
    if (!sh || sh->version == 0) {
        pthread_mutex_unlock(&g_hands_lock);
        return 0;
    }
    MacVRSharedHands copy;
    memcpy(&copy, sh, sizeof(copy));
    pthread_mutex_unlock(&g_hands_lock);

    uint64_t age = now_ms() > copy.updatedAtMs ? (now_ms() - copy.updatedAtMs) : 0;
    if (copy.updatedAtMs == 0 || age > 5000) {
        return 0;
    }

    float r0 = copy.head[0]*copy.head[0] + copy.head[1]*copy.head[1] + copy.head[2]*copy.head[2];
    if (!isfinite(r0) || r0 < 0.25f) return 0;

    if (head_col_major) memcpy(head_col_major, copy.head, sizeof(copy.head));
    if (out_vision_timestamp) *out_vision_timestamp = copy.hasTimestamp ? copy.visionTimestamp : 0.0;
    if (out_eye_tangents) {
        if (copy.hasEyeTangents && copy.eyeTangents[0] > 0.1f && copy.eyeTangents[1] > 0.1f) {
            memcpy(out_eye_tangents, copy.eyeTangents, sizeof(copy.eyeTangents));
        } else {
            // Default Apple Vision Pro asymmetric eye tangents
            static const float def_tan[8] = {
                1.000f, 0.839f, 0.839f, 1.000f,
                0.839f, 1.000f, 0.839f, 1.000f
            };
            memcpy(out_eye_tangents, def_tan, sizeof(def_tan));
        }
    }
    if (out_ipd) {
        *out_ipd = (copy.hasEyeTangents && copy.ipd > 0.040f && copy.ipd < 0.080f) ? copy.ipd : 0.063f;
    }
    if (out_updated_ms) *out_updated_ms = copy.updatedAtMs;
    return 1;
}

// ============================================================================
// Controller Right-Stick Pitch Suppression & Mouse Delta Injection
// ============================================================================
static atomic_int g_vr_driving_camera = 0;

typedef void (*set_val_changed_imp)(id, SEL, GCControllerDirectionPadValueChangedHandler);
static set_val_changed_imp o_set_dpad_handler = NULL;

static void h_set_dpad_handler(GCControllerDirectionPad *self, SEL s, GCControllerDirectionPadValueChangedHandler handler) {
    if (!handler) {
        o_set_dpad_handler(self, s, nil);
        return;
    }
    GCControllerDirectionPadValueChangedHandler wrapped = ^(GCControllerDirectionPad *dpad, float xValue, float yValue) {
        if (atomic_load(&g_vr_driving_camera) && [dpad.localizedName.lowercaseString containsString:@"right"]) {
            // Suppress right thumbstick vertical pitch while head-tracking drives camera pitch
            yValue = 0.0f;
        }
        handler(dpad, xValue, yValue);
    };
    o_set_dpad_handler(self, s, wrapped);
}

static void macvr_send_mouse_delta(float dx, float dy) {
    int32_t ix = (int32_t)lrintf(dx);
    int32_t iy = (int32_t)lrintf(dy);
    if (ix == 0 && iy == 0) return;

    // 1. Deliver to GCMouse.current.mouseInput.mouseMovedHandler if registered by REDengine
    GCMouse *mouse = GCMouse.current;
    if (mouse && mouse.mouseInput && mouse.mouseInput.mouseMovedHandler) {
        mouse.mouseInput.mouseMovedHandler(mouse.mouseInput, (float)ix, (float)-iy);
    }

    // 2. Post CGEvent mouseMoved with relative deltas to this process
    CGEventRef ev = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, CGPointMake(640, 360), kCGMouseButtonLeft);
    if (ev) {
        CGEventSetIntegerValueField(ev, kCGMouseEventDeltaX, ix);
        CGEventSetIntegerValueField(ev, kCGMouseEventDeltaY, iy);
        NSEvent *nsEv = [NSEvent eventWithCGEvent:ev];
        if (nsEv && NSApp) {
            [NSApp postEvent:nsEv atStart:NO];
        }
        CFRelease(ev);
    }
}

// ============================================================================
// Closed-Loop Head-Tracked Camera Driver (modeled after xros_vr.m)
// ============================================================================
static pthread_mutex_t g_drv_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_head_inited = 0;
static float g_prev_head_yaw = 0.0f;
static float g_prev_head_pitch = 0.0f;
static float g_rem_dx = 0.0f;
static float g_rem_dy = 0.0f;
static float g_anchor_head_yaw = 0.0f;
static float g_anchor_game_yaw = 0.0f;
static int g_anchor_valid = 0;
static uint64_t g_last_cam_seq = 0;
static float g_last_cam_pos[3] = {0, 0, 0};
static double g_last_cam_time = 0.0;
static float g_cam_speed = 0.0f;

static float wrap_pi(float a) {
    while (a > (float)M_PI) a -= 2.0f * (float)M_PI;
    while (a < -(float)M_PI) a += 2.0f * (float)M_PI;
    return a;
}

static void extract_head_euler(const float h[16], float *yaw, float *pitch, float *roll) {
    // Column-major Vision Pro head matrix (Y-up, -Z forward):
    // Forward vector is -col2 = (-h[8], -h[9], -h[10])
    float fx = -h[8], fy = -h[9], fz = -h[10];
    *yaw = atan2f(-fx, -fz);
    *pitch = asinf(fmaxf(-1.0f, fminf(1.0f, fy)));
    *roll = atan2f(h[1], h[5]);
}

void macvr_input_step_camera(int scene_active,
                             int hud_menu,
                             float out_render_head_col[16],
                             double *out_vision_ts,
                             float *out_res_yaw,
                             float *out_res_pitch,
                             float *out_res_roll,
                             int *out_driving) {
    float head[16] = {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,1.6f,0,1
    };
    double vts = 0.0;
    int has_track = macvr_read_head_tracking(head, &vts, NULL, NULL, NULL);
    if (out_render_head_col) memcpy(out_render_head_col, head, sizeof(head));
    if (out_vision_ts) *out_vision_ts = vts;
    if (out_res_yaw) *out_res_yaw = 0.0f;
    if (out_res_pitch) *out_res_pitch = 0.0f;
    if (out_res_roll) *out_res_roll = 0.0f;

    int mouse_look = macvr_game_mouse_look();
    int driving = (has_track && scene_active && !hud_menu && mouse_look);
    atomic_store(&g_vr_driving_camera, driving);
    if (out_driving) *out_driving = driving;

    pthread_mutex_lock(&g_drv_lock);
    if (!has_track) {
        g_head_inited = 0;
        g_anchor_valid = 0;
        pthread_mutex_unlock(&g_drv_lock);
        return;
    }

    float hyaw = 0.0f, hpitch = 0.0f, hroll = 0.0f;
    extract_head_euler(head, &hyaw, &hpitch, &hroll);
    if (out_res_roll) *out_res_roll = hroll;

    if (!g_head_inited || !driving) {
        g_head_inited = 1;
        g_prev_head_yaw = hyaw;
        g_prev_head_pitch = hpitch;
        g_anchor_valid = 0;
        g_rem_dx = 0.0f;
        g_rem_dy = 0.0f;
        pthread_mutex_unlock(&g_drv_lock);
        return;
    }

    float dyaw = wrap_pi(hyaw - g_prev_head_yaw);
    float dpitch = hpitch - g_prev_head_pitch;
    g_prev_head_yaw = hyaw;
    g_prev_head_pitch = hpitch;

    // Default counts-per-radian scale (~954.9 counts/rad, configurable via VERANTYX_MOUSE_SCALE)
    static float counts_per_rad = 0.0f;
    if (counts_per_rad <= 0.0f) {
        const char *s = getenv("VERANTYX_MOUSE_SCALE");
        counts_per_rad = (s && atof(s) > 10.0) ? (float)atof(s) : 954.92966f;
    }

    // Closed-loop correction against the game's real camera matrix (when available)
    float gcam[16];
    uint64_t cseq = macvr_game_camera(gcam);
    if (cseq && cseq != g_last_cam_seq) {
        g_last_cam_seq = cseq;
        // Row 2 of world-to-view matrix is forward in game world (Z-up: X right, Y forward, Z up)
        float gfx = gcam[8], gfy = gcam[9], gfz = gcam[10];
        float game_yaw = atan2f(-gfx, gfy);
        float game_pitch = asinf(fmaxf(-1.0f, fminf(1.0f, gfz)));

        // Camera world position = -R^T * t
        float tx = gcam[3], ty = gcam[7], tz = gcam[11];
        float wx = -(gcam[0]*tx + gcam[4]*ty + gcam[8]*tz);
        float wy = -(gcam[1]*tx + gcam[5]*ty + gcam[9]*tz);
        float wz = -(gcam[2]*tx + gcam[6]*ty + gcam[10]*tz);
        double now = CACurrentMediaTime();
        if (g_last_cam_time > 0.0 && now > g_last_cam_time) {
            float dist = sqrtf((wx - g_last_cam_pos[0])*(wx - g_last_cam_pos[0]) +
                               (wy - g_last_cam_pos[1])*(wy - g_last_cam_pos[1]) +
                               (wz - g_last_cam_pos[2])*(wz - g_last_cam_pos[2]));
            float inst_speed = dist / (float)(now - g_last_cam_time);
            if (inst_speed < 80.0f) {
                g_cam_speed = 0.85f * g_cam_speed + 0.15f * inst_speed;
            }
        }
        g_last_cam_pos[0] = wx;
        g_last_cam_pos[1] = wy;
        g_last_cam_pos[2] = wz;
        g_last_cam_time = now;

        if (!g_anchor_valid) {
            g_anchor_valid = 1;
            g_anchor_head_yaw = hyaw;
            g_anchor_game_yaw = game_yaw;
        }
        // Closed-loop pitch correction (gentle proportional feedback so horizon never drifts)
        float pitch_err = hpitch - game_pitch;
        if (fabsf(pitch_err) < 0.6f) {
            dpitch += 0.18f * pitch_err;
            if (out_res_pitch) *out_res_pitch = fmaxf(-0.12f, fminf(0.12f, pitch_err));
        }
    }

    // Mouse X is right (+X for -dyaw), Mouse Y is down (+Y for -dpitch)
    float fx = (-dyaw) * counts_per_rad + g_rem_dx;
    float fy = (-dpitch) * counts_per_rad + g_rem_dy;
    float send_x = roundf(fx);
    float send_y = roundf(fy);
    g_rem_dx = fx - send_x;
    g_rem_dy = fy - send_y;

    pthread_mutex_unlock(&g_drv_lock);

    if (send_x != 0.0f || send_y != 0.0f) {
        macvr_send_mouse_delta(send_x, send_y);
    }
}

// ============================================================================
// 3D Viewmodel Hand Tracking Poses (SharedHands -> weapon/hand bone CBV)
// ============================================================================
int macvr_viewmodel_poses(float g_delta[3], float g_rot[3], float o_delta[3], float o_rot[3], int *off_ok) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("VERANTYX_HAND_VIEWMODEL");
        enabled = !(v && strcmp(v, "0") == 0);
    }
    if (!enabled) return 0;

    pthread_mutex_lock(&g_hands_lock);
    const MacVRSharedHands *sh = macvr_map_hands_locked();
    if (!sh || sh->version == 0) {
        pthread_mutex_unlock(&g_hands_lock);
        return 0;
    }
    MacVRSharedHands copy;
    memcpy(&copy, sh, sizeof(copy));
    pthread_mutex_unlock(&g_hands_lock);

    uint64_t age = now_ms() > copy.updatedAtMs ? (now_ms() - copy.updatedAtMs) : 0;
    if (copy.updatedAtMs == 0 || age > 1000) return 0;
    if (!copy.right.tracked && !copy.left.tracked) return 0;

    const MacVRSharedHandPose *primary = copy.right.tracked ? &copy.right : &copy.left;
    int side = copy.right.tracked ? 1 : -1;

    // Transform wrist position into head-local coordinates
    const float *H = copy.head;
    const float *W = primary->wrist;
    float dx = W[12] - H[12];
    float dy = W[13] - H[13];
    float dz = W[14] - H[14];

    // Head-space basis (column-major H: col0=right, col1=up, col2=back)
    float hx = H[0]*dx + H[1]*dy + H[2]*dz;
    float hy = H[4]*dx + H[5]*dy + H[6]*dz;
    float hz = -(H[8]*dx + H[9]*dy + H[10]*dz); // positive forward

    // Rest pose of weapon hand relative to head: (~0.16m right, -0.24m down, 0.38m forward)
    float rest_x = side > 0 ? 0.16f : -0.16f;
    float rest_y = -0.24f;
    float rest_z = 0.38f;

    g_delta[0] = hx - rest_x;
    g_delta[1] = hy - rest_y;
    g_delta[2] = hz - rest_z;

    // Relative wrist orientation in head space
    float fwd_x = -(H[0]*W[8] + H[1]*W[9] + H[2]*W[10]);
    float fwd_y = -(H[4]*W[8] + H[5]*W[9] + H[6]*W[10]);
    float fwd_z = (H[8]*W[8] + H[9]*W[9] + H[10]*W[10]);
    g_rot[0] = atan2f(fwd_x, fmaxf(0.1f, fwd_z)) * 0.5f;
    g_rot[1] = asinf(fmaxf(-1.0f, fminf(1.0f, fwd_y))) * 0.5f;
    g_rot[2] = 0.0f;

    if (off_ok) *off_ok = (copy.right.tracked && copy.left.tracked) ? 1 : 0;
    if (o_delta) memset(o_delta, 0, 3 * sizeof(float));
    if (o_rot) memset(o_rot, 0, 3 * sizeof(float));
    return side;
}

void macvr_input_init(void) {
    Class dpadCls = objc_getClass("GCControllerDirectionPad");
    if (dpadCls) {
        o_set_dpad_handler = (set_val_changed_imp)class_getMethodImplementation(dpadCls, @selector(setValueChangedHandler:));
        Method m = class_getInstanceMethod(dpadCls, @selector(setValueChangedHandler:));
        if (m) method_setImplementation(m, (IMP)h_set_dpad_handler);
    }
    macvr_log("MacVR input & head-tracking bridge initialized.");
}
