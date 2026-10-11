// Shared state of the VeRantyx OpenVR runtime: shared memory with the Mac
// encoder, pose sampling, display pacing, events. See vr_runtime.h.
#include "vr_runtime.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <deque>
#include <atomic>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

namespace vrrt {

static Config g_cfg;
Config &config() { return g_cfg; }

static uint8_t *g_map;
static DoubleBufferHeader *g_hdr;
static SharedHands *g_hands;
static SharedFrame *g_frames[2];
static uint8_t *g_pixels[2];
static std::mutex g_mu;
static PoseSet g_last;
static bool g_haveLast;
static std::deque<vr::VREvent_t> g_events;
static std::mutex g_evmu;
static double g_epoch;
static uint64_t g_frameCounter;
static double g_lastVsync;
static uint32_t g_frameSequence;

void log(const char *fmt, ...) {
    char buf[1024];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    fprintf(stderr, "  [vrrt] %s\n", buf);
}

double now_seconds() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double env_number(const char *name, double dflt, double lo, double hi) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    char *end = nullptr; double d = strtod(v, &end);
    if (end == v || !std::isfinite(d) || d < lo || d > hi) return dflt;
    return d;
}

static std::string home() { const char *h = getenv("HOME"); return h ? h : "/tmp"; }

static std::string shm_path() {
    const char *p = getenv("VERANTYX_SHARED_FRAME");
    if (p && *p) return p;
    const char *wp = getenv("WINEPREFIX");
    if (wp && *wp) return std::string(wp) + "/drive_c/vr_shared_frame.dat";
    return home() + "/Verantyx_VR_Drive/SteamVR_Prefix_Wine11_Test/drive_c/vr_shared_frame.dat";
}

static bool shm_open_map() {
    if (g_map) return true;
    std::string path = shm_path();
    int fd = open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) { log("cannot open shared frame file %s: %s", path.c_str(), strerror(errno)); return false; }
    struct stat sb;
    if (fstat(fd, &sb) == 0 && (size_t)sb.st_size < kSharedMapSize && ftruncate(fd, (off_t)kSharedMapSize) != 0)
        log("cannot size %s to %zu bytes: %s", path.c_str(), kSharedMapSize, strerror(errno));
    void *m = mmap(nullptr, kSharedMapSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { log("mmap %s: %s", path.c_str(), strerror(errno)); return false; }
    g_map = (uint8_t *)m;
    g_hdr = (DoubleBufferHeader *)g_map;
    for (int i = 0; i < 2; i++) {
        g_frames[i] = (SharedFrame *)(g_map + sizeof(DoubleBufferHeader) + kFrameBlockSize * i);
        g_pixels[i] = (uint8_t *)g_frames[i] + sizeof(SharedFrame);
    }
    g_hands = (SharedHands *)(g_map + kHandsOffset);
    log("shared frame file %s mapped (%zu MB)", path.c_str(), kSharedMapSize >> 20);
    return true;
}

bool shm_ready() { return g_map != nullptr; }
uint8_t *shm_base() { return g_map; }

bool read_hands(SharedHands *out) {
    if (!g_map) return false;
    for (int attempt = 0; attempt < 64; attempt++) {
        int32_t s1 = __atomic_load_n(&g_hdr->poseSequence, __ATOMIC_ACQUIRE);
        if (s1 & 1) continue;
        memcpy(out, g_hands, sizeof *out);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        int32_t s2 = __atomic_load_n(&g_hdr->poseSequence, __ATOMIC_ACQUIRE);
        if (s1 == s2) return true;
    }
    memcpy(out, g_hands, sizeof *out);
    return true;
}

uint8_t *frame_begin(uint32_t combinedWidth, uint32_t height, uint32_t *writeIndex) {
    if (!g_map) return nullptr;
    if ((size_t)combinedWidth * height * 4 > kMaxPixelBytes) return nullptr;
    uint32_t idx = ((uint32_t)__atomic_load_n(&g_hdr->latestReadyIndex, __ATOMIC_ACQUIRE) & 1u) ^ 1u;
    *writeIndex = idx;
    return g_pixels[idx];
}

void frame_publish(uint32_t idx, uint32_t combinedWidth, uint32_t height, uint32_t format,
                   const float renderHeadTransform[16], double visionTimestamp) {
    SharedFrame *f = g_frames[idx & 1];
    uint32_t seq = ++g_frameSequence; if (seq == 0) seq = ++g_frameSequence;
    f->sequenceNumber = seq;
    f->width = combinedWidth; f->height = height; f->format = format;
    memcpy(f->renderHeadTransform, renderHeadTransform, sizeof f->renderHeadTransform);
    f->visionTimestamp = visionTimestamp;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&g_hdr->latestReadyIndex, (int32_t)idx, __ATOMIC_RELEASE);
    __atomic_store_n(&g_hdr->captureSourceMode, 2, __ATOMIC_RELEASE);     // game frames
}

// ---- configuration ----
static bool valid_tangents(const float *t) {
    return std::isfinite(t[0]) && std::isfinite(t[1]) && std::isfinite(t[2]) && std::isfinite(t[3]) &&
           t[0] < 0 && t[1] > 0 && t[2] < 0 && t[3] > 0;
}

static void latch_tangents() {
    SharedHands h = {};
    const char *source = "16:9-ish default";
    if (read_hands(&h) && valid_tangents(h.leftTangents) && valid_tangents(h.rightTangents)) {
        memcpy(g_cfg.tangentsLeft, h.leftTangents, sizeof g_cfg.tangentsLeft);
        memcpy(g_cfg.tangentsRight, h.rightTangents, sizeof g_cfg.tangentsRight);
        source = "live Vision Pro frustum";
    } else {
        // The Windows driver saved the headset's frustum beside the prefix.
        std::string p = home() + "/Verantyx_VR_Drive/SteamVR_Prefix_Wine11_Test/drive_c/vr_verantyx_tangents.txt";
        const char *env = getenv("VERANTYX_TANGENTS_FILE");
        if (env && *env) p = env;
        FILE *f = fopen(p.c_str(), "r");
        if (f) {
            float t[8];
            if (fscanf(f, "%f %f %f %f %f %f %f %f", &t[0], &t[1], &t[2], &t[3], &t[4], &t[5], &t[6], &t[7]) == 8 &&
                valid_tangents(t) && valid_tangents(t + 4)) {
                memcpy(g_cfg.tangentsLeft, t, 16); memcpy(g_cfg.tangentsRight, t + 4, 16);
                source = "Vision Pro frustum saved by a previous session";
            }
            fclose(f);
        }
    }
    // Render target: VERANTYX_EYE_WIDTH/HEIGHT, or VERANTYX_EYE_PIXELS at the eye's aspect.
    uint32_t w = (uint32_t)env_number("VERANTYX_EYE_WIDTH", 0, 0, 8192);
    uint32_t hgt = (uint32_t)env_number("VERANTYX_EYE_HEIGHT", 0, 0, 8192);
    if (!w || !hgt) {
        double pixels = env_number("VERANTYX_EYE_PIXELS", 1000000.0, 50000.0, 16000000.0);
        float aspect = (g_cfg.tangentsLeft[1] - g_cfg.tangentsLeft[0]) / (g_cfg.tangentsLeft[3] - g_cfg.tangentsLeft[2]);
        aspect = std::min(2.0f, std::max(0.5f, aspect));
        double height = sqrt(pixels / aspect);
        hgt = (uint32_t)(((uint32_t)lround(height) + 1u) & ~1u);
        w = (uint32_t)(((uint32_t)lround(height * aspect) + 1u) & ~1u);
    }
    g_cfg.eyeWidth = w; g_cfg.eyeHeight = hgt;
    log("eye frustum: %s; left=(%.3f %.3f %.3f %.3f) right=(%.3f %.3f %.3f %.3f); render target %ux%u per eye; display %.0f Hz",
        source, g_cfg.tangentsLeft[0], g_cfg.tangentsLeft[1], g_cfg.tangentsLeft[2], g_cfg.tangentsLeft[3],
        g_cfg.tangentsRight[0], g_cfg.tangentsRight[1], g_cfg.tangentsRight[2], g_cfg.tangentsRight[3],
        w, hgt, g_cfg.displayHz);
}

void init(const char *gameDir) {
    static bool done;
    if (done) return;
    done = true;
    g_epoch = now_seconds();
    g_lastVsync = g_epoch;
    g_cfg.displayHz = env_number("VERANTYX_DISPLAY_HZ", 60.0, 30.0, 144.0);
    g_cfg.bench = env_number("VERANTYX_BENCH", 0, 0, 1) >= 1.0;
    g_cfg.ipd = (float)env_number("VERANTYX_IPD", 0.063, 0.050, 0.080);
    if (gameDir) g_cfg.gameDir = gameDir;
    const char *ct = getenv("VERANTYX_CONTROLLER_TYPE");
    if (ct && *ct) g_cfg.controllerType = ct;
    shm_open_map();
    latch_tangents();
    memset(&g_last, 0, sizeof g_last);
}

// ---- poses ----
void identity34(vr::HmdMatrix34_t *m) {
    memset(m, 0, sizeof *m);
    m->m[0][0] = m->m[1][1] = m->m[2][2] = 1.0f;
}

vr::HmdMatrix34_t matrix34_from_column_major(const float *t) {
    vr::HmdMatrix34_t m;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) m.m[r][c] = t[c * 4 + r];
    return m;
}

static bool finite16(const float *t) { for (int i = 0; i < 16; i++) if (!std::isfinite(t[i])) return false; return true; }

static void column_major_identity(float *t, float x, float y, float z) {
    memset(t, 0, 16 * sizeof(float));
    t[0] = t[5] = t[10] = t[15] = 1.0f; t[12] = x; t[13] = y; t[14] = z;
}

struct Presence {
    double lastTs = 0; double lastAdvance = 0; bool baseline = false; bool freshTracked = false;
    bool update(double ts, bool tracked, double now) {
        if (!std::isfinite(ts) || ts <= 0) { freshTracked = false; return false; }
        if (!baseline || ts < lastTs) { baseline = true; freshTracked = false; }
        else if (ts > lastTs) { lastAdvance = now; freshTracked = tracked; }
        lastTs = ts;
        if (!tracked) freshTracked = false;
        return freshTracked && now - lastAdvance < 0.5;
    }
};
static Presence g_presence;
static float g_heldHead[16];
static bool g_haveHeldHead;

void sample_poses(PoseSet *out, double predictSeconds) {
    (void)predictSeconds;
    memset(out, 0, sizeof *out);
    SharedHands h = {};
    bool have = read_hands(&h);
    double now = now_seconds();
    bool fresh = have && g_presence.update(h.visionTimestamp, h.headTracked != 0 && finite16(h.headTransform), now);
    out->hands = h;
    out->fresh = fresh;
    out->visionTimestamp = have ? h.visionTimestamp : 0.0;

    // Head.
    const float *head = nullptr;
    float synth[16];
    if (fresh) { head = h.headTransform; memcpy(g_heldHead, head, sizeof g_heldHead); g_haveHeldHead = true; }
    else if (g_cfg.bench) {
        // Standing, with a slow yaw sway so the picture visibly moves.
        double t = now - g_epoch, yaw = 0.25 * sin(t * 0.5);
        column_major_identity(synth, 0.f, 1.6f, 0.f);
        synth[0] = (float)cos(yaw); synth[2] = (float)-sin(yaw); synth[8] = (float)sin(yaw); synth[10] = (float)cos(yaw);
        head = synth;
    } else if (g_haveHeldHead) head = g_heldHead;
    else { column_major_identity(synth, 0.f, 1.6f, 0.f); head = synth; }
    memcpy(out->headTransform, head, sizeof out->headTransform);

    vr::TrackedDevicePose_t &hmd = out->dev[0];
    hmd.mDeviceToAbsoluteTracking = matrix34_from_column_major(head);
    hmd.eTrackingResult = vr::TrackingResult_Running_OK;
    hmd.bPoseIsValid = true;
    hmd.bDeviceIsConnected = true;

    for (int hand = 0; hand < 2; hand++) {
        vr::TrackedDevicePose_t &p = out->dev[1 + hand];
        const uint8_t tracked = hand == 0 ? h.leftTracked : h.rightTracked;
        const float *t = hand == 0 ? h.leftTransform : h.rightTransform;
        p.bDeviceIsConnected = true;
        identity34(&p.mDeviceToAbsoluteTracking);
        if (fresh && tracked && finite16(t)) {
            p.mDeviceToAbsoluteTracking = matrix34_from_column_major(t);
            p.eTrackingResult = vr::TrackingResult_Running_OK;
            p.bPoseIsValid = true;
            if (h.velocityValid) {
                const float *lv = hand == 0 ? h.leftVelocity : h.rightVelocity;
                const float *av = hand == 0 ? h.leftAngularVelocity : h.rightAngularVelocity;
                for (int i = 0; i < 3; i++) {
                    p.vVelocity.v[i] = std::isfinite(lv[i]) ? lv[i] : 0.f;
                    p.vAngularVelocity.v[i] = std::isfinite(av[i]) ? av[i] : 0.f;
                }
            }
        } else if (g_cfg.bench) {
            // Hands in front of the head, so the game sees two controllers.
            float m[16]; column_major_identity(m, hand == 0 ? -0.2f : 0.2f, 1.2f, -0.4f);
            p.mDeviceToAbsoluteTracking = matrix34_from_column_major(m);
            p.eTrackingResult = vr::TrackingResult_Running_OK;
            p.bPoseIsValid = true;
        } else {
            p.eTrackingResult = vr::TrackingResult_Running_OutOfRange;
            p.bPoseIsValid = false;
        }
    }
}

const PoseSet &last_poses() { return g_last; }
void set_last_poses(const PoseSet &p) { std::lock_guard<std::mutex> l(g_mu); g_last = p; g_haveLast = true; }

// ---- time ----
double display_period() { return 1.0 / g_cfg.displayHz; }
uint64_t frame_counter() { return g_frameCounter; }
double seconds_since_vsync() { return now_seconds() - g_lastVsync; }

void wait_next_vsync() {
    const double period = display_period();
    double now = now_seconds();
    uint64_t n = (uint64_t)floor((now - g_epoch) / period) + 1;
    double target = g_epoch + (double)n * period;
    double remaining = target - now;
    if (remaining > 0.0015) {
        struct timespec ts; double s = remaining - 0.001;
        ts.tv_sec = (time_t)s; ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
        nanosleep(&ts, nullptr);
    }
    while (now_seconds() < target) { /* spin the last millisecond */ }
    g_lastVsync = target;
    g_frameCounter = n;
}

// ---- events ----
void push_event(const vr::VREvent_t &e) { std::lock_guard<std::mutex> l(g_evmu); if (g_events.size() < 256) g_events.push_back(e); }
bool pop_event(vr::VREvent_t *e) {
    std::lock_guard<std::mutex> l(g_evmu);
    if (g_events.empty()) return false;
    *e = g_events.front(); g_events.pop_front(); return true;
}

} // namespace vrrt

extern "C" void vrrt_init(const char *gameDir) { vrrt::init(gameDir); }
