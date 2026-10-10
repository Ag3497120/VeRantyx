// IVRCompositor: the frame loop.
//
// WaitGetPoses is the frame clock: it blocks until CompositorServices wants a
// frame, pins the head pose that frame will be drawn from, and hands the game
// its render poses. Submit copies each eye into the platform's images; the
// second eye closes the frame and hands the platform the pose it was drawn
// from, so reprojection corrects against the truth rather than a guess.
#include "klvr_internal.h"
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <thread>

namespace klvr {
namespace {
std::mutex g_mu;
vr::ETrackingUniverseOrigin g_space = vr::TrackingUniverseStanding;
uint32_t g_frame_id = 0;
int g_stage = -1;             // stage being filled, -1 = none
uint32_t g_submitted = 0;     // bit per eye
Frame g_render_frame;         // the frame the current stage is being drawn for
// The head pose the game says it rendered this stage with (Submit_TextureWithPose),
// in platform space. SteamVR reprojects from this pose, not from the one it
// handed out, and so does the shim: a game that renders from a pose it read
// elsewhere would otherwise be corrected against a head it never used.
bool g_have_submit_head = false;
klvr_pose g_submit_head{};
// The copies of the stage being filled: their fences, for the frame worker.
struct StageCopies { void *device = nullptr; uint64_t fence[2] = { 0, 0 }; uint32_t n = 0; };
StageCopies g_copies;

// ---- the frame worker (KLVR_ASYNC_FRAMES=1) ---------------------------------
// Klepton's end of frame (kl_vulkan_frame_done) makes the game's queue idle
// before it publishes, on the game's thread, so the game cannot record frame
// N+1 while the GPU draws frame N: a frame costs CPU + GPU instead of the
// larger of the two. Measured on device at 4608x2880: 4.8 ms of CPU, then
// 28 ms blocked in the second Submit, 30 fps.
//
// With the worker, the second Submit hands the stage to a thread that waits
// for the shim's OWN two copies (submitted on the game's queue after its
// rendering, so their fences mean the eye images are complete) and only then
// publishes it; the game returns at once. A stage is not refilled until the
// worker has published its previous use, which is also what keeps the copy
// fences from being reset under the worker's wait.
struct Job { int stage; bool have_head; klvr_pose head; float tan[2][4]; StageCopies copies; double queued = 0; };
// Never destroyed: the worker is still waiting on them when the process
// exits, and destroying a condition variable with a waiter blocks the exit.
std::mutex &g_job_mu = *new std::mutex;
std::condition_variable &g_job_cv = *new std::condition_variable;
std::deque<Job> &g_jobs = *new std::deque<Job>;
int g_stage_busy[32];

bool async_frames() { return env_on("KLVR_ASYNC_FRAMES", false); }

void frame_worker() {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> l(g_job_mu);
            g_job_cv.wait(l, [] { return !g_jobs.empty(); });
            j = g_jobs.front();
            g_jobs.pop_front();
        }
        bool done = vk_wait_copies(j.copies.device, j.copies.fence, j.copies.n, 1.0);
        // How long the GPU took to finish the frame after the game handed it
        // over (enqueue -> copies complete): on the async path this is the
        // GPU-bound share of a frame, which the sync path hides inside "after".
        {
            static double sum, worst, t0;
            static int n;
            double ms = (klvr_plat_now() - j.queued) * 1000;
            sum += ms; n++; if (ms > worst) worst = ms;
            if (!t0) t0 = klvr_plat_now();
            if (klvr_plat_now() - t0 >= 5.0) {
                logf("frame worker: GPU finish after handover avg %.1f / worst %.1f ms over %d frames", sum / n, worst, n);
                sum = worst = 0; n = 0; t0 = klvr_plat_now();
            }
        }
        if (!done) {
            static int said;
            if (said++ < 5) logf("frame worker: stage %d's copies did not finish within 1 s; publishing anyway", j.stage);
        }
        klvr_plat_end_stage(j.stage, j.have_head ? &j.head : nullptr, j.tan);
        std::lock_guard<std::mutex> l(g_job_mu);
        g_stage_busy[j.stage & 31]--;
        g_job_cv.notify_all();
    }
}

void publish_async(const Job &j) {
    static std::once_flag started;
    std::call_once(started, [] {
        klvr_plat_async_frames();
        std::thread(frame_worker).detach();
        logf("frames are published from a worker thread (KLVR_ASYNC_FRAMES)");
    });
    std::lock_guard<std::mutex> l(g_job_mu);
    g_stage_busy[j.stage & 31]++;
    g_jobs.push_back(j);
    g_jobs.back().queued = klvr_plat_now();
    g_job_cv.notify_all();
}

// Before refilling `stage`: its last use must be published.
void wait_stage_free(int stage) {
    if (!async_frames()) return;
    std::unique_lock<std::mutex> l(g_job_mu);
    if (!g_job_cv.wait_for(l, std::chrono::seconds(2), [stage] { return g_stage_busy[stage & 31] == 0; })) {
        static int said;
        if (said++ < 5) logf("frame worker: stage %d still unpublished after 2 s; reusing it", stage);
    }
}
vr::TrackedDevicePose_t g_last_render[vr::k_unMaxTrackedDeviceCount];
vr::TrackedDevicePose_t g_last_game[vr::k_unMaxTrackedDeviceCount];
vr::Compositor_CumulativeStats g_stats{};
// A fade runs from where it currently is to its target over its duration, as
// SteamVR's does. Games poll the current value and wait for it to arrive:
// Source 2 fades to the grid before a level load and does not start loading
// until GetCurrentGridAlpha reaches 1.
struct Fade {
    float from[4] = { 0, 0, 0, 0 }, to[4] = { 0, 0, 0, 0 };
    double t0 = 0, seconds = 0;
    void value(double now, float out[4]) const {
        float k = seconds > 0 ? (float)((now - t0) / seconds) : 1.0f;
        k = k < 0 ? 0 : k > 1 ? 1 : k;
        for (int i = 0; i < 4; i++) out[i] = from[i] + (to[i] - from[i]) * k;
    }
    void start(double now, const float target[4], float secs) {
        value(now, from);
        memcpy(to, target, sizeof to);
        t0 = now;
        seconds = secs > 0 ? secs : 0;
    }
};
Fade g_fade_color, g_fade_grid;   // grid alpha is component 0

double frame_period() {
    klvr_display d{};
    klvr_plat_display(&d);
    return d.refresh_hz > 0 ? 1.0 / d.refresh_hz : 1.0 / 90;
}

// KLVR_TRACE_FRAMES=1: where each frame's time goes, from the shim's side of
// the fence, printed every 5 s as averages and worsts in ms:
//   wait    blocked inside WaitGetPoses (the shim's frame clock)
//   render  WaitGetPoses returning to the first eye's Submit (the game's CPU
//           work, plus any GPU waits it makes before submitting)
//   submit  first eye's Submit to the second's
//   after   second Submit to the next WaitGetPoses (post-submit work/waits)
// If "wait" is small and "render" or "after" is large, the time is the game's
// (or the Vulkan layer under it), not the shim's.
struct FrameTrace {
    enum { kWait, kRender, kSubmit, kAfter, kN };
    bool on = env_on("KLVR_TRACE_FRAMES", false);
    double last_mark = 0, window_start = 0, sum[kN] = {}, worst[kN] = {};
    unsigned frames = 0;
    void add(int k, double now) {
        if (last_mark > 0) { double d = now - last_mark; sum[k] += d; if (d > worst[k]) worst[k] = d; }
        last_mark = now;
    }
    void frame_done(double now) {
        if (!window_start) window_start = now;
        frames++;
        if (now - window_start < 5) return;
        double n = frames ? frames : 1;
        logf("frames: %.1f fps | ms avg/worst: wait %.1f/%.1f  render %.1f/%.1f  submit %.1f/%.1f  after %.1f/%.1f",
             frames / (now - window_start), sum[kWait] / n * 1e3, worst[kWait] * 1e3, sum[kRender] / n * 1e3,
             worst[kRender] * 1e3, sum[kSubmit] / n * 1e3, worst[kSubmit] * 1e3, sum[kAfter] / n * 1e3, worst[kAfter] * 1e3);
        *this = FrameTrace();
        on = true;
        window_start = now;
        last_mark = now;
    }
};
FrameTrace g_trace;   // under g_mu
std::atomic<double> g_game_hz{0};

// Per-frame timing for GetFrameTiming, indexed by frame id.
struct Timing { uint32_t id = 0, vsyncs = 1; double start = 0, interval = 0; };
constexpr uint32_t kTimingRing = 16;
Timing g_timing[kTimingRing];   // under g_mu

// KLVR_TRACE_POSE=1: once a second, everything the game is told about where
// it is and what it sees, next to what the platform reported. For stereo and
// tracking bugs that only show on the headset: compare the two columns.
void trace_pose(const Frame &f, const vr::TrackedDevicePose_t *rp) {
    static bool on = env_on("KLVR_TRACE_POSE", false);
    static double last;
    if (!on || klvr_plat_now() - last < 1.0) return;
    last = klvr_plat_now();
    auto mat = [](const char *what, const vr::TrackedDevicePose_t &d) {
        const auto &m = d.mDeviceToAbsoluteTracking.m;
        // position, then where -Z (forward) and +Y (up) point
        logf("  %-6s valid %d  p (%.3f %.3f %.3f)  fwd (%.2f %.2f %.2f)  up (%.2f %.2f %.2f)", what,
             (int)d.bPoseIsValid, m[0][3], m[1][3], m[2][3], -m[0][2], -m[1][2], -m[2][2], m[0][1], m[1][1], m[2][1]);
    };
    klvr_display d{};
    klvr_plat_display(&d);
    const klvr_pose &h = f.head.pose;
    logf("pose: platform head p (%.3f %.3f %.3f) q (%.3f %.3f %.3f %.3f)  display %.1f Hz, eye %ux%u", h.p[0], h.p[1],
         h.p[2], h.q[0], h.q[1], h.q[2], h.q[3], d.refresh_hz, d.eye_w, d.eye_h);
    for (int e = 0; e < 2; e++)
        logf("  eye %d tan L %.3f R %.3f U %.3f D %.3f  in head p (%.4f %.4f %.4f) q (%.3f %.3f %.3f %.3f)", e,
             d.tan[e][0], d.tan[e][1], d.tan[e][2], d.tan[e][3], d.eye_in_head[e].p[0], d.eye_in_head[e].p[1],
             d.eye_in_head[e].p[2], d.eye_in_head[e].q[0], d.eye_in_head[e].q[1], d.eye_in_head[e].q[2],
             d.eye_in_head[e].q[3]);
    mat("hmd", rp[kHmd]);
    for (int hand = 0; hand < 2; hand++) {
        klvr_tracked t{};
        klvr_plat_hand(hand, f.display_time, &t);
        klvr_controller c{};
        klvr_plat_controller(hand, &c);
        logf("  %s platform valid %d p (%.3f %.3f %.3f) q (%.3f %.3f %.3f %.3f)  buttons %#x touches %#x trig %.2f grip %.2f "
             "stick (%.2f %.2f)", hand ? "right" : "left", t.valid, t.pose.p[0], t.pose.p[1], t.pose.p[2], t.pose.q[0],
             t.pose.q[1], t.pose.q[2], t.pose.q[3], c.buttons, c.touches, c.trigger, c.grip, c.stick[0], c.stick[1]);
        mat(hand ? "right" : "left", rp[device_of_hand(hand)]);
    }
}

// What the game rendered with, against what it was handed: said for the first
// frames and then once a second under KLVR_TRACE_POSE. A difference of more
// than a hair means the game took its render pose from somewhere other than
// WaitGetPoses, which KLVR_SUBMIT_POSE (on) already follows.
void note_submit_pose(const Pose &submitted) {
    static int said;
    static double last;
    static bool trace = getenv("KLVR_TRACE_POSE") != nullptr;
    double now = klvr_plat_now();
    if (said >= 3 && !(trace && now - last >= 1.0)) return;
    said++;
    last = now;
    Pose handed = to_pose(g_render_frame.head.pose);
    Vec3 d = vsub(submitted.p, handed.p);
    Quat dq = qmul(submitted.q, qconj(handed.q));
    float ang = 2 * std::acos(std::fmin(1.0f, std::fabs(dq.w))) * 180 / 3.14159265f;
    logf("submitted render pose p (%.3f %.3f %.3f) is %.2f deg / %.4f m from the pose handed out", submitted.p.x,
         submitted.p.y, submitted.p.z, ang, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
}

// The projection the game actually rendered with, from the depth info it
// attaches (Submit_TextureWithDepth), read back as tangents in the same
// {L, R, U, D} form as the platform's. Anything but a match with the eye's
// frustum is a field-of-view mismatch: the picture is shown magnified or
// shrunk, doubled between the eyes, and swims as the head turns. So when the
// two differ the stage is closed with the game's own frustum (below), which
// places the picture where it was drawn whatever the game did with ours.
//
// Read without assuming a y convention: a y-up view space and a perspective
// matrix put NDC +-1 at tan = (+-1 + m12) / m11, whichever sign m11 has (a
// Vulkan-flipped matrix has it negative), so the larger of the two is the up
// extent. Row-major as OpenVR defines it (w = -z, m[3][2] = -1), or its
// transpose, which a column-major engine may hand over as is.
bool decode_projection(const vr::HmdMatrix44_t &in, float out[4]) {
    vr::HmdMatrix44_t m = in;
    if (std::fabs(m.m[3][2] + 1) > 1e-3f && std::fabs(m.m[2][3] + 1) < 1e-3f)
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) m.m[i][j] = in.m[j][i];
    if (std::fabs(m.m[3][2] + 1) > 1e-3f) return false;
    float a = m.m[0][0], b = m.m[0][2], c = m.m[1][1], dd = m.m[1][2];
    if (std::fabs(a) < 1e-4f || std::fabs(c) < 1e-4f) return false;
    float x1 = (1 + b) / a, x0 = (-1 + b) / a, y1 = (1 + dd) / c, y0 = (-1 + dd) / c;
    out[0] = -std::fmin(x0, x1);
    out[1] = std::fmax(x0, x1);
    out[2] = std::fmax(y0, y1);
    out[3] = -std::fmin(y0, y1);
    for (int i = 0; i < 4; i++)
        if (!(out[i] > 0.05f && out[i] < 8.0f)) return false;
    return true;
}

float g_rendered_tan[2][4];   // under g_mu
bool g_have_rendered_tan[2];  // this frame's, reset when the stage closes

void note_depth_projection(int eye, const vr::HmdMatrix44_t &m) {
    static float last[2][4];
    static int said, said_bad;
    float t4[4];
    if (!decode_projection(m, t4)) {
        if (said_bad++ < 3)
            logf("eye %d depth projection not decodable: rows (%.4f %.4f %.4f %.4f) (%.4f %.4f %.4f %.4f) (%.4f %.4f %.4f "
                 "%.4f) (%.4f %.4f %.4f %.4f)", eye, m.m[0][0], m.m[0][1], m.m[0][2], m.m[0][3], m.m[1][0], m.m[1][1],
                 m.m[1][2], m.m[1][3], m.m[2][0], m.m[2][1], m.m[2][2], m.m[2][3], m.m[3][0], m.m[3][1], m.m[3][2],
                 m.m[3][3]);
        return;
    }
    memcpy(g_rendered_tan[eye], t4, sizeof t4);
    g_have_rendered_tan[eye] = true;
    bool changed = false;
    for (int i = 0; i < 4; i++) changed |= std::fabs(t4[i] - last[eye][i]) > 1e-3f;
    if (!changed || said >= 12) return;
    said++;
    memcpy(last[eye], t4, sizeof t4);
    klvr_display d{};
    klvr_plat_display(&d);
    logf("eye %d rendered with tan L %.3f R %.3f U %.3f D %.3f (platform L %.3f R %.3f U %.3f D %.3f; m00 %.4f m02 %.4f "
         "m11 %.4f m12 %.4f)%s", eye, t4[0], t4[1], t4[2], t4[3], d.tan[eye][0], d.tan[eye][1], d.tan[eye][2],
         d.tan[eye][3], m.m[0][0], m.m[0][2], m.m[1][1], m.m[1][2],
         std::fabs(t4[0] - d.tan[eye][0]) + std::fabs(t4[1] - d.tan[eye][1]) + std::fabs(t4[2] - d.tan[eye][2]) +
                     std::fabs(t4[3] - d.tan[eye][3]) > 0.02f ? "  MISMATCH" : "");
}

// The frustum the stage is closed with: the platform's, unless the game said
// it rendered with another (KLVR_RENDERED_FOV=0 always uses the platform's).
void stage_tangents(const klvr_display &d, float tan[2][4]) {
    static bool use_rendered = env_on("KLVR_RENDERED_FOV", true);
    memcpy(tan, d.tan, sizeof d.tan);
    for (int e = 0; e < 2; e++)
        if (use_rendered && g_have_rendered_tan[e]) memcpy(tan[e], g_rendered_tan[e], sizeof g_rendered_tan[e]);
}

void close_stage_locked() {
    if (g_stage < 0) return;
    klvr_plat_frame_closed();
    klvr_display d{};
    klvr_plat_display(&d);
    static bool use_submit_pose = env_on("KLVR_SUBMIT_POSE", true);
    const klvr_pose *head = g_render_frame.head.valid ? &g_render_frame.head.pose : nullptr;
    if (use_submit_pose && g_have_submit_head) head = &g_submit_head;
    if (async_frames() && klvr_plat_presents()) {
        Job j;
        j.stage = g_stage;
        j.have_head = head != nullptr;
        if (head) j.head = *head;
        stage_tangents(d, j.tan);
        j.copies = g_copies;
        publish_async(j);
    } else {
        float tan[2][4];
        stage_tangents(d, tan);
        klvr_plat_end_stage(g_stage, head, tan);
    }
    g_copies = StageCopies();
    g_have_submit_head = false;
    g_have_rendered_tan[0] = g_have_rendered_tan[1] = false;
    g_stage = -1;
    g_submitted = 0;
    g_stats.m_nNumFramePresents++;
}
} // namespace
} // namespace klvr

using namespace klvr;
namespace klvr { namespace impl {

void IVRCompositor__SetTrackingSpace(void *, vr::ETrackingUniverseOrigin u) {
    std::lock_guard<std::mutex> l(g_mu);
    g_space = u;
}
vr::ETrackingUniverseOrigin IVRCompositor__GetTrackingSpace(void *) {
    std::lock_guard<std::mutex> l(g_mu);
    return g_space;
}

vr::EVRCompositorError IVRCompositor__WaitGetPoses(void *, vr::TrackedDevicePose_t *render, uint32_t nrender,
                                                  vr::TrackedDevicePose_t *game, uint32_t ngame) {
    {
        // A frame whose second eye never arrived (a game that submits only on
        // some frames, or crashes out of its render path) must not leave the
        // stage open forever: present what there is.
        std::lock_guard<std::mutex> l(g_mu);
        if (g_stage >= 0 && g_submitted) close_stage_locked();
        if (g_trace.on) g_trace.add(FrameTrace::kAfter, klvr_plat_now());
    }
    // Read here, not at static-init time: the visionOS app sets KLVR_TRACE_FRAMES
    // after this library's globals are constructed (it stayed off on device).
    static int trace_init;
    if (!trace_init) { trace_init = 1; std::lock_guard<std::mutex> l(g_mu); g_trace.on = env_on("KLVR_TRACE_FRAMES", false); }
    double t = klvr_plat_wait_frame();
    {   // The game's real frame rate, smoothed, for Prop_DisplayFrequency (see
        // klvr_game_hz): Alyx advances its clock by one display period per
        // rendered frame in VR, so below the display rate it runs slow.
        static double last_wait;
        double now_w = klvr_plat_now();
        if (last_wait > 0) {
            double dt = now_w - last_wait;
            if (dt > 0.002 && dt < 0.5) {
                double hz = 1.0 / dt, cur = g_game_hz.load();
                g_game_hz.store(cur > 0 ? cur + (hz - cur) * 0.1 : hz);
            }
            walk_update(dt);
        }
        last_wait = now_w;
    }
    if (g_trace.on) {
        std::lock_guard<std::mutex> l(g_mu);
        double now = klvr_plat_now();
        g_trace.add(FrameTrace::kWait, now);
        g_trace.frame_done(now);
    }
    Frame f;
    klvr_plat_head(t, &f.head);
    f.display_time = t;
    vr::ETrackingUniverseOrigin space;
    {
        std::lock_guard<std::mutex> l(g_mu);
        f.id = ++g_frame_id;
        space = g_space;
        double now = klvr_plat_now(), period = frame_period();
        const Timing &prev = g_timing[(f.id - 1) % kTimingRing];
        Timing &cur = g_timing[f.id % kTimingRing];
        cur = Timing();
        cur.id = f.id;
        cur.start = now;
        cur.interval = prev.start > 0 ? now - prev.start : period;
        long n = std::lround(cur.interval / period);
        cur.vsyncs = n < 1 ? 1 : n > 16 ? 16 : (uint32_t)n;
        g_render_frame = f;
    }
    set_current_frame(f);
    input_frame_begin(f);

    vr::TrackedDevicePose_t rp[vr::k_unMaxTrackedDeviceCount], gp[vr::k_unMaxTrackedDeviceCount];
    device_poses(t, space, rp, vr::k_unMaxTrackedDeviceCount);
    // Game poses are for the frame after this one, which is what SteamVR means
    // by them: the simulation runs a frame ahead of the renderer.
    device_poses(t + frame_period(), space, gp, vr::k_unMaxTrackedDeviceCount);
    {
        std::lock_guard<std::mutex> l(g_mu);
        memcpy(g_last_render, rp, sizeof rp);
        memcpy(g_last_game, gp, sizeof gp);
    }
    trace_pose(f, rp);
    if (render) memcpy(render, rp, sizeof(vr::TrackedDevicePose_t) * std::min<uint32_t>(nrender, vr::k_unMaxTrackedDeviceCount));
    if (game) memcpy(game, gp, sizeof(vr::TrackedDevicePose_t) * std::min<uint32_t>(ngame, vr::k_unMaxTrackedDeviceCount));
    return vr::VRCompositorError_None;
}

vr::EVRCompositorError IVRCompositor__GetLastPoses(void *, vr::TrackedDevicePose_t *render, uint32_t nrender,
                                                  vr::TrackedDevicePose_t *game, uint32_t ngame) {
    std::lock_guard<std::mutex> l(g_mu);
    if (render) memcpy(render, g_last_render, sizeof(vr::TrackedDevicePose_t) * std::min<uint32_t>(nrender, vr::k_unMaxTrackedDeviceCount));
    if (game) memcpy(game, g_last_game, sizeof(vr::TrackedDevicePose_t) * std::min<uint32_t>(ngame, vr::k_unMaxTrackedDeviceCount));
    return vr::VRCompositorError_None;
}

vr::EVRCompositorError IVRCompositor__GetLastPoseForTrackedDeviceIndex(void *, vr::TrackedDeviceIndex_t d,
                                                                      vr::TrackedDevicePose_t *render,
                                                                      vr::TrackedDevicePose_t *game) {
    if (d >= vr::k_unMaxTrackedDeviceCount) return vr::VRCompositorError_IndexOutOfRange;
    std::lock_guard<std::mutex> l(g_mu);
    if (render) *render = g_last_render[d];
    if (game) *game = g_last_game[d];
    return vr::VRCompositorError_None;
}

vr::EVRCompositorError IVRCompositor__GetPosesForFrame(void *, uint32_t, vr::TrackedDevicePose_t *poses, uint32_t n) {
    std::lock_guard<std::mutex> l(g_mu);
    if (poses) memcpy(poses, g_last_render, sizeof(vr::TrackedDevicePose_t) * std::min<uint32_t>(n, vr::k_unMaxTrackedDeviceCount));
    return vr::VRCompositorError_None;
}

static void rg_note_size(uint32_t w, uint32_t h);   // the raise guard, above GetFrameTiming
static vr::EVRCompositorError submit(vr::EVREye eye, const vr::Texture_t *tex, uint32_t array_index,
                                     const vr::VRTextureBounds_t *bounds, vr::EVRSubmitFlags flags) {
    if (!tex || !tex->handle) return vr::VRCompositorError_InvalidTexture;
    if (tex->eType != vr::TextureType_Vulkan) {
        static bool said = false;
        if (!said) { said = true; logf("Submit: texture type %d is not Vulkan; only Vulkan is served", (int)tex->eType); }
        return vr::VRCompositorError_TextureUsesUnsupportedFormat;
    }
    int e = eye == vr::Eye_Right ? 1 : 0;
    // KLVR_SWAP_EYES=1: file each eye's image under the other eye — an A/B for
    // a left/right swap anywhere between the game and the display.
    static int swap = -1;
    if (swap < 0) swap = env_on("KLVR_SWAP_EYES", false);
    if (swap) e = 1 - e;
    const vr::VRVulkanTextureData_t *vk = (const vr::VRVulkanTextureData_t *)tex->handle;
    if (flags & vr::Submit_VulkanTextureWithArrayData) {
        const vr::VRVulkanTextureArrayData_t *a = (const vr::VRVulkanTextureArrayData_t *)tex->handle;
        array_index = a->m_unArrayIndex;
    }
    {
        // What the game hands over, each time it changes (and at most 40
        // times): size, format, MSAA, the sub-rectangle and the array layer.
        // Stereo bugs on device start here.
        static int said;
        static uint64_t last_key[2];
        float u0 = bounds ? bounds->uMin : 0, u1 = bounds ? bounds->uMax : 1;
        float v0 = bounds ? bounds->vMin : 0, v1 = bounds ? bounds->vMax : 1;
        uint64_t key = (uint64_t)vk->m_nWidth << 48 ^ (uint64_t)vk->m_nHeight << 32 ^ (uint64_t)array_index << 24 ^
                       (uint64_t)(u0 * 1000) << 12 ^ (uint64_t)(u1 * 1000) ^ (uint64_t)(v1 * 1000) << 20 ^ flags;
        if (key != last_key[e] && said < 40) {
            said++;
            last_key[e] = key;
            logf("Submit %s: %ux%u fmt %u samples %u layer %u bounds u %.3f..%.3f v %.3f..%.3f flags %#x", e ? "right" : "left",
                 vk->m_nWidth, vk->m_nHeight, vk->m_nFormat, vk->m_nSampleCount, array_index, u0, u1, v0, v1, (unsigned)flags);
        }
    }
    std::lock_guard<std::mutex> l(g_mu);
    if (g_trace.on) g_trace.add(g_submitted ? FrameTrace::kSubmit : FrameTrace::kRender, klvr_plat_now());
    if (g_stage >= 0 && (g_submitted & (1u << e))) close_stage_locked();   // same eye twice: new frame
    if (g_stage < 0) {
        g_stage = klvr_plat_begin_stage();
        g_submitted = 0;
        wait_stage_free(g_stage);
    }
    if (flags & vr::Submit_TextureWithPose) {
        const vr::VRTextureWithPose_t *tp = (const vr::VRTextureWithPose_t *)tex;
        Pose floor = pmul(pinv(universe_from_floor(g_space)), from_mat34(tp->mDeviceToAbsoluteTracking));
        g_submit_head = { { floor.p.x, floor.p.y, floor.p.z }, { floor.q.x, floor.q.y, floor.q.z, floor.q.w } };
        g_have_submit_head = true;
        note_submit_pose(floor);
    }
    if (flags & vr::Submit_TextureWithDepth) {
        const vr::VRTextureDepthInfo_t &depth = (flags & vr::Submit_TextureWithPose)
            ? ((const vr::VRTextureWithPoseAndDepth_t *)tex)->depth
            : ((const vr::VRTextureWithDepth_t *)tex)->depth;
        note_depth_projection(e, depth.mProjection);
        static int said_depth;
        if (said_depth < 4 && depth.handle) {
            said_depth++;
            const vr::VRVulkanTextureData_t *dv = (const vr::VRVulkanTextureData_t *)depth.handle;
            const vr::HmdMatrix44_t &m = depth.mProjection;
            logf("eye %d depth: image %#llx %ux%u fmt %u samples %u; range %.4f..%.4f; projection z rows (%.5f %.5f %.5f %.5f) (%.5f %.5f %.5f %.5f)",
                 e, (unsigned long long)dv->m_nImage, dv->m_nWidth, dv->m_nHeight, dv->m_nFormat, dv->m_nSampleCount,
                 depth.vRange.v[0], depth.vRange.v[1], m.m[2][0], m.m[2][1], m.m[2][2], m.m[2][3], m.m[3][0], m.m[3][1], m.m[3][2], m.m[3][3]);
            // The x and y rows are the field of view the game really rendered: x' = m00 x + m02 z, so the view's
            // tangents are (-1 - m02)/m00 .. (1 - m02)/m00 (sign by the matrix's z convention). Compare with what
            // GetProjectionRaw reported; a mismatch means the picture is placed with the wrong field of view.
            float tl = (1 + m.m[0][2]) / m.m[0][0], tr = (1 - m.m[0][2]) / m.m[0][0];
            float tu = (1 - m.m[1][2]) / m.m[1][1], td = (1 + m.m[1][2]) / m.m[1][1];
            logf("eye %d depth: projection x row (%.5f %.5f %.5f %.5f) y row (%.5f %.5f %.5f %.5f) -> tangents l %.3f r %.3f "
                 "(one sign convention) / y %.3f %.3f",
                 e, m.m[0][0], m.m[0][1], m.m[0][2], m.m[0][3], m.m[1][0], m.m[1][1], m.m[1][2], m.m[1][3], tl, tr, tu, td);
        }
    }
    if (klvr_plat_presents()) {
        uint64_t fence = 0;
        void *device = nullptr;
        const vr::VRTextureDepthInfo_t *dinfo = nullptr;
        if (flags & vr::Submit_TextureWithDepth)
            dinfo = (flags & vr::Submit_TextureWithPose) ? &((const vr::VRTextureWithPoseAndDepth_t *)tex)->depth
                                                         : &((const vr::VRTextureWithDepth_t *)tex)->depth;
        if (e == 0 && vk) {
            // The level is the size of the region the game drew: Source 2 keeps one render target and moves the
            // submitted bounds when the level changes, so the texture's own size does not change.
            float u0 = bounds ? bounds->uMin : 0.f, u1 = bounds ? bounds->uMax : 1.f;
            float v0 = bounds ? bounds->vMin : 0.f, v1 = bounds ? bounds->vMax : 1.f;
            auto cl = [](float x) { return x < 0 ? 0.f : x > 1 ? 1.f : x; };   // as vk_copy_eye clamps them
            rg_note_size((uint32_t)std::lround(std::fabs(cl(u1) - cl(u0)) * vk->m_nWidth),
                         (uint32_t)std::lround(std::fabs(cl(v1) - cl(v0)) * vk->m_nHeight));
        }
        int err = vk_copy_eye(g_stage, e, vk, array_index, bounds, &fence, &device, dinfo);
        if (err) return (vr::EVRCompositorError)err;
        if (fence && g_copies.n < 2) {
            g_copies.device = device;
            g_copies.fence[g_copies.n++] = fence;
        }
    }
    g_submitted |= 1u << e;
    if (g_submitted == 3) close_stage_locked();
    return vr::VRCompositorError_None;
}

vr::EVRCompositorError IVRCompositor__Submit(void *, vr::EVREye eye, const vr::Texture_t *tex,
                                            const vr::VRTextureBounds_t *bounds, vr::EVRSubmitFlags flags) {
    return submit(eye, tex, 0, bounds, flags);
}

vr::EVRCompositorError IVRCompositor__SubmitWithArrayIndex(void *, vr::EVREye eye, const vr::Texture_t *tex,
                                                          uint32_t index, const vr::VRTextureBounds_t *bounds,
                                                          vr::EVRSubmitFlags flags) {
    return submit(eye, tex, index, bounds, flags);
}

void IVRCompositor__PostPresentHandoff(void *) {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_stage >= 0 && g_submitted) close_stage_locked();
}
void IVRCompositor__ClearLastSubmittedFrame(void *) {}

// Instance and device extensions the game must enable. The eye images are
// handed to Metal by Klepton's Vulkan layer, which adds VK_EXT_metal_objects
// to every device itself, so the game needs nothing extra.
uint32_t IVRCompositor__GetVulkanInstanceExtensionsRequired(void *, char *buf, uint32_t size) {
    return put_string("", buf, size);
}
uint32_t IVRCompositor__GetVulkanDeviceExtensionsRequired(void *, void *, char *buf, uint32_t size) {
    return put_string("", buf, size);
}

// ---- Raise guard (KLVR_RAISE_GUARD=1; off by default since 2026-09-29: the demo runs FIXED settings, auto-fidelity
// off. Kept for auto-fidelity experiments on the Mac.) -------------------------------------------------------
// Source 2's auto-fidelity raises a level after a few frames under its floor (vr_fidelity_threshold_frame_percent_min
// of the budget) and can keep climbing every second. On the headset (2026-09-29 10:03 CDT) a cheap moment took it
// from L0 to L4 (3456x2208) in 8 s; the footprint went from 6.56 to 7.94 GB and the system ended the app. Valve's
// system still makes every decision; the guard only stops it from SEEING headroom unless all of these hold:
//   - at least 5 s since the eye image last changed size (a level change either way; KLVR_RAISE_SECS),
//   - the reported GPU time (game + compositor, smoothed) under the floor for the last 5 s without a break,
//   - at least 1200 MB free (KLVR_RAISE_MEM_MB; skipped where the platform cannot tell).
// Then the true times go through for THREE frames (KLVR_RAISE_OPEN): Valve raises when the last three frames it
// was told about are all under its floor ("Last frames = a, b, c < 10.00"), so it can raise once, on the third;
// the fourth is held, so its history is no longer all under and it cannot raise again (left open, it climbed two
// levels in one second on the Mac). If no raise came of the opening, the next one is 1 s later. Otherwise a total under the floor (or no GPU time yet) is reported as
// KLVR_RAISE_HOLD (0.72) of the budget: above the raise line and below the drop line (0.85), so the level holds.
// A time above it is untouched, so drops are never delayed.
// Budget: KLVR_FID_BUDGET_MS (16.67 = vr_target_fps 60); floor: KLVR_RAISE_BELOW (0.6).
struct RaiseGuard {
    int on = -1, mem_mb = 1200, mem = -1, open_n = 3;
    float budget = 1000.0f / 60, below = 0.6f, hold = 0.72f, secs = 5.0f;
    uint32_t w = 0, h = 0, last_id = 0, held = 0, passed = 0, allow_id = 0;
    double changed = 0, under_since = 0, said = 0, cooldown = 0;
    bool allow = true;
    const char *why = "";
};
static RaiseGuard g_rg;
static float rg_env(const char *n, float d) { const char *e = getenv(n); return e && *e ? (float)atof(e) : d; }
static bool rg_enabled() {
    if (g_rg.on < 0) {
        const char *e = getenv("KLVR_RAISE_GUARD");
        g_rg.on = e && *e == '1';
        g_rg.budget = rg_env("KLVR_FID_BUDGET_MS", g_rg.budget);
        g_rg.below = rg_env("KLVR_RAISE_BELOW", g_rg.below);
        g_rg.hold = rg_env("KLVR_RAISE_HOLD", g_rg.hold);
        g_rg.secs = rg_env("KLVR_RAISE_SECS", g_rg.secs);
        g_rg.mem_mb = (int)rg_env("KLVR_RAISE_MEM_MB", (float)g_rg.mem_mb);
        g_rg.open_n = (int)rg_env("KLVR_RAISE_OPEN", (float)g_rg.open_n);
        if (g_rg.on)
            logf("raise guard: a level up only after %.0f s since the last change, %.0f s under %.1f ms, and %d MB free",
                 g_rg.secs, g_rg.secs, g_rg.below * g_rg.budget, g_rg.mem_mb);
    }
    return g_rg.on > 0;
}
// Called with g_mu held, from submit: the eye image's size is the level.
static void rg_note_size(uint32_t w, uint32_t h) {
    if (!rg_enabled() || (w == g_rg.w && h == g_rg.h)) return;
    if (g_rg.w) logf("raise guard: eye image %ux%u -> %ux%u (%u raises held off since the last change)", g_rg.w, g_rg.h, w, h, g_rg.held);
    g_rg.w = w; g_rg.h = h;
    g_rg.changed = klvr_plat_now();
    g_rg.under_since = 0;
    g_rg.held = 0;
    g_rg.allow = false;
    g_rg.allow_id = 0;
}
// Once per new frame, with the smoothed total it is about to report.
static void rg_update(uint32_t id, float total) {
    if (!rg_enabled() || id == g_rg.last_id) return;
    g_rg.last_id = id;
    double now = klvr_plat_now();
    if (total > 0 && total < g_rg.below * g_rg.budget) { if (!g_rg.under_since) g_rg.under_since = now; }
    else g_rg.under_since = 0;                      // over the floor, or no GPU time yet: not headroom
    if ((id & 31) == 0 || g_rg.mem < 0) g_rg.mem = klvr_plat_mem_free_mb();
    if (g_rg.allow_id && id - g_rg.allow_id >= (uint32_t)g_rg.open_n) {   // the open frames have passed
        g_rg.allow_id = 0;
        g_rg.cooldown = now + 1.0;
    }
    g_rg.why = g_rg.changed && now - g_rg.changed < g_rg.secs ? "level changed under 5 s ago"
             : !g_rg.under_since || now - g_rg.under_since < g_rg.secs ? "headroom not yet 5 s"
             : g_rg.mem >= 0 && g_rg.mem < g_rg.mem_mb ? "free memory under the limit"
             : now < g_rg.cooldown ? "1 s after an opening" : "";
    if (!*g_rg.why && !g_rg.allow_id) {
        g_rg.allow_id = id;
        g_rg.passed++;
        logf("raise guard: %d frames open for a raise (%.1f ms reported, %d MB free)", g_rg.open_n, total, g_rg.mem);
    }
    g_rg.allow = g_rg.allow_id && id - g_rg.allow_id < (uint32_t)g_rg.open_n;
}
static void rg_apply(float *gpu, float *comp) {
    if (!rg_enabled() || g_rg.allow) return;
    float want = g_rg.hold * g_rg.budget;
    if (*gpu + *comp >= g_rg.below * g_rg.budget) return;
    if (!*g_rg.why) g_rg.why = "no GPU time yet";
    g_rg.held++;
    if (*comp < want) *gpu = want - *comp; else *comp = want, *gpu = 0;
    double now = klvr_plat_now();
    if (now - g_rg.said >= 5.0) {
        g_rg.said = now;
        logf("raise guard: holding the level (%s; %d MB free)", g_rg.why, g_rg.mem);
    }
}

bool IVRCompositor__GetFrameTiming(void *, vr::Compositor_FrameTiming *t, uint32_t frames_ago) {
    if (!t || t->m_nSize < sizeof(uint32_t) * 2) return false;
    uint32_t size = t->m_nSize;
    memset(t, 0, size < sizeof *t ? size : sizeof *t);
    t->m_nSize = size;
    std::lock_guard<std::mutex> l(g_mu);
    if (frames_ago >= kTimingRing || frames_ago >= g_frame_id) return false;
    const Timing &x = g_timing[(g_frame_id - frames_ago) % kTimingRing];
    // A frame that took N display intervals was on screen for N vsyncs, as
    // SteamVR counts it: presented once, then re-shown N-1 times. A game that
    // advances its clock per frame by the display period needs these to know
    // it is running below the display rate.
    t->m_nFrameIndex = x.id;
    t->m_nNumFramePresents = x.vsyncs;
    t->m_nNumDroppedFrames = x.vsyncs - 1;
    t->m_nNumVSyncsReadyForUse = 1;
    t->m_nNumVSyncsToFirstView = 1;
    t->m_flSystemTimeInSeconds = x.start;
    t->m_flClientFrameIntervalMs = (float)(x.interval * 1e3);
    // GPU time for the frame: the game's own, all of it before Submit (the eye
    // copies ride along with it), and the compositor's. Total is both, as in
    // SteamVR. Source 2's auto-fidelity reads Total against its frame budget;
    // with 0 here it only ever sees headroom.
    float comp = 0, gpu = klvr_plat_gpu_ms(frames_ago, &comp), k = klvr_plat_gpu_report_scale();
    gpu *= k;
    comp *= k;
    // Smoothed for the newest frame (KLVR_GPU_EMA, default 0.1 per frame, about a quarter second at 45 fps):
    // Source 2's auto-fidelity drops on ONE frame over its critical line and raises after three under its
    // floor, and with raw GPU times it changed level 285 times in two minutes on the Mac (24 in 70 s on the
    // headset), each change reallocating the render targets. Spikes no longer reach it; sustained load still
    // moves it within a few frames. KLVR_GPU_EMA=1 reports the raw time.
    if (frames_ago == 0 && gpu > 0) {
        static float alpha = -1, g_ema, c_ema;
        static uint32_t last_id;
        if (alpha < 0) { const char *e = getenv("KLVR_GPU_EMA"); alpha = e && *e ? (float)atof(e) : 0.1f; }
        if (alpha > 0 && alpha < 1) {
            if (x.id != last_id) {
                last_id = x.id;
                g_ema = g_ema > 0 ? g_ema + alpha * (gpu - g_ema) : gpu;
                c_ema = c_ema > 0 ? c_ema + alpha * (comp - c_ema) : comp;
            }
            gpu = g_ema;
            comp = c_ema;
        }
    }
    if (frames_ago == 0) rg_update(x.id, gpu + comp);
    rg_apply(&gpu, &comp);
    t->m_flPreSubmitGpuMs = gpu;
    t->m_flCompositorRenderGpuMs = comp;
    t->m_flTotalRenderGpuMs = gpu + comp;
    return true;
}
uint32_t IVRCompositor__GetFrameTimings(void *self, vr::Compositor_FrameTiming *t, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) IVRCompositor__GetFrameTiming(self, &t[i], i);
    return n;
}
float IVRCompositor__GetFrameTimeRemaining(void *) {
    Frame f = current_frame();
    double r = f.display_time - klvr_plat_now();
    return r > 0 ? (float)r : 0.0f;
}
void IVRCompositor__GetCumulativeStats(void *, vr::Compositor_CumulativeStats *s, uint32_t size) {
    std::lock_guard<std::mutex> l(g_mu);
    g_stats.m_nPid = 0;
    if (s) memcpy(s, &g_stats, size < sizeof g_stats ? size : sizeof g_stats);
}

void IVRCompositor__FadeToColor(void *, float seconds, float r, float g, float b, float a, bool background) {
    (void)background;
    const float c[4] = { r, g, b, a };
    std::lock_guard<std::mutex> l(g_mu);
    g_fade_color.start(klvr_plat_now(), c, seconds);
}
vr::HmdColor_t IVRCompositor__GetCurrentFadeColor(void *, bool) {
    float c[4];
    std::lock_guard<std::mutex> l(g_mu);
    g_fade_color.value(klvr_plat_now(), c);
    return { c[0], c[1], c[2], c[3] };
}
void IVRCompositor__FadeGrid(void *, float seconds, bool fade_in) {
    const float a[4] = { fade_in ? 1.0f : 0.0f, 0, 0, 0 };
    std::lock_guard<std::mutex> l(g_mu);
    g_fade_grid.start(klvr_plat_now(), a, seconds);
    logf("FadeGrid(%.2f s, %s)", seconds, fade_in ? "in" : "out");
}
float IVRCompositor__GetCurrentGridAlpha(void *) {
    float a[4];
    std::lock_guard<std::mutex> l(g_mu);
    g_fade_grid.value(klvr_plat_now(), a);
    return a[0];
}
vr::EVRCompositorError IVRCompositor__SetSkyboxOverride(void *, const vr::Texture_t *, uint32_t) {
    return vr::VRCompositorError_None;
}
void IVRCompositor__ClearSkyboxOverride(void *) {}
void IVRCompositor__CompositorBringToFront(void *) {}
void IVRCompositor__CompositorGoToBack(void *) {}
void IVRCompositor__CompositorQuit(void *) {}
bool IVRCompositor__IsFullscreen(void *) { return true; }
uint32_t IVRCompositor__GetCurrentSceneFocusProcess(void *) { return 0; }
uint32_t IVRCompositor__GetLastFrameRenderer(void *) { return 0; }
bool IVRCompositor__CanRenderScene(void *) { return true; }
void IVRCompositor__ShowMirrorWindow(void *) {}
void IVRCompositor__HideMirrorWindow(void *) {}
bool IVRCompositor__IsMirrorWindowVisible(void *) { return false; }
void IVRCompositor__CompositorDumpImages(void *) {}
bool IVRCompositor__ShouldAppRenderWithLowResources(void *) { return false; }
void IVRCompositor__ForceInterleavedReprojectionOn(void *, bool) {}
void IVRCompositor__ForceReconnectProcess(void *) {}
void IVRCompositor__SuspendRendering(void *, bool) {}
bool IVRCompositor__IsMotionSmoothingEnabled(void *) { return false; }
bool IVRCompositor__IsMotionSmoothingSupported(void *) { return false; }
// SteamVR infers "loading" from the app having faded to the grid.
bool IVRCompositor__IsCurrentSceneFocusAppLoading(void *) {
    std::lock_guard<std::mutex> l(g_mu);
    return g_fade_grid.to[0] > 0;
}
// The stage (the room Alyx shows around a loading screen) is SteamVR's to
// draw; nothing here draws one, so the override is accepted and dropped.
vr::EVRCompositorError IVRCompositor__SetStageOverride_Async(void *, const char *, const vr::HmdMatrix34_t *,
                                                            const vr::Compositor_StageRenderSettings *, uint32_t) {
    return vr::VRCompositorError_None;
}
void IVRCompositor__ClearStageOverride(void *) {}
// No benchmark has run: the game falls back to its own default fidelity.
bool IVRCompositor__GetCompositorBenchmarkResults(void *, vr::Compositor_BenchmarkResults *r, uint32_t size) {
    if (r) memset(r, 0, size < sizeof *r ? size : sizeof *r);
    return false;
}
void IVRCompositor__SetExplicitTimingMode(void *, vr::EVRCompositorTimingMode) {}
vr::EVRCompositorError IVRCompositor__SubmitExplicitTimingData(void *) { return vr::VRCompositorError_None; }
vr::EVRCompositorError IVRCompositor__GetLastPosePredictionIDs(void *, uint32_t *a, uint32_t *b) {
    Frame f = current_frame();
    if (a) *a = f.id;
    if (b) *b = f.id;
    return vr::VRCompositorError_None;
}
uint32_t IVRCompositor__GetLastError(void *, char *buf, uint32_t size) { return put_string("", buf, size); }

}} // namespace klvr::impl

namespace klvr {
// What Prop_DisplayFrequency should say. In VR Alyx steps its clock by one
// display period per rendered frame (measured on the Mac: at 24 fps against a
// 72 Hz report, scripted intro events took 3x as long; desktop mode keeps
// time), so reporting the panel's rate while rendering at 28 fps ran the game
// at a quarter speed on Vision Pro. KLVR_REPORT_GAME_HZ=1 (default) reports
// the game's smoothed frame rate instead, clamped to [20, panel].
float klvr_game_hz(float panel) {
    static bool on = env_on("KLVR_REPORT_GAME_HZ", true);
    double g = g_game_hz.load();
    if (!on || g <= 0) return panel;
    if (g > panel) g = panel;
    if (g < 20) g = 20;
    // Whole hertz, moved only when the rate is more than 1 Hz off: Alyx
    // re-derives its frame timing and logs "Converting display frequency" on
    // every change of this number, which the raw smoothed rate made ~44 times
    // a second (9,220 console lines in 3.5 min on the Mac).
    static std::atomic<float> held{0};
    float h = held.load();
    if (h <= 0 || std::fabs(g - h) > 1.0) {
        h = (float)std::lround(g);
        if (h > panel) h = panel;
        held.store(h);
    }
    return h;
}
}
