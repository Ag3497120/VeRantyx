// Shared state of the VeRantyx OpenVR runtime: shared memory with the Mac
// encoder, pose sampling, display pacing, events. See vr_runtime.h.
#include "vr_runtime.h"
#include "json_mini.h"
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

static bool write_file(const char *path, const std::string &text) {
    if (!path || !*path) return false;
    if (text.empty()) return unlink(path) == 0 || errno == ENOENT;
    std::string temporary = std::string(path) + ".XXXXXX";
    int fd = mkstemp(temporary.data());
    if (fd < 0) return false;
    size_t written = 0;
    while (written < text.size()) {
        ssize_t n = write(fd, text.data() + written, text.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        written += (size_t)n;
    }
    bool closed = close(fd) == 0;
    bool ok = written == text.size() && closed && rename(temporary.c_str(), path) == 0;
    unlink(temporary.c_str());
    return ok;
}

bool publish_file(const char *variable, const std::string &text) {
    return write_file(getenv(variable), text);
}

// The mailbox announces the input hint before the engine finishes its first
// frame. Only the pause at the end of this load makes that hint actionable.
static std::mutex g_loading_mu;
static std::string g_loading, g_loading_prompt;
static bool g_loading_paused, g_initial_load = true, g_loading_hidden;
static void publish_loading_locked() {
    // show_message carries localized text; readiness must not depend on its wording.
    std::string prompt = g_loading_hidden || !g_loading_paused ? "" : g_loading_prompt;
    publish_file("VERANTYX_GAME_PROMPT_FILE", prompt);
    size_t end = g_loading.find_last_of('}');
    publish_file("VERANTYX_NATIVE_LOADING_FILE", end == std::string::npos ? "" :
        g_loading.substr(0, end) + ",\"visible\":" + (g_loading_hidden ? "false" : "true") +
        ",\"messageText\":" + json::quote(prompt) + "}");
}
void loading_reset() {
    std::lock_guard<std::mutex> lock(g_loading_mu);
    g_loading.clear(); g_loading_prompt.clear(); g_loading_paused = false; g_loading_hidden = false;
    publish_loading_locked();
}
void loading_message(const std::string &message) {
    json::Value body;
    if (!json::Parser(message).parse(body)) return;
    std::lock_guard<std::mutex> lock(g_loading_mu);
    const std::string type = body["type"].string_or("");
    if (type == "begin_loading") {
        g_loading = message; g_loading_prompt.clear(); g_loading_paused = false;
        g_loading_hidden = g_initial_load && body["mapName"].string_or("") == "startup";
    } else if (type == "end_loading") {
        if (!g_loading.empty()) g_initial_load = false;
        g_loading.clear(); g_loading_prompt.clear(); g_loading_paused = false; g_loading_hidden = false;
    } else if (type == "show_message") {
        if (g_loading.empty()) return; // a late hint cannot resurrect a completed load
        g_loading_prompt = body["text"].string_or("");
    } else return;
    publish_loading_locked();
}
void loading_log(const char *text) {
    if (!text || !strstr(text, "paused the game")) return;
    std::string line(text);
    const size_t end = line.find_last_not_of("\r\n ");
    if (end == std::string::npos) return;
    line.resize(end + 1);
    auto ends = [&](const char *suffix) {
        size_t n = strlen(suffix);
        return line.size() > n && line.compare(line.size() - n, n, suffix) == 0;
    };
    const bool paused = ends(" paused the game");
    if (!paused && !ends(" unpaused the game")) return;
    std::lock_guard<std::mutex> lock(g_loading_mu);
    if (g_loading.empty() || g_loading_paused == paused) return;
    g_loading_paused = paused;
    publish_loading_locked();
}

static Config g_cfg;
Config &config() { return g_cfg; }

static uint8_t *g_map;
static DoubleBufferHeader *g_hdr;
static SharedHands *g_hands;
static SharedFrame *g_frames[2];
static uint8_t *g_pixels[2];
static std::mutex g_mu;
static PoseSet g_last;
static std::mutex g_sampleMu;
static std::deque<vr::VREvent_t> g_events;
static std::mutex g_evmu;
static double g_epoch;
static std::atomic<uint64_t> g_frameCounter{0};
static std::atomic<double> g_lastVsync{0};
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
    if (end == v || *end || !std::isfinite(d) || d < lo || d > hi) return dflt;
    return d;
}

static std::string shm_path() {
    const char *p = getenv("VERANTYX_SHARED_FRAME");
    return p && *p ? p : "";
}

static bool shm_open_map() {
    if (g_map) return true;
    std::string path = shm_path();
    if (path.empty()) { log("VERANTYX_SHARED_FRAME is required; start native Alyx from VeRantyx"); return false; }
    int fd = open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) { log("cannot open shared frame file %s: %s", path.c_str(), strerror(errno)); return false; }
    struct stat sb;
    if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) ||
        ((size_t)sb.st_size < kSharedMapSize && ftruncate(fd, (off_t)kSharedMapSize) != 0)) {
        log("cannot size shared frame file %s: %s", path.c_str(), strerror(errno));
        close(fd);
        return false;
    }
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
    if (!g_map || !out) return false;
    for (int attempt = 0; attempt < 64; attempt++) {
        int32_t s1 = __atomic_load_n(&g_hdr->poseSequence, __ATOMIC_ACQUIRE);
        if (!s1 || (s1 & 1)) continue;
        SharedHands snapshot;
        memcpy(&snapshot, g_hands, sizeof snapshot);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        int32_t s2 = __atomic_load_n(&g_hdr->poseSequence, __ATOMIC_ACQUIRE);
        if (s1 == s2) { *out = snapshot; return true; }
    }
    return false;                          // never hand a torn pose to the game
}

float headset_ipd() {
    if (getenv("VERANTYX_IPD")) return g_cfg.ipd;
    if (g_map) {
        const SharedExtras *extras = (const SharedExtras *)(g_map + kExtrasOffset);
        if (__atomic_load_n(&extras->ipdMagic, __ATOMIC_ACQUIRE) == kIpdMagic) {
            float ipd = extras->ipdMeters;
            if (std::isfinite(ipd) && ipd >= 0.04f && ipd <= 0.09f) return ipd;
        }
    }
    return g_cfg.ipd;
}

struct PendingHaptic { int hand; double at; float duration, frequency, amplitude; };
static std::deque<PendingHaptic> g_haptics;
static std::mutex g_hapticMu;
static void flush_haptics_locked(double now) {
    for (auto it = g_haptics.begin(); it != g_haptics.end();) {
        if (it->at > now) { ++it; continue; }
        SharedHaptic *dst = (SharedHaptic *)(g_map + kHapticsOffset) + it->hand;
        dst->durationSeconds = it->duration; dst->frequency = it->frequency; dst->amplitude = it->amplitude;
        uint32_t seq = __atomic_load_n(&dst->sequence, __ATOMIC_RELAXED) + 1u;
        __atomic_store_n(&dst->sequence, seq ? seq : 1u, __ATOMIC_RELEASE);
        it = g_haptics.erase(it);
    }
}
bool haptic(int hand, float start, float duration, float frequency, float amplitude) {
    if (!g_map || hand < 0 || hand > 1 || !std::isfinite(start) || !std::isfinite(duration) ||
        !std::isfinite(frequency) || !std::isfinite(amplitude) || start < 0 || start > 5 || duration < 0 ||
        frequency < 0 || amplitude < 0 || amplitude > 1) return false;
    std::lock_guard<std::mutex> l(g_hapticMu);
    if (g_haptics.size() >= 64) return false;
    double now = now_seconds();
    g_haptics.push_back({ hand, now + start, std::min(duration, 5.f), frequency, amplitude });
    flush_haptics_locked(now);
    return true;
}

uint8_t *frame_begin(uint32_t combinedWidth, uint32_t height, uint32_t *writeIndex) {
    if (!g_map || !writeIndex || !combinedWidth || !height || combinedWidth > 8192 || height > 4096) return nullptr;
    if ((uint64_t)combinedWidth * height * 4 > kMaxPixelBytes) return nullptr;
    uint32_t idx = ((uint32_t)__atomic_load_n(&g_hdr->latestReadyIndex, __ATOMIC_ACQUIRE) & 1u) ^ 1u;
    *writeIndex = idx;
    // The consumer may still be copying this slot from two frames ago.
    // Invalidate its sequence before touching any pixel, so it drops that copy.
    __atomic_store_n(&g_frames[idx]->sequenceNumber, 0u, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return g_pixels[idx];
}

// The frustum each streamed eye image spans, for the headset (MacEncoder sends
// it on as a "VTAN" packet): header padding[0..7] = left, right tangents,
// padding[8] = "VTAN". Same contract as the Windows driver's
// PublishEffectiveTangents. Rewritten whenever the magic is missing, because
// the encoder clears the header when it maps the file.
static void publish_effective_tangents() {
    static const uint32_t kTangentMagic = 0x4E415456;   // "VTAN"
    if (!g_hdr) return;
    if (__atomic_load_n(&g_hdr->padding[8], __ATOMIC_ACQUIRE) == kTangentMagic) return;
    memcpy(&g_hdr->padding[0], g_cfg.tangentsLeft, sizeof g_cfg.tangentsLeft);
    memcpy(&g_hdr->padding[4], g_cfg.tangentsRight, sizeof g_cfg.tangentsRight);
    __atomic_store_n(&g_hdr->padding[8], kTangentMagic, __ATOMIC_RELEASE);
}

void frame_publish(uint32_t idx, uint32_t combinedWidth, uint32_t height, uint32_t format,
                   const float renderHeadTransform[16], double visionTimestamp) {
    publish_effective_tangents();
    SharedFrame *f = g_frames[idx & 1];
    uint32_t seq = ++g_frameSequence; if (seq == 0) seq = ++g_frameSequence;
    f->width = combinedWidth; f->height = height; f->format = format;
    memcpy(f->renderHeadTransform, renderHeadTransform, sizeof f->renderHeadTransform);
    f->visionTimestamp = visionTimestamp;
    __atomic_store_n(&f->sequenceNumber, seq, __ATOMIC_RELEASE);
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
        source = "live headset frustum";
    } else {
        // The encoder saves the headset's frustum beside this session's map.
        std::string framePath = shm_path();
        std::string p = framePath.substr(0, framePath.find_last_of('/') + 1) + "vr_verantyx_tangents.txt";
        const char *env = getenv("VERANTYX_TANGENTS_FILE");
        if (env && *env) p = env;
        FILE *f = fopen(p.c_str(), "r");
        if (f) {
            float t[8];
            if (fscanf(f, "%f %f %f %f %f %f %f %f", &t[0], &t[1], &t[2], &t[3], &t[4], &t[5], &t[6], &t[7]) == 8 &&
                valid_tangents(t) && valid_tangents(t + 4)) {
                memcpy(g_cfg.tangentsLeft, t, 16); memcpy(g_cfg.tangentsRight, t + 4, 16);
                source = "headset frustum saved beside the shared map";
            }
            fclose(f);
        }
    }
    // The frustum the game renders: the union of both eyes (see Config).
    g_cfg.sharedFrustum = env_number("VERANTYX_SHARED_FRUSTUM", 1, 0, 1) >= 1.0;
    const float *L = g_cfg.tangentsLeft, *R = g_cfg.tangentsRight;
    g_cfg.renderTangents[0] = std::min(L[0], R[0]); g_cfg.renderTangents[1] = std::max(L[1], R[1]);
    g_cfg.renderTangents[2] = std::min(L[2], R[2]); g_cfg.renderTangents[3] = std::max(L[3], R[3]);
    // Render target: VERANTYX_EYE_WIDTH/HEIGHT, or VERANTYX_EYE_PIXELS of the
    // eye's own frustum (averaged over both eyes), grown by the union's extra
    // field so the eye's own part keeps that pixel density.
    uint32_t w = (uint32_t)env_number("VERANTYX_EYE_WIDTH", 0, 0, 8192);
    uint32_t hgt = (uint32_t)env_number("VERANTYX_EYE_HEIGHT", 0, 0, 8192);
    if (!w || !hgt) {
        double pixels = env_number("VERANTYX_EYE_PIXELS", 1000000.0, 50000.0, 16000000.0);
        float ew = 0.5f * ((L[1] - L[0]) + (R[1] - R[0])), eh = 0.5f * ((L[3] - L[2]) + (R[3] - R[2]));
        float aspect = std::min(2.0f, std::max(0.5f, ew / eh));
        double height = sqrt(pixels / aspect), width = height * aspect;
        if (g_cfg.sharedFrustum) {
            const float *U = g_cfg.renderTangents;
            width *= (U[1] - U[0]) / ew;
            height *= (U[3] - U[2]) / eh;
        }
        hgt = (uint32_t)(((uint32_t)lround(height) + 1u) & ~1u);
        w = (uint32_t)(((uint32_t)lround(width) + 1u) & ~1u);
    }
    g_cfg.eyeWidth = w; g_cfg.eyeHeight = hgt;
    log("eye frustum: %s; left=(%.3f %.3f %.3f %.3f) right=(%.3f %.3f %.3f %.3f); render target %ux%u per eye; display %.0f Hz",
        source, g_cfg.tangentsLeft[0], g_cfg.tangentsLeft[1], g_cfg.tangentsLeft[2], g_cfg.tangentsLeft[3],
        g_cfg.tangentsRight[0], g_cfg.tangentsRight[1], g_cfg.tangentsRight[2], g_cfg.tangentsRight[3],
        w, hgt, g_cfg.displayHz);
    if (g_cfg.sharedFrustum) {
        float a, b, c, d, e, f2, g, h2;
        eye_crop(0, &a, &b, &c, &d); eye_crop(1, &e, &f2, &g, &h2);
        log("game renders the union frustum (%.3f %.3f %.3f %.3f) for both eyes; streamed crops left u %.3f-%.3f v %.3f-%.3f, right u %.3f-%.3f v %.3f-%.3f",
            g_cfg.renderTangents[0], g_cfg.renderTangents[1], g_cfg.renderTangents[2], g_cfg.renderTangents[3], a, b, c, d, e, f2, g, h2);
    }
}

const float *render_tangents(int eye) {
    if (g_cfg.sharedFrustum) return g_cfg.renderTangents;
    return eye == 0 ? g_cfg.tangentsLeft : g_cfg.tangentsRight;
}

void eye_crop(int eye, float *u0, float *u1, float *v0, float *v1) {
    *u0 = 0; *u1 = 1; *v0 = 0; *v1 = 1;
    if (!g_cfg.sharedFrustum) return;
    const float *t = eye == 0 ? g_cfg.tangentsLeft : g_cfg.tangentsRight;
    const float *U = g_cfg.renderTangents;
    const float uw = U[1] - U[0], vh = U[3] - U[2];
    if (uw <= 0 || vh <= 0) return;
    *u0 = (t[0] - U[0]) / uw; *u1 = (t[1] - U[0]) / uw;     // tangent x grows to the right
    // Rows: OpenVR names the tangents from the clipped plane, so image row 0
    // is the t[3] ("bottom") edge — measured: a frame rendered with an
    // asymmetric frustum matches the wide one cropped this way (NCC 0.95 vs
    // 0.74 the other way round).
    *v0 = (U[3] - t[3]) / vh; *v1 = (U[3] - t[2]) / vh;
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
    std::lock_guard<std::mutex> sampleLock(g_sampleMu);
    { std::lock_guard<std::mutex> l(g_hapticMu); if (g_map) flush_haptics_locked(now_seconds()); }
    (void)predictSeconds;
    memset(out, 0, sizeof *out);
    SharedHands h = {};
    const bool coherent = read_hands(&h);
    static SharedHands previousHands;
    static bool havePreviousHands;
    if (coherent) { previousHands = h; havePreviousHands = true; }
    else if (havePreviousHands) h = previousHands;
    // A writer may be preempted with an odd sequence. Preserve its last whole
    // packet, including the ALVR pose ID and held buttons; only advancing
    // timestamps below refresh liveness, so a stalled writer still times out.
    const bool have = coherent || havePreviousHands;
    bool validTime = have && std::isfinite(h.visionTimestamp) && h.visionTimestamp > 0;
    double now = now_seconds();
    bool fresh = have && g_presence.update(h.visionTimestamp, h.headTracked != 0 && finite16(h.headTransform), now);
    static double lastInputTs, lastInputAdvance;
    if (validTime) {
        if (h.visionTimestamp != lastInputTs) { lastInputTs = h.visionTimestamp; lastInputAdvance = now; }
    }
    const bool inputsFresh = validTime && lastInputAdvance > 0 && now - lastInputAdvance < 0.5;
    out->hands = h;
    out->fresh = fresh;
    out->inputsFresh = inputsFresh;
    out->visionTimestamp = validTime ? h.visionTimestamp : 0.0;
    if (!coherent) {
        static uint64_t misses, reused;
        static double lastLog;
        misses++; if (havePreviousHands) reused++;
        if (misses == 1 || now - lastLog >= 5.0) {
            lastLog = now;
            log("pose snapshot unavailable: reads=%llu reused=%llu inputAge=%.1fms inputsFresh=%d timestamp=%.9f",
                (unsigned long long)misses, (unsigned long long)reused,
                lastInputAdvance > 0 ? (now - lastInputAdvance) * 1000.0 : -1.0,
                (int)inputsFresh, out->visionTimestamp);
        }
    }

    // Head.
    const float *head = nullptr;
    float synth[16];
    if (fresh) { head = h.headTransform; memcpy(g_heldHead, head, sizeof g_heldHead); g_haveHeldHead = true; }
    else if (g_cfg.bench) {
        // Standing, with a slow yaw sway so the picture visibly moves.
        static const bool sway = env_number("VERANTYX_BENCH_SWAY", 1, 0, 1) >= 1.0;   // 0: hold still (image tests)
        double t = now - g_epoch, yaw = sway ? 0.25 * sin(t * 0.5) : 0.0;
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
        if (inputsFresh && tracked && finite16(t)) {
            p.mDeviceToAbsoluteTracking = matrix34_from_column_major(t);
            p.eTrackingResult = vr::TrackingResult_Running_OK;
            p.bPoseIsValid = true;
            if (h.velocityValid) {
                // The wire layout starts angular velocities at byte 287.
                // Taking a float pointer loses the packed-field alignment.
                float lv[3], av[3];
                memcpy(lv, hand == 0 ? h.leftVelocity : h.rightVelocity, sizeof lv);
                memcpy(av, hand == 0 ? h.leftAngularVelocity : h.rightAngularVelocity, sizeof av);
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

PoseSet last_poses() { std::lock_guard<std::mutex> l(g_mu); return g_last; }
void set_last_poses(const PoseSet &p) { std::lock_guard<std::mutex> l(g_mu); g_last = p; }

// ---- time ----
double display_period() { return 1.0 / g_cfg.displayHz; }
uint64_t frame_counter() { return g_frameCounter.load(std::memory_order_relaxed); }
double seconds_since_vsync() { return now_seconds() - g_lastVsync.load(std::memory_order_relaxed); }

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
static uint64_t g_stage_token;
static bool g_quit_delivered;
static void quit_status(const char *suffix) {
    const char *path = getenv("VERANTYX_NATIVE_QUIT_FILE");
    if (path && *path) write_file((std::string(path) + suffix).c_str(), "1\n");
}
void acknowledge_quit() {
    std::lock_guard<std::mutex> l(g_evmu);
    if (!g_quit_delivered) return;
    quit_status(".ack");
    log("application acknowledged quit");
}
void expect_stage_ready(uint64_t token) {
    std::lock_guard<std::mutex> l(g_evmu);
    g_stage_token = token;
}
void push_event(const vr::VREvent_t &e) { std::lock_guard<std::mutex> l(g_evmu); if (g_events.size() < 256) g_events.push_back(e); }
bool pop_event(vr::VREvent_t *e) {
    std::lock_guard<std::mutex> l(g_evmu);
    // A session-private request survives loading and is delivered only once.
    // Give it priority even when the ordinary input queue is full.
    if (!g_quit_delivered) {
        const char *path = getenv("VERANTYX_NATIVE_QUIT_FILE");
        FILE *f = path && *path ? fopen(path, "r") : nullptr;
        char request[8] = {};
        size_t n = f ? fread(request, 1, sizeof request, f) : 0;
        if (f) fclose(f);
        if (n == 5 && memcmp(request, "quit\n", 5) == 0) {
            g_quit_delivered = true;
            *e = {};
            e->eventType = vr::VREvent_Quit;
            e->trackedDeviceIndex = vr::k_unTrackedDeviceIndexInvalid;
            e->data.process.pid = (uint32_t)getpid();
            quit_status(".delivered");
            log("VREvent_Quit delivered; waiting for the application to exit");
            return true;
        }
    }
    if (g_stage_token) {
        const char *path = getenv("VERANTYX_NATIVE_STAGE_FILE");
        FILE *f = path ? fopen((std::string(path) + ".ready").c_str(), "r") : nullptr;
        unsigned long long ready = 0;
        if (f) { if (fscanf(f, "%llu", &ready) != 1) ready = 0; fclose(f); }
        if (ready == g_stage_token) {
            vr::VREvent_t event = {};
            event.eventType = vr::VREvent_Compositor_StageOverrideReady;
            g_events.push_back(event); g_stage_token = 0;
        }
    }
    if (g_events.empty()) return false;
    *e = g_events.front(); g_events.pop_front(); return true;
}

} // namespace vrrt

extern "C" void vrrt_init(const char *gameDir) { vrrt::init(gameDir); }

extern "C" void vrrt_loading_log(const char *text) { vrrt::loading_log(text); }
