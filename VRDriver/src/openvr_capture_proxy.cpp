#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <GL/gl.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/openvr.h"

#pragma pack(push, 1)
struct SharedFrame {
    uint32_t sequenceNumber;
    uint32_t width;
    uint32_t height;
    uint32_t format;
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
    // Hand velocities in SteamVR space from the headset (hand tracking fused
    // with Joy-Con inertial sensors). Linear ones use the fields above.
    float leftAngularVelocity[3];
    float rightAngularVelocity[3];
    uint8_t velocityValid;
};
#pragma pack(pop)

struct SharedHeader {
    volatile LONG latestReadyIndex;
    volatile LONG pendingTextureHandle;
    volatile LONG captureRequestSequence;
    volatile LONG captureAckSequence;
    volatile LONG captureSourceMode;
    volatile LONG poseSequence;
    LONG padding[10];
};

// Written by driver_verantyx.cpp each time it hands an HMD pose to SteamVR.
#pragma pack(push, 1)
struct PoseHistoryEntry {
    double visionTimestamp;
    float position[3];
    float rotation[4]; // w, x, y, z
    float reserved;
};

struct PoseHistory {
    volatile LONG writeCount;
    uint32_t magic;
    uint32_t reserved[2];
    PoseHistoryEntry entries[16];
};
#pragma pack(pop)

static constexpr uint32_t kCaptureSlotCount = 3;
struct StagedEye {
    ID3D11Texture2D* texture = nullptr;
    uint32_t allocatedWidth = 0;
    uint32_t allocatedHeight = 0;
    DXGI_FORMAT allocatedFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t sourceX = 0;
    uint32_t sourceY = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool copied = false;
};
struct StagedPair {
    StagedEye eyes[2];
    uint64_t order = 0;
    uint64_t queuedAt = 0;
    SharedHands renderHands = {};
    bool selected = false;
    bool pending = false;
};

static const size_t kMaxPixelBytes = 4096ull * 4096ull * 8ull;
static const size_t kFrameBlockSize = sizeof(SharedFrame) + kMaxPixelBytes;
static const size_t kHandsOffset = sizeof(SharedHeader) + 2 * kFrameBlockSize;
static const size_t kMapSize = kHandsOffset + sizeof(SharedHands) + 1024;
static const size_t kPoseHistoryOffset = kHandsOffset + 512;
static const uint32_t kPoseHistoryMagic = 0x53485056; // "VPHS"
static_assert(sizeof(SharedFrame) == 88, "Shared frame layout mismatch");
static_assert(sizeof(SharedHands) == 312, "Shared hand layout mismatch");
static_assert(sizeof(SharedHeader) == 64, "Shared header layout mismatch");
static_assert(offsetof(SharedHeader, poseSequence) == 20, "Pose sequence offset mismatch");
static_assert(sizeof(PoseHistoryEntry) == 40, "Pose history entry layout mismatch");
static_assert(sizeof(PoseHistory) == 656, "Pose history layout mismatch");
static_assert(kPoseHistoryOffset + sizeof(PoseHistory) <= kMapSize, "Pose history must fit in the shared map");

static HMODULE g_realOpenVR = nullptr;
static HANDLE g_sharedFile = INVALID_HANDLE_VALUE;
static HANDLE g_sharedMapping = nullptr;
static uint8_t* g_sharedBytes = nullptr;
static SharedHeader* g_sharedHeader = nullptr;
static SharedFrame* g_frames[2] = { nullptr, nullptr };
static uint8_t* g_pixels[2] = { nullptr, nullptr };
static StagedPair g_stagedPairs[kCaptureSlotCount] = {};
static ID3D11Device* g_captureDevice = nullptr;
static ID3D11DeviceContext* g_captureContext = nullptr;
static ID3D10Multithread* g_captureMultithread = nullptr;
static LONG g_captureLock = 0;
static LONG g_logCount = 0;
static LONG g_interfaceLogCount = 0;
static LONG g_waitLogCount = 0;
static LONG g_submitHookLogCount = 0;
static LONG g_handoffLogCount = 0;
static LONG g_pairCount = 0;
static uint32_t g_sequence = 0;
static uint32_t g_eyeWidth = 0;
static uint32_t g_eyeHeight = 0;
static DXGI_FORMAT g_eyeFormat = DXGI_FORMAT_UNKNOWN;
static uint32_t g_pairWidth = 0;
static uint32_t g_pairHeight = 0;
static DXGI_FORMAT g_pairFormat = DXGI_FORMAT_UNKNOWN;
static uint32_t g_writeIndex = 0;
static uint32_t g_eyeMask = 0;
static bool g_capturePair = false;
static uint32_t g_currentPairIndex = 0;
static uint32_t g_captureEveryN = 1;
static LONG64 g_waitPoseTicks = 0;
static LONG64 g_waitPoseCalls = 0;
static LONG g_captureContention = 0;
// The pose WaitGetPoses gave the game for the frame it is now rendering.
static SRWLOCK g_renderPoseLock = SRWLOCK_INIT;
static SharedHands g_renderPose = {};
static uint64_t g_renderPoseCapturedAt = 0;
static volatile LONG g_submitThreadId = 0;
static LONG g_renderPoseMatched = 0;
static LONG g_renderPoseFallback = 0;
// SteamVR's compositor never accepts this game's Submit (the shared texture
// cannot be opened across Wine processes), so during a long level load it
// decides the game is not responding, takes its scene focus away and answers
// WaitGetPoses with DoNotHaveFocus. Alyx then stops rendering until focus
// returns, which it never does because focus returns on the next accepted
// Submit. The game still has to see poses, so the proxy answers those calls
// itself from the last good set plus the headset's current pose.
static SRWLOCK g_lastPosesLock = SRWLOCK_INIT;
static vr::TrackedDevicePose_t g_lastRenderPoses[vr::k_unMaxTrackedDeviceCount] = {};
static uint32_t g_lastRenderPoseCount = 0;
static int g_leftHandDeviceIndex = -1;
static int g_rightHandDeviceIndex = -1;
static LONG g_focusLossCount = 0;
static uint64_t g_lastWaitReturnTicks = 0;
struct CaptureMetrics {
    uint64_t windowStart = 0;
    uint64_t submitted = 0;
    uint64_t published = 0;
    uint64_t queuedEyes = 0;
    uint64_t mapAttempts = 0;
    uint64_t mapBusy = 0;
    uint64_t dropped = 0;
    uint64_t copiedBytes = 0;
    double queueMs = 0;
    double mapMs = 0;
    double copyMs = 0;
    double ageMs = 0;
};
static CaptureMetrics g_metrics;

static uint64_t PerformanceTicks() {
    LARGE_INTEGER ticks;
    QueryPerformanceCounter(&ticks);
    return (uint64_t)ticks.QuadPart;
}

static double TicksToMilliseconds(uint64_t ticks) {
    static const double frequency = []() {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return (double)value.QuadPart;
    }();
    return (double)ticks * 1000.0 / frequency;
}

static void Log(const char* message) {
    FILE* file = fopen("C:\\vr_openvr_capture_proxy.log", "a");
    if (!file) return;
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    fprintf(file, "%04u-%02u-%02u %02u:%02u:%02u pid=%lu %s\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        (unsigned long)GetCurrentProcessId(), message);
    fclose(file);
}

static bool TryLockCapture() {
    if (InterlockedCompareExchange(&g_captureLock, 1, 0) == 0) return true;
    InterlockedIncrement(&g_captureContention);
    return false;
}

static void UnlockCapture() {
    InterlockedExchange(&g_captureLock, 0);
}

static void LogCaptureMetrics() {
    const uint64_t now = PerformanceTicks();
    if (!g_metrics.windowStart) g_metrics.windowStart = now;
    const double elapsedMs = TicksToMilliseconds(now - g_metrics.windowStart);
    if (elapsedMs < 5000.0) return;
    const LONG64 waitTicks = InterlockedExchange64(&g_waitPoseTicks, 0);
    const LONG64 waitCalls = InterlockedExchange64(&g_waitPoseCalls, 0);
    const LONG contention = InterlockedExchange(&g_captureContention, 0);
    const LONG poseMatched = InterlockedExchange(&g_renderPoseMatched, 0);
    const LONG poseFallback = InterlockedExchange(&g_renderPoseFallback, 0);
    char message[600];
    snprintf(message, sizeof(message),
        "Capture metrics: submit_fps=%.2f capture_fps=%.2f queue_ms=%.2f map_ms=%.2f copy_ms=%.2f pair_age_ms=%.2f wait_poses_ms=%.2f readback_MBps=%.2f busy=%llu dropped=%llu contention=%ld render_pose_exact=%ld render_pose_nearest=%ld",
        g_metrics.submitted * 1000.0 / elapsedMs, g_metrics.published * 1000.0 / elapsedMs,
        g_metrics.queuedEyes ? g_metrics.queueMs / g_metrics.queuedEyes : 0.0,
        g_metrics.mapAttempts ? g_metrics.mapMs / g_metrics.mapAttempts : 0.0,
        g_metrics.published ? g_metrics.copyMs / g_metrics.published : 0.0,
        g_metrics.published ? g_metrics.ageMs / g_metrics.published : 0.0,
        waitCalls ? TicksToMilliseconds((uint64_t)waitTicks) / waitCalls : 0.0,
        g_metrics.copiedBytes / (elapsedMs * 1000.0),
        (unsigned long long)g_metrics.mapBusy, (unsigned long long)g_metrics.dropped, contention,
        poseMatched, poseFallback);
    Log(message);
    g_metrics = {};
    g_metrics.windowStart = now;
}

static HMODULE LoadRealOpenVR() {
    if (g_realOpenVR) return g_realOpenVR;
    char executablePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, executablePath, MAX_PATH) != 0) {
        char* separator = strrchr(executablePath, '\\');
        if (!separator) separator = strrchr(executablePath, '/');
        if (separator) {
            strcpy_s(separator + 1, MAX_PATH - (size_t)(separator + 1 - executablePath), "openvr_api_valve.dll");
            g_realOpenVR = LoadLibraryA(executablePath);
        }
    }
    if (!g_realOpenVR) g_realOpenVR = LoadLibraryA("openvr_api_valve.dll");
    if (g_realOpenVR) Log("Loaded Valve OpenVR client from openvr_api_valve.dll");
    else Log("ERROR: could not load openvr_api_valve.dll");
    return g_realOpenVR;
}

static bool InitSharedMemory() {
    if (g_sharedBytes) return true;
    g_sharedFile = CreateFileA("C:\\vr_shared_frame.dat", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_sharedFile == INVALID_HANDLE_VALUE) {
        Log("ERROR: could not open C:\\vr_shared_frame.dat");
        return false;
    }
    g_sharedMapping = CreateFileMappingA(g_sharedFile, nullptr, PAGE_READWRITE,
        (DWORD)((kMapSize >> 32) & 0xffffffffu), (DWORD)(kMapSize & 0xffffffffu), nullptr);
    if (!g_sharedMapping) {
        Log("ERROR: could not create shared frame mapping");
        CloseHandle(g_sharedFile);
        g_sharedFile = INVALID_HANDLE_VALUE;
        return false;
    }
    g_sharedBytes = (uint8_t*)MapViewOfFile(g_sharedMapping, FILE_MAP_ALL_ACCESS, 0, 0, kMapSize);
    if (!g_sharedBytes) {
        Log("ERROR: could not map shared frame data");
        CloseHandle(g_sharedMapping);
        CloseHandle(g_sharedFile);
        g_sharedMapping = nullptr;
        g_sharedFile = INVALID_HANDLE_VALUE;
        return false;
    }
    g_sharedHeader = (SharedHeader*)g_sharedBytes;
    uint8_t* frameBase = g_sharedBytes + sizeof(SharedHeader);
    for (uint32_t i = 0; i < 2; ++i) {
        uint8_t* block = frameBase + i * kFrameBlockSize;
        g_frames[i] = (SharedFrame*)block;
        g_pixels[i] = block + sizeof(SharedFrame);
    }
    const uint32_t oldSequence = g_frames[g_sharedHeader->latestReadyIndex & 1]->sequenceNumber;
    if (oldSequence > g_sequence) g_sequence = oldSequence;
    InterlockedExchange(&g_sharedHeader->captureSourceMode, 0);
    Log("Attached to shared frame memory; SteamVR capture remains enabled until a complete app eye pair is copied");
    char setting[32] = {};
    DWORD settingLength = GetEnvironmentVariableA("VERANTYX_CAPTURE_EVERY_N_PAIRS", setting, sizeof(setting));
    if (settingLength > 0 && settingLength < sizeof(setting)) {
        const unsigned long requested = strtoul(setting, nullptr, 10);
        if (requested >= 1 && requested <= 30) g_captureEveryN = (uint32_t)requested;
    }
    return true;
}

static bool IsSupportedFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
        format == DXGI_FORMAT_R10G10B10A2_UNORM;
}

static void ReleaseStaging() {
    for (uint32_t pair = 0; pair < kCaptureSlotCount; ++pair) {
        for (uint32_t eye = 0; eye < 2; ++eye) {
            if (g_stagedPairs[pair].eyes[eye].texture) {
                g_stagedPairs[pair].eyes[eye].texture->Release();
                g_stagedPairs[pair].eyes[eye].texture = nullptr;
            }
            g_stagedPairs[pair].eyes[eye] = {};
        }
        g_stagedPairs[pair].pending = false;
        g_stagedPairs[pair].selected = false;
    }
    if (g_captureDevice) {
        g_captureDevice->Release();
        g_captureDevice = nullptr;
    }
    if (g_captureMultithread) {
        g_captureMultithread->Release();
        g_captureMultithread = nullptr;
    }
    if (g_captureContext) {
        g_captureContext->Release();
        g_captureContext = nullptr;
    }
}

static bool EnsureStaging(StagedEye* eye, const D3D11_TEXTURE2D_DESC& desc, ID3D11Device* device) {
    if (!eye || !device) return false;
    if (g_captureDevice && g_captureDevice != device) ReleaseStaging();
    if (!g_captureDevice) {
        device->AddRef();
        g_captureDevice = device;
        device->GetImmediateContext(&g_captureContext);
        if (!g_captureContext) {
            ReleaseStaging();
            return false;
        }
        if (SUCCEEDED(g_captureContext->QueryInterface(__uuidof(ID3D10Multithread),
            (void**)&g_captureMultithread)) && g_captureMultithread) {
            g_captureMultithread->SetMultithreadProtected(TRUE);
        }
    }
    // Keep each slot's capacity when auto fidelity changes the submitted crop.
    // Reallocating every slot for each resolution change discards pending frames.
    if (eye->texture && eye->allocatedWidth >= desc.Width &&
        eye->allocatedHeight >= desc.Height && eye->allocatedFormat == desc.Format) return true;

    if (eye->texture) {
        eye->texture->Release();
        eye->texture = nullptr;
    }
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    if (eye->allocatedFormat == desc.Format) {
        if (eye->allocatedWidth > stagingDesc.Width) stagingDesc.Width = eye->allocatedWidth;
        if (eye->allocatedHeight > stagingDesc.Height) stagingDesc.Height = eye->allocatedHeight;
    }
    if (stagingDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
        stagingDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    HRESULT result = device->CreateTexture2D(&stagingDesc, nullptr, &eye->texture);
    if (FAILED(result) || !eye->texture) {
        char message[160];
        snprintf(message, sizeof(message), "CreateTexture2D staging failed: 0x%08lx", (unsigned long)result);
        Log(message);
        return false;
    }
    eye->allocatedWidth = stagingDesc.Width;
    eye->allocatedHeight = stagingDesc.Height;
    eye->allocatedFormat = desc.Format;
    return true;
}

static bool QueueEyeTexture(vr::EVREye eye, const vr::Texture_t* texture,
    const vr::VRTextureBounds_t* bounds, StagedPair* pair) {
    if (!texture || !texture->handle || texture->eType != vr::TextureType_DirectX) return false;
    if (!pair) return false;
    ID3D11Texture2D* source = (ID3D11Texture2D*)texture->handle;
    D3D11_TEXTURE2D_DESC desc = {};
    source->GetDesc(&desc);
    const uint32_t sourceX = bounds ? (uint32_t)(bounds->uMin * (float)desc.Width) : 0;
    const uint32_t sourceY = bounds ? (uint32_t)(bounds->vMin * (float)desc.Height) : 0;
    const uint32_t sourceRight = bounds ? (uint32_t)(bounds->uMax * (float)desc.Width + 0.5f) : desc.Width;
    const uint32_t sourceBottom = bounds ? (uint32_t)(bounds->vMax * (float)desc.Height + 0.5f) : desc.Height;
    if (desc.Width == 0 || desc.Height == 0 || desc.Width > 4096 || desc.Height > 4096 ||
        desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
        sourceRight <= sourceX || sourceBottom <= sourceY || sourceRight > desc.Width ||
        sourceBottom > desc.Height || !IsSupportedFormat(desc.Format)) {
        LONG logIndex = InterlockedIncrement(&g_logCount);
        if (logIndex <= 8 || logIndex % 300 == 0) {
            char message[256];
            snprintf(message, sizeof(message),
                "Unsupported DirectX Submit texture: eye=%d size=%ux%u format=%u mips=%u samples=%u array=%u bind=%u misc=%u",
                (int)eye, desc.Width, desc.Height, (unsigned)desc.Format, desc.MipLevels,
                desc.SampleDesc.Count, desc.ArraySize, desc.BindFlags, desc.MiscFlags);
            Log(message);
        }
        return false;
    }
    const uint32_t eyeWidth = sourceRight - sourceX;
    const uint32_t eyeHeight = sourceBottom - sourceY;
    if (eyeWidth > 2048 || eyeHeight > 4096) return false;

    const uint32_t eyeIndex = eye == vr::Eye_Right ? 1u : 0u;
    StagedEye* stagedEye = &pair->eyes[eyeIndex];

    ID3D11Device* device = nullptr;
    source->GetDevice(&device);
    if (!device) return false;
    D3D11_TEXTURE2D_DESC croppedDesc = desc;
    croppedDesc.Width = eyeWidth;
    croppedDesc.Height = eyeHeight;
    if (!EnsureStaging(stagedEye, croppedDesc, device)) {
        device->Release();
        return false;
    }
    device->Release();
    const uint64_t queueStart = PerformanceTicks();
    if (g_captureMultithread) g_captureMultithread->Enter();
    // Alyx commonly submits two bounds within one SBS texture. Read back only
    // this eye's rectangle, instead of copying the entire SBS texture twice.
    const D3D11_BOX sourceBox = { sourceX, sourceY, 0, sourceRight, sourceBottom, 1 };
    g_captureContext->CopySubresourceRegion(stagedEye->texture, 0, 0, 0, 0, source, 0, &sourceBox);
    stagedEye->sourceX = 0;
    stagedEye->sourceY = 0;
    stagedEye->width = eyeWidth;
    stagedEye->height = eyeHeight;
    stagedEye->format = desc.Format;
    stagedEye->copied = true;
    if (g_captureMultithread) g_captureMultithread->Leave();
    g_metrics.queueMs += TicksToMilliseconds(PerformanceTicks() - queueStart);
    ++g_metrics.queuedEyes;
    g_metrics.copiedBytes += (uint64_t)eyeWidth * eyeHeight * 4;
    return true;
}

static bool ReadSharedHandsSnapshot(SharedHands* snapshot) {
    if (!snapshot || !g_sharedBytes || !g_sharedHeader) return false;
    const size_t handsOffset = kHandsOffset;
    const SharedHands* sharedHands = reinterpret_cast<const SharedHands*>(g_sharedBytes + handsOffset);
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG before = InterlockedCompareExchange(&g_sharedHeader->poseSequence, 0, 0);
        if (before & 1) continue;
        MemoryBarrier();
        memcpy(snapshot, sharedHands, sizeof(*snapshot));
        MemoryBarrier();
        const LONG after = InterlockedCompareExchange(&g_sharedHeader->poseSequence, 0, 0);
        if (before == after && !(after & 1)) return true;
    }
    return false;
}

// Copies the driver's recent HMD poses, newest first. Returns how many entries
// were stable for the whole copy (the driver may be writing one concurrently).
static uint32_t ReadPoseHistory(PoseHistoryEntry* newestFirst, uint32_t capacity) {
    if (!g_sharedBytes || !newestFirst || capacity == 0) return 0;
    const PoseHistory* history = reinterpret_cast<const PoseHistory*>(g_sharedBytes + kPoseHistoryOffset);
    if (history->magic != kPoseHistoryMagic) return 0;
    const LONG before = InterlockedCompareExchange(const_cast<volatile LONG*>(&history->writeCount), 0, 0);
    if (before <= 0) return 0;
    MemoryBarrier();
    PoseHistoryEntry copy[16];
    memcpy(copy, (const void*)history->entries, sizeof(copy));
    MemoryBarrier();
    const LONG after = InterlockedCompareExchange(const_cast<volatile LONG*>(&history->writeCount), 0, 0);
    // The driver writes slot (writeCount % 16) before advancing writeCount, so
    // entries older than after-15 may have been overwritten during the copy.
    const LONG oldestSafe = after - 15 > 0 ? after - 15 : 0;
    uint32_t count = 0;
    for (LONG index = before - 1; index >= oldestSafe && count < capacity && count < 16; --index) {
        newestFirst[count++] = copy[(uint32_t)index % 16u];
    }
    return count;
}

static void QuaternionToColumnMajor(const PoseHistoryEntry& entry, float* m) {
    const float w = entry.rotation[0], x = entry.rotation[1], y = entry.rotation[2], z = entry.rotation[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + w * z);     m[2] = 2 * (x * z - w * y);     m[3] = 0;
    m[4] = 2 * (x * y - w * z);     m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z + w * x);     m[7] = 0;
    m[8] = 2 * (x * z + w * y);     m[9] = 2 * (y * z - w * x);     m[10] = 1 - 2 * (x * x + y * y); m[11] = 0;
    m[12] = entry.position[0];      m[13] = entry.position[1];      m[14] = entry.position[2];      m[15] = 1;
}

// Identify which Vision Pro pose SteamVR returned for this frame. The driver
// sends HMD poses without velocity, so the render pose equals one pushed pose
// exactly unless the room-setup universe adds an offset; then use the newest.
static void CaptureRenderPose(const vr::TrackedDevicePose_t* renderPoses, uint32_t renderPoseCount) {
    if (!InitSharedMemory()) return;
    SharedHands hands = {};
    if (!ReadSharedHandsSnapshot(&hands)) return;
    PoseHistoryEntry history[16];
    const uint32_t count = ReadPoseHistory(history, 16);
    int best = -1;
    if (count > 0 && renderPoses && renderPoseCount > 0 && renderPoses[0].bPoseIsValid) {
        const vr::HmdMatrix34_t& pose = renderPoses[0].mDeviceToAbsoluteTracking;
        double bestError = 1e9;
        for (uint32_t i = 0; i < count; ++i) {
            float candidate[16];
            QuaternionToColumnMajor(history[i], candidate);
            double positionError = 0.0;
            double rotationError = 0.0;
            for (int row = 0; row < 3; ++row) {
                const double delta = pose.m[row][3] - candidate[12 + row];
                positionError += delta * delta;
                for (int column = 0; column < 3; ++column) {
                    rotationError += fabs(pose.m[row][column] - candidate[column * 4 + row]);
                }
            }
            positionError = sqrt(positionError);
            if (positionError < 0.002 && rotationError < 0.01 && positionError + rotationError < bestError) {
                bestError = positionError + rotationError;
                best = (int)i;
            }
        }
    }
    const PoseHistoryEntry* chosen = nullptr;
    if (best >= 0) {
        chosen = &history[best];
        InterlockedIncrement(&g_renderPoseMatched);
    } else if (count > 0) {
        chosen = &history[0];
        InterlockedIncrement(&g_renderPoseFallback);
    }
    if (chosen) {
        QuaternionToColumnMajor(*chosen, hands.headTransform);
        hands.visionTimestamp = chosen->visionTimestamp;
    }
    AcquireSRWLockExclusive(&g_renderPoseLock);
    g_renderPose = hands;
    g_renderPoseCapturedAt = PerformanceTicks();
    ReleaseSRWLockExclusive(&g_renderPoseLock);
}

static void ReadRenderPoseForSubmit(SharedHands* hands) {
    AcquireSRWLockShared(&g_renderPoseLock);
    const uint64_t capturedAt = g_renderPoseCapturedAt;
    const SharedHands pose = g_renderPose;
    ReleaseSRWLockShared(&g_renderPoseLock);
    // A stale entry means the game stopped calling WaitGetPoses (or this is the
    // first frame); the latest headset sample is then the best estimate.
    if (capturedAt != 0 && TicksToMilliseconds(PerformanceTicks() - capturedAt) < 250.0) {
        *hands = pose;
    } else {
        *hands = {};
        ReadSharedHandsSnapshot(hands);
    }
}

static bool PublishOldestStagedPair() {
    StagedPair* pair = nullptr;
    for (uint32_t index = 0; index < kCaptureSlotCount; ++index) {
        if (!g_stagedPairs[index].pending) continue;
        if (!pair || g_stagedPairs[index].order < pair->order) pair = &g_stagedPairs[index];
    }
    if (!pair) return false;
    if (!pair->eyes[0].copied || !pair->eyes[1].copied ||
        pair->eyes[0].width != pair->eyes[1].width ||
        pair->eyes[0].height != pair->eyes[1].height ||
        pair->eyes[0].format != pair->eyes[1].format) {
        pair->pending = false;
        return false;
    }

    ID3D11DeviceContext* context = g_captureContext;
    if (!context) return false;
    const uint64_t mapStart = PerformanceTicks();
    if (g_captureMultithread) g_captureMultithread->Enter();

    D3D11_MAPPED_SUBRESOURCE mapped[2] = {};
    bool isMapped[2] = { false, false };
    HRESULT mapResult[2] = { E_FAIL, E_FAIL };
    for (uint32_t eye = 0; eye < 2; ++eye) {
        mapResult[eye] = context->Map(pair->eyes[eye].texture, 0, D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped[eye]);
        isMapped[eye] = SUCCEEDED(mapResult[eye]) && mapped[eye].pData;
    }
    ++g_metrics.mapAttempts;
    g_metrics.mapMs += TicksToMilliseconds(PerformanceTicks() - mapStart);

    if (!isMapped[0] || !isMapped[1]) {
        for (uint32_t eye = 0; eye < 2; ++eye) {
            if (isMapped[eye]) context->Unmap(pair->eyes[eye].texture, 0);
        }
        if ((FAILED(mapResult[0]) && mapResult[0] != DXGI_ERROR_WAS_STILL_DRAWING) ||
            (FAILED(mapResult[1]) && mapResult[1] != DXGI_ERROR_WAS_STILL_DRAWING)) {
            LONG logIndex = InterlockedIncrement(&g_logCount);
            if (logIndex <= 8 || logIndex % 300 == 0) {
                char message[160];
                snprintf(message, sizeof(message), "Nonblocking staging read failed: left=0x%08lx right=0x%08lx",
                    (unsigned long)mapResult[0], (unsigned long)mapResult[1]);
                Log(message);
            }
            pair->pending = false;
            ++g_metrics.dropped;
        } else {
            ++g_metrics.mapBusy;
        }
        if (g_captureMultithread) g_captureMultithread->Leave();
        return false;
    }
    // Only this capture path owns the mapped staging textures. Release the
    // game's immediate-context lock while copying CPU memory, then reacquire
    // it for Unmap. The capture lock keeps these slots from being reused.
    if (g_captureMultithread) g_captureMultithread->Leave();
    const uint64_t copyStart = PerformanceTicks();

    const uint32_t eyeWidth = pair->eyes[0].width;
    const uint32_t eyeHeight = pair->eyes[0].height;
    const uint32_t combinedWidth = eyeWidth * 2;
    const size_t combinedRowBytes = (size_t)combinedWidth * 4;
    const uint32_t writeIndex = static_cast<uint32_t>(InterlockedCompareExchange(
        &g_sharedHeader->latestReadyIndex, 0, 0)) ^ 1u;
    uint8_t* destination = g_pixels[writeIndex];
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const StagedEye& info = pair->eyes[eye];
        const uint32_t destinationX = eye == 1 ? eyeWidth : 0;
        const uint8_t* sourceBytes = static_cast<const uint8_t*>(mapped[eye].pData);
        for (uint32_t row = 0; row < eyeHeight; ++row) {
            uint8_t* destinationRow = destination + (size_t)row * combinedRowBytes + (size_t)destinationX * 4;
            if (info.format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                const uint32_t* sourceRow = reinterpret_cast<const uint32_t*>(
                    sourceBytes + (size_t)(info.sourceY + row) * mapped[eye].RowPitch) + info.sourceX;
                for (uint32_t column = 0; column < eyeWidth; ++column) {
                    const uint32_t packed = sourceRow[column];
                    destinationRow[column * 4 + 0] = (uint8_t)((((packed >> 20) & 0x3ffu) * 255u + 511u) / 1023u);
                    destinationRow[column * 4 + 1] = (uint8_t)((((packed >> 10) & 0x3ffu) * 255u + 511u) / 1023u);
                    destinationRow[column * 4 + 2] = (uint8_t)(((packed & 0x3ffu) * 255u + 511u) / 1023u);
                    destinationRow[column * 4 + 3] = (uint8_t)((((packed >> 30) & 0x3u) * 255u + 1u) / 3u);
                }
            } else {
                memcpy(destinationRow,
                    sourceBytes + (size_t)(info.sourceY + row) * mapped[eye].RowPitch + (size_t)info.sourceX * 4,
                    (size_t)eyeWidth * 4);
            }
        }
    }

    g_metrics.copyMs += TicksToMilliseconds(PerformanceTicks() - copyStart);
    if (g_captureMultithread) g_captureMultithread->Enter();
    for (uint32_t eye = 0; eye < 2; ++eye) context->Unmap(pair->eyes[eye].texture, 0);
    if (g_captureMultithread) g_captureMultithread->Leave();

    SharedFrame* frame = g_frames[writeIndex];
    frame->sequenceNumber = ++g_sequence;
    if (frame->sequenceNumber == 0) frame->sequenceNumber = ++g_sequence;
    frame->width = combinedWidth;
    frame->height = eyeHeight;
    frame->format = pair->eyes[0].format == DXGI_FORMAT_R10G10B10A2_UNORM
        ? DXGI_FORMAT_B8G8R8A8_UNORM
        : (pair->eyes[0].format == DXGI_FORMAT_R8G8B8A8_TYPELESS
            ? DXGI_FORMAT_R8G8B8A8_UNORM : pair->eyes[0].format);

    const SharedHands& hands = pair->renderHands;
    memcpy(frame->renderHeadTransform, hands.headTransform, sizeof(frame->renderHeadTransform));
    if (frame->renderHeadTransform[0] == 0.0f && frame->renderHeadTransform[5] == 0.0f &&
        frame->renderHeadTransform[10] == 0.0f) {
        memset(frame->renderHeadTransform, 0, sizeof(frame->renderHeadTransform));
        frame->renderHeadTransform[0] = 1.0f;
        frame->renderHeadTransform[5] = 1.0f;
        frame->renderHeadTransform[10] = 1.0f;
        frame->renderHeadTransform[15] = 1.0f;
    }
    frame->visionTimestamp = hands.visionTimestamp;
    MemoryBarrier();
    InterlockedExchange(&g_sharedHeader->latestReadyIndex, (LONG)writeIndex);
    MemoryBarrier();
    InterlockedExchange(&g_sharedHeader->captureSourceMode, 2);
    pair->pending = false;
    ++g_metrics.published;
    g_metrics.ageMs += TicksToMilliseconds(PerformanceTicks() - pair->queuedAt);
    static bool firstCaptureLogged = false;
    if (!firstCaptureLogged || frame->sequenceNumber % 90 == 0) {
        firstCaptureLogged = true;
        char message[160];
        snprintf(message, sizeof(message), "Captured game eye pair %u (%ux%u format=%u) using deferred readback",
            frame->sequenceNumber, frame->width, frame->height, frame->format);
        Log(message);
    }
    return true;
}

// ---- OpenGL games (Google Earth VR) ----------------------------------------
//
// An OpenGL game submits a GL texture name. SteamVR on Windows hands such a
// texture to its D3D11 compositor through WGL_NV_DX_interop, which Wine on
// macOS does not have, so the proxy reads the texture itself: Submit runs on
// the game's render thread with its GL context current, so glGetTexImage on
// that context returns the eye image. Synchronous (the GL path has no staging
// ring), which costs a few milliseconds a frame; good enough to bring an
// OpenGL title up. Rows are flipped unless the game's bounds already flip v
// (vMin > vMax), because OpenVR treats a GL texture as bottom-up.
#ifndef GL_PIXEL_PACK_BUFFER_BINDING
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#endif
#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_PACK_ROW_LENGTH
#define GL_PACK_ROW_LENGTH 0x0D02
#endif
typedef void (APIENTRY *GlBindBufferFn)(GLenum, GLuint);
struct GlEye {
    uint8_t* pixels = nullptr;   // RGBA8, top row first, cropped to the bounds
    size_t capacity = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool ready = false;
};
static GlEye g_glEyes[2];
static uint8_t* g_glTexture = nullptr;
static size_t g_glTextureCapacity = 0;
static SharedHands g_glPairHands = {};
static LONG g_glLogCount = 0;

static bool ReadGlEye(vr::EVREye eye, const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds) {
    if (!wglGetCurrentContext()) return false;
    const GLuint name = (GLuint)(uintptr_t)texture->handle;
    GLint previous = 0, packBuffer = 0, packAlignment = 4, packRowLength = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &packRowLength);
    static GlBindBufferFn bindBuffer = (GlBindBufferFn)wglGetProcAddress("glBindBuffer");
    if (packBuffer && bindBuffer) bindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, name);
    GLint width = 0, height = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    bool ok = width > 0 && height > 0 && width <= 8192 && height <= 8192;
    if (ok) {
        const size_t bytes = (size_t)width * height * 4;
        if (g_glTextureCapacity < bytes) {
            free(g_glTexture);
            g_glTexture = (uint8_t*)malloc(bytes);
            g_glTextureCapacity = g_glTexture ? bytes : 0;
        }
        ok = g_glTexture != nullptr;
        if (ok) {
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glPixelStorei(GL_PACK_ROW_LENGTH, 0);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_glTexture);
            ok = glGetError() == GL_NO_ERROR;
        }
    }
    glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, packRowLength);
    glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
    if (packBuffer && bindBuffer) bindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)packBuffer);
    if (!ok) {
        LONG n = InterlockedIncrement(&g_glLogCount);
        if (n <= 8 || n % 300 == 0) {
            char message[160];
            snprintf(message, sizeof(message), "OpenGL Submit texture %u could not be read (eye=%d size=%dx%d)",
                name, (int)eye, width, height);
            Log(message);
        }
        return false;
    }
    float u0 = 0, u1 = 1, v0 = 0, v1 = 1;
    if (bounds) { u0 = bounds->uMin; u1 = bounds->uMax; v0 = bounds->vMin; v1 = bounds->vMax; }
    const bool flipped = v0 > v1;  // the game already turned it top-down
    const float ulo = u0 < u1 ? u0 : u1, uhi = u0 < u1 ? u1 : u0;
    const float vlo = v0 < v1 ? v0 : v1, vhi = v0 < v1 ? v1 : v0;
    const uint32_t x0 = (uint32_t)(ulo * width + 0.5f), x1 = (uint32_t)(uhi * width + 0.5f);
    const uint32_t y0 = (uint32_t)(vlo * height + 0.5f), y1 = (uint32_t)(vhi * height + 0.5f);
    if (x1 <= x0 || y1 <= y0 || x1 > (uint32_t)width || y1 > (uint32_t)height) return false;
    GlEye& out = g_glEyes[eye == vr::Eye_Right ? 1 : 0];
    out.width = x1 - x0;
    out.height = y1 - y0;
    const size_t need = (size_t)out.width * out.height * 4;
    if (out.capacity < need) {
        free(out.pixels);
        out.pixels = (uint8_t*)malloc(need);
        out.capacity = out.pixels ? need : 0;
        if (!out.pixels) return false;
    }
    for (uint32_t row = 0; row < out.height; ++row) {
        // glGetTexImage's row 0 is the bottom of the texture.
        const uint32_t sourceRow = flipped ? (y0 + row) : (y1 - 1 - row);
        memcpy(out.pixels + (size_t)row * out.width * 4,
            g_glTexture + ((size_t)sourceRow * width + x0) * 4, (size_t)out.width * 4);
    }
    out.ready = true;
    return true;
}

static void PublishGlPair() {
    const GlEye& left = g_glEyes[0];
    const GlEye& right = g_glEyes[1];
    if (!left.ready || !right.ready || left.width != right.width || left.height != right.height) return;
    const uint32_t eyeWidth = left.width, eyeHeight = left.height;
    const size_t combinedRowBytes = (size_t)eyeWidth * 2 * 4;
    if (combinedRowBytes * eyeHeight > kMaxPixelBytes) return;
    const uint32_t writeIndex = static_cast<uint32_t>(InterlockedCompareExchange(
        &g_sharedHeader->latestReadyIndex, 0, 0)) ^ 1u;
    uint8_t* destination = g_pixels[writeIndex];
    for (uint32_t row = 0; row < eyeHeight; ++row) {
        memcpy(destination + row * combinedRowBytes, left.pixels + (size_t)row * eyeWidth * 4, (size_t)eyeWidth * 4);
        memcpy(destination + row * combinedRowBytes + (size_t)eyeWidth * 4,
            right.pixels + (size_t)row * eyeWidth * 4, (size_t)eyeWidth * 4);
    }
    SharedFrame* frame = g_frames[writeIndex];
    frame->sequenceNumber = ++g_sequence;
    if (frame->sequenceNumber == 0) frame->sequenceNumber = ++g_sequence;
    frame->width = eyeWidth * 2;
    frame->height = eyeHeight;
    frame->format = DXGI_FORMAT_R8G8B8A8_UNORM;
    memcpy(frame->renderHeadTransform, g_glPairHands.headTransform, sizeof(frame->renderHeadTransform));
    if (frame->renderHeadTransform[0] == 0.0f && frame->renderHeadTransform[5] == 0.0f &&
        frame->renderHeadTransform[10] == 0.0f) {
        memset(frame->renderHeadTransform, 0, sizeof(frame->renderHeadTransform));
        frame->renderHeadTransform[0] = frame->renderHeadTransform[5] = 1.0f;
        frame->renderHeadTransform[10] = frame->renderHeadTransform[15] = 1.0f;
    }
    frame->visionTimestamp = g_glPairHands.visionTimestamp;
    MemoryBarrier();
    InterlockedExchange(&g_sharedHeader->latestReadyIndex, (LONG)writeIndex);
    MemoryBarrier();
    InterlockedExchange(&g_sharedHeader->captureSourceMode, 2);
    ++g_metrics.published;
    static bool firstLogged = false;
    if (!firstLogged || frame->sequenceNumber % 90 == 0) {
        firstLogged = true;
        char message[160];
        snprintf(message, sizeof(message), "Captured game eye pair %u (%ux%u format=%u) using OpenGL readback",
            frame->sequenceNumber, frame->width, frame->height, frame->format);
        Log(message);
    }
}

static void CaptureSubmitGl(vr::EVREye eye, const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds) {
    InterlockedExchange(&g_submitThreadId, (LONG)GetCurrentThreadId());
    InterlockedExchange(&g_sharedHeader->padding[9], (LONG)GetTickCount());
    if (!TryLockCapture()) return;
    if (eye == vr::Eye_Left) {
        ++g_metrics.submitted;
        g_glEyes[0].ready = g_glEyes[1].ready = false;
        ReadRenderPoseForSubmit(&g_glPairHands);
    }
    const bool read = ReadGlEye(eye, texture, bounds);
    if (read && eye == vr::Eye_Right) PublishGlPair();
    LogCaptureMetrics();
    UnlockCapture();
}

static void CaptureSubmit(vr::EVREye eye, const vr::Texture_t* texture,
    const vr::VRTextureBounds_t* bounds) {
    LONG logIndex = InterlockedIncrement(&g_logCount);
    if (logIndex <= 12 || logIndex % 300 == 0) {
        char message[224];
        snprintf(message, sizeof(message), "Submit #%ld eye=%d textureStruct=%p",
            logIndex, (int)eye, texture);
        Log(message);
    }
    if (!texture) return;
    if (logIndex <= 12 || logIndex % 300 == 0) {
        char message[224];
        snprintf(message, sizeof(message), "Submit texture details type=%d handle=%p colorSpace=%d",
            (int)texture->eType, texture->handle, (int)texture->eColorSpace);
        Log(message);
    }
    if (texture->eType == vr::TextureType_OpenGL && texture->handle && InitSharedMemory()) {
        CaptureSubmitGl(eye, texture, bounds);
        return;
    }
    if (texture->eType != vr::TextureType_DirectX || !texture->handle || !InitSharedMemory()) return;
    InterlockedExchange(&g_submitThreadId, (LONG)GetCurrentThreadId());
    // Heartbeat for the driver: while the game submits, the driver leaves
    // capture to this proxy; when it stops (level load) the driver shows
    // SteamVR's own compositor output (the loading screen) instead.
    InterlockedExchange(&g_sharedHeader->padding[9], (LONG)GetTickCount());
    if (!TryLockCapture()) return;
    if (eye == vr::Eye_Left) {
        ++g_metrics.submitted;
        PublishOldestStagedPair();
        const uint32_t pairNumber = (uint32_t)InterlockedIncrement(&g_pairCount);
        g_currentPairIndex = pairNumber % kCaptureSlotCount;
        g_capturePair = (pairNumber % g_captureEveryN) == 0;
        g_eyeMask = 0;
        StagedPair& pair = g_stagedPairs[g_currentPairIndex];
        if (pair.pending) {
            ++g_metrics.dropped;
            LONG dropped = InterlockedIncrement(&g_logCount);
            if (dropped <= 8 || dropped % 300 == 0) Log("Dropping an old staged eye pair to keep Submit nonblocking");
        }
        pair.pending = false;
        pair.order = pairNumber;
        pair.queuedAt = PerformanceTicks();
        // Tag the pair with the pose WaitGetPoses returned for this frame, not
        // a newer sample: the headset reprojects from exactly this pose.
        ReadRenderPoseForSubmit(&pair.renderHands);
        pair.selected = g_capturePair;
        pair.eyes[0].copied = false;
        pair.eyes[1].copied = false;
    }
    if (!g_capturePair) {
        if (eye == vr::Eye_Right) g_capturePair = false;
        LogCaptureMetrics();
        UnlockCapture();
        return;
    }

    StagedPair& pair = g_stagedPairs[g_currentPairIndex];
    const bool copied = QueueEyeTexture(eye, texture, bounds, &pair);
    if (copied && eye == vr::Eye_Left) {
        g_eyeMask |= 1u;
    } else if (copied && eye == vr::Eye_Right) {
        const StagedEye& leftEye = pair.eyes[0];
        const StagedEye& rightEye = pair.eyes[1];
        if (leftEye.width != rightEye.width || leftEye.height != rightEye.height || leftEye.format != rightEye.format) {
            Log("Skipping eye pair because left and right texture dimensions or formats differ");
        } else {
            g_eyeMask |= 2u;
            pair.pending = true;
        }
    }
    if (eye == vr::Eye_Right) {
        if (!copied || g_eyeMask != 3u) {
            g_eyeMask = 0;
            pair.pending = false;
        }
        g_capturePair = false;
    }
    LogCaptureMetrics();
    UnlockCapture();
}

typedef vr::EVRCompositorError (VR_CALLTYPE *SubmitFn)(void*, vr::EVREye, const vr::Texture_t*, const vr::VRTextureBounds_t*, vr::EVRSubmitFlags);
typedef vr::EVRCompositorError (VR_CALLTYPE *WaitGetPosesFn)(void*, vr::TrackedDevicePose_t*, uint32_t, vr::TrackedDevicePose_t*, uint32_t);
typedef void (VR_CALLTYPE *PostPresentHandoffFn)(void*);

struct CompositorHook {
    void** vtable;
    size_t submitIndex;
    size_t postPresentHandoffIndex;
    WaitGetPosesFn waitGetPoses;
    SubmitFn submit;
    PostPresentHandoffFn postPresentHandoff;
};

static vr::EVRCompositorError VR_CALLTYPE HookedSubmit(void* self, vr::EVREye eye,
    const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags);

static CompositorHook g_compositorHooks[8] = {};
static volatile LONG g_compositorHookCount = 0;
static SRWLOCK g_compositorHookLock = SRWLOCK_INIT;

static CompositorHook* FindCompositorHook(void* self) {
    if (!self) return nullptr;
    void** vtable = *(void***)self;
    LONG count = InterlockedCompareExchange(&g_compositorHookCount, 0, 0);
    for (LONG i = 0; i < count; ++i) {
        if (g_compositorHooks[i].vtable == vtable) return &g_compositorHooks[i];
    }
    return nullptr;
}

static void ColumnMajorToHmdMatrix(const float* m, vr::HmdMatrix34_t* out) {
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) out->m[row][column] = m[column * 4 + row];
        out->m[row][3] = m[12 + row];
    }
}

static bool TransformIsFinite(const float* m) {
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(m[i])) return false;
    }
    return true;
}

static double DisplayPeriodSeconds() {
    static const double period = []() {
        char value[32] = {};
        const DWORD length = GetEnvironmentVariableA("VERANTYX_DISPLAY_HZ", value, sizeof(value));
        double hz = 60.0;
        if (length > 0 && length < sizeof(value)) {
            const double parsed = strtod(value, nullptr);
            if (std::isfinite(parsed) && parsed >= 30.0 && parsed <= 144.0) hz = parsed;
        }
        return 1.0 / hz;
    }();
    return period;
}

// Remembers the last pose set SteamVR returned and which device indices are
// the two hands (by position, since the game process does not see the driver).
static void RememberRenderPoses(const vr::TrackedDevicePose_t* renderPoses, uint32_t renderPoseCount) {
    if (!renderPoses || renderPoseCount == 0) return;
    const uint32_t count = renderPoseCount < vr::k_unMaxTrackedDeviceCount ? renderPoseCount : vr::k_unMaxTrackedDeviceCount;
    SharedHands hands = {};
    const bool haveHands = ReadSharedHandsSnapshot(&hands);
    AcquireSRWLockExclusive(&g_lastPosesLock);
    memcpy(g_lastRenderPoses, renderPoses, count * sizeof(vr::TrackedDevicePose_t));
    g_lastRenderPoseCount = count;
    if (haveHands) {
        for (uint32_t i = 1; i < count; ++i) {
            const vr::TrackedDevicePose_t& pose = renderPoses[i];
            if (!pose.bDeviceIsConnected || !pose.bPoseIsValid) continue;
            const float* candidates[2] = { hands.leftTransform, hands.rightTransform };
            const uint8_t tracked[2] = { hands.leftTracked, hands.rightTracked };
            for (int hand = 0; hand < 2; ++hand) {
                if (!tracked[hand] || !TransformIsFinite(candidates[hand])) continue;
                double error = 0.0;
                for (int row = 0; row < 3; ++row) {
                    const double delta = pose.mDeviceToAbsoluteTracking.m[row][3] - candidates[hand][12 + row];
                    error += delta * delta;
                }
                if (sqrt(error) < 0.01) (hand == 0 ? g_leftHandDeviceIndex : g_rightHandDeviceIndex) = (int)i;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lastPosesLock);
}

// Answers WaitGetPoses while SteamVR withholds focus: the last good set with
// the HMD and hands replaced by the headset's current shared-memory poses.
static void SynthesizePoses(vr::TrackedDevicePose_t* poses, uint32_t poseCount) {
    if (!poses || poseCount == 0) return;
    const uint32_t count = poseCount < vr::k_unMaxTrackedDeviceCount ? poseCount : vr::k_unMaxTrackedDeviceCount;
    AcquireSRWLockShared(&g_lastPosesLock);
    const uint32_t cached = g_lastRenderPoseCount < count ? g_lastRenderPoseCount : count;
    memcpy(poses, g_lastRenderPoses, cached * sizeof(vr::TrackedDevicePose_t));
    const int leftIndex = g_leftHandDeviceIndex;
    const int rightIndex = g_rightHandDeviceIndex;
    ReleaseSRWLockShared(&g_lastPosesLock);
    for (uint32_t i = cached; i < count; ++i) poses[i] = {};
    SharedHands hands = {};
    if (!ReadSharedHandsSnapshot(&hands)) return;
    struct Target { int index; const float* transform; bool tracked; };
    const Target targets[3] = {
        { 0, hands.headTransform, hands.headTracked != 0 },
        { leftIndex, hands.leftTransform, hands.leftTracked != 0 },
        { rightIndex, hands.rightTransform, hands.rightTracked != 0 },
    };
    for (const Target& target : targets) {
        if (target.index < 0 || (uint32_t)target.index >= count) continue;
        vr::TrackedDevicePose_t& pose = poses[target.index];
        pose.bDeviceIsConnected = true;
        if (!target.tracked || !TransformIsFinite(target.transform)) {
            pose.bPoseIsValid = false;
            pose.eTrackingResult = vr::TrackingResult_Running_OutOfRange;
            continue;
        }
        ColumnMajorToHmdMatrix(target.transform, &pose.mDeviceToAbsoluteTracking);
        memset(pose.vVelocity.v, 0, sizeof(pose.vVelocity.v));
        memset(pose.vAngularVelocity.v, 0, sizeof(pose.vAngularVelocity.v));
        pose.eTrackingResult = vr::TrackingResult_Running_OK;
        pose.bPoseIsValid = true;
    }
}

static vr::EVRCompositorError VR_CALLTYPE HookedWaitGetPoses(void* self,
    vr::TrackedDevicePose_t* renderPoses, uint32_t renderPoseCount,
    vr::TrackedDevicePose_t* gamePoses, uint32_t gamePoseCount) {
    LONG count = InterlockedIncrement(&g_waitLogCount);
    CompositorHook* hook = FindCompositorHook(self);
    const uint64_t waitStart = PerformanceTicks();
    vr::EVRCompositorError result = hook && hook->waitGetPoses
        ? hook->waitGetPoses(self, renderPoses, renderPoseCount, gamePoses, gamePoseCount)
        : vr::VRCompositorError_RequestFailed;
    InterlockedExchangeAdd64(&g_waitPoseTicks, (LONG64)(PerformanceTicks() - waitStart));
    InterlockedIncrement64(&g_waitPoseCalls);
    if (result == vr::VRCompositorError_None) {
        RememberRenderPoses(renderPoses, renderPoseCount);
    } else if (result == vr::VRCompositorError_DoNotHaveFocus && InitSharedMemory()) {
        // The compositor returns at once without focus; keep the game at the
        // virtual display's rate so it neither spins nor stalls.
        const double periodMs = DisplayPeriodSeconds() * 1000.0;
        if (g_lastWaitReturnTicks != 0) {
            const double sinceLast = TicksToMilliseconds(PerformanceTicks() - g_lastWaitReturnTicks);
            if (sinceLast < periodMs - 1.0) Sleep((DWORD)(periodMs - sinceLast));
        }
        SynthesizePoses(renderPoses, renderPoseCount);
        SynthesizePoses(gamePoses, gamePoseCount);
        const LONG lost = InterlockedIncrement(&g_focusLossCount);
        if (lost <= 8 || lost % 600 == 0) {
            char message[160];
            snprintf(message, sizeof(message),
                "WaitGetPoses: SteamVR reports no scene focus (#%ld); answering with headset poses so the game keeps rendering",
                lost);
            Log(message);
        }
        result = vr::VRCompositorError_None;
    }
    g_lastWaitReturnTicks = PerformanceTicks();
    if (result == vr::VRCompositorError_None) {
        CaptureRenderPose(renderPoses, renderPoseCount);
        // The previous frame's GPU copy has usually finished by running start.
        // Publishing here instead of at the next Submit saves a game frame of
        // latency. Only the submitting thread may touch the game's context.
        if (InterlockedCompareExchange(&g_submitThreadId, 0, 0) == (LONG)GetCurrentThreadId() && TryLockCapture()) {
            PublishOldestStagedPair();
            UnlockCapture();
        }
    }
    if (count <= 24 || count % 600 == 0) {
        void** vtable = self ? *(void***)self : nullptr;
        CompositorHook* hook = FindCompositorHook(self);
        size_t submitIndex = hook ? hook->submitIndex : 5;
        char message[256];
        snprintf(message, sizeof(message),
            "IVRCompositor::WaitGetPoses #%ld returned error=%d self=%p vtable=%p submitIndex=%llu submitSlot=%p submitHook=%p hooked=%d",
            count, (int)result, self, vtable, (unsigned long long)submitIndex,
            vtable ? vtable[submitIndex] : nullptr, (void*)&HookedSubmit,
            vtable && vtable[submitIndex] == (void*)&HookedSubmit ? 1 : 0);
        Log(message);
    }
    return result;
}

static void VR_CALLTYPE HookedPostPresentHandoff(void* self) {
    LONG count = InterlockedIncrement(&g_handoffLogCount);
    if (count <= 12 || count % 600 == 0) {
        char message[112];
        snprintf(message, sizeof(message), "IVRCompositor::PostPresentHandoff called #%ld", count);
        Log(message);
    }
    CompositorHook* hook = FindCompositorHook(self);
    if (hook && hook->postPresentHandoff) hook->postPresentHandoff(self);
}

// SteamVR's client shares the submitted texture with vrcompositor through
// its DXGI shared handle. DXVK on macOS cannot export texture memory
// (VK_KHR_external_memory_win32), so GetSharedHandle fails and every Submit
// is rejected with TextureUsesUnsupportedFormat; SteamVR then never sees the
// game render, treats it as not responding during long loads and withholds
// its scene focus. The compositor-side D3D11 proxy understands a fake handle
// that encodes the texture's size and format and answers OpenSharedResource
// with a stand-in texture, so a Submit carrying such a handle succeeds. The
// compositor's own output is not what the headset shows (this proxy copies
// the frames in-process), so the stand-in's content does not matter.
static const uint64_t kFakeSharedTextureTag = 0xBE00000000000000ull;
static LONG g_nextFakeTextureId = 0;
typedef HRESULT (STDMETHODCALLTYPE *GetSharedHandleFn)(IDXGIResource*, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *CreateSharedHandleFn)(IDXGIResource1*, const SECURITY_ATTRIBUTES*, DWORD, LPCWSTR, HANDLE*);
static GetSharedHandleFn g_originalGetSharedHandle = nullptr;
static CreateSharedHandleFn g_originalCreateSharedHandle = nullptr;
static void** g_hookedResourceVtable = nullptr;
static void** g_hookedResource1Vtable = nullptr;
static LONG g_fakeHandleLogCount = 0;

static bool FakeSharedHandlesEnabled() {
    static const bool enabled = []() {
        char value[8] = {};
        return !(GetEnvironmentVariableA("VERANTYX_FAKE_SHARED_HANDLE", value, sizeof(value)) > 0 && value[0] == '0');
    }();
    return enabled;
}

static HANDLE MakeFakeSharedTextureHandle(const D3D11_TEXTURE2D_DESC& desc) {
    uint32_t id = static_cast<uint32_t>(InterlockedIncrement(&g_nextFakeTextureId)) & 0x3FFFFu;
    if (id == 0) id = static_cast<uint32_t>(InterlockedIncrement(&g_nextFakeTextureId)) & 0x3FFFFu;
    const uint64_t value = kFakeSharedTextureTag |
        ((static_cast<uint64_t>(desc.Width - 1) & 0x1FFFu) << 43) |
        ((static_cast<uint64_t>(desc.Height - 1) & 0x1FFFu) << 30) |
        ((static_cast<uint64_t>(desc.Format) & 0x00FFu) << 22) |
        ((static_cast<uint64_t>(desc.SampleDesc.Count) & 0x0Fu) << 18) |
        id;
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
}

static HRESULT FakeSharedHandleFor(IUnknown* resource, HANDLE* out, const char* api, HRESULT originalResult) {
    ID3D11Texture2D* texture = nullptr;
    if (!resource || FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture)) || !texture) return originalResult;
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    texture->Release();
    if (desc.Width == 0 || desc.Height == 0 || desc.Format == DXGI_FORMAT_UNKNOWN) return originalResult;
    *out = MakeFakeSharedTextureHandle(desc);
    const LONG count = InterlockedIncrement(&g_fakeHandleLogCount);
    if (count <= 6 || count % 600 == 0) {
        char message[200];
        snprintf(message, sizeof(message), "%s failed (0x%08lX); answered with stand-in shared handle %p for %ux%u format=%u (#%ld)",
            api, (unsigned long)originalResult, *out, desc.Width, desc.Height, (unsigned)desc.Format, count);
        Log(message);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE HookedGetSharedHandle(IDXGIResource* self, HANDLE* out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    const HRESULT result = g_originalGetSharedHandle ? g_originalGetSharedHandle(self, out) : E_NOTIMPL;
    if (SUCCEEDED(result) && *out) return result;
    return FakeSharedHandleFor(self, out, "IDXGIResource::GetSharedHandle", result);
}

static HRESULT STDMETHODCALLTYPE HookedCreateSharedHandle(IDXGIResource1* self, const SECURITY_ATTRIBUTES* attributes,
    DWORD access, LPCWSTR name, HANDLE* out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    const HRESULT result = g_originalCreateSharedHandle ? g_originalCreateSharedHandle(self, attributes, access, name, out) : E_NOTIMPL;
    if (SUCCEEDED(result) && *out) return result;
    return FakeSharedHandleFor(self, out, "IDXGIResource1::CreateSharedHandle", result);
}

static bool PatchVtableEntry(void** vtable, size_t index, void* replacement, void** original) {
    if (vtable[index] == replacement) return true;
    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    if (!*original) *original = vtable[index];
    vtable[index] = replacement;
    DWORD ignored = 0;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &ignored);
    return true;
}

// Every DXVK texture shares one IDXGIResource vtable, so patching it once
// covers each texture the game submits.
static void InstallSharedHandleHooks(void* textureHandle) {
    if (!FakeSharedHandlesEnabled() || !textureHandle) return;
    IUnknown* unknown = static_cast<IUnknown*>(textureHandle);
    IDXGIResource* resource = nullptr;
    if (SUCCEEDED(unknown->QueryInterface(__uuidof(IDXGIResource), (void**)&resource)) && resource) {
        void** vtable = *(void***)resource;
        if (vtable != g_hookedResourceVtable) {
            // IUnknown (3) + IDXGIObject (4) + IDXGIDeviceSubObject (1): GetSharedHandle is slot 8.
            if (PatchVtableEntry(vtable, 8, (void*)&HookedGetSharedHandle, (void**)&g_originalGetSharedHandle)) {
                g_hookedResourceVtable = vtable;
                Log("Installed IDXGIResource::GetSharedHandle hook: Submits carry a stand-in handle when DXVK cannot share the texture");
            }
        }
        resource->Release();
    }
    IDXGIResource1* resource1 = nullptr;
    if (SUCCEEDED(unknown->QueryInterface(__uuidof(IDXGIResource1), (void**)&resource1)) && resource1) {
        void** vtable = *(void***)resource1;
        if (vtable != g_hookedResource1Vtable) {
            if (PatchVtableEntry(vtable, 13, (void*)&HookedCreateSharedHandle, (void**)&g_originalCreateSharedHandle)) {
                g_hookedResource1Vtable = vtable;
            }
        }
        resource1->Release();
    }
}

static vr::EVRCompositorError VR_CALLTYPE HookedSubmit(void* self, vr::EVREye eye,
    const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags) {
    LONG hookLogIndex = InterlockedIncrement(&g_submitHookLogCount);
    if (hookLogIndex <= 16 || hookLogIndex % 300 == 0) {
        void** vtable = self ? *(void***)self : nullptr;
        char message[320];
        snprintf(message, sizeof(message),
            "HookedSubmit #%ld self=%p vtable=%p eye=%d flags=%u texture=%p type=%d handle=%p colorSpace=%d bounds=%f,%f,%f,%f",
            hookLogIndex, self, vtable, (int)eye, (unsigned)flags, texture,
            texture ? (int)texture->eType : -99, texture ? texture->handle : nullptr,
            texture ? (int)texture->eColorSpace : -99,
            bounds ? bounds->uMin : 0.0f, bounds ? bounds->vMin : 0.0f,
            bounds ? bounds->uMax : 1.0f, bounds ? bounds->vMax : 1.0f);
        Log(message);
    }
    CaptureSubmit(eye, texture, bounds);
    if (texture && texture->eType == vr::TextureType_DirectX) InstallSharedHandleHooks(texture->handle);
    // An OpenGL texture is not forwarded: SteamVR's client would hand it to its
    // D3D11 compositor through WGL_NV_DX_interop, which Wine on macOS lacks. The
    // frame has been captured above, and WaitGetPoses already answers the game
    // itself when SteamVR withholds focus (see g_lastRenderPoses).
    // VERANTYX_GL_FORWARD=1 forwards it anyway.
    if (texture && texture->eType == vr::TextureType_OpenGL) {
        static const bool forwardGl = []() {
            char value[8] = {};
            return GetEnvironmentVariableA("VERANTYX_GL_FORWARD", value, sizeof(value)) > 0 && value[0] == '1';
        }();
        if (!forwardGl) return vr::VRCompositorError_None;
    }
    CompositorHook* hook = FindCompositorHook(self);
    if (hook && hook->submit) {
        vr::Texture_t baseTexture = {};
        const vr::Texture_t* forwardedTexture = texture;
        vr::EVRSubmitFlags forwardedFlags = flags;
        if (texture && (((uint32_t)flags & 0x218u) == 0x218u)) {
            // Wine's OpenVR compositor rejects Alyx's motion/depth extension texture path.
            // Submit the same color texture through the stable base-texture path instead.
            baseTexture = *texture;
            forwardedTexture = &baseTexture;
            forwardedFlags = (vr::EVRSubmitFlags)((uint32_t)flags & ~0x218u);
        }
        vr::EVRCompositorError result = hook->submit(self, eye, forwardedTexture, bounds, forwardedFlags);
        if (hookLogIndex <= 16 || hookLogIndex % 300 == 0) {
            char message[192];
            snprintf(message, sizeof(message),
                "Forwarded Submit #%ld flags=%u->%u result=%d textureHandle=%p",
                hookLogIndex, (unsigned)flags, (unsigned)forwardedFlags, (int)result,
                forwardedTexture ? forwardedTexture->handle : nullptr);
            Log(message);
        }
        return result;
    }
    return vr::VRCompositorError_RequestFailed;
}

static bool InstallSubmitHook(void* compositor, const char* version) {
    if (!compositor) return false;
    void** vtable = *(void***)compositor;
    AcquireSRWLockExclusive(&g_compositorHookLock);
    LONG count = InterlockedCompareExchange(&g_compositorHookCount, 0, 0);
    for (LONG i = 0; i < count; ++i) {
        if (g_compositorHooks[i].vtable == vtable) {
            ReleaseSRWLockExclusive(&g_compositorHookLock);
            return true;
        }
    }
    if (count >= (LONG)(sizeof(g_compositorHooks) / sizeof(g_compositorHooks[0]))) {
        ReleaseSRWLockExclusive(&g_compositorHookLock);
        Log("ERROR: compositor hook table is full");
        return false;
    }
    DWORD oldProtect = 0;
    unsigned interfaceNumber = 0;
    if (version) {
        const char* suffix = strrchr(version, '_');
        if (suffix) interfaceNumber = (unsigned)strtoul(suffix + 1, nullptr, 10);
    }
    // IVRCompositor_027 added GetSubmitTexture before Submit. IVRCompositor_029
    // also added SubmitWithArrayIndex after it, shifting later methods again.
    const size_t submitIndex = interfaceNumber >= 27 ? 6 : 5;
    const size_t postPresentHandoffIndex = interfaceNumber >= 29 ? 9 :
        (interfaceNumber >= 27 ? 8 : 7);
    const size_t lastHookedIndex = postPresentHandoffIndex;
    if (!VirtualProtect(&vtable[2], sizeof(void*) * (lastHookedIndex - 2 + 1), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("ERROR: VirtualProtect failed for IVRCompositor capture vtable entries");
        ReleaseSRWLockExclusive(&g_compositorHookLock);
        return false;
    }
    g_compositorHooks[count].vtable = vtable;
    g_compositorHooks[count].submitIndex = submitIndex;
    g_compositorHooks[count].postPresentHandoffIndex = postPresentHandoffIndex;
    g_compositorHooks[count].waitGetPoses = (WaitGetPosesFn)vtable[2];
    g_compositorHooks[count].submit = (SubmitFn)vtable[submitIndex];
    g_compositorHooks[count].postPresentHandoff = (PostPresentHandoffFn)vtable[postPresentHandoffIndex];
    void* originalSubmit = vtable[submitIndex];
    vtable[2] = (void*)&HookedWaitGetPoses;
    vtable[submitIndex] = (void*)&HookedSubmit;
    vtable[postPresentHandoffIndex] = (void*)&HookedPostPresentHandoff;
    DWORD ignored = 0;
    VirtualProtect(&vtable[2], sizeof(void*) * (lastHookedIndex - 2 + 1), oldProtect, &ignored);
    MemoryBarrier();
    InterlockedExchange(&g_compositorHookCount, count + 1);
    char message[192];
    snprintf(message, sizeof(message),
        "Installed IVRCompositor hooks for %s object=%p vtable=%p submitIndex=%llu originalSubmit=%p replacementSubmit=%p verified=%d",
        version ? version : "(unknown)", compositor, vtable,
        (unsigned long long)submitIndex, originalSubmit,
        (void*)&HookedSubmit, vtable[submitIndex] == (void*)&HookedSubmit ? 1 : 0);
    Log(message);
    ReleaseSRWLockExclusive(&g_compositorHookLock);
    return true;
}

// IVRSystem hooks. Besides WaitGetPoses, a Source 2 game learns that SteamVR
// took its focus from IsInputAvailable, ShouldApplicationPause and the focus
// and dashboard events, and then sleeps instead of rendering. SteamVR never
// returns focus here (see HookedWaitGetPoses), so those answers are fixed.
typedef bool (VR_CALLTYPE *PollNextEventFn)(void*, vr::VREvent_t*, uint32_t);
typedef bool (VR_CALLTYPE *PollNextEventWithPoseFn)(void*, vr::ETrackingUniverseOrigin, vr::VREvent_t*, uint32_t, vr::TrackedDevicePose_t*);
typedef bool (VR_CALLTYPE *BoolQueryFn)(void*);
typedef const char* (VR_CALLTYPE *EventNameFn)(void*, vr::EVREventType);
struct SystemHook {
    void** vtable;
    PollNextEventFn pollNextEvent;
    PollNextEventWithPoseFn pollNextEventWithPose;
    BoolQueryFn isInputAvailable;
    BoolQueryFn shouldApplicationPause;
    BoolQueryFn shouldApplicationReduceRenderingWork;
};
static SystemHook g_systemHooks[4] = {};
static LONG g_systemHookCount = 0;
static LONG g_suppressedEventCount = 0;

static SystemHook* FindSystemHook(void* self) {
    if (!self) return nullptr;
    void** vtable = *(void***)self;
    const LONG count = InterlockedCompareExchange(&g_systemHookCount, 0, 0);
    for (LONG i = 0; i < count; ++i) {
        if (g_systemHooks[i].vtable == vtable) return &g_systemHooks[i];
    }
    return nullptr;
}

static bool IsFocusEvent(uint32_t type) {
    switch (type) {
    case vr::VREvent_InputFocusCaptured:            // 400
    case vr::VREvent_InputFocusChanged:             // 406
    case vr::VREvent_SceneApplicationStateChanged:  // 412
    case vr::VREvent_DashboardActivated:            // 502
        return true;
    default:
        return false;
    }
}

static void NoteSuppressedEvent(uint32_t type) {
    const LONG count = InterlockedIncrement(&g_suppressedEventCount);
    if (count <= 8 || count % 300 == 0) {
        char message[128];
        snprintf(message, sizeof(message), "Suppressed SteamVR focus event %u (#%ld); the game keeps running", type, count);
        Log(message);
    }
}

static bool VR_CALLTYPE HookedPollNextEvent(void* self, vr::VREvent_t* event, uint32_t size) {
    SystemHook* hook = FindSystemHook(self);
    if (!hook || !hook->pollNextEvent) return false;
    for (int attempt = 0; attempt < 64; ++attempt) {
        if (!hook->pollNextEvent(self, event, size)) return false;
        if (!event || !IsFocusEvent(event->eventType)) return true;
        NoteSuppressedEvent(event->eventType);
    }
    return false;
}

static bool VR_CALLTYPE HookedPollNextEventWithPose(void* self, vr::ETrackingUniverseOrigin origin,
    vr::VREvent_t* event, uint32_t size, vr::TrackedDevicePose_t* pose) {
    SystemHook* hook = FindSystemHook(self);
    if (!hook || !hook->pollNextEventWithPose) return false;
    for (int attempt = 0; attempt < 64; ++attempt) {
        if (!hook->pollNextEventWithPose(self, origin, event, size, pose)) return false;
        if (!event || !IsFocusEvent(event->eventType)) return true;
        NoteSuppressedEvent(event->eventType);
    }
    return false;
}

static bool VR_CALLTYPE HookedIsInputAvailable(void*) { return true; }
static bool VR_CALLTYPE HookedShouldApplicationPause(void*) { return false; }
static bool VR_CALLTYPE HookedShouldApplicationReduceRenderingWork(void*) { return false; }

typedef void* (VR_CALLTYPE *GetGenericInterfaceFn)(const char*, vr::EVRInitError*);

// Reads vtable entries without faulting on an unmapped page.
static bool ReadVtableEntries(void** vtable, void** out, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        MEMORY_BASIC_INFORMATION info = {};
        if (VirtualQuery(&vtable[i], &info, sizeof(info)) == 0 || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
        out[i] = vtable[i];
    }
    return true;
}

// The game asks for an IVRSystem version newer than the public SDK, so its
// slot order is unknown. SteamVR's client builds every version's methods
// from the same thunks (identical functions are folded), so each slot of a
// public version with a known layout is matched by pointer against the
// game's table. Nothing is called until every slot has been found.
static bool InstallSystemHook(void* system, const char* version) {
    if (!system || !version) return false;
    char setting[8] = {};
    if (GetEnvironmentVariableA("VERANTYX_FOCUS_HOOKS", setting, sizeof(setting)) > 0 && setting[0] == '0') return false;
    if (FindSystemHook(system)) return true;
    const LONG count = InterlockedCompareExchange(&g_systemHookCount, 0, 0);
    if (count >= (LONG)(sizeof(g_systemHooks) / sizeof(g_systemHooks[0]))) return false;
    HMODULE real = LoadRealOpenVR();
    GetGenericInterfaceFn getInterface = real ? (GetGenericInterfaceFn)GetProcAddress(real, "VR_GetGenericInterface") : nullptr;
    if (!getInterface) return false;

    struct KnownLayout { const char* version; size_t pollNextEvent, pollWithPose, isInputAvailable, shouldPause, shouldReduce; };
    // IVRSystem_021 (this tree's header) and _022 (ResetSeatedZeroPose removed).
    static const KnownLayout layouts[] = {
        { "IVRSystem_022", 29, 30, 38, 40, 41 },
        { "IVRSystem_021", 30, 31, 39, 41, 42 },
    };
    const size_t scanCount = 64;
    void** vtable = *(void***)system;
    void* entries[scanCount] = {};
    if (!ReadVtableEntries(vtable, entries, scanCount)) {
        Log("Not hooking IVRSystem: its vtable could not be read");
        return false;
    }
    auto findSlot = [&](void* pointer) -> int {
        if (!pointer) return -1;
        for (size_t i = 0; i < scanCount; ++i) {
            if (entries[i] == pointer) return (int)i;
        }
        return -1;
    };
    int poll = -1, pollPose = -1, input = -1, pause = -1, reduce = -1;
    const char* matchedVersion = nullptr;
    for (const KnownLayout& layout : layouts) {
        if (strcmp(layout.version, version) == 0) {
            poll = (int)layout.pollNextEvent; pollPose = (int)layout.pollWithPose;
            input = (int)layout.isInputAvailable; pause = (int)layout.shouldPause; reduce = (int)layout.shouldReduce;
            matchedVersion = layout.version;
            break;
        }
        vr::EVRInitError error = vr::VRInitError_None;
        void* reference = getInterface(layout.version, &error);
        if (!reference || error != vr::VRInitError_None) continue;
        void* referenceEntries[scanCount] = {};
        if (!ReadVtableEntries(*(void***)reference, referenceEntries, scanCount)) continue;
        if (*(void***)reference == vtable) {
            // Same object class: the public layout applies as is.
            poll = (int)layout.pollNextEvent; pollPose = (int)layout.pollWithPose;
            input = (int)layout.isInputAvailable; pause = (int)layout.shouldPause; reduce = (int)layout.shouldReduce;
        } else {
            poll = findSlot(referenceEntries[layout.pollNextEvent]);
            pollPose = findSlot(referenceEntries[layout.pollWithPose]);
            input = findSlot(referenceEntries[layout.isInputAvailable]);
            pause = findSlot(referenceEntries[layout.shouldPause]);
            reduce = findSlot(referenceEntries[layout.shouldReduce]);
        }
        char message[256];
        snprintf(message, sizeof(message),
            "IVRSystem slot map from %s to %s: PollNextEvent=%d PollNextEventWithPose=%d IsInputAvailable=%d ShouldApplicationPause=%d ShouldApplicationReduceRenderingWork=%d",
            layout.version, version, poll, pollPose, input, pause, reduce);
        Log(message);
        if (poll > 0 && pollPose == poll + 1 && input > pollPose && pause == input + 2 && reduce == pause + 1) {
            matchedVersion = layout.version;
            break;
        }
        poll = pollPose = input = pause = reduce = -1;
    }
    if (!matchedVersion) {
        char message[160];
        snprintf(message, sizeof(message), "Not hooking %s: no public IVRSystem layout matched its vtable; focus hooks disabled", version);
        Log(message);
        return false;
    }
    const size_t first = (size_t)poll, last = (size_t)reduce;
    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[first], sizeof(void*) * (last - first + 1), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("ERROR: VirtualProtect failed for IVRSystem vtable entries");
        return false;
    }
    SystemHook& hook = g_systemHooks[count];
    hook.vtable = vtable;
    hook.pollNextEvent = (PollNextEventFn)vtable[poll];
    hook.pollNextEventWithPose = (PollNextEventWithPoseFn)vtable[pollPose];
    hook.isInputAvailable = (BoolQueryFn)vtable[input];
    hook.shouldApplicationPause = (BoolQueryFn)vtable[pause];
    hook.shouldApplicationReduceRenderingWork = (BoolQueryFn)vtable[reduce];
    vtable[poll] = (void*)&HookedPollNextEvent;
    vtable[pollPose] = (void*)&HookedPollNextEventWithPose;
    vtable[input] = (void*)&HookedIsInputAvailable;
    vtable[pause] = (void*)&HookedShouldApplicationPause;
    vtable[reduce] = (void*)&HookedShouldApplicationReduceRenderingWork;
    DWORD ignored = 0;
    VirtualProtect(&vtable[first], sizeof(void*) * (last - first + 1), oldProtect, &ignored);
    MemoryBarrier();
    InterlockedExchange(&g_systemHookCount, count + 1);
    char message[200];
    snprintf(message, sizeof(message), "Installed IVRSystem focus hooks for %s (layout of %s) object=%p vtable=%p",
        version, matchedVersion, system, vtable);
    Log(message);
    return true;
}

typedef uint32_t (VR_CALLTYPE *InitInternalFn)(vr::EVRInitError*, vr::EVRApplicationType);
typedef uint32_t (VR_CALLTYPE *InitInternal2Fn)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
typedef void (VR_CALLTYPE *ShutdownInternalFn)();

extern "C" __declspec(dllexport) void* VR_CALLTYPE VR_GetGenericInterface(const char* version, vr::EVRInitError* error) {
    HMODULE real = LoadRealOpenVR();
    if (!real) return nullptr;
    GetGenericInterfaceFn function = (GetGenericInterfaceFn)GetProcAddress(real, "VR_GetGenericInterface");
    if (!function) return nullptr;
    void* result = function(version, error);
    LONG count = InterlockedIncrement(&g_interfaceLogCount);
    if (count <= 20) {
        char message[192];
        snprintf(message, sizeof(message), "VR_GetGenericInterface #%ld version=%s result=%p error=%d",
            count, version ? version : "(null)", result, error ? (int)*error : -1);
        Log(message);
    }
    if (version && strstr(version, "IVRCompositor") && result) InstallSubmitHook(result, version);
    if (version && strncmp(version, "IVRSystem_", 10) == 0 && result) InstallSystemHook(result, version);
    return result;
}

extern "C" __declspec(dllexport) uint32_t VR_CALLTYPE VR_InitInternal(vr::EVRInitError* error, vr::EVRApplicationType type) {
    if (InitSharedMemory()) InterlockedExchange(&g_sharedHeader->captureSourceMode, 0);
    HMODULE real = LoadRealOpenVR();
    InitInternalFn function = real ? (InitInternalFn)GetProcAddress(real, "VR_InitInternal") : nullptr;
    return function ? function(error, type) : 0;
}

extern "C" __declspec(dllexport) uint32_t VR_CALLTYPE VR_InitInternal2(vr::EVRInitError* error,
    vr::EVRApplicationType type, const char* startupInfo) {
    if (InitSharedMemory()) InterlockedExchange(&g_sharedHeader->captureSourceMode, 0);
    HMODULE real = LoadRealOpenVR();
    InitInternal2Fn function = real ? (InitInternal2Fn)GetProcAddress(real, "VR_InitInternal2") : nullptr;
    return function ? function(error, type, startupInfo) : 0;
}

extern "C" __declspec(dllexport) void VR_CALLTYPE VR_ShutdownInternal() {
    HMODULE real = LoadRealOpenVR();
    ShutdownInternalFn function = real ? (ShutdownInternalFn)GetProcAddress(real, "VR_ShutdownInternal") : nullptr;
    if (function) function();
}

#define FORWARD0(ret, name, fallback) \
    extern "C" __declspec(dllexport) ret VR_CALLTYPE name() { \
        HMODULE real = LoadRealOpenVR(); \
        typedef ret (VR_CALLTYPE *Function)(); \
        Function function = real ? (Function)GetProcAddress(real, #name) : nullptr; \
        return function ? function() : fallback; \
    }
#define FORWARD1(ret, name, type1, arg1, fallback) \
    extern "C" __declspec(dllexport) ret VR_CALLTYPE name(type1 arg1) { \
        HMODULE real = LoadRealOpenVR(); \
        typedef ret (VR_CALLTYPE *Function)(type1); \
        Function function = real ? (Function)GetProcAddress(real, #name) : nullptr; \
        return function ? function(arg1) : fallback; \
    }

FORWARD0(uint32_t, VR_GetInitToken, 0)
extern "C" __declspec(dllexport) bool VR_CALLTYPE VR_GetRuntimePath(char* path, uint32_t size, uint32_t* required) {
    HMODULE real = LoadRealOpenVR();
    typedef bool (VR_CALLTYPE *Function)(char*, uint32_t, uint32_t*);
    Function function = real ? (Function)GetProcAddress(real, "VR_GetRuntimePath") : nullptr;
    return function ? function(path, size, required) : false;
}
FORWARD1(const char*, VR_GetStringForHmdError, vr::EVRInitError, error, "")
FORWARD1(const char*, VR_GetVRInitErrorAsEnglishDescription, vr::EVRInitError, error, "")
FORWARD1(const char*, VR_GetVRInitErrorAsSymbol, vr::EVRInitError, error, "")
FORWARD0(bool, VR_IsHmdPresent, false)
FORWARD1(bool, VR_IsInterfaceVersionValid, const char*, version, false)
FORWARD0(bool, VR_IsRuntimeInstalled, false)

#undef FORWARD0
#undef FORWARD1

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
