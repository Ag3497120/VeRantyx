// VeRantyx OpenVR runtime — the vrclient.so that Valve's Linux ARM64 Half-Life:
// Alyx talks to when it runs on macOS through NativeARM/host.
//
// It is the Windows pair "driver_verantyx.dll + openvr_capture_proxy.dll"
// collapsed into one in-process runtime: poses and inputs come from the Vision
// Pro through the shared frame file the Mac encoder (MacEncoder/HardwareEncoder)
// maintains, and every Submit'ed eye pair goes back into that file for the
// encoder to stream. No SteamVR, no compositor process.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include "openvr/openvr.h"

namespace vrrt {

// ---- shared memory with MacEncoder/HardwareEncoder.swift (byte-identical to
// VRDriver/src/driver_verantyx.cpp) ----
#pragma pack(push, 1)
struct SharedFrame {
    uint32_t sequenceNumber;
    uint32_t width;          // both eyes side by side
    uint32_t height;
    uint32_t format;         // DXGI code: 28 = RGBA8, 87 = BGRA8
    float renderHeadTransform[16];
    double visionTimestamp;
};
struct SharedHands {
    float headTransform[16];
    float leftTransform[16];
    float rightTransform[16];
    uint8_t leftTracked;
    uint8_t rightTracked;
    uint8_t leftTrigger;
    uint8_t rightTrigger;
    uint32_t rightButtons;
    uint32_t leftButtons;
    float rightStickX;
    float rightStickY;
    float leftStickX;
    float leftStickY;
    float rightVelocity[3];
    float leftVelocity[3];
    double visionTimestamp;
    float leftTangents[4];
    float rightTangents[4];
    uint8_t leftGrip;
    uint8_t rightGrip;
    uint8_t headTracked;
    float leftAngularVelocity[3];
    float rightAngularVelocity[3];
    uint8_t velocityValid;
};
#pragma pack(pop)
struct DoubleBufferHeader {
    volatile int32_t latestReadyIndex;
    volatile int32_t pendingTextureHandle;
    volatile int32_t captureRequestSequence;
    volatile int32_t captureAckSequence;
    volatile int32_t captureSourceMode;
    volatile int32_t poseSequence;
    uint32_t padding[10];
};
static_assert(sizeof(SharedFrame) == 88, "SharedFrame layout");
static_assert(sizeof(SharedHands) == 312, "SharedHands layout");
static_assert(sizeof(DoubleBufferHeader) == 64, "DoubleBufferHeader layout");
constexpr size_t kMaxPixelBytes = 4096ull * 4096ull * 8ull;
constexpr size_t kFrameBlockSize = sizeof(SharedFrame) + kMaxPixelBytes;
constexpr size_t kHandsOffset = sizeof(DoubleBufferHeader) + kFrameBlockSize * 2;
constexpr size_t kSharedMapSize = kHandsOffset + sizeof(SharedHands) + 1024;

// Button bits in SharedHands::{left,right}Buttons (left ones shifted by 8).
enum : uint32_t { kBtnPrimary = 1, kBtnSecondary = 2, kBtnStickClick = 4, kBtnStickTouch = 8, kBtnMenu = 16 };

struct Config {
    double displayHz = 60.0;
    float tangentsLeft[4] = { -8.0f / 9.0f, 8.0f / 9.0f, -1.0f, 1.0f };   // left, right, top, bottom
    float tangentsRight[4] = { -8.0f / 9.0f, 8.0f / 9.0f, -1.0f, 1.0f };
    // What the game is told. Source 2's Linux ARM (Deckard) path sizes the eye
    // texture for the union of both eyes' frusta but draws every eye with the
    // left eye's horizontal range, so a headset whose eyes look in different
    // directions (Vision Pro: each eye's frustum leans outward) comes out wrong
    // in the right eye. Both eyes are therefore given the union frustum, and
    // the compositor cuts each eye's own range back out (eye_crop).
    float renderTangents[4] = { -8.0f / 9.0f, 8.0f / 9.0f, -1.0f, 1.0f };
    bool sharedFrustum = true;               // VERANTYX_SHARED_FRUSTUM=0: per-eye frusta
    uint32_t eyeWidth = 0, eyeHeight = 0;   // recommended render target, per eye
    float ipd = 0.063f;
    bool bench = false;                      // synthesize poses when no headset sends any
    std::string gameDir;                     // <root>/game/hlvr, for the action manifest
    std::string controllerType = "oculus_touch";
};
Config &config();
// The projection handed to the game for `eye` (0 left, 1 right).
const float *render_tangents(int eye);
// The part of the rendered eye image (normalised, top-left origin) that is
// that eye's own frustum: all of it unless sharedFrustum.
void eye_crop(int eye, float *u0, float *u1, float *v0, float *v1);
void init(const char *gameDir);              // reads env, opens shared memory, latches tangents

// Shared memory.
bool shm_ready();
uint8_t *shm_base();
bool read_hands(SharedHands *out);           // seqlock-consistent snapshot; false if none yet
// Publish a finished eye pair (both eyes side by side, 4 bytes per pixel).
// `format` is the DXGI code the encoder understands (28 RGBA8 / 87 BGRA8).
uint8_t *frame_begin(uint32_t combinedWidth, uint32_t height, uint32_t *writeIndex);
void frame_publish(uint32_t writeIndex, uint32_t combinedWidth, uint32_t height, uint32_t format,
                   const float renderHeadTransform[16], double visionTimestamp);

// Poses.
struct PoseSet {
    vr::TrackedDevicePose_t dev[3];          // 0 HMD, 1 left, 2 right
    float headTransform[16];                 // what the frame was rendered with
    double visionTimestamp;
    SharedHands hands;                       // the inputs that came with it
    bool fresh;                              // headset data newer than 500 ms
};
void sample_poses(PoseSet *out, double predictSeconds);
const PoseSet &last_poses();                 // as handed out by the last WaitGetPoses
void set_last_poses(const PoseSet &p);
vr::HmdMatrix34_t matrix34_from_column_major(const float *t);
void identity34(vr::HmdMatrix34_t *m);

// Time.
double now_seconds();
double display_period();
void wait_next_vsync();                      // paces WaitGetPoses
uint64_t frame_counter();
double seconds_since_vsync();

// Events (IVRSystem::PollNextEvent).
void push_event(const vr::VREvent_t &e);
bool pop_event(vr::VREvent_t *e);

void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace vrrt

// The dlsym the guest's openvr_api performs on vrclient.so.
extern "C" void *vrrt_dlsym(const char *name);
extern "C" void *VRClientCoreFactory(const char *pInterfaceName, int *pReturnCode);
