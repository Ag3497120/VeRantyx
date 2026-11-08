#ifndef VERANTYX_MACVR_H
#define VERANTYX_MACVR_H

#include <stdint.h>
#include <stddef.h>
#include <objc/objc.h>

#ifdef __cplusplus
extern "C" {
#endif

// Logging (writes to stderr and $VERANTYX_MACVR_LOG if set)
void macvr_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Hooks installed by macvr_metal.m and called per pass / per drawable present
typedef void (*macvr_pass_fn)(id commandBuffer, id colorTex, id depthTex, int loadAction);
typedef void (*macvr_present_fn)(id commandBuffer, id drawable);

void macvr_set_pass_hook(macvr_pass_fn fn);
void macvr_set_present_hook(macvr_present_fn fn);

// Scene camera & projection extracted from G-buffer top-level argument buffer (idx 2 -> word 0 -> table+24 -> +128/+320)
// Returns sequence > 0 if a valid orthonormal world-to-view matrix was read.
uint64_t macvr_game_camera(float m[16]);
// Returns 1 if valid half-angle tangents (tx = 1/P[0], ty = 1/P[5]) were read from the projection matrix.
int macvr_game_tan(float *tx, float *ty);

// HUD texture captured from the game's offscreen UI render pass (RGBA8Unorm_sRGB, mipmapLevelCount > 1, no depth).
// Returns (__bridge void *)id<MTLTexture> once per frame if UI was drawn into the HUD layer, else NULL.
void *macvr_hud_take(void);
void macvr_hud_suppress_set(int on);
int macvr_ui_menu(void);
int macvr_viewmodel_active(void);

// Input & Head Tracking (macvr_input.m)
void macvr_input_init(void);
int macvr_game_mouse_look(void);

// Reads latest SharedHands state from /tmp/verantyx_vr_hands (or $VERANTYX_SHARED_HANDS).
// Returns 1 if tracking is live and fresh, 0 otherwise.
int macvr_read_head_tracking(float head_col_major[16],
                             double *out_vision_timestamp,
                             float out_eye_tangents[8],
                             float *out_ipd,
                             uint64_t *out_updated_ms);

// Drives the game's camera on each presented frame using Vision Pro head pose & closed-loop camera matrix anchoring.
// Outputs the renderHeadTransform (column-major 4x4) and visionTimestamp corresponding to the rendered frame,
// plus residual yaw/pitch/roll (rad) for sub-frame stereo warp stabilization.
void macvr_input_step_camera(int scene_active,
                             int hud_menu,
                             float out_render_head_col[16],
                             double *out_vision_ts,
                             float *out_res_yaw,
                             float *out_res_pitch,
                             float *out_res_roll,
                             int *out_driving);

// Optional 3D viewmodel pose driven by Vision Pro hand tracking (SharedHands left/right).
// Returns 1 (right-hand primary) or -1 (left-hand primary) when active, 0 when inactive.
int macvr_viewmodel_poses(float g_delta[3], float g_rot[3], float o_delta[3], float o_rot[3], int *off_ok);

// Presenter (macvr_presenter.m)
void macvr_presenter_init(void);

// Test helpers (exposed for macvr_test)
uint64_t macvr_fnv1a64(const void *data, size_t len);
int macvr_test_parse_camera_from_tlab(const void *tlab, size_t len, float m[16], float *tx, float *ty);
void macvr_test_register_buffer(id mtlBuffer);
int macvr_test_present_synthetic_frame(id mtlDevice, const char *sharedFramePath, int vrMode);

#ifdef __cplusplus
}
#endif

#endif // VERANTYX_MACVR_H
