// The OpenVR shim's platform, on Klepton.
//
// Klepton already has everything the shim needs, because its OVRPlugin and
// OpenXR runtimes need the same things: the visionOS frontend (Swift:
// KleptonCompositor, KleptonControllers) publishes ARKit head and Sense
// controller poses, button state and display geometry into kl_ovrp, and the
// compositor draws whatever kl_vulkan's eye images hold once a frame is closed
// with kl_vulkan_frame_done + kl_ovrp_frame_end_external. This file reads and
// writes that seam exactly as kl_openxr.c does, so a third XR API gets the
// same head, the same hands and the same frame pacing as the other two.
//
// Built inside the Klepton fork (runtime/xr/), not in this repo's host build:
// it includes Klepton's headers. See README.md, "Integrating into Klepton".
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "klvr_platform.h"
#include "kl_ovrp.h"
#include "kl_vulkan.h"

// ---- frame pacing ---------------------------------------------------------
// Installed by the driver (visionos/Sources/kl_app.c) exactly as it installs
// kl_openxr_set_frame_pacer: the callback blocks until the compositor has
// published a new pose, and returns on its own if none comes. NULL on the
// host, where nothing presents and the game should free-run.
static void (*g_pacer)(void);
void klvr_klepton_set_frame_pacer(void (*wait)(void)) { g_pacer = wait; }

// ---- host mode --------------------------------------------------------------
// A desktop run (mains/m_alyx on the Mac) has no frontend: nothing publishes a
// head or hands and nothing composites eye images. The game's own companion
// window is the picture there, so host mode keeps VR's code path running
// without pretending to be a headset:
//   - the frame clock is a plain timer at the display rate;
//   - the head is fixed (KLVR_HOST_HEAD="x,y,z,yaw_deg", default 0,1.6,0,0);
//   - both controllers are connected and held still in front of the body
//     (KLVR_HOST_HANDS=0 disconnects them);
//   - Submit copies nothing (klvr_plat_presents() == 0).
// It is on when no frame pacer was installed, which is exactly the device
// app's signal, and KLVR_HOST=0/1 overrides that.
static int host(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("KLVR_HOST");
        v = e && *e ? atoi(e) != 0 : g_pacer == NULL;
        if (v) fprintf(stderr, "  [openvr] host mode: fixed head, still hands, frames not copied\n");
    }
    return v;
}

static void host_head(float p[3], float q[4]) {
    float x = 0, y = 1.6f, z = 0, yaw = 0;
    const char *e = getenv("KLVR_HOST_HEAD");
    if (e) sscanf(e, "%f,%f,%f,%f", &x, &y, &z, &yaw);
    float a = yaw * 3.14159265f / 360;
    p[0] = x; p[1] = y; p[2] = z;
    q[0] = 0; q[1] = sinf(a); q[2] = 0; q[3] = cosf(a);
}

// ---- the clock --------------------------------------------------------------
// Every instant the shim hands Klepton (a display time to predict the head to,
// a hand pose's time) and every instant it gets back (the predicted display
// time) must be on ONE clock. The frontend's are CACurrentMediaTime's:
// LayerRenderer's presentation time, and ARKit's queryDeviceAnchor(atTimestamp:)
// behind kl_ovrp_head_predict_delta. That is mach_absolute_time, which STOPS
// while the device sleeps. Darwin's CLOCK_MONOTONIC keeps counting through
// sleep, so the two agree only until the headset first sleeps (taking it off
// is enough), and after that they are apart by the total time asleep. Mixed,
// every display time looked like the past and was replaced by "now + one
// period", the head was predicted to an instant hours away, and each hand was
// extrapolated the full 50 ms cap on every frame.
//
// So the shim's clock is CLOCK_MONOTONIC plus an offset, and the offset is
// taken from the frontend's own display time: whichever of the two system
// clocks that time is close to is the one in use. Measured rather than
// assumed, so a frontend on the other clock is followed too.
static int64_t g_clock_offset_ns;   // atomic; frontend clock - CLOCK_MONOTONIC

static int64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

double klvr_plat_now(void) {
    return (mono_ns() + __atomic_load_n(&g_clock_offset_ns, __ATOMIC_RELAXED)) / 1e9;
}

// `t` is a display time the frontend published, in seconds.
static void follow_frontend_clock(double t) {
#ifdef CLOCK_UPTIME_RAW
    int64_t mono = mono_ns();
    int64_t up = (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    double to_mono = fabs(t - mono / 1e9), to_up = fabs(t - up / 1e9);
    // A published display time is within a few frames of now on its own clock.
    int64_t want = __atomic_load_n(&g_clock_offset_ns, __ATOMIC_RELAXED);
    if (to_up < 1.0 && to_mono >= 1.0) want = up - mono;
    else if (to_mono < 1.0 && to_up >= 1.0) want = 0;
    int64_t had = __atomic_exchange_n(&g_clock_offset_ns, want, __ATOMIC_RELAXED);
    static int said;
    if (!said || (want == 0) != (had == 0)) {
        said = 1;
        fprintf(stderr, "  [openvr] clock: frontend display time %.3f s; CLOCK_UPTIME_RAW %.3f s, CLOCK_MONOTONIC "
                        "%.3f s (apart %.3f s) -> following %s\n", t, up / 1e9, mono / 1e9, (mono - up) / 1e9,
                want ? "CLOCK_UPTIME_RAW (CACurrentMediaTime)" : "CLOCK_MONOTONIC");
    }
#else
    (void)t;
#endif
}

static double period(void) {
    float hz = kl_ovrp_display_frequency();
    return hz > 0 ? 1.0 / hz : 1.0 / 90;
}

// KLVR_HOST_FPS=N paces host frames at N per second while the display still
// reports its own rate: the headset's "30 fps on a 90 Hz display" on the Mac,
// to see whether the game's clock slows down with its frame rate.
// KLVR_HOST_FPS_SWITCH="<s>:<fps>" (the Mac check of the device's KL_GUEST_PACE_SWITCH): <s> seconds after the first
// host frame, pace at <fps> instead, logged once.
static double host_period(void) {
    static double fps = -1, t0, sw_at = -1, sw_fps;
    if (fps < 0) {
        const char *e = getenv("KLVR_HOST_FPS");
        fps = e ? atof(e) : 0;
        if (fps > 0) fprintf(stderr, "  [openvr] host frames paced at %.1f fps (display reports %.1f Hz)\n", fps,
                             1.0 / period());
        const char *w = getenv("KLVR_HOST_FPS_SWITCH");
        if (w && strchr(w, ':')) { sw_at = atof(w); sw_fps = atof(strchr(w, ':') + 1); }
        t0 = klvr_plat_now();
    }
    if (sw_at >= 0 && sw_fps > 0 && klvr_plat_now() - t0 >= sw_at) {
        fprintf(stderr, "  [openvr] host pace switch at %.1f s: %.1f -> %.1f fps (KLVR_HOST_FPS_SWITCH)\n",
                klvr_plat_now() - t0, fps, sw_fps);
        fps = sw_fps; sw_at = -1;
    }
    return fps > 0 ? 1.0 / fps : period();
}

// ---- Pace plan and governor (KLVR_PACE_FALLBACK=1) ----------------------------
// Quality first. The panel rate is visionOS's choice (90 or 120 Hz on the M5 unit;
// kl_app paces the guest at 1/N of it, KL_GUEST_PACE_HZ=60). The "base" pace is
// 60 at 120 Hz, 45 at 90, 60 at 60; the "fallback" is the next divider when it
// keeps 40 fps or more (40 at 120 Hz; none at 90 or 60).
//  - The game budgets for 60 (vr_target_fps 60) and is told its GPU time as a
//    share of the slowest pace allowed (klvr_plat_gpu_report_scale): the fallback
//    where there is one, the base otherwise. So auto-fidelity aims for top quality
//    at 40 fps (120 Hz) or 45 (90 Hz), and drops quality only when even that frame
//    does not fit. It never drops to the floor just to chase 60.
//  - The governor picks 60 or 40 at 120 Hz from the real GPU time (game plus
//    compositor): up to 60 after 5 s in a row with the average under 12.5 ms and
//    no frame over 15 ms; back to 40 after one second with the average over 15 ms
//    or more than 3% of frames over 16 ms. It starts at 40.
// On the device kl_app's pacer takes the step (kl_guest_pace_extra); on the Mac,
// KLVR_HOST_FPS is the base and KLVR_HOST_PANEL_HZ (default twice that) the panel.
// kl_app.c (the visionOS app) defines the real one; this weak no-op stands in
// where there is no app pacer (the Mac), which host_wait_frame covers.
void kl_guest_pace_extra(unsigned extra);
__attribute__((weak)) void kl_guest_pace_extra(unsigned extra) { (void)extra; }
static unsigned g_pace_extra;
static void pace_plan(float *base, float *fallback) {
    float panel;
    const char *hf = getenv("KLVR_HOST_FPS");
    if (hf && atof(hf) > 0) {
        const char *hp = getenv("KLVR_HOST_PANEL_HZ");
        panel = hp && atof(hp) > 0 ? (float)atof(hp) : 2.0f * (float)atof(hf);
    } else {
        panel = kl_ovrp_display_frequency();
    }
    if (!(panel > 20)) panel = 90;
    unsigned div = (unsigned)ceil(panel / 60.0 - 0.05);
    if (div < 1) div = 1;
    *base = panel / div;
    *fallback = panel / (div + 1) >= 39.0f ? panel / (div + 1) : 0;
}
// os_proc_available_memory(): visionOS/iOS only, so weak (NULL on the Mac, where the answer is "unknown").
extern size_t os_proc_available_memory(void) __attribute__((weak_import));
int klvr_plat_mem_free_mb(void) {
    const char *e = getenv("KLVR_RAISE_MEM_TEST_MB");
    if (e && *e) return atoi(e);
    if (&os_proc_available_memory) {
        size_t a = os_proc_available_memory();
        if (a) return (int)(a >> 20);
    }
    return -1;
}
float klvr_plat_gpu_report_scale(void) {
    float base, fb;
    pace_plan(&base, &fb);
    float s = (fb > 0 ? fb : base) / 60.0f;
    return s < 0.5f ? 0.5f : s > 1.0f ? 1.0f : s;
}
static void pace_governor(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("KLVR_PACE_FALLBACK");
        on = e && *e == '1';
        if (on) fprintf(stderr, "  [openvr] pace governor: top quality at the slowest allowed pace, 60 when it fits\n");
    }
    if (!on) return;
    float base, fb;
    pace_plan(&base, &fb);
    static float said_base = -1, said_fb = -1;
    if (base != said_base || fb != said_fb) {
        said_base = base; said_fb = fb;
        fprintf(stderr, "  [openvr] pace plan: base %.1f fps, fallback %.1f (GPU time reported as a share of the %.1f fps frame)\n",
                base, fb, fb > 0 ? fb : base);
    }
    static int started;
    if (fb <= 0) {                                   // no slower pace to choose: stay at the base
        if (g_pace_extra) { g_pace_extra = 0; kl_guest_pace_extra(0); }
        started = 0;
        return;
    }
    if (!started) {
        started = 1; g_pace_extra = 1; kl_guest_pace_extra(1);
        fprintf(stderr, "  [openvr] pace governor: starting at the fallback, %.1f fps\n", fb);
    }
    static double t0, sum;
    static int n, late, over, good;
    float comp = 0, game = klvr_plat_gpu_ms(0, &comp);
    if (game > 0) {
        // The compositor draws once per panel refresh; at the fallback pace a frame
        // holds more refreshes than at the base, so scale its share to the base frame.
        float per = game + (g_pace_extra ? comp * fb / base : comp);
        sum += per; n++;
        if (per > (g_pace_extra ? 15.0f : 16.0f)) late++;
    }
    double now = klvr_plat_now();
    if (!t0) t0 = now;
    if (now - t0 < 1.0) return;
    double avg = n ? sum / n : 0;
    int nl = late, nn = n;
    t0 = now; sum = 0; n = 0; late = 0;
    if (!nn) return;
    unsigned want = g_pace_extra;
    if (!g_pace_extra) {                             // at the base: leave at once when it does not fit
        over = (avg > 15.0 || nl > 0.03 * nn) ? over + 1 : 0;
        if (over >= 1) want = 1;
    } else {                                         // at the fallback: up only with clear headroom
        good = (avg < 12.5 && nl == 0) ? good + 1 : 0;
        if (good >= 5) want = 0;
    }
    if (want != g_pace_extra) {
        fprintf(stderr, "  [openvr] pace governor: GPU %.2f ms per %.0f fps frame (%d of %d frames late) -> %s\n", avg, base,
                nl, nn, want ? "fallback pace, top quality kept" : "base pace");
        g_pace_extra = want;
        over = good = 0;
        kl_guest_pace_extra(want);
    }
}

static double host_wait_frame(void) {
    static double next;
    double now = klvr_plat_now(), dt = host_period();
    if (g_pace_extra) {                  // the fallback pace: panel / (divider + 1)
        float base, fb;
        pace_plan(&base, &fb);
        if (fb > 0) dt = 1.0 / fb;
    }
    // A frame that took longer than a period starts the next one at once:
    // sleeping a further period on top of a slow frame would halve the rate.
    next += dt;
    if (next < now) next = now;
    double wait = next - now;
    if (wait > 0) {
        struct timespec ts = { (time_t)wait, (long)((wait - (time_t)wait) * 1e9) };
        nanosleep(&ts, NULL);
    }
    return next + dt;
}

// KLVR_FAKE_INPUT=<file>: scripted controllers for a run with no frontend
// (the Mac with KLVR_HOST=0). Both hands are published a forearm in front of
// the head every frame, and each line of the file
//     <start s> <duration s> <hand 0|1> <raw buttons> <raw touches> <trigger> <grip> <stick x> <stick y>
// holds that input from `start` (seconds after the first frame) for `duration`.
// The values are kl_ovrp RAW bits (A 0x1, X 0x100, ...), what
// KleptonControllers.swift publishes on the headset, so the whole path from
// the frontend's words to the game's actions is the one the device runs.
static void qrot(const float q[4], const float v[3], float o[3]);
static void qmul(const float a[4], const float b[4], float o[4]);
static void fake_input_tick(void) {
    static int state = -1;       // -1 unread, 0 off, 1 on
    static double t0;
    // Room for a device session replayed from its [ctrl] log lines.
    static struct { double start, dur; int hand; unsigned b, t; float trig, grip, sx, sy; } ev[4096];
    static int n;
    if (state == 0) return;
    if (state < 0) {
        const char *path = getenv("KLVR_FAKE_INPUT");
        FILE *f = path && *path ? fopen(path, "r") : NULL;
        state = f != NULL;
        if (!f) return;
        char line[256];
        while (n < (int)(sizeof ev / sizeof ev[0]) && fgets(line, sizeof line, f)) {
            if (line[0] == '#') continue;
            char bs[32], ts[32];
            if (sscanf(line, "%lf %lf %d %31s %31s %f %f %f %f", &ev[n].start, &ev[n].dur, &ev[n].hand, bs, ts,
                       &ev[n].trig, &ev[n].grip, &ev[n].sx, &ev[n].sy) == 9) {
                ev[n].b = (unsigned)strtoul(bs, NULL, 0);
                ev[n].t = (unsigned)strtoul(ts, NULL, 0);
                n++;
            }
        }
        fclose(f);
        t0 = klvr_plat_now();
        fprintf(stderr, "  [openvr] fake input: %d events from %s\n", n, path);
    }
    double now = klvr_plat_now() - t0;
    // A standing head (KLVR_FAKE_HEAD_Y, default 1.6 m): with no frontend the
    // head would sit at the floor, and a zero-height player cannot walk.
    // KLVR_FAKE_HEAD_PITCH / _YAW (degrees; pitch < 0 looks down, yaw > 0 turns left) aim the head, to
    // reproduce a device view on the Mac (e.g. standing and looking down over the balcony railing).
    // KLVR_FAKE_HEAD_SPIN (degrees/s) turns the head continuously and KLVR_FAKE_HEAD_WAVE (degrees)
    // swings the pitch over a 4 s period: the pipeline-coverage crawler looks everywhere.
    {
        static float hy = -1, p0, y0, spin, wave;
        if (hy < 0) {
            const char *e = getenv("KLVR_FAKE_HEAD_Y"); hy = e && *e ? (float)atof(e) : 1.6f;
            const char *pe = getenv("KLVR_FAKE_HEAD_PITCH"), *ye = getenv("KLVR_FAKE_HEAD_YAW");
            const char *se = getenv("KLVR_FAKE_HEAD_SPIN"), *we = getenv("KLVR_FAKE_HEAD_WAVE");
            p0 = pe && *pe ? (float)atof(pe) : 0; y0 = ye && *ye ? (float)atof(ye) : 0;
            spin = se && *se ? (float)atof(se) : 0; wave = we && *we ? (float)atof(we) : 0;
        }
        // KLVR_FAKE_AIM="<t>:<pitch>:<yaw>,..." moves that aim over time (seconds on the fake input's clock), e.g. to
        // point at one menu button and then another in one run.
        static struct { float t, p, y; } aim[32];
        static int naim = -1;
        if (naim < 0) {
            naim = 0;
            const char *ae = getenv("KLVR_FAKE_AIM");
            for (const char *c = ae; c && *c && naim < 32; ) {
                if (sscanf(c, "%f:%f:%f", &aim[naim].t, &aim[naim].p, &aim[naim].y) == 3) naim++;
                c = strchr(c, ','); if (c) c++;
            }
        }
        float pa = p0, ya = y0;
        for (int i = 0; i < naim; i++) if ((float)now >= aim[i].t) { pa = aim[i].p; ya = aim[i].y; }
        const float d2r = 3.14159265f / 180;
        float p = (pa + wave * sinf((float)now * 2 * 3.14159265f / 4)) * d2r, y = (ya + spin * (float)now) * d2r;
        float qp[4] = { sinf(p / 2), 0, 0, cosf(p / 2) }, qy[4] = { 0, sinf(y / 2), 0, cosf(y / 2) }, q[4];
        qmul(qy, qp, q);
        kl_ovrp_set_head_pose(0, hy, 0, q[0], q[1], q[2], q[3]);
    }
    klvr_tracked head;
    klvr_plat_head(klvr_plat_now(), &head);
    for (int hand = 0; hand < 2; hand++) {
        float off[3] = { hand ? 0.2f : -0.2f, -0.35f, -0.35f }, o[3];
        qrot(head.pose.q, off, o);
        kl_ovrp_set_hand_pose(hand, head.pose.p[0] + o[0], head.pose.p[1] + o[1], head.pose.p[2] + o[2],
                              head.pose.q[0], head.pose.q[1], head.pose.q[2], head.pose.q[3]);
        unsigned b = 0, t = 0;
        float trig = 0, grip = 0, sx = 0, sy = 0;
        for (int i = 0; i < n; i++) {
            if (ev[i].hand != hand || now < ev[i].start || now >= ev[i].start + ev[i].dur) continue;
            b |= ev[i].b; t |= ev[i].t;
            if (ev[i].trig > trig) trig = ev[i].trig;
            if (ev[i].grip > grip) grip = ev[i].grip;
            if (ev[i].sx != 0) sx = ev[i].sx;
            if (ev[i].sy != 0) sy = ev[i].sy;
        }
        kl_ovrp_set_controller_input(hand, b, t, trig, grip, sx, sy);
    }
}

double klvr_plat_wait_frame(void) {
    pace_governor();
    if (host()) return host_wait_frame();
    fake_input_tick();
    // No frontend pacing us (the Mac with KLVR_HOST=0) but KLVR_HOST_FPS set:
    // pace like host mode, so the game's clock runs at real time in a
    // scripted-input run instead of at hundreds of frames per second.
    if (!g_pacer && getenv("KLVR_HOST_FPS")) (void)host_wait_frame();
    // Pace BEFORE the latch, for the reason kl_openxr's xrWaitFrame gives:
    // the pose this frame is pinned to must be the one just published.
    if (g_pacer) g_pacer();
    kl_ovrp_frame_latch();
    kl_ovrp_frame_begin_external();
    double t = kl_ovrp_predicted_display_time();
    // Only once the pacer has let us through is `t` the frontend's own number
    // (before that, kl_ovrp answers with CLOCK_MONOTONIC).
    if (g_pacer) follow_frontend_clock(t);
    double now = klvr_plat_now();
    // No frontend pushing a presentation time (a host run): one period out.
    if (!(t > now)) t = now + period();
    return t;
}

// ---- math -----------------------------------------------------------------
static void qmul(const float a[4], const float b[4], float o[4]) {
    float r[4] = { a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                   a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                   a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                   a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2] };
    memcpy(o, r, sizeof r);
}
static void qrot(const float q[4], const float v[3], float o[3]) {
    float p[4] = { v[0], v[1], v[2], 0 }, c[4] = { -q[0], -q[1], -q[2], q[3] }, t[4];
    qmul(q, p, t);
    qmul(t, c, t);
    o[0] = t[0]; o[1] = t[1]; o[2] = t[2];
}

// ---- display --------------------------------------------------------------
// True once CompositorServices has published the real per-eye frustums.
// Weak, so the shim still builds against a Klepton without it.
extern int kl_ovrp_eye_frustum_ready(void) __attribute__((weak));

// Alyx reads the projection once, at VR init, which on device can come before
// CompositorServices has published the per-eye frustums; it then keeps the
// 90-degree placeholder for the whole run. So the first answer waits for the
// real ones, up to 5 s.
static void wait_for_frustums(void) {
    static int done;
    if (done) return;
    done = 1;
    if (host() || !kl_ovrp_eye_frustum_ready) return;
    double t0 = klvr_plat_now();
    while (!kl_ovrp_eye_frustum_ready() && klvr_plat_now() - t0 < 5.0) {
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    fprintf(stderr, "  [openvr] eye frustums %s after %.2f s\n",
            kl_ovrp_eye_frustum_ready() ? "published" : "NOT published (using placeholders)", klvr_plat_now() - t0);
}

void klvr_plat_display(klvr_display *out) {
    wait_for_frustums();
    memset(out, 0, sizeof *out);
    int w = 0, h = 0;
    kl_ovrp_eye_texture_size(&w, &h);
    // KLVR_EYE_SIZE="W,H": the recommended per-eye size, in place of the
    // platform's. On the Mac it stands in for the headset's 3072x2464 so GPU
    // costs are measured at the size the device asks for.
    const char *es = getenv("KLVR_EYE_SIZE");
    if (es) sscanf(es, "%d,%d", &w, &h);
    out->eye_w = (uint32_t)w;
    out->eye_h = (uint32_t)h;
    out->refresh_hz = kl_ovrp_display_frequency();

    // Eye poses come out of kl_ovrp in tracking space, composed on the latched
    // head; the shim wants them relative to that head.
    float hp[3], hq[4];
    kl_ovrp_get_guest_head_pose(&hp[0], &hp[1], &hp[2], &hq[0], &hq[1], &hq[2], &hq[3]);
    if (hq[0] == 0 && hq[1] == 0 && hq[2] == 0 && hq[3] == 0) hq[3] = 1;   // never published
    float hqi[4] = { -hq[0], -hq[1], -hq[2], hq[3] };
    for (int e = 0; e < 2; e++) {
        float p[3], q[4];
        kl_ovrp_eye_view(e, &p[0], &p[1], &p[2], &q[0], &q[1], &q[2], &q[3], out->tan[e]);
        float d[3] = { p[0] - hp[0], p[1] - hp[1], p[2] - hp[2] };
        qrot(hqi, d, out->eye_in_head[e].p);
        qmul(hqi, q, out->eye_in_head[e].q);
    }
    // KLVR_TAN="L,R,U,D": the left eye's frustum tangents (the right eye gets
    // them mirrored), in place of the platform's. On the Mac, where there are
    // no real frustums, it gives the game the headset's asymmetric ones, so
    // the "rendered with tan" line can be checked without a headset.
    const char *tan_env = getenv("KLVR_TAN");
    float lt[4];
    if (tan_env && sscanf(tan_env, "%f,%f,%f,%f", &lt[0], &lt[1], &lt[2], &lt[3]) == 4) {
        memcpy(out->tan[0], lt, sizeof lt);
        out->tan[1][0] = lt[1]; out->tan[1][1] = lt[0];
        out->tan[1][2] = lt[2]; out->tan[1][3] = lt[3];
    }
    // No eye geometry published (host mode, or a frontend not up yet): a
    // zero IPD renders both eyes from one point, and the game says so
    // ("IPD 0.00 mm"). Use an average adult IPD instead, KLVR_IPD_M to change.
    float dx = out->eye_in_head[1].p[0] - out->eye_in_head[0].p[0];
    float dy = out->eye_in_head[1].p[1] - out->eye_in_head[0].p[1];
    float dz = out->eye_in_head[1].p[2] - out->eye_in_head[0].p[2];
    if (dx * dx + dy * dy + dz * dz < 1e-6f) {
        const char *env = getenv("KLVR_IPD_M");
        float ipd = env ? (float)atof(env) : 0.063f;
        for (int e = 0; e < 2; e++) {
            out->eye_in_head[e].p[0] = (e ? 0.5f : -0.5f) * ipd;
            out->eye_in_head[e].p[1] = out->eye_in_head[e].p[2] = 0;
        }
    }
    static int said;
    if (!said) {
        said = 1;
        for (int e = 0; e < 2; e++)
            fprintf(stderr, "  [openvr] eye %d: tan L %.3f R %.3f U %.3f D %.3f, in head (%.4f, %.4f, %.4f)\n", e,
                    out->tan[e][0], out->tan[e][1], out->tan[e][2], out->tan[e][3],
                    out->eye_in_head[e].p[0], out->eye_in_head[e].p[1], out->eye_in_head[e].p[2]);
    }
}

// ---- tracking -------------------------------------------------------------
void klvr_plat_head(double t, klvr_tracked *out) {
    memset(out, 0, sizeof *out);
    float *p = out->pose.p, *q = out->pose.q;
    if (host()) { host_head(p, q); out->valid = 1; return; }
    kl_ovrp_get_guest_head_pose(&p[0], &p[1], &p[2], &q[0], &q[1], &q[2], &q[3]);
    // Carry the latched head to `t` with the frontend's own predictor, the
    // same left-multiplied delta kl_openxr applies for xrLocateViews.
    float dq[4], dp[3];
    if (kl_ovrp_head_predict_delta(t, dq, dp)) {
        float np[3];
        qrot(dq, p, np);
        p[0] = np[0] + dp[0]; p[1] = np[1] + dp[1]; p[2] = np[2] + dp[2];
        qmul(dq, q, q);
    }
    out->motion_known = kl_ovrp_head_motion(out->vel, out->ang);
    out->valid = 1;
}

static int host_hands(void) {
    const char *e = getenv("KLVR_HOST_HANDS");
    return !e || atoi(e) != 0;
}

// Ride-through for controller drop-outs (KLVR_CTRL_GRACE seconds, default
// 2): on the headset visionOS briefly empties the spatial controller list
// (both Sense controllers gone for 0.55-1.6 s, twice in one run). A game told
// the controllers vanished drops the hands and whatever they held; holding the
// last pose with no buttons pressed for a moment lets it ride through.
static double g_hand_seen[2];
static float g_hand_last_p[2][3], g_hand_last_q[2][4];
static double ctrl_grace(void) {
    static double g = -1;
    if (g < 0) { const char *e = getenv("KLVR_CTRL_GRACE"); g = e && *e ? atof(e) : 2.0; }
    return g;
}

void klvr_plat_hand(int hand, double t, klvr_tracked *out) {
    memset(out, 0, sizeof *out);
    if (host()) {
        // Waist height, a forearm in front of the fixed head, pointing ahead.
        if (!host_hands()) return;
        float hp[3], hq[4], off[3] = { hand ? 0.2f : -0.2f, -0.35f, -0.35f }, o[3];
        host_head(hp, hq);
        qrot(hq, off, o);
        for (int i = 0; i < 3; i++) out->pose.p[i] = hp[i] + o[i];
        memcpy(out->pose.q, hq, sizeof hq);
        out->valid = out->motion_known = 1;
        return;
    }
    out->valid = kl_ovrp_hand_motion(hand, out->pose.p, out->pose.q, out->vel, out->ang);
    {
        int h = hand & 1;
        double now = klvr_plat_now();
        if (out->valid) {
            g_hand_seen[h] = now;
            memcpy(g_hand_last_p[h], out->pose.p, sizeof g_hand_last_p[h]);
            memcpy(g_hand_last_q[h], out->pose.q, sizeof g_hand_last_q[h]);
        } else if (g_hand_seen[h] > 0 && now - g_hand_seen[h] < ctrl_grace()) {
            // Still inside the grace window: the last (raw) pose, not moving;
            // it falls through to the pitch correction like a live one.
            memcpy(out->pose.p, g_hand_last_p[h], sizeof g_hand_last_p[h]);
            memcpy(out->pose.q, g_hand_last_q[h], sizeof g_hand_last_q[h]);
            memset(out->vel, 0, sizeof out->vel); memset(out->ang, 0, sizeof out->ang);
            out->valid = 1; out->motion_known = 0;
            static int said;
            if (said++ < 4) fprintf(stderr, "  [openvr] hand %d dropped out: holding its last pose (grace %.1f s)\n", h, ctrl_grace());
        }
    }
    out->motion_known = out->valid && kl_ovrp_hand_motion_known(hand);
    if (!out->valid) return;
    // kl_ovrp holds the Sense pose in the frame KleptonControllers tuned for
    // OVRPlugin guests: Apple's .grip pitched -37 degrees about X (the Touch
    // "hilt" angle Beat Saber was measured at). kl_openxr turns it back into an
    // OpenXR grip by pitching +37 (KLXR_GRIP_PITCH_DEFAULT, checked by eye
    // against SteamVR over Steam Link), and so does this, with the same knob.
    // A rotation about the pose's own origin, so position and velocities stand.
    static float pitch = -1000;
    if (pitch == -1000) {
        const char *e = getenv("KL_XR_GRIP_PITCH");
        pitch = e && *e ? (float)atof(e) : 37.0f;
        fprintf(stderr, "  [openvr] Sense pose -> OpenXR grip: pitched %.1f deg about X (KL_XR_GRIP_PITCH)\n", pitch);
    }
    float a = pitch * 3.14159265f / 360;
    float rx[4] = { sinf(a), 0, 0, cosf(a) };
    qmul(out->pose.q, rx, out->pose.q);
    if (!out->motion_known) return;
    // The frontend predicts the Sense pose to the frame's presentation time;
    // a caller asking for another instant is carried there on the measured
    // velocity, capped so a bad sample cannot fling the hand.
    double dt = t - kl_ovrp_predicted_display_time();
    if (dt > 0.05) dt = 0.05;
    if (dt < -0.05) dt = -0.05;
    for (int i = 0; i < 3; i++) out->pose.p[i] += (float)(out->vel[i] * dt);
    float w = sqrtf(out->ang[0] * out->ang[0] + out->ang[1] * out->ang[1] + out->ang[2] * out->ang[2]);
    if (w > 1e-6f) {
        float a = w * (float)dt / 2, s = sinf(a) / w;
        float dq[4] = { out->ang[0] * s, out->ang[1] * s, out->ang[2] * s, cosf(a) };
        qmul(dq, out->pose.q, out->pose.q);
    }
}

// ovrp raw bits back into Sense buttons. KleptonControllers.swift composes
// them: Sense "Button A/B" arrive as X/Y on the left and A/B on the right;
// Options and PS both arrive as BACK on the right and START/BACK on the left.
void klvr_plat_controller(int hand, klvr_controller *out) {
    memset(out, 0, sizeof *out);
    if (host()) { out->connected = host_hands(); return; }
    uint32_t b = 0, t = 0;
    float it = 0, ht = 0, sx = 0, sy = 0;
    int present = kl_ovrp_controller_input(hand, &b, &t, &it, &ht, &sx, &sy);
    static double said[2];
    if (getenv("KLVR_TRACE_POSE") && klvr_plat_now() - said[hand & 1] >= 1.0) {
        said[hand & 1] = klvr_plat_now();
        fprintf(stderr, "  [openvr] %s raw ovrp: present %d buttons %#x touches %#x trigger %.2f grip %.2f stick (%.2f %.2f)\n",
                hand ? "right" : "left", present, b, t, it, ht, sx, sy);
    }
    float pos[3], quat[4];
    out->connected = present && kl_ovrp_hand_motion(hand, pos, quat, NULL, NULL);
    // Grace window (see klvr_plat_hand): still connected, nothing pressed.
    if (!out->connected && g_hand_seen[hand & 1] > 0 && klvr_plat_now() - g_hand_seen[hand & 1] < ctrl_grace()) {
        out->connected = 1;
        return;
    }
    const uint32_t lower = hand ? KL_OVRP_RAW_A : KL_OVRP_RAW_X;
    const uint32_t upper = hand ? KL_OVRP_RAW_B : KL_OVRP_RAW_Y;
    const uint32_t stick = hand ? KL_OVRP_RAW_RTHUMBSTICK : KL_OVRP_RAW_LTHUMBSTICK;
    if (b & lower) out->buttons |= KLVR_BTN_FACE_LOWER;
    if (b & upper) out->buttons |= KLVR_BTN_FACE_UPPER;
    if (b & stick) out->buttons |= KLVR_BTN_STICK;
    if (hand == 0 && (b & KL_OVRP_RAW_START)) out->buttons |= KLVR_BTN_MENU;
    if (hand == 0 && (b & KL_OVRP_RAW_BACK)) out->buttons |= KLVR_BTN_SYSTEM;
    if (hand == 1 && (b & KL_OVRP_RAW_BACK)) out->buttons |= KLVR_BTN_MENU;
    if (t & lower) out->touches |= KLVR_TOUCH_FACE_LOWER;
    if (t & upper) out->touches |= KLVR_TOUCH_FACE_UPPER;
    if (t & stick) out->touches |= KLVR_TOUCH_STICK;
    // kl_ovrp's trigger bits mean "above zero", not a click, so the trigger
    // click is left to the shim's own threshold on the analog value.
    out->trigger = it;
    out->grip = ht;
    if (ht >= 0.5f) out->buttons |= KLVR_BTN_GRIP;
    out->stick[0] = sx;
    out->stick[1] = sy;
}

void klvr_plat_haptic(int hand, float amplitude, float frequency, float seconds) {
    (void)frequency;
    if (host()) return;   // kl_ovrp's seam is amplitude and duration; see kl_ovrp.h
    if (amplitude <= 0) kl_ovrp_haptics_stop(hand);
    else kl_ovrp_haptics_apply(hand, amplitude, seconds);
}

// ---- frames ---------------------------------------------------------------
static int g_next_stage;

int klvr_plat_presents(void) { return !host(); }
// kl_gpu_time.m: the union of the game's (and the compositor's) Metal command buffer GPU intervals per frame.
extern float kl_gpu_time_frame_ms(unsigned frames_ago, float *compositor, unsigned *frame) __attribute__((weak));
extern void kl_gpu_time_frame_end(void) __attribute__((weak));
void klvr_plat_frame_closed(void) {
    if (kl_gpu_time_frame_end) kl_gpu_time_frame_end();
}
// KLVR_GPU_MS_SCALE multiplies it: a Mac stands in for a slower GPU, to check
// that the game's auto-fidelity reacts before a device run.
float klvr_plat_gpu_ms(uint32_t frames_ago, float *compositor) {
    static float scale = -1;
    if (scale < 0) {
        const char *e = getenv("KLVR_GPU_MS_SCALE");
        scale = e && *e ? (float)atof(e) : 1.0f;
        if (scale != 1.0f) fprintf(stderr, "[klvr] reporting GPU time x%.2f (KLVR_GPU_MS_SCALE)\n", scale);
    }
    float comp = 0, game = kl_gpu_time_frame_ms ? kl_gpu_time_frame_ms(frames_ago, &comp, NULL) : 0;
    if (compositor) *compositor = comp * scale;
    return game * scale;
}

// The shim now waits for its own copies before klvr_plat_end_stage, from its
// frame worker. kl_vulkan_frame_done must then not make the game's queue idle
// itself: that would block on the game's NEXT frame, and a queue may not be
// used from two threads at once. KL_VK_FRAME_SYNC is read on every call.
void klvr_plat_async_frames(void) {
    setenv("KL_VK_FRAME_SYNC", "0", 1);
    fprintf(stderr, "  [openvr] KL_VK_FRAME_SYNC=0: frames complete when the shim's copies do\n");
}

int klvr_plat_stage_count(void) {
    int n = kl_ovrp_stage_count();
    return n > 0 ? n : 3;
}

int klvr_plat_begin_stage(void) {
    int s = g_next_stage;
    g_next_stage = (g_next_stage + 1) % klvr_plat_stage_count();
    return s;
}

uint64_t klvr_plat_eye_image(int stage, int eye, uint32_t w, uint32_t h, int srgb) {
    // The OVRPlugin path's eye images: runtime-owned VkImages on the game's
    // own (interposed) device, published into the eye table every
    // compositor samples.
    return kl_vulkan_eye_image(stage, eye, w, h, srgb);
}

uint64_t klvr_plat_eye_depth_half(int eye, uint32_t w, uint32_t h) { return kl_vulkan_eye_depth_half(eye, w, h); }
uint64_t klvr_plat_eye_depth_image(int stage, int eye, uint32_t w, uint32_t h) {
    return kl_vulkan_eye_depth_image(stage, eye, w, h);
}
uint64_t klvr_plat_eye_depth_staging(int stage, int eye, uint64_t bytes, void **mapped) {
    return kl_vulkan_eye_depth_staging(stage, eye, bytes, mapped);
}
void klvr_plat_eye_depth_map(int stage, int eye, float m22, float m23, uint32_t w, uint32_t h, uint32_t pitch) {
    kl_vulkan_set_eye_depth_map(stage, eye, m22, m23, w, h, pitch);
}

void klvr_plat_end_stage(int stage, const klvr_pose *head, const float tan[2][4]) {
    // Order is load-bearing (see kl_openxr.c's xrEndFrame): completion first,
    // then the record that describes it.
    if (host()) return;
    // KL_VK_OUT=<dir>: write this stage's eye images (post-crop, exactly what
    // the compositor gets) as PNGs every KL_VK_OUT_EVERY frames, up to
    // KL_VK_OUT_MAX files. Before frame_done, on this thread: the capture
    // submits on the game's queue, so a capture run keeps KLVR_ASYNC_FRAMES=0.
    static int cap = -1;
    static unsigned cap_frame;
    if (cap < 0) cap = getenv("KL_VK_OUT") != NULL;
    if (cap) kl_vulkan_capture_eyes(cap_frame++, stage);
    kl_vulkan_frame_done(stage);
    float pose7[7], tan8[8];
    if (head) {
        memcpy(pose7, head->p, sizeof head->p);
        memcpy(pose7 + 3, head->q, sizeof head->q);
    }
    memcpy(tan8, tan, sizeof tan8);
    kl_ovrp_frame_end_external(stage, head ? pose7 : NULL, tan8);
}

void *klvr_plat_vk_proc(const char *name) { return kl_vulkan_lookup(name); }

void klvr_plat_log(const char *line) { fprintf(stderr, "  %s\n", line); }

// ---- the front door -------------------------------------------------------
// Alyx's libvr.so imports these from the libopenvr_api.so it ships. Klepton
// replaces that library rather than translating it, the same move as
// kl_openxr_claims for the Khronos loader: kl_dl asks klvr_klepton_claims
// before loading a DT_NEEDED or dlopen'd library, and resolves the imports
// through klvr_klepton_lookup. NULL for a name we do not serve, so the
// unresolved-import report still names it.
#include <stdbool.h>
extern uint32_t VR_InitInternal2(int *, int, const char *);
extern uint32_t VR_InitInternal(int *, int);
extern void VR_ShutdownInternal(void);
extern bool VR_IsHmdPresent(void);
extern bool VR_IsRuntimeInstalled(void);
extern const char *VR_RuntimePath(void);
extern bool VR_GetRuntimePath(char *, uint32_t, uint32_t *);
extern const char *VR_GetVRInitErrorAsSymbol(int);
extern const char *VR_GetVRInitErrorAsEnglishDescription(int);
extern void *VR_GetGenericInterface(const char *, int *);
extern bool VR_IsInterfaceVersionValid(const char *);
extern uint32_t VR_GetInitToken(void);
extern void *VRControlPanel(void);

int klvr_klepton_claims(const char *soname) {
    const char *base = strrchr(soname, '/');
    base = base ? base + 1 : soname;
    return strncmp(base, "libopenvr_api.so", 16) == 0;
}

void *klvr_klepton_lookup(const char *name) {
    static const struct { const char *n; void *f; } k[] = {
        { "VR_InitInternal2", (void *)VR_InitInternal2 },
        { "VR_InitInternal", (void *)VR_InitInternal },
        { "VR_ShutdownInternal", (void *)VR_ShutdownInternal },
        { "VR_IsHmdPresent", (void *)VR_IsHmdPresent },
        { "VR_IsRuntimeInstalled", (void *)VR_IsRuntimeInstalled },
        { "VR_RuntimePath", (void *)VR_RuntimePath },
        { "VR_GetRuntimePath", (void *)VR_GetRuntimePath },
        { "VR_GetVRInitErrorAsSymbol", (void *)VR_GetVRInitErrorAsSymbol },
        { "VR_GetVRInitErrorAsEnglishDescription", (void *)VR_GetVRInitErrorAsEnglishDescription },
        { "VR_GetGenericInterface", (void *)VR_GetGenericInterface },
        { "VR_IsInterfaceVersionValid", (void *)VR_IsInterfaceVersionValid },
        { "VR_GetInitToken", (void *)VR_GetInitToken },
        { "VRControlPanel", (void *)VRControlPanel },   // SteamVR-private, not VR_-prefixed
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (strcmp(k[i].n, name) == 0) return k[i].f;
    return NULL;
}
