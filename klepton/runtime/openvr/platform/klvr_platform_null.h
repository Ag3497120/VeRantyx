// State of the test platform. See klvr_platform_null.c.
#ifndef KLVR_PLATFORM_NULL_H
#define KLVR_PLATFORM_NULL_H
#include "../src/klvr_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // inputs a test sets
    klvr_display    display;
    klvr_tracked    head;
    klvr_tracked    hand[2];
    klvr_controller ctl[2];
    int             stage_count;
    int             no_display;       // klvr_plat_presents() returns 0
    double          fixed_now;                 // 0 = real clock
    void         *(*vk_proc)(const char *name);
    int             quiet;
    // outputs a test reads
    unsigned        frames_waited;
    double          last_head_time;
    unsigned        haptic_count[2];
    float           haptic_amp[2], haptic_freq[2], haptic_secs[2];
    int             next_stage, last_stage;
    unsigned        stages_ended;
    int             async_frames;   // klvr_plat_async_frames was called
    klvr_pose       last_render_head;
    float           last_tan[2][4];     // the frustum the last stage was closed with
    uint32_t        eye_w[2], eye_h[2];
    int             eye_srgb;
    char            last_log[512];
} klvr_null_state;

extern klvr_null_state klvr_null;

#ifdef __cplusplus
}
#endif
#endif
