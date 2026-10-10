// A scriptable platform for the Linux test harness: no headset, no GPU.
//
// Tests write `klvr_null` directly (head, hands, controller state, display)
// and read back what the shim sent (haptics, stages). Vulkan entry points come
// from `klvr_null.vk_proc`, so a test can supply fakes and inspect the copy
// the shim records.
#include "klvr_platform_null.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

klvr_null_state klvr_null = {
    .display = {
        .eye_w = 1920, .eye_h = 1824, .refresh_hz = 90.0f,
        .tan = { { 1.0f, 0.9f, 1.1f, 1.2f }, { 0.9f, 1.0f, 1.1f, 1.2f } },
        .eye_in_head = { { { -0.032f, 0, 0 }, { 0, 0, 0, 1 } }, { { 0.032f, 0, 0 }, { 0, 0, 0, 1 } } },
    },
    .head = { .valid = 1, .pose = { { 0, 1.6f, 0 }, { 0, 0, 0, 1 } } },
    .stage_count = 3,
};

void klvr_plat_display(klvr_display *out) { *out = klvr_null.display; }

double klvr_plat_now(void) {
    if (klvr_null.fixed_now) return klvr_null.fixed_now;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

double klvr_plat_wait_frame(void) {
    klvr_null.frames_waited++;
    return klvr_plat_now() + 0.02;
}

void klvr_plat_head(double t, klvr_tracked *out) {
    klvr_null.last_head_time = t;
    *out = klvr_null.head;
}

void klvr_plat_hand(int hand, double t, klvr_tracked *out) {
    (void)t;
    *out = klvr_null.hand[hand & 1];
}

void klvr_plat_controller(int hand, klvr_controller *out) { *out = klvr_null.ctl[hand & 1]; }

void klvr_plat_haptic(int hand, float amplitude, float frequency, float seconds) {
    klvr_null.haptic_count[hand & 1]++;
    klvr_null.haptic_amp[hand & 1] = amplitude;
    klvr_null.haptic_freq[hand & 1] = frequency;
    klvr_null.haptic_secs[hand & 1] = seconds;
}

int klvr_plat_presents(void) { return !klvr_null.no_display; }
float klvr_plat_gpu_ms(uint32_t frames_ago, float *compositor) { (void)frames_ago; if (compositor) *compositor = 0; return 0; }
void klvr_plat_frame_closed(void) {}
float klvr_plat_gpu_report_scale(void) { return 1.0f; }
int klvr_plat_mem_free_mb(void) { const char *e = getenv("KLVR_RAISE_MEM_TEST_MB"); return e && *e ? atoi(e) : -1; }
void klvr_plat_async_frames(void) { klvr_null.async_frames = 1; }
int klvr_plat_stage_count(void) { return klvr_null.stage_count; }

int klvr_plat_begin_stage(void) {
    int s = klvr_null.next_stage;
    klvr_null.next_stage = (klvr_null.next_stage + 1) % klvr_null.stage_count;
    return s;
}

uint64_t klvr_plat_eye_image(int stage, int eye, uint32_t w, uint32_t h, int srgb) {
    klvr_null.eye_w[eye & 1] = w;
    klvr_null.eye_h[eye & 1] = h;
    klvr_null.eye_srgb = srgb;
    return klvr_null.vk_proc ? 0x1000u + (uint64_t)stage * 2 + (uint64_t)eye : 0;
}

uint64_t klvr_plat_eye_depth_image(int stage, int eye, uint32_t w, uint32_t h) { (void)stage; (void)eye; (void)w; (void)h; return 0; }
uint64_t klvr_plat_eye_depth_half(int eye, uint32_t w, uint32_t h) { (void)eye; (void)w; (void)h; return 0; }
uint64_t klvr_plat_eye_depth_staging(int stage, int eye, uint64_t bytes, void **mapped) {
    (void)stage; (void)eye; (void)bytes; if (mapped) *mapped = NULL; return 0;
}
void klvr_plat_eye_depth_map(int stage, int eye, float m22, float m23, uint32_t w, uint32_t h, uint32_t pitch) {
    (void)stage; (void)eye; (void)m22; (void)m23; (void)w; (void)h; (void)pitch;
}

void klvr_plat_end_stage(int stage, const klvr_pose *head, const float tan[2][4]) {
    memcpy(klvr_null.last_tan, tan, sizeof klvr_null.last_tan);
    klvr_null.stages_ended++;
    klvr_null.last_stage = stage;
    if (head) klvr_null.last_render_head = *head;
}

void *klvr_plat_vk_proc(const char *name) { return klvr_null.vk_proc ? klvr_null.vk_proc(name) : NULL; }

void klvr_plat_log(const char *line) {
    if (!klvr_null.quiet) fprintf(stderr, "%s\n", line);
    snprintf(klvr_null.last_log, sizeof klvr_null.last_log, "%s", line);
}
