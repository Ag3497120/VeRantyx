// The seam between the OpenVR shim and whatever owns the headset.
//
// Everything the shim knows about the device arrives through these functions,
// and everything it sends back (frames, haptics) leaves through them. On the
// Vision Pro they are implemented by platform/klvr_platform_klepton.c on top
// of Klepton's existing pose/input/compositor seam (kl_ovrp_*, kl_vulkan_*),
// which is already fed by CompositorServices, ARKit and GameController. In the
// Linux test harness they are implemented by platform/klvr_platform_null.c,
// which a test can script.
//
// Plain C, no Vulkan or OpenVR types: the Klepton side is C and must not need
// either header, the same rule kl_openxr.c keeps.
//
// Conventions, all of them OpenVR's (which are also OpenXR's and ARKit's):
//   right-handed, +Y up, -Z forward, metres, quaternions as {x, y, z, w}.
//   Poses are in the platform's FLOOR space: y = 0 is the floor, and the
//   standing universe is exactly this space. The shim derives the seated
//   universe from it.
//   Times are seconds on one monotonic clock (CLOCK_MONOTONIC on Darwin,
//   which is what CACurrentMediaTime reads).
#ifndef KLVR_PLATFORM_H
#define KLVR_PLATFORM_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { float p[3]; float q[4]; } klvr_pose;

typedef struct {
    int       valid;          // 0 = not tracked right now
    klvr_pose pose;
    float     vel[3];         // m/s, floor space
    float     ang[3];         // rad/s, floor space
    int       motion_known;   // 0 = vel/ang are not a measurement
} klvr_tracked;

typedef struct {
    uint32_t  eye_w, eye_h;   // recommended per-eye render target
    float     refresh_hz;
    float     tan[2][4];      // per eye: left, right, up, down, all positive
    klvr_pose eye_in_head[2]; // eye pose relative to the head pose
} klvr_display;

// PSVR2 Sense, named by position rather than by the PlayStation glyphs so the
// bindings read the same for either hand: FACE_LOWER is Cross on the right
// controller and Square on the left, FACE_UPPER is Circle / Triangle.
enum {
    KLVR_BTN_FACE_LOWER   = 1u << 0,
    KLVR_BTN_FACE_UPPER   = 1u << 1,
    KLVR_BTN_STICK        = 1u << 2,   // thumbstick click (L3/R3)
    KLVR_BTN_MENU         = 1u << 3,   // Create (left) / Options (right)
    KLVR_BTN_SYSTEM       = 1u << 4,   // PS button
    KLVR_BTN_TRIGGER      = 1u << 5,   // L2/R2 fully pulled
    KLVR_BTN_GRIP         = 1u << 6,   // L1/R1, a digital button on Sense
};
// Capacitive touch. Sense detects thumb, index and middle finger contact.
enum {
    KLVR_TOUCH_FACE_LOWER = 1u << 0,
    KLVR_TOUCH_FACE_UPPER = 1u << 1,
    KLVR_TOUCH_STICK      = 1u << 2,
    KLVR_TOUCH_TRIGGER    = 1u << 5,
    KLVR_TOUCH_GRIP       = 1u << 6,
};

typedef struct {
    int      connected;
    uint32_t buttons;         // KLVR_BTN_*
    uint32_t touches;         // KLVR_TOUCH_*; a pressed button is also touched
    float    trigger;         // 0..1
    float    grip;            // 0..1 (0 or 1 on Sense, whose grip is digital)
    float    stick[2];        // -1..1, +y up
} klvr_controller;

// ---- display and tracking -------------------------------------------------

void   klvr_plat_display(klvr_display *out);
double klvr_plat_now(void);

// Block until the compositor wants the next frame, then return the time that
// frame is predicted to reach the eyes. Must return on its own if the display
// stalls, so a stalled display makes the game render against an old pose
// rather than wedging it.
double klvr_plat_wait_frame(void);

// Head and grip poses predicted to `display_time`. hand: 0 = left, 1 = right.
// The grip pose is OpenXR's grip convention: origin in the palm, -Z along the
// hilt, +Y up out of its top.
void   klvr_plat_head(double display_time, klvr_tracked *out);
void   klvr_plat_hand(int hand, double display_time, klvr_tracked *out);
void   klvr_plat_controller(int hand, klvr_controller *out);

// amplitude 0..1, frequency Hz (0 = device default), seconds <= 0 = a click.
void   klvr_plat_haptic(int hand, float amplitude, float frequency, float seconds);

// ---- frame submission (Vulkan) --------------------------------------------
//
// Frames are copied by the shim, on the game's own VkQueue, into images the
// platform owns. `stage` rotates through klvr_plat_stage_count() so the
// compositor can read one while the game draws the next.

// 0 when nothing will show the frames (a desktop run of the game, where its
// own mirror window is the only picture): Submit then accepts each eye without
// copying it, so the game's frame loop runs exactly as it does on device.
int      klvr_plat_presents(void);
// GPU milliseconds the game's own rendering took in a finished frame, 0 when
// unknown (and the compositor's GPU time in that frame in *compositor):
// frames_ago 0 is the newest frame whose GPU work has all completed. It feeds
// Compositor_FrameTiming's GPU fields, which auto-fidelity reads.
float    klvr_plat_gpu_ms(uint32_t frames_ago, float *compositor);
// The game has submitted both eyes: GPU work it issues from now on is the next frame's.
void     klvr_plat_frame_closed(void);
// What the GPU time reported to the game is multiplied by: below 1 while the pace
// governor holds a slower frame rate, so auto-fidelity budgets for that frame
// (its vr_target_fps stays 60) and climbs back to top quality.
float    klvr_plat_gpu_report_scale(void);
// Megabytes this process may still allocate before the system ends it (os_proc_available_memory), or -1 when
// the platform cannot tell. KLVR_RAISE_MEM_TEST_MB stands in for it on a Mac test.
int      klvr_plat_mem_free_mb(void);
int      klvr_plat_stage_count(void);
int      klvr_plat_begin_stage(void);   // next stage to fill, and marks it busy
// VkImage (as uint64) for one eye of a stage, created on first use. 0 if the
// platform has no Vulkan path.
uint64_t klvr_plat_eye_image(int stage, int eye, uint32_t w, uint32_t h, int srgb);
// The game's depth for one eye of a stage (KL_CP_DEPTH): an R32_SFLOAT VkImage the compositor samples, a
// staging VkBuffer for the depth-aspect copy (host-visible; *mapped for a readback, may be NULL), and the depth
// mapping the game rendered with (OpenVR row-major: clip.z = m22*z + m23, clip.w = -z). 0 / no-op if the
// platform has no depth path.
uint64_t klvr_plat_eye_depth_image(int stage, int eye, uint32_t w, uint32_t h);
// KL_CP_DEPTH_HALF: a D32S8 image per eye to blit the game's depth into at half size (0 where unsupported).
uint64_t klvr_plat_eye_depth_half(int eye, uint32_t w, uint32_t h);
uint64_t klvr_plat_eye_depth_staging(int stage, int eye, uint64_t bytes, void **mapped);
void     klvr_plat_eye_depth_map(int stage, int eye, float m22, float m23, uint32_t w, uint32_t h, uint32_t pitch);
// Both eyes of `stage` are copied and the copy has been submitted. `render_head`
// is the head pose the frame was rendered from, and `tan` the eye tangents it
// used, so the compositor reprojects against the truth.
void     klvr_plat_end_stage(int stage, const klvr_pose *render_head, const float tan[2][4]);
// Called once, before the first klvr_plat_end_stage made from the shim's
// frame worker (KLVR_ASYNC_FRAMES): from then on end_stage is called only
// after the shim has waited for that stage's copies itself, from a thread
// that is not the game's, and must not touch the game's queue.
void     klvr_plat_async_frames(void);
// Vulkan entry points, resolved through the same loader the game uses, so the
// shim's commands go to the same interposed device.
void    *klvr_plat_vk_proc(const char *name);

// ---- logging --------------------------------------------------------------
void     klvr_plat_log(const char *line);

#ifdef __cplusplus
}
#endif
#endif
