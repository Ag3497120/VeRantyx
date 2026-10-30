#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <openvr_driver.h>
#include <algorithm>
#include <cmath>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <atomic>
#include <stddef.h>

void LogDriver(const char* msg) {
    FILE* f = fopen("C:\\vr_shared_frame.log", "a");
    if (f) {
        SYSTEMTIME now = {};
        GetLocalTime(&now);
        fprintf(f, "%02u:%02u:%02u.%03u %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, msg);
        fclose(f);
    }
}

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

struct DoubleBufferHeader {
    volatile LONG latestReadyIndex;
    volatile LONG pendingTextureHandle;
    volatile LONG captureRequestSequence;
    volatile LONG captureAckSequence;
    volatile LONG captureSourceMode;
    volatile LONG poseSequence;
    uint32_t padding[10];
};

// The driver records every HMD pose it hands to SteamVR. The game-side capture
// proxy matches WaitGetPoses' render pose against this ring, so each streamed
// frame carries the exact Vision Pro pose ID that the game rendered with.
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
    uint32_t heightMagic;
    volatile LONG heightOffsetBits;
    PoseHistoryEntry entries[16];
};
#pragma pack(pop)

static_assert(sizeof(SharedFrame) == 88, "SharedFrame layout must match HardwareEncoder.swift");
static_assert(sizeof(DoubleBufferHeader) == 64, "DoubleBufferHeader layout must match HardwareEncoder.swift");
static_assert(sizeof(SharedHands) == 312, "SharedHands layout must match HardwareEncoder.swift offsets");
static_assert(offsetof(SharedHands, visionTimestamp) == 244, "SharedHands timestamp offset changed");
static_assert(offsetof(SharedHands, leftTangents) == 252, "SharedHands tangent offsets changed");
static_assert(offsetof(SharedHands, leftGrip) == 284, "SharedHands grip offsets changed");
static_assert(offsetof(SharedHands, leftAngularVelocity) == 287, "SharedHands angular velocity offset changed");
static_assert(offsetof(SharedHands, velocityValid) == 311, "SharedHands velocity flag offset changed");
static_assert(offsetof(DoubleBufferHeader, poseSequence) == 20, "Pose sequence offset changed");
static_assert(sizeof(PoseHistoryEntry) == 40, "PoseHistoryEntry layout must match openvr_capture_proxy.cpp");
static_assert(sizeof(PoseHistory) == 656, "PoseHistory layout must match openvr_capture_proxy.cpp");

static const size_t kMaxPixelBytes = 4096ull * 4096ull * 8ull;
static const size_t kFrameBlockSize = sizeof(SharedFrame) + kMaxPixelBytes;
static const size_t kSharedMapSize = sizeof(DoubleBufferHeader) +
    (kFrameBlockSize * 2) + sizeof(SharedHands) + 1024;
static const size_t kHandsOffset = sizeof(DoubleBufferHeader) + (kFrameBlockSize * 2);
// Lives in the map's existing 1024-byte tail, so the encoder's layout is unchanged.
static const size_t kPoseHistoryOffset = kHandsOffset + 512;
// Also in that tail, after SharedHands: what an ALVR headset reports beyond SharedHands (HardwareEncoder.swift writes
// it): the distance between its eyes. The Vision Pro app's stream leaves it unset (the default IPD stays).
struct SharedExtras { uint32_t ipdMagic; float ipdMeters; };
static const size_t kSharedExtrasOffset = kHandsOffset + 320;
static const uint32_t kIpdMagic = 0x44504956; // "VIPD"
// The game's vibrations, per hand (left, right), for the encoder to pass on (ALVR's send_haptics): seq counts the
// pulses, written last.
struct SharedHaptic { uint32_t seq; float durationSeconds, frequency, amplitude; };
static const size_t kSharedHapticsOffset = kHandsOffset + 336;
static_assert(kSharedHapticsOffset >= kSharedExtrasOffset + sizeof(SharedExtras) && kSharedHapticsOffset + 2 * sizeof(SharedHaptic) <= kPoseHistoryOffset,
              "SharedHaptic must sit between SharedExtras and the pose history");
static_assert(kSharedExtrasOffset >= kHandsOffset + sizeof(SharedHands) && kSharedExtrasOffset + sizeof(SharedExtras) <= kPoseHistoryOffset,
              "SharedExtras must sit between SharedHands and the pose history");
static const uint32_t kPoseHistoryMagic = 0x53485056; // "VPHS"
static const uint32_t kHeightOffsetMagic = 0x464f4856; // "VHOF"
static_assert(kPoseHistoryOffset + sizeof(PoseHistory) <= kSharedMapSize, "Pose history must fit in the shared map");
static HANDLE hMapFile = NULL;
static void* g_sharedMap = nullptr;
static DoubleBufferHeader* g_doubleBufferHeader = nullptr;
static SharedFrame* g_frameHeaders[2] = { nullptr, nullptr };
static uint8_t* g_framePixels[2] = { nullptr, nullptr };
static SharedHands* pSharedHands = NULL;
static PoseHistory* g_poseHistory = nullptr;
static std::mutex g_sharedMemoryMutex;
static uint32_t g_frameSequence = 0;
static ID3D11Device* g_captureDevice = nullptr;
static ID3D11DeviceContext* g_captureContext = nullptr;
static const uint32_t kStagingRingSize = 3;
struct StagingReadbackSlot {
    ID3D11Texture2D* texture = nullptr;
    bool pending = false;
    float headTransform[16] = {0};
    double visionTimestamp = 0.0;
};
static StagingReadbackSlot g_stagingRing[kStagingRingSize];
static uint32_t g_stagingWriteIndex = 0;
static uint32_t g_stagingWidth = 0;
static uint32_t g_stagingHeight = 0;
static DXGI_FORMAT g_stagingFormat = DXGI_FORMAT_UNKNOWN;
static std::mutex g_captureMutex;

static bool InitSharedMemoryDriver();

static void ResetStagingRing() {
    for (uint32_t i = 0; i < kStagingRingSize; ++i) {
        if (g_stagingRing[i].texture) {
            g_stagingRing[i].texture->Release();
            g_stagingRing[i].texture = nullptr;
        }
        g_stagingRing[i].pending = false;
    }
    g_stagingWriteIndex = 0;
    g_stagingWidth = 0;
    g_stagingHeight = 0;
    g_stagingFormat = DXGI_FORMAT_UNKNOWN;
}

// Taking the Vision Pro off and putting it back on can move its idea of the floor: the head then came back a metre
// higher (seen: 1.12 m -> 2.27 m) and a seated game showed its cockpit far below the eyes. After such a break in the
// tracking (no new head sample for a second), once the head has been back for half a second, a jump in height of
// more than 0.7 m (more than sitting down or standing up while it was off) is taken out, keeping the height from
// before the break; head and hands move together. VERANTYX_KEEP_HEIGHT=0 leaves the heights as the headset sends them.
static SRWLOCK g_heightLock = SRWLOCK_INIT;
static float g_heightOffset = 0.0f;
static double g_lastHeadTimestamp = 0.0;
static ULONGLONG g_lastHeadSampleMs = 0, g_backSinceMs = 0;
static float g_heightBeforeBreak = 0.0f, g_lastCorrectedHeight = 0.0f;
static bool g_afterBreak = false;
static void KeepHeightAcrossBreaks(SharedHands* hands) {
    static int enabled = -1;
    if (enabled < 0) { char v[8] = {}; enabled = !(GetEnvironmentVariableA("VERANTYX_KEEP_HEIGHT", v, sizeof(v)) > 0 && v[0] == '0'); }
    if (!enabled) return;
    const ULONGLONG now = GetTickCount64();
    char message[192] = {};
    AcquireSRWLockExclusive(&g_heightLock);
    if (hands->headTracked && hands->visionTimestamp != g_lastHeadTimestamp && std::isfinite(hands->headTransform[13])) {
        g_lastHeadTimestamp = hands->visionTimestamp;
        const float raw = hands->headTransform[13];
        if (g_lastHeadSampleMs && now - g_lastHeadSampleMs > 1000) {
            g_afterBreak = true;
            g_backSinceMs = now;
            g_heightBeforeBreak = g_lastCorrectedHeight;
        }
        g_lastHeadSampleMs = now;
        if (g_afterBreak && now - g_backSinceMs >= 500) {
            g_afterBreak = false;
            const float jump = raw + g_heightOffset - g_heightBeforeBreak;
            if (fabsf(jump) > 0.7f) {
                g_heightOffset = g_heightBeforeBreak - raw;
                snprintf(message, sizeof(message), "[Verantyx] Head came back %.2f m %s after a break in tracking; keeping it at %.2f m",
                    fabsf(jump), jump > 0 ? "higher" : "lower", g_heightBeforeBreak);
            }
        }
        g_lastCorrectedHeight = raw + g_heightOffset;
    }
    const float offset = g_heightOffset;
    // Action poses and the proxy's decoupled poses must use the same origin after tracking resumes.
    if (g_poseHistory) {
        LONG bits;
        memcpy(&bits, &offset, sizeof(bits));
        InterlockedExchange(&g_poseHistory->heightOffsetBits, bits);
    }
    ReleaseSRWLockExclusive(&g_heightLock);
    if (message[0]) LogDriver(message);
    if (offset != 0.0f) {
        hands->headTransform[13] += offset;
        hands->leftTransform[13] += offset;
        hands->rightTransform[13] += offset;
    }
}

static bool ReadSharedHandsRaw(SharedHands* snapshot);
static bool ReadSharedHandsSnapshot(SharedHands* snapshot) {
    if (!ReadSharedHandsRaw(snapshot)) return false;
    KeepHeightAcrossBreaks(snapshot);
    return true;
}
static bool ReadSharedHandsRaw(SharedHands* snapshot) {
    if (!snapshot || !pSharedHands || !g_doubleBufferHeader) return false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG before = InterlockedCompareExchange(&g_doubleBufferHeader->poseSequence, 0, 0);
        if (before & 1) continue;
        MemoryBarrier();
        memcpy(snapshot, pSharedHands, sizeof(*snapshot));
        MemoryBarrier();
        const LONG after = InterlockedCompareExchange(&g_doubleBufferHeader->poseSequence, 0, 0);
        if (before == after && !(after & 1)) return true;
    }
    return false;
}

static void LogInputComponentStatus(const char* path, vr::EVRInputError error) {
    if (error == vr::VRInputError_None) return;
    char message[192];
    snprintf(message, sizeof(message), "[Verantyx] SteamVR input component %s failed: %d",
        path ? path : "(unknown)", (int)error);
    LogDriver(message);
}

static void ReleaseCaptureResources() {
    ResetStagingRing();
    if (g_captureContext) {
        g_captureContext->Release();
        g_captureContext = nullptr;
    }
    if (g_captureDevice) {
        g_captureDevice->Release();
        g_captureDevice = nullptr;
    }
}

static bool EnsureCaptureDevice() {
    if (g_captureDevice && g_captureContext) return true;

    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };
    D3D_FEATURE_LEVEL selectedLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &g_captureDevice,
        &selectedLevel,
        &g_captureContext
    );
    if (result == E_INVALIDARG) {
        result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            &featureLevels[1],
            1,
            D3D11_SDK_VERSION,
            &g_captureDevice,
            &selectedLevel,
            &g_captureContext
        );
    }
    if (FAILED(result)) {
        char message[128];
        snprintf(message, sizeof(message), "[Verantyx] D3D11CreateDevice failed: 0x%08lx", (unsigned long)result);
        LogDriver(message);
        ReleaseCaptureResources();
        return false;
    }
    LogDriver("[Verantyx] D3D11 capture device ready");
    return true;
}

static bool IsFourByteColorFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

static void CapturePresentedTexture(const vr::PresentInfo_t* presentInfo) {
    if (!presentInfo || presentInfo->backbufferTextureHandle == INVALID_SHARED_TEXTURE_HANDLE) return;
    if (!InitSharedMemoryDriver()) return;
    if (InterlockedCompareExchange(&g_doubleBufferHeader->captureSourceMode, 0, 0) == 2) {
        // The game's capture proxy owns the stream while it submits frames.
        // padding[9] is its last Submit tick; after 1.5 s without one (level
        // load) fall back to the compositor's output so the headset shows the
        // loading screen rather than a frozen frame.
        const DWORD lastSubmit = (DWORD)InterlockedCompareExchange((volatile LONG*)&g_doubleBufferHeader->padding[9], 0, 0);
        if (lastSubmit == 0 || GetTickCount() - lastSubmit < 1500) return;
    }

    std::lock_guard<std::mutex> captureLock(g_captureMutex);
    if (!EnsureCaptureDevice()) return;

    ID3D11Texture2D* sourceTexture = nullptr;
    HANDLE sharedHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(presentInfo->backbufferTextureHandle));
    HRESULT result = g_captureDevice->OpenSharedResource(
        sharedHandle,
        __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&sourceTexture)
    );
    if (FAILED(result) || !sourceTexture) {
        const HRESULT legacyResult = result;
        HRESULT openSharedResource1Result = E_NOINTERFACE;
        ID3D11Device1* captureDevice1 = nullptr;
        if (SUCCEEDED(g_captureDevice->QueryInterface(__uuidof(ID3D11Device1), (void**)&captureDevice1)) && captureDevice1) {
            openSharedResource1Result = captureDevice1->OpenSharedResource1(
                sharedHandle,
                __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&sourceTexture)
            );
            captureDevice1->Release();
        }
        if (SUCCEEDED(openSharedResource1Result) && sourceTexture) {
            result = openSharedResource1Result;
        } else {
            if (sourceTexture) {
                sourceTexture->Release();
                sourceTexture = nullptr;
            }
            // The compositor owns this resource in another process. Ask its
            // D3D11 proxy to copy the source texture into our shared frame map.
            InterlockedExchange(
                &g_doubleBufferHeader->pendingTextureHandle,
                static_cast<LONG>(static_cast<uint32_t>(presentInfo->backbufferTextureHandle))
            );
            MemoryBarrier();
            InterlockedIncrement(&g_doubleBufferHeader->captureRequestSequence);
        static uint32_t failures = 0;
        if ((failures++ % 120) == 0) {
            char message[224];
            snprintf(message, sizeof(message),
                "[Verantyx] Shared texture open failed for handle 0x%016llx: OpenSharedResource=0x%08lx OpenSharedResource1=0x%08lx pid=%lu",
                (unsigned long long)presentInfo->backbufferTextureHandle,
                (unsigned long)legacyResult,
                (unsigned long)openSharedResource1Result,
                (unsigned long)GetCurrentProcessId());
            LogDriver(message);
        }
        return;
        }
    }

    D3D11_TEXTURE2D_DESC sourceDesc = {};
    sourceTexture->GetDesc(&sourceDesc);
    if (sourceDesc.Width == 0 || sourceDesc.Height == 0 ||
        sourceDesc.Width > 8192 || sourceDesc.Height > 4096 ||
        sourceDesc.ArraySize != 1 || sourceDesc.SampleDesc.Count != 1 ||
        !IsFourByteColorFormat(sourceDesc.Format)) {
        static uint32_t unsupported = 0;
        if ((unsupported++ % 120) == 0) {
            char message[160];
            snprintf(message, sizeof(message),
                "[Verantyx] Unsupported SteamVR texture: %ux%u format=%u samples=%u array=%u",
                sourceDesc.Width, sourceDesc.Height, (unsigned)sourceDesc.Format,
                sourceDesc.SampleDesc.Count, sourceDesc.ArraySize);
            LogDriver(message);
        }
        sourceTexture->Release();
        return;
    }

    if (sourceDesc.Width != g_stagingWidth || sourceDesc.Height != g_stagingHeight ||
        sourceDesc.Format != g_stagingFormat || !g_stagingRing[0].texture ||
        !g_stagingRing[1].texture || !g_stagingRing[2].texture) {
        ResetStagingRing();
        D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        for (uint32_t i = 0; i < kStagingRingSize; ++i) {
            result = g_captureDevice->CreateTexture2D(&stagingDesc, nullptr, &g_stagingRing[i].texture);
            if (FAILED(result) || !g_stagingRing[i].texture) {
                char message[128];
                snprintf(message, sizeof(message), "[Verantyx] CreateTexture2D staging ring[%u] failed: 0x%08lx", i, (unsigned long)result);
                LogDriver(message);
                ResetStagingRing();
                sourceTexture->Release();
                return;
            }
        }
        g_stagingWidth = sourceDesc.Width;
        g_stagingHeight = sourceDesc.Height;
        g_stagingFormat = sourceDesc.Format;
    }

    StagingReadbackSlot& slot = g_stagingRing[g_stagingWriteIndex];
    if (slot.pending) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        result = g_captureContext->Map(slot.texture, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
            // Keep this slot intact and try it again on the next compositor frame.
            sourceTexture->Release();
            return;
        }
        if (FAILED(result) || !mapped.pData) {
            static uint32_t mapFailures = 0;
            if ((mapFailures++ % 120) == 0) {
                char message[144];
                snprintf(message, sizeof(message), "[Verantyx] Nonblocking staging Map failed: 0x%08lx", (unsigned long)result);
                LogDriver(message);
            }
            sourceTexture->Release();
            return;
        }

        const uint32_t frameIndex = ((uint32_t)g_doubleBufferHeader->latestReadyIndex) ^ 1u;
        uint8_t* destination = g_framePixels[frameIndex];
        const size_t rowBytes = (size_t)sourceDesc.Width * 4;
        const uint8_t* source = static_cast<const uint8_t*>(mapped.pData);
        for (uint32_t row = 0; row < sourceDesc.Height; ++row) {
            memcpy(destination + ((size_t)row * rowBytes), source + ((size_t)row * mapped.RowPitch), rowBytes);
        }
        g_captureContext->Unmap(slot.texture, 0);

        SharedFrame* frame = g_frameHeaders[frameIndex];
        frame->sequenceNumber = ++g_frameSequence;
        if (frame->sequenceNumber == 0) frame->sequenceNumber = ++g_frameSequence;
        frame->width = sourceDesc.Width;
        frame->height = sourceDesc.Height;
        frame->format = (uint32_t)sourceDesc.Format;
        memcpy(frame->renderHeadTransform, slot.headTransform, sizeof(frame->renderHeadTransform));
        frame->visionTimestamp = slot.visionTimestamp;

        MemoryBarrier();
        InterlockedExchange(&g_doubleBufferHeader->latestReadyIndex, (LONG)frameIndex);
        slot.pending = false;
        if (frame->sequenceNumber == 1 || frame->sequenceNumber % 900 == 0) {
            char message[144];
            snprintf(message, sizeof(message), "[Verantyx] Captured SteamVR frame %u (%ux%u, DXGI %u) via async readback pid=%lu",
                frame->sequenceNumber, frame->width, frame->height, frame->format,
                (unsigned long)GetCurrentProcessId());
            LogDriver(message);
        }
    }

    g_captureContext->CopyResource(slot.texture, sourceTexture);
    sourceTexture->Release();
    SharedHands handsSnapshot = {};
    if (ReadSharedHandsSnapshot(&handsSnapshot)) {
        memcpy(slot.headTransform, handsSnapshot.headTransform, sizeof(slot.headTransform));
        slot.visionTimestamp = handsSnapshot.visionTimestamp;
    } else {
        memset(slot.headTransform, 0, sizeof(slot.headTransform));
        slot.headTransform[0] = slot.headTransform[5] = slot.headTransform[10] = slot.headTransform[15] = 1.0f;
        slot.visionTimestamp = 0.0;
    }
    slot.pending = true;
    g_stagingWriteIndex = (g_stagingWriteIndex + 1) % kStagingRingSize;
}

static bool InitSharedMemoryDriver() {
    if (g_sharedMap) return true;

    std::lock_guard<std::mutex> lock(g_sharedMemoryMutex);
    if (g_sharedMap) return true;

    HANDLE hFile = CreateFileA(
        "C:\\vr_shared_frame.dat",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (hFile == INVALID_HANDLE_VALUE) {
        LogDriver("[Verantyx] Failed to open C:\\vr_shared_frame.dat");
        return false;
    }

    LARGE_INTEGER fileSize = {};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart < (LONGLONG)kSharedMapSize) {
        LARGE_INTEGER requestedSize = {};
        requestedSize.QuadPart = (LONGLONG)kSharedMapSize;
        if (!SetFilePointerEx(hFile, requestedSize, NULL, FILE_BEGIN) || !SetEndOfFile(hFile)) {
            LogDriver("[Verantyx] Failed to size the shared frame file");
            CloseHandle(hFile);
            return false;
        }
    }

    hMapFile = CreateFileMappingA(
        hFile,
        NULL,
        PAGE_READWRITE,
        (DWORD)(kSharedMapSize >> 32),
        (DWORD)(kSharedMapSize & 0xffffffffu),
        NULL
    );
    CloseHandle(hFile);
    if (!hMapFile) {
        LogDriver("[Verantyx] Failed to map the shared frame file");
        return false;
    }

    g_sharedMap = MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, kSharedMapSize);
    if (!g_sharedMap) {
        LogDriver("[Verantyx] Failed to map the shared frame view");
        CloseHandle(hMapFile);
        hMapFile = NULL;
        return false;
    }

    g_doubleBufferHeader = (DoubleBufferHeader*)g_sharedMap;
    uint8_t* frameBase = (uint8_t*)g_sharedMap + sizeof(DoubleBufferHeader);
    for (int index = 0; index < 2; ++index) {
        uint8_t* block = frameBase + (kFrameBlockSize * index);
        g_frameHeaders[index] = (SharedFrame*)block;
        g_framePixels[index] = block + sizeof(SharedFrame);
    }
    pSharedHands = (SharedHands*)(frameBase + (kFrameBlockSize * 2));
    g_poseHistory = (PoseHistory*)((uint8_t*)g_sharedMap + kPoseHistoryOffset);

    // The file can survive a reboot or be copied from another machine. Reset
    // only the small metadata area so the encoder never treats an old frame as
    // the first live SteamVR image; the two large pixel buffers stay untouched.
    memset(g_doubleBufferHeader, 0, sizeof(*g_doubleBufferHeader));
    memset(g_frameHeaders[0], 0, sizeof(SharedFrame));
    memset(g_frameHeaders[1], 0, sizeof(SharedFrame));
    memset(pSharedHands, 0, sizeof(SharedHands));
    memset(g_poseHistory, 0, sizeof(PoseHistory));
    g_poseHistory->magic = kPoseHistoryMagic;
    g_poseHistory->heightMagic = kHeightOffsetMagic;
    g_frameSequence = 0;
    MemoryBarrier();

    LogDriver("[Verantyx] Shared frame memory ready; stale frame metadata cleared");
    return true;
}

static vr::HmdQuaternion_t ExtractQuaternion(const float* m) {
    vr::HmdQuaternion_t q;
    float t = m[0] + m[5] + m[10];
    if (t > 0.0f) {
        float S = sqrtf(1.0f + t) * 2.0f;
        q.w = 0.25f * S;
        q.x = (m[6] - m[9]) / S;
        q.y = (m[8] - m[2]) / S;
        q.z = (m[1] - m[4]) / S;
    } else if ((m[0] > m[5]) && (m[0] > m[10])) {
        float S = sqrtf(1.0f + m[0] - m[5] - m[10]) * 2.0f;
        q.w = (m[6] - m[9]) / S;
        q.x = 0.25f * S;
        q.y = (m[1] + m[4]) / S;
        q.z = (m[8] + m[2]) / S;
    } else if (m[5] > m[10]) {
        float S = sqrtf(1.0f + m[5] - m[0] - m[10]) * 2.0f;
        q.w = (m[8] - m[2]) / S;
        q.x = (m[1] + m[4]) / S;
        q.y = 0.25f * S;
        q.z = (m[6] + m[9]) / S;
    } else {
        float S = sqrtf(1.0f + m[10] - m[0] - m[5]) * 2.0f;
        q.w = (m[1] - m[4]) / S;
        q.x = (m[8] + m[2]) / S;
        q.y = (m[6] + m[9]) / S;
        q.z = 0.25f * S;
    }
    const double length = sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (length > 1e-6 && std::isfinite(length)) {
        q.w /= length; q.x /= length; q.y /= length; q.z /= length;
    } else {
        q = { 1, 0, 0, 0 };
    }
    return q;
}

static bool IsFiniteTransform(const float* m) {
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(m[i])) return false;
    }
    return true;
}

static double ReadEnvironmentNumber(const char* name, double fallback, double minimum, double maximum) {
    char value[32] = {};
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    if (length == 0 || length >= sizeof(value)) return fallback;
    char* end = nullptr;
    const double parsed = strtod(value, &end);
    if (end == value || !std::isfinite(parsed) || parsed < minimum || parsed > maximum) return fallback;
    return parsed;
}

static double DisplayFrequency() {
    // 60 Hz: SteamVR's compositor redraws the (virtual) display every vsync,
    // and the game runs well below 60 fps on this CPU-bound Mac anyway.
    static const double frequency = ReadEnvironmentNumber("VERANTYX_DISPLAY_HZ", 60.0, 30.0, 144.0);
    return frequency;
}

// One virtual panel clock drives both the compositor's vsync estimate and its
// present throttle. Sleeping a fixed 11 ms after each Present drifted against
// this clock, so Alyx was released from WaitGetPoses at irregular intervals.
static const std::chrono::steady_clock::time_point g_vsyncEpoch = std::chrono::steady_clock::now();

static double SecondsSinceVsyncEpoch() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_vsyncEpoch).count();
}

static void SleepUntilVsyncSeconds(double target) {
    for (;;) {
        const double remaining = target - SecondsSinceVsyncEpoch();
        if (remaining <= 0.0) return;
        if (remaining > 0.0015) {
            Sleep((DWORD)std::max(1.0, floor((remaining - 0.0005) * 1000.0)));
        } else {
            SwitchToThread();
        }
    }
}

static vr::HmdQuaternion_t MultiplyQuaternions(const vr::HmdQuaternion_t& a, const vr::HmdQuaternion_t& b) {
    return {
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w
    };
}

static vr::HmdQuaternion_t ConjugateQuaternion(const vr::HmdQuaternion_t& q) {
    return { q.w, -q.x, -q.y, -q.z };
}

// ARKit poses carry no velocity. Alyx needs controller velocity for throws and
// SteamVR uses it to predict hands to photon time, so estimate it from
// consecutive Vision Pro samples and smooth out tracking noise.
struct MotionEstimator {
    bool hasSample = false;
    double lastTimestamp = 0.0;
    double lastPosition[3] = { 0, 0, 0 };
    vr::HmdQuaternion_t lastRotation = { 1, 0, 0, 0 };
    double velocity[3] = { 0, 0, 0 };
    double angularVelocity[3] = { 0, 0, 0 };

    void Reset() {
        hasSample = false;
        for (int i = 0; i < 3; ++i) velocity[i] = angularVelocity[i] = 0.0;
    }

    void Update(double timestamp, const float* position, const vr::HmdQuaternion_t& rotation) {
        if (hasSample && timestamp > lastTimestamp) {
            const double dt = timestamp - lastTimestamp;
            if (dt >= 0.002 && dt <= 0.1) {
                constexpr double kSmoothing = 0.45;
                double linear[3];
                double speedSquared = 0.0;
                for (int i = 0; i < 3; ++i) {
                    linear[i] = (position[i] - lastPosition[i]) / dt;
                    speedSquared += linear[i] * linear[i];
                }
                // A tracking pop is not motion; drop it instead of flinging objects.
                if (speedSquared < 12.0 * 12.0) {
                    for (int i = 0; i < 3; ++i) velocity[i] += (linear[i] - velocity[i]) * kSmoothing;
                }

                vr::HmdQuaternion_t delta = MultiplyQuaternions(rotation, ConjugateQuaternion(lastRotation));
                if (delta.w < 0) delta = { -delta.w, -delta.x, -delta.y, -delta.z };
                const double sinHalf = sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
                double angular[3] = { 0, 0, 0 };
                if (sinHalf > 1e-7) {
                    const double angle = 2.0 * atan2(sinHalf, std::min(1.0, delta.w));
                    const double scale = angle / (sinHalf * dt);
                    angular[0] = delta.x * scale;
                    angular[1] = delta.y * scale;
                    angular[2] = delta.z * scale;
                }
                const double angularSpeedSquared = angular[0] * angular[0] + angular[1] * angular[1] + angular[2] * angular[2];
                if (angularSpeedSquared < 40.0 * 40.0) {
                    for (int i = 0; i < 3; ++i) angularVelocity[i] += (angular[i] - angularVelocity[i]) * kSmoothing;
                }
            } else {
                for (int i = 0; i < 3; ++i) velocity[i] = angularVelocity[i] = 0.0;
            }
        } else if (hasSample && timestamp < lastTimestamp) {
            Reset();
        }
        hasSample = true;
        lastTimestamp = timestamp;
        for (int i = 0; i < 3; ++i) lastPosition[i] = position[i];
        lastRotation = rotation;
    }
};

static void RecordPushedHmdPose(double visionTimestamp, const vr::DriverPose_t& pose) {
    if (!g_poseHistory) return;
    const LONG count = InterlockedCompareExchange(&g_poseHistory->writeCount, 0, 0);
    PoseHistoryEntry& entry = g_poseHistory->entries[(uint32_t)count % 16u];
    entry.visionTimestamp = visionTimestamp;
    for (int i = 0; i < 3; ++i) entry.position[i] = (float)pose.vecPosition[i];
    entry.rotation[0] = (float)pose.qRotation.w;
    entry.rotation[1] = (float)pose.qRotation.x;
    entry.rotation[2] = (float)pose.qRotation.y;
    entry.rotation[3] = (float)pose.qRotation.z;
    MemoryBarrier();
    InterlockedExchange(&g_poseHistory->writeCount, count + 1);
}

class CVerantyxDirectMode : public vr::IVRDriverDirectModeComponent {
public:
    virtual void CreateSwapTextureSet(uint32_t unPid, const vr::IVRDriverDirectModeComponent::SwapTextureSetDesc_t *pSwapTextureSetDesc, vr::IVRDriverDirectModeComponent::SwapTextureSet_t *pOutSwapTextureSet) override {}
    virtual void DestroyAllSwapTextureSets(uint32_t unPid) override {}
    virtual void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2], uint32_t (*pIndices)[2]) override {}
    virtual void SubmitLayer(const vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t (&perEye)[2]) override {}
    virtual void Present(vr::SharedTextureHandle_t syncTexture) override {}
    virtual void PostPresent(const vr::IVRDriverDirectModeComponent::Throttling_t *pThrottling) override {}
    virtual void GetFrameTiming(vr::DriverDirectMode_FrameTiming *pFrameTiming) override {}
};

class CVerantyxVirtualDisplay : public vr::IVRVirtualDisplay {
public:
    virtual void Present(const vr::PresentInfo_t *pPresentInfo, uint32_t unPresentInfoSize) override {
        if (unPresentInfoSize < sizeof(vr::PresentInfo_t)) return;
        CapturePresentedTexture(pPresentInfo);
    }
    
    virtual void WaitForPresent() override {
        // Return at the next virtual vsync, like a real panel scanning out
        // the frame. The compositor then stays phase-locked to the same clock
        // that GetTimeSinceLastVsync reports to it.
        const double period = 1.0 / DisplayFrequency();
        const double now = SecondsSinceVsyncEpoch();
        SleepUntilVsyncSeconds((floor(now / period) + 1.0) * period);
    }

    virtual bool GetTimeSinceLastVsync(float *pfSecondsSinceLastVsync, uint64_t *pulFrameCounter) override {
        const double period = 1.0 / DisplayFrequency();
        const double elapsed = SecondsSinceVsyncEpoch();
        const uint64_t frameCounter = (uint64_t)(elapsed / period);
        if (pulFrameCounter) *pulFrameCounter = frameCounter;
        if (pfSecondsSinceLastVsync) {
            *pfSecondsSinceLastVsync = (float)(elapsed - ((double)frameCounter * period));
        }
        return true;
    }
};

static bool HasValidProjectionTangents(const float* tangents) {
    return tangents &&
        std::isfinite(tangents[0]) && std::isfinite(tangents[1]) &&
        std::isfinite(tangents[2]) && std::isfinite(tangents[3]) &&
        tangents[0] < 0.0f && tangents[1] > 0.0f &&
        tangents[2] < 0.0f && tangents[3] > 0.0f;
}

struct EyeRenderSize {
    uint32_t width;
    uint32_t height;
};

// SteamVR reads the eye frustum once, when it starts, which is before the
// Vision Pro connects. The driver therefore answers with the frustum saved
// from the previous session, and never changes it during a session: Alyx
// builds its projection from the first answer. The value in use is published
// in the shared header so the headset can place the image at the right angles.
struct EyeTangents {
    float left[4];
    float right[4];
};

static const char* kTangentFile = "C:\\vr_verantyx_tangents.txt";
static const uint32_t kTangentMagic = 0x4E415456; // "VTAN"
static std::mutex g_tangentMutex;
static bool g_tangentsLatched = false;
static EyeTangents g_latchedTangents = {};
static EyeTangents g_lastPersistedTangents = {};

static bool LoadPersistedTangents(EyeTangents* tangents) {
    FILE* file = fopen(kTangentFile, "r");
    if (!file) return false;
    EyeTangents loaded = {};
    const int count = fscanf(file, "%f %f %f %f %f %f %f %f",
        &loaded.left[0], &loaded.left[1], &loaded.left[2], &loaded.left[3],
        &loaded.right[0], &loaded.right[1], &loaded.right[2], &loaded.right[3]);
    fclose(file);
    if (count != 8 || !HasValidProjectionTangents(loaded.left) || !HasValidProjectionTangents(loaded.right)) return false;
    *tangents = loaded;
    return true;
}

static void PublishEffectiveTangents() {
    if (!g_doubleBufferHeader) return;
    EyeTangents tangents;
    {
        std::lock_guard<std::mutex> lock(g_tangentMutex);
        if (!g_tangentsLatched) return;
        tangents = g_latchedTangents;
    }
    // Another process that maps the file clears the header on start, so the
    // pose thread calls this repeatedly.
    if (g_doubleBufferHeader->padding[8] == kTangentMagic) return;
    memcpy(&g_doubleBufferHeader->padding[0], tangents.left, sizeof(tangents.left));
    memcpy(&g_doubleBufferHeader->padding[4], tangents.right, sizeof(tangents.right));
    MemoryBarrier();
    g_doubleBufferHeader->padding[8] = kTangentMagic;
}

static EyeTangents LatchedTangents() {
    EyeTangents result;
    {
        std::lock_guard<std::mutex> lock(g_tangentMutex);
        if (!g_tangentsLatched) {
            const char* source = "8:9 fallback (no saved Vision Pro frustum yet)";
            EyeTangents chosen = { { -8.0f / 9.0f, 8.0f / 9.0f, -1.0f, 1.0f },
                                   { -8.0f / 9.0f, 8.0f / 9.0f, -1.0f, 1.0f } };
            SharedHands snapshot = {};
            if (InitSharedMemoryDriver() && ReadSharedHandsSnapshot(&snapshot) &&
                HasValidProjectionTangents(snapshot.leftTangents) &&
                HasValidProjectionTangents(snapshot.rightTangents)) {
                memcpy(chosen.left, snapshot.leftTangents, sizeof(chosen.left));
                memcpy(chosen.right, snapshot.rightTangents, sizeof(chosen.right));
                source = "live Vision Pro frustum";
            } else if (LoadPersistedTangents(&chosen)) {
                source = "Vision Pro frustum saved by a previous session";
            }
            g_latchedTangents = chosen;
            g_tangentsLatched = true;
            char message[256];
            snprintf(message, sizeof(message),
                "[Verantyx] Eye frustum for this SteamVR session: %s; left=(%.3f %.3f %.3f %.3f) right=(%.3f %.3f %.3f %.3f)",
                source, chosen.left[0], chosen.left[1], chosen.left[2], chosen.left[3],
                chosen.right[0], chosen.right[1], chosen.right[2], chosen.right[3]);
            LogDriver(message);
        }
        result = g_latchedTangents;
    }
    PublishEffectiveTangents();
    return result;
}

// Save the headset's real frustum for the next SteamVR start.
static void PersistLiveTangents(const SharedHands& hands) {
    if (!HasValidProjectionTangents(hands.leftTangents) || !HasValidProjectionTangents(hands.rightTangents)) return;
    if (memcmp(hands.leftTangents, g_lastPersistedTangents.left, sizeof(hands.leftTangents)) == 0 &&
        memcmp(hands.rightTangents, g_lastPersistedTangents.right, sizeof(hands.rightTangents)) == 0) return;
    memcpy(g_lastPersistedTangents.left, hands.leftTangents, sizeof(hands.leftTangents));
    memcpy(g_lastPersistedTangents.right, hands.rightTangents, sizeof(hands.rightTangents));
    EyeTangents saved = {};
    if (LoadPersistedTangents(&saved) &&
        memcmp(&saved, &g_lastPersistedTangents, sizeof(saved)) == 0) return;
    FILE* file = fopen(kTangentFile, "w");
    if (!file) return;
    fprintf(file, "%.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n",
        hands.leftTangents[0], hands.leftTangents[1], hands.leftTangents[2], hands.leftTangents[3],
        hands.rightTangents[0], hands.rightTangents[1], hands.rightTangents[2], hands.rightTangents[3]);
    fclose(file);
    LogDriver("[Verantyx] Saved the Vision Pro eye frustum; SteamVR uses it from its next start");
}

// Pixels per eye the game renders: 800x900 by default (Wine left the game CPU-bound, so more pixels only cost
// frames). VERANTYX_RENDER_SCALE multiplies each side: 1.5 renders 2.25 times the pixels, for a sharper
// picture where the GPU has room (VXRuntime: about 40% busy at 1.0 on an M1 Max).
static EyeRenderSize EyeSizeAtScale(double scale);
static EyeRenderSize GetEyeRenderSize() {
    static const double scale = ReadEnvironmentNumber("VERANTYX_RENDER_SCALE", 1.0, 0.5, 4.0);   // (3.2: about 2.5K per eye)
    return EyeSizeAtScale(scale);
}
// The compositor's own output (the virtual display), VERANTYX_DISPLAY_SCALE (default: the render scale). The stream
// takes the game's frames straight from its Submit, so the compositor's output is only streamed while the game
// shows none (loading); at 2.5K per eye composing it every vsync held the game back (15 ms per frame).
static EyeRenderSize GetEyeDisplaySize() {
    static const double render = ReadEnvironmentNumber("VERANTYX_RENDER_SCALE", 1.0, 0.5, 4.0);
    static const double scale = ReadEnvironmentNumber("VERANTYX_DISPLAY_SCALE", render, 0.5, 4.0);
    return EyeSizeAtScale(scale);
}
static EyeRenderSize EyeSizeAtScale(double scale) {
    const double kTargetPixelsPerEye = 800.0 * 900.0 * scale * scale;
    const EyeTangents tangents = LatchedTangents();
    float eyeAspect = (tangents.left[1] - tangents.left[0]) / (tangents.left[3] - tangents.left[2]);
    eyeAspect = std::min(2.0f, std::max(0.5f, eyeAspect));

    const double height = sqrt(kTargetPixelsPerEye / eyeAspect);
    const double width = height * eyeAspect;
    const auto evenDimension = [](double value) {
        const uint32_t rounded = (uint32_t)std::lround(value);
        return std::max(2u, (rounded + 1u) & ~1u);
    };
    return { evenDimension(width), evenDimension(height) };
}

struct HeadPresenceState {
    double lastTimestamp = 0.0;
    uint64_t lastAdvanceMs = 0;
    bool hasBaseline = false;
    bool hasFreshTrackedPacket = false;

    bool IsFresh(uint64_t nowMs) const {
        return hasFreshTrackedPacket && nowMs >= lastAdvanceMs && nowMs - lastAdvanceMs < 500;
    }

    bool Update(double timestamp, bool tracked, uint64_t nowMs) {
        if (!std::isfinite(timestamp) || timestamp <= 0.0) {
            hasFreshTrackedPacket = false;
            return false;
        }
        // Shared memory can contain a previous session's tracked pose. Require
        // timestamp progress before reporting presence, including after a reset.
        if (!hasBaseline || timestamp < lastTimestamp) {
            hasBaseline = true;
            hasFreshTrackedPacket = false;
        } else if (timestamp > lastTimestamp) {
            lastAdvanceMs = nowMs;
            hasFreshTrackedPacket = tracked;
        }
        lastTimestamp = timestamp;
        if (!tracked) hasFreshTrackedPacket = false;
        return IsFresh(nowMs);
    }
};

class CVerantyxDeviceDriver : public vr::ITrackedDeviceServerDriver, public vr::IVRDisplayComponent {
public:
    CVerantyxDirectMode m_directMode;
    CVerantyxVirtualDisplay m_virtualDisplay;

    vr::TrackedDeviceIndex_t m_unObjectId = vr::k_unTrackedDeviceIndexInvalid;
    vr::PropertyContainerHandle_t m_ulPropertyContainer = vr::k_ulInvalidPropertyContainer;
    vr::VRInputComponentHandle_t m_proximityComponent = 0;
    HeadPresenceState m_headPresence;
    bool m_userPresent = false;
    ULONGLONG m_lastPresenceUpdateMs = 0;
    // Set once Activate has created every component the pose thread touches.
    std::atomic<bool> m_ready{ false };

    virtual vr::EVRInitError Activate(uint32_t unObjectId) override {
        LogDriver("[Verantyx] CVerantyxDeviceDriver Activated!");
        m_unObjectId = unObjectId;
        m_ulPropertyContainer = vr::VRProperties()->TrackedDeviceToPropertyContainer(m_unObjectId);

        // Half-Life: Alyx picks its fallback controller scheme from the HMD
        // manufacturer. "Verantyx" made it log "falling back to gamepad" and
        // drop the Touch scheme whenever a hand briefly lost tracking. These
        // controllers emulate Oculus Touch, so report the matching vendor.
        char manufacturer[64] = "Oculus";
        const DWORD manufacturerLength = GetEnvironmentVariableA("VERANTYX_HMD_MANUFACTURER", manufacturer, sizeof(manufacturer));
        if (manufacturerLength == 0 || manufacturerLength >= sizeof(manufacturer)) strcpy(manufacturer, "Oculus");
        vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_ManufacturerName_String, manufacturer);
        vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_ModelNumber_String, "VerantyxHMD");
        vr::VRProperties()->SetBoolProperty(m_ulPropertyContainer, vr::Prop_HasVirtualDisplayComponent_Bool, true);
        vr::VRProperties()->SetUint64Property(m_ulPropertyContainer, vr::Prop_CurrentUniverseId_Uint64, 1);
        vr::VRProperties()->SetBoolProperty(m_ulPropertyContainer, vr::Prop_ContainsProximitySensor_Bool, false);
        vr::VRProperties()->SetInt32Property(m_ulPropertyContainer, vr::Prop_EdidVendorID_Int32, 0);
        vr::VRProperties()->SetInt32Property(m_ulPropertyContainer, vr::Prop_EdidProductID_Int32, 0);
        
        // SteamVR chooses its graphics adapter. Enumerating DXGI here did not
        // set a LUID and could block activation inside Wine's display handling.
        vr::VRProperties()->SetBoolProperty(m_ulPropertyContainer, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);
        vr::VRProperties()->SetFloatProperty(m_ulPropertyContainer, vr::Prop_UserIpdMeters_Float, 0.063f);
        vr::VRProperties()->SetFloatProperty(m_ulPropertyContainer, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.f);
        vr::VRProperties()->SetFloatProperty(m_ulPropertyContainer, vr::Prop_DisplayFrequency_Float, (float)DisplayFrequency());
        vr::VRProperties()->SetFloatProperty(m_ulPropertyContainer, vr::Prop_SecondsFromVsyncToPhotons_Float, 0.011f);
        char message[128];
        snprintf(message, sizeof(message), "[Verantyx] HMD Activate: properties set (virtual display %.1f Hz)", DisplayFrequency());
        LogDriver(message);

        m_headPresence = {};
        m_userPresent = false;
        m_proximityComponent = 0;
        const vr::EVRInputError proximityResult = vr::VRDriverInput()->CreateBooleanComponent(
            m_ulPropertyContainer, "/proximity", &m_proximityComponent);
        const bool proximityReady = proximityResult == vr::VRInputError_None && m_proximityComponent != 0;
        if (proximityReady) LogDriver("[Verantyx] HMD Activate: proximity component created");
        vr::VRProperties()->SetBoolProperty(m_ulPropertyContainer, vr::Prop_ContainsProximitySensor_Bool, proximityReady);
        if (proximityReady) {
            vr::VRDriverInput()->UpdateBooleanComponent(m_proximityComponent, false, 0.0);
        } else {
            m_proximityComponent = 0;
            LogDriver("[Verantyx] Could not create HMD proximity input; presence sensor remains disabled");
        }
        LogDriver("[Verantyx] HMD Activate: complete");
        m_ready = true;
        return vr::VRInitError_None;
    }
    // An ALVR headset's eye distance (SharedExtras), once it differs from the one in use by half a millimetre:
    // SteamVR moves the eyes when the property changes (63 mm before any, and for the Vision Pro app).
    float m_ipd = 0.063f;
    void UpdateIpd() {
        if (!g_sharedMap || m_ulPropertyContainer == vr::k_ulInvalidPropertyContainer) return;
        const SharedExtras* extras = (const SharedExtras*)((const uint8_t*)g_sharedMap + kSharedExtrasOffset);
        if (extras->ipdMagic != kIpdMagic) return;
        MemoryBarrier();
        const float ipd = extras->ipdMeters;
        if (!(ipd > 0.04f && ipd < 0.09f) || fabsf(ipd - m_ipd) < 0.0005f) return;
        m_ipd = ipd;
        vr::VRProperties()->SetFloatProperty(m_ulPropertyContainer, vr::Prop_UserIpdMeters_Float, ipd);
        char message[128];
        snprintf(message, sizeof(message), "[Verantyx] IPD %.1f mm (the headset's)", ipd * 1000.0f);
        LogDriver(message);
    }

    void UpdatePresence(const SharedHands* hands) {
        if (!m_proximityComponent) return;
        const uint64_t nowMs = GetTickCount64();
        bool present = m_headPresence.IsFresh(nowMs);
        if (hands) {
            const bool validHead = hands->headTracked != 0 && IsFiniteTransform(hands->headTransform);
            present = m_headPresence.Update(hands->visionTimestamp, validHead, nowMs);
        }
        const bool changed = present != m_userPresent;
        if (changed) {
            m_userPresent = present;
            LogDriver(present ? "[Verantyx] HMD proximity: present (fresh Vision Pro tracking)"
                : "[Verantyx] HMD proximity: absent (tracking stopped or older than 500ms)");
        }
        if (changed || nowMs - m_lastPresenceUpdateMs >= 100) {
            m_lastPresenceUpdateMs = nowMs;
            vr::VRDriverInput()->UpdateBooleanComponent(m_proximityComponent, present, 0.0);
        }
    }

    vr::DriverPose_t MakePose(const SharedHands& hands) const {
        vr::DriverPose_t pose = { 0 };
        pose.poseIsValid = false;
        pose.result = vr::TrackingResult_Running_OutOfRange;
        pose.deviceIsConnected = true;
        pose.qWorldFromDriverRotation = { 1, 0, 0, 0 };
        pose.qDriverFromHeadRotation = { 1, 0, 0, 0 };
        pose.qRotation = { 1, 0, 0, 0 };
        if (hands.headTracked && IsFiniteTransform(hands.headTransform)) {
            // No velocity on purpose: SteamVR then renders exactly this pose,
            // which the headset's late reprojection identifies by timestamp.
            // Vision Pro already predicts this pose to the expected display time.
            pose.poseIsValid = true;
            pose.result = vr::TrackingResult_Running_OK;
            pose.qRotation = ExtractQuaternion(hands.headTransform);
            pose.vecPosition[0] = hands.headTransform[12];
            pose.vecPosition[1] = hands.headTransform[13];
            pose.vecPosition[2] = hands.headTransform[14];
        }
        return pose;
    }
    virtual void Deactivate() override {}
    virtual void EnterStandby() override {}
    virtual void* GetComponent(const char* pchComponentNameAndVersion) override {
        if (!strcmp(pchComponentNameAndVersion, vr::IVRDisplayComponent_Version)) {
            return (vr::IVRDisplayComponent*)this;
        }
        if (!strcmp(pchComponentNameAndVersion, vr::IVRVirtualDisplay_Version)) {
            return &m_virtualDisplay;
        }
        return nullptr;
    }
    virtual void DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize) override {}
#if defined(__GNUC__) && !defined(__clang__)
    virtual vr::DriverPose_t* GetPose(vr::DriverPose_t* ret) override {
#else
    virtual vr::DriverPose_t GetPose() override {
#endif
        InitSharedMemoryDriver();
        SharedHands handsSnapshot = {};
        if (!ReadSharedHandsSnapshot(&handsSnapshot)) handsSnapshot.headTracked = 0;
        const vr::DriverPose_t pose = MakePose(handsSnapshot);

#if defined(__GNUC__) && !defined(__clang__)
        if (ret) *ret = pose;
        return ret;
    }
#else
        return pose;
    }
#endif
    virtual void GetWindowBounds(int32_t* pnX, int32_t* pnY, uint32_t* pnWidth, uint32_t* pnHeight) override {
        const EyeRenderSize eye = GetEyeDisplaySize();
        static LONG logged = 0;
        if (InterlockedCompareExchange(&logged, 1, 0) == 0) {
            const EyeRenderSize render = GetEyeRenderSize();
            char message[200];
            snprintf(message, sizeof(message),
                "[Verantyx] Stereo target initialized: %ux%u per eye rendered, %ux%u per eye composited, at %.0f Hz",
                render.width, render.height, eye.width, eye.height, DisplayFrequency());
            LogDriver(message);
        }
        *pnX = 0; *pnY = 0; *pnWidth = eye.width * 2; *pnHeight = eye.height;
    }
    virtual bool IsDisplayOnDesktop() override { return false; }
    virtual bool IsDisplayRealDisplay() override { return false; }
    virtual void GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight) override {
        const EyeRenderSize eye = GetEyeRenderSize();
        *pnWidth = eye.width; *pnHeight = eye.height;
    }
    virtual void GetEyeOutputViewport(vr::EVREye eEye, uint32_t* pnX, uint32_t* pnY, uint32_t* pnWidth, uint32_t* pnHeight) override {
        const EyeRenderSize eye = GetEyeDisplaySize();
        *pnY = 0; *pnWidth = eye.width; *pnHeight = eye.height;
        if (eEye == vr::Eye_Left) *pnX = 0; else *pnX = eye.width;
    }
    virtual void GetProjectionRaw(vr::EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom) override {
        // OpenVR names these values from the clipped plane: pfTop is the
        // tangent for the bottom (-Y) edge and pfBottom is for the top (+Y).
        const EyeTangents latched = LatchedTangents();
        const float* tangents = eEye == vr::Eye_Left ? latched.left : latched.right;
        *pfLeft = tangents[0];
        *pfRight = tangents[1];
        *pfTop = tangents[2];
        *pfBottom = tangents[3];
    }
    virtual vr::DistortionCoordinates_t* ComputeDistortion(vr::DistortionCoordinates_t* pRet, vr::EVREye eEye, float fU, float fV) override {
        pRet->rfBlue[0] = fU; pRet->rfBlue[1] = fV;
        pRet->rfGreen[0] = fU; pRet->rfGreen[1] = fV;
        pRet->rfRed[0] = fU; pRet->rfRed[1] = fV;
        return pRet;
    }
    virtual bool ComputeInverseDistortion( vr::HmdVector2_t *pResult, vr::EVREye eEye, uint32_t unChannel, float fU, float fV ) override {
        return false;
    }
};

class CVerantyxControllerDriver : public vr::ITrackedDeviceServerDriver {
public:
    vr::TrackedDeviceIndex_t m_unObjectId = vr::k_unTrackedDeviceIndexInvalid;
    vr::PropertyContainerHandle_t m_ulPropertyContainer = vr::k_ulInvalidPropertyContainer;
    bool m_bIsLeft = false;
    MotionEstimator m_motion;
    // Set once Activate has created every component the pose thread touches.
    std::atomic<bool> m_ready{ false };

    vr::VRInputComponentHandle_t m_compTrigger = 0;
    vr::VRInputComponentHandle_t m_compTriggerValue = 0;
    vr::VRInputComponentHandle_t m_compTriggerTouch = 0;
    vr::VRInputComponentHandle_t m_compGrip = 0;
    vr::VRInputComponentHandle_t m_compGripValue = 0;
    vr::VRInputComponentHandle_t m_compGripTouch = 0;
    vr::VRInputComponentHandle_t m_compJoystickX = 0;
    vr::VRInputComponentHandle_t m_compJoystickY = 0;
    vr::VRInputComponentHandle_t m_compJoystickClick = 0;
    vr::VRInputComponentHandle_t m_compJoystickTouch = 0;
    vr::VRInputComponentHandle_t m_compA = 0;
    vr::VRInputComponentHandle_t m_compB = 0;
    vr::VRInputComponentHandle_t m_compATouch = 0;
    vr::VRInputComponentHandle_t m_compBTouch = 0;
    vr::VRInputComponentHandle_t m_compThumbrestTouch = 0;
    vr::VRInputComponentHandle_t m_compSystem = 0;
    vr::VRInputComponentHandle_t m_compSystemTouch = 0;
    vr::VRInputComponentHandle_t m_compHaptic = vr::k_ulInvalidInputComponentHandle;

    CVerantyxControllerDriver(bool isLeft) : m_bIsLeft(isLeft) {}

    virtual vr::EVRInitError Activate(uint32_t unObjectId) override {
        m_unObjectId = unObjectId;
        m_ulPropertyContainer = vr::VRProperties()->TrackedDeviceToPropertyContainer(m_unObjectId);

        vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_ManufacturerName_String, "Oculus");
        vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_ModelNumber_String, m_bIsLeft ? "Oculus Rift S (Left Controller)" : "Oculus Rift S (Right Controller)");
        vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_RenderModelName_String, m_bIsLeft ? "oculus_rifts_controller_left" : "oculus_rifts_controller_right");
        
        // This is strictly required for SteamVR to process inputs!
        vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_InputProfilePath_String, "{oculus}/input/touch_profile.json");
        vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_ControllerType_String, "oculus_touch");
        
        vr::VRProperties()->SetInt32Property(m_ulPropertyContainer, vr::Prop_ControllerRoleHint_Int32, m_bIsLeft ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
        vr::VRProperties()->SetInt32Property(m_ulPropertyContainer, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_Controller);
        vr::VRProperties()->SetInt32Property(m_ulPropertyContainer, vr::Prop_Axis0Type_Int32, vr::k_eControllerAxis_Joystick);
        vr::VRProperties()->SetInt32Property(m_ulPropertyContainer, vr::Prop_Axis1Type_Int32, vr::k_eControllerAxis_Trigger);
        
        // SteamVR Input requires at least a basic input profile to be valid for games like Alyx
        LogInputComponentStatus("/input/trigger/click", vr::VRDriverInput()->CreateBooleanComponent(
            m_ulPropertyContainer, "/input/trigger/click", &m_compTrigger));
        LogInputComponentStatus("/input/trigger/value", vr::VRDriverInput()->CreateScalarComponent(
            m_ulPropertyContainer,
            "/input/trigger/value",
            &m_compTriggerValue,
            vr::VRScalarType_Absolute,
            vr::VRScalarUnits_NormalizedOneSided
        ));
        LogInputComponentStatus("/input/trigger/touch", vr::VRDriverInput()->CreateBooleanComponent(
            m_ulPropertyContainer, "/input/trigger/touch", &m_compTriggerTouch));
        LogInputComponentStatus("/input/grip/click", vr::VRDriverInput()->CreateBooleanComponent(
            m_ulPropertyContainer, "/input/grip/click", &m_compGrip));
        LogInputComponentStatus("/input/grip/value", vr::VRDriverInput()->CreateScalarComponent(
            m_ulPropertyContainer,
            "/input/grip/value",
            &m_compGripValue,
            vr::VRScalarType_Absolute,
            vr::VRScalarUnits_NormalizedOneSided
        ));
        LogInputComponentStatus("/input/grip/touch", vr::VRDriverInput()->CreateBooleanComponent(
            m_ulPropertyContainer, "/input/grip/touch", &m_compGripTouch));
        LogInputComponentStatus("/input/joystick/x", vr::VRDriverInput()->CreateScalarComponent(
            m_ulPropertyContainer, "/input/joystick/x", &m_compJoystickX,
            vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided
        ));
        LogInputComponentStatus("/input/joystick/y", vr::VRDriverInput()->CreateScalarComponent(
            m_ulPropertyContainer, "/input/joystick/y", &m_compJoystickY,
            vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided
        ));
        LogInputComponentStatus("/input/joystick/click", vr::VRDriverInput()->CreateBooleanComponent(
            m_ulPropertyContainer, "/input/joystick/click", &m_compJoystickClick));
        LogInputComponentStatus(m_bIsLeft ? "/input/x/click" : "/input/a/click",
            vr::VRDriverInput()->CreateBooleanComponent(m_ulPropertyContainer,
                m_bIsLeft ? "/input/x/click" : "/input/a/click", &m_compA));
        LogInputComponentStatus(m_bIsLeft ? "/input/y/click" : "/input/b/click",
            vr::VRDriverInput()->CreateBooleanComponent(m_ulPropertyContainer,
                m_bIsLeft ? "/input/y/click" : "/input/b/click", &m_compB));
        LogInputComponentStatus(m_bIsLeft ? "/input/x/touch" : "/input/a/touch",
            vr::VRDriverInput()->CreateBooleanComponent(m_ulPropertyContainer,
                m_bIsLeft ? "/input/x/touch" : "/input/a/touch", &m_compATouch));
        LogInputComponentStatus(m_bIsLeft ? "/input/y/touch" : "/input/b/touch",
            vr::VRDriverInput()->CreateBooleanComponent(m_ulPropertyContainer,
                m_bIsLeft ? "/input/y/touch" : "/input/b/touch", &m_compBTouch));
        LogInputComponentStatus("/input/thumbrest/touch", vr::VRDriverInput()->CreateBooleanComponent(
            m_ulPropertyContainer, "/input/thumbrest/touch", &m_compThumbrestTouch));
        LogInputComponentStatus("/input/joystick/touch", vr::VRDriverInput()->CreateBooleanComponent(
            m_ulPropertyContainer, "/input/joystick/touch", &m_compJoystickTouch));
        if (m_bIsLeft) {
            // The Touch menu button; the Oculus profile defines it on the left hand only.
            LogInputComponentStatus("/input/system/click", vr::VRDriverInput()->CreateBooleanComponent(
                m_ulPropertyContainer, "/input/system/click", &m_compSystem));
            LogInputComponentStatus("/input/system/touch", vr::VRDriverInput()->CreateBooleanComponent(
                m_ulPropertyContainer, "/input/system/touch", &m_compSystemTouch));
        }
        // Vibration: SteamVR sends VREvent_Input_HapticVibration for this component (RunFrame passes it on).
        LogInputComponentStatus("/output/haptic", vr::VRDriverInput()->CreateHapticComponent(
            m_ulPropertyContainer, "/output/haptic", &m_compHaptic));
        
        m_ready = true;
        return vr::VRInitError_None;
    }
    virtual void Deactivate() override {}
    virtual void EnterStandby() override {}
    virtual void* GetComponent(const char* pchComponentNameAndVersion) override { return nullptr; }
    virtual void DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize) override {}
#if defined(__GNUC__) && !defined(__clang__)
    virtual vr::DriverPose_t* GetPose(vr::DriverPose_t* ret) override {
#else
    virtual vr::DriverPose_t GetPose() override {
#endif
        InitSharedMemoryDriver();
        SharedHands handsSnapshot = {};
        const bool hasSnapshot = ReadSharedHandsSnapshot(&handsSnapshot);
        // SteamVR may call this on its own thread; leave velocity state to the pose thread.
        const vr::DriverPose_t pose = MakePose(handsSnapshot, hasSnapshot, false, false);

#if defined(__GNUC__) && !defined(__clang__)
        if (ret) *ret = pose;
        return ret;
    }
#else
        return pose;
    }
#endif

    // Vision Pro hand states: 0 = not tracked, 1 = measured this frame,
    // 2 = briefly out of the cameras' view and held relative to the head.
    static constexpr uint8_t kHandMeasured = 1;

    vr::DriverPose_t MakePose(const SharedHands& hands, bool fresh, bool newSample, bool ownsMotion = true) {
        vr::DriverPose_t pose = { 0 };
        pose.poseIsValid = false;
        pose.result = vr::TrackingResult_Running_OutOfRange;
        pose.deviceIsConnected = true;
        pose.qWorldFromDriverRotation = { 1, 0, 0, 0 };
        pose.qDriverFromHeadRotation = { 1, 0, 0, 0 };
        pose.qRotation = { 1, 0, 0, 0 };

        const uint8_t state = m_bIsLeft ? hands.leftTracked : hands.rightTracked;
        const float* transform = m_bIsLeft ? hands.leftTransform : hands.rightTransform;
        if (!fresh || state == 0 || !IsFiniteTransform(transform)) {
            if (ownsMotion) m_motion.Reset();
            return pose;
        }
        pose.poseIsValid = true;
        pose.result = vr::TrackingResult_Running_OK;
        pose.qRotation = ExtractQuaternion(transform);
        pose.vecPosition[0] = transform[12];
        pose.vecPosition[1] = transform[13];
        pose.vecPosition[2] = transform[14];
        if (!ownsMotion) return pose;
        if (hands.velocityValid) {
            // The headset measured these (Joy-Con gyro/accelerometer fused with
            // hand tracking); they carry the sharp wrist flick that Alyx's
            // gravity gloves look for, which position differences smooth away.
            const float* linear = m_bIsLeft ? hands.leftVelocity : hands.rightVelocity;
            const float* angular = m_bIsLeft ? hands.leftAngularVelocity : hands.rightAngularVelocity;
            for (int i = 0; i < 3; ++i) {
                pose.vecVelocity[i] = std::isfinite(linear[i]) ? linear[i] : 0.0;
                pose.vecAngularVelocity[i] = std::isfinite(angular[i]) ? angular[i] : 0.0;
            }
            m_motion.Reset();
        } else if (state == kHandMeasured) {
            if (newSample) m_motion.Update(hands.visionTimestamp, &transform[12], pose.qRotation);
            for (int i = 0; i < 3; ++i) {
                pose.vecVelocity[i] = m_motion.velocity[i];
                pose.vecAngularVelocity[i] = m_motion.angularVelocity[i];
            }
        } else {
            m_motion.Reset();
        }
        return pose;
    }

    void UpdateInputs(const SharedHands& hands, bool fresh) {
        if (m_unObjectId == vr::k_unTrackedDeviceIndexInvalid) return;
        const uint8_t state = m_bIsLeft ? hands.leftTracked : hands.rightTracked;
        // Inputs do not depend on the hand being tracked: Joy-Con buttons
        // keep working out of the cameras' view, and the headset already
        // zeroes gesture inputs for a lost hand.
        const bool tracked = fresh;
        const float trigger = tracked ? (float)(m_bIsLeft ? hands.leftTrigger : hands.rightTrigger) / 255.0f : 0.0f;
        const float grip = tracked ? (float)(m_bIsLeft ? hands.leftGrip : hands.rightGrip) / 255.0f : 0.0f;
        // Vision Pro applies hysteresis and keeps engaged values above Alyx's
        // 0.75 click threshold, so these clicks match the analog values.
        vr::VRDriverInput()->UpdateBooleanComponent(m_compTrigger, trigger >= 0.75f, 0.0);
        vr::VRDriverInput()->UpdateScalarComponent(m_compTriggerValue, trigger, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_compTriggerTouch, trigger > 0.05f, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_compGrip, grip >= 0.75f, 0.0);
        vr::VRDriverInput()->UpdateScalarComponent(m_compGripValue, grip, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_compGripTouch, grip > 0.05f, 0.0);

        // Right-hand bits 0-4 and left-hand bits 8-12: primary (A/X),
        // secondary (B/Y), joystick click, joystick touch, menu (system).
        const uint32_t shift = m_bIsLeft ? 8u : 0u;
        const uint32_t buttons = tracked ? (m_bIsLeft ? hands.leftButtons : hands.rightButtons) : 0;
        const bool primaryPressed = (buttons & (1u << shift)) != 0;
        const bool secondaryPressed = (buttons & (2u << shift)) != 0;
        const bool stickClicked = (buttons & (4u << shift)) != 0;
        const bool stickEngaged = (buttons & (8u << shift)) != 0;
        const bool menuPressed = (buttons & (16u << shift)) != 0;

        float stickX = tracked ? (m_bIsLeft ? hands.leftStickX : hands.rightStickX) : 0.0f;
        float stickY = tracked ? (m_bIsLeft ? hands.leftStickY : hands.rightStickY) : 0.0f;
        if (!std::isfinite(stickX) || !std::isfinite(stickY)) stickX = stickY = 0.0f;
        stickX = std::max(-1.0f, std::min(1.0f, stickX));
        stickY = std::max(-1.0f, std::min(1.0f, stickY));
        vr::VRDriverInput()->UpdateScalarComponent(m_compJoystickX, stickX, 0.0);
        vr::VRDriverInput()->UpdateScalarComponent(m_compJoystickY, stickY, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_compJoystickClick, stickClicked, 0.0);
        // Half-Life: Alyx's Oculus Touch locomotion binding uses a joystick
        // d-pad in "touch" mode. Without this component SteamVR silently
        // ignores those move/turn bindings even when the axes change.
        vr::VRDriverInput()->UpdateBooleanComponent(
            m_compJoystickTouch,
            stickEngaged || stickClicked || fabsf(stickX) >= 0.02f || fabsf(stickY) >= 0.02f,
            0.0
        );

        vr::VRDriverInput()->UpdateBooleanComponent(m_compA, primaryPressed, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_compB, secondaryPressed, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_compATouch, primaryPressed, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_compBTouch, secondaryPressed, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_compThumbrestTouch, grip > 0.05f, 0.0);
        if (m_compSystem) {
            vr::VRDriverInput()->UpdateBooleanComponent(m_compSystem, menuPressed, 0.0);
            vr::VRDriverInput()->UpdateBooleanComponent(m_compSystemTouch, menuPressed, 0.0);
        }

        const ULONGLONG now = GetTickCount64();
        if (now - m_lastInputLogAt >= 5000) {
            m_lastInputLogAt = now;
            char message[256];
            snprintf(message, sizeof(message),
                "[Verantyx] %s input: state=%u fresh=%d trigger=%.2f grip=%.2f joystick=(%.2f,%.2f) buttons=0x%x velocity=(%.2f,%.2f,%.2f)",
                m_bIsLeft ? "left" : "right", (unsigned)state, fresh ? 1 : 0, trigger, grip,
                stickX, stickY, (unsigned)(buttons >> shift) & 0x1fu,
                m_motion.velocity[0], m_motion.velocity[1], m_motion.velocity[2]);
            LogDriver(message);
        }
    }

    ULONGLONG m_lastInputLogAt = 0;
};

class CVerantyxServerProvider : public vr::IServerTrackedDeviceProvider {
public:
    CVerantyxDeviceDriver* m_pHmd = nullptr;
    CVerantyxControllerDriver* m_pControllerLeft = nullptr;
    CVerantyxControllerDriver* m_pControllerRight = nullptr;
    std::thread m_poseThread;
    std::atomic<bool> m_stopPoseThread{ false };
    std::atomic<bool> m_poseThreadRunning{ false };
    double m_lastRunFramePoseTimestamp = -1.0;

    virtual vr::EVRInitError Init(vr::IVRDriverContext* pDriverContext) override { 
        VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);
        LogDriver("[Verantyx] ServerProvider Init called!");
        
        m_pHmd = new CVerantyxDeviceDriver();
        vr::VRServerDriverHost()->TrackedDeviceAdded("verantyx_hmd", vr::TrackedDeviceClass_HMD, m_pHmd);

        m_pControllerLeft = new CVerantyxControllerDriver(true);
        vr::VRServerDriverHost()->TrackedDeviceAdded("verantyx_ctrl_l", vr::TrackedDeviceClass_Controller, m_pControllerLeft);
        
        m_pControllerRight = new CVerantyxControllerDriver(false);
        vr::VRServerDriverHost()->TrackedDeviceAdded("verantyx_ctrl_r", vr::TrackedDeviceClass_Controller, m_pControllerRight);

        // RunFrame follows the compositor loop, which is irregular under Wine.
        // Forward each Vision Pro pose to SteamVR as soon as it lands instead.
        m_stopPoseThread = false;
        try {
            m_poseThread = std::thread(&CVerantyxServerProvider::PoseLoop, this);
            m_poseThreadRunning = true;
            LogDriver("[Verantyx] Pose thread started; poses are forwarded as soon as they arrive");
        } catch (...) {
            m_poseThreadRunning = false;
            LogDriver("[Verantyx] Could not start the pose thread; updating poses from RunFrame");
        }

        return vr::VRInitError_None; 
    }
    virtual void Cleanup() override {
        m_stopPoseThread = true;
        if (m_poseThread.joinable()) m_poseThread.join();
        m_poseThreadRunning = false;
        if (m_pHmd) { delete m_pHmd; m_pHmd = nullptr; }
        if (m_pControllerLeft) { delete m_pControllerLeft; m_pControllerLeft = nullptr; }
        if (m_pControllerRight) { delete m_pControllerRight; m_pControllerRight = nullptr; }
    }
    virtual const char* const* GetInterfaceVersions() override { return vr::k_InterfaceVersions; }

    void PublishTracking(const SharedHands& hands, bool newPose, bool fresh) {
        if (m_pHmd && m_pHmd->m_ready.load()) {
            const vr::DriverPose_t pose = m_pHmd->MakePose(hands);
            // Record before publishing: once SteamVR has the pose, the game can
            // render with it and look it up in the ring.
            if (newPose && pose.poseIsValid) RecordPushedHmdPose(hands.visionTimestamp, pose);
            vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_pHmd->m_unObjectId, pose, sizeof(vr::DriverPose_t));
        }
        CVerantyxControllerDriver* controllers[2] = { m_pControllerLeft, m_pControllerRight };
        for (CVerantyxControllerDriver* controller : controllers) {
            if (!controller || !controller->m_ready.load()) continue;
            const vr::DriverPose_t pose = controller->MakePose(hands, fresh, newPose);
            vr::VRServerDriverHost()->TrackedDevicePoseUpdated(controller->m_unObjectId, pose, sizeof(vr::DriverPose_t));
            controller->UpdateInputs(hands, fresh);
        }
    }

    // VERANTYX_BENCH=1: with no headset connected, publish a standing head
    // that slowly looks around and two hands in front of it, so the game can
    // be run and measured without anyone wearing the Vision Pro. Real headset
    // poses take over as soon as they arrive.
    static void MakeBenchHands(SharedHands* hands, double seconds) {
        *hands = {};
        const float yaw = 0.35f * sinf((float)seconds * 0.4f);
        const float pitch = 0.08f * sinf((float)seconds * 0.9f);
        const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
        // Column-major yaw*pitch rotation, SteamVR axes (x right, y up, -z forward).
        float* h = hands->headTransform;
        h[0] = cy;      h[1] = 0.0f; h[2] = -sy;     h[3] = 0.0f;
        h[4] = sy * sp; h[5] = cp;   h[6] = cy * sp; h[7] = 0.0f;
        h[8] = sy * cp; h[9] = -sp;  h[10] = cy * cp; h[11] = 0.0f;
        h[12] = 0.02f * sinf((float)seconds * 0.7f); h[13] = 1.65f; h[14] = 0.0f; h[15] = 1.0f;
        const float handOffsets[2][3] = { { -0.22f, 1.25f, -0.38f }, { 0.22f, 1.25f, -0.38f } };
        float* handTransforms[2] = { hands->leftTransform, hands->rightTransform };
        for (int hand = 0; hand < 2; ++hand) {
            float* m = handTransforms[hand];
            m[0] = 1.0f; m[5] = 1.0f; m[10] = 1.0f; m[15] = 1.0f;
            m[12] = handOffsets[hand][0] + 0.03f * sinf((float)seconds * 1.3f + hand);
            m[13] = handOffsets[hand][1] + 0.02f * cosf((float)seconds * 1.1f);
            m[14] = handOffsets[hand][2];
        }
        hands->headTracked = 1;
        hands->leftTracked = 1;
        hands->rightTracked = 1;
        hands->visionTimestamp = seconds;
    }

    void PoseLoop() {
        LONG lastSequence = 0;
        bool hasSequence = false;
        double lastTimestamp = -1.0;
        ULONGLONG lastAdvanceMs = 0;
        ULONGLONG lastPublishMs = 0;
        ULONGLONG lastIpdCheckMs = 0;
        SharedHands latest = {};
        bool hasLatest = false;
        uint64_t publishedPoses = 0;
        const bool benchMode = ReadEnvironmentNumber("VERANTYX_BENCH", 0.0, 0.0, 1.0) >= 1.0;
        bool benchActive = false;
        ULONGLONG lastBenchMs = 0;
        if (benchMode) LogDriver("[Verantyx] VERANTYX_BENCH=1: synthetic poses will be published while no headset sends any");
        while (!m_stopPoseThread.load()) {
            if (!InitSharedMemoryDriver()) {
                Sleep(100);
                continue;
            }
            const ULONGLONG nowMs = GetTickCount64();
            const LONG sequence = InterlockedCompareExchange(&g_doubleBufferHeader->poseSequence, 0, 0);
            bool newInput = false;
            bool newPose = false;
            if ((!hasSequence || sequence != lastSequence) && !(sequence & 1)) {
                SharedHands snapshot = {};
                if (ReadSharedHandsSnapshot(&snapshot)) {
                    hasSequence = true;
                    lastSequence = sequence;
                    newInput = true;
                    if (std::isfinite(snapshot.visionTimestamp) && snapshot.visionTimestamp > 0.0 &&
                        snapshot.visionTimestamp != lastTimestamp) {
                        lastTimestamp = snapshot.visionTimestamp;
                        lastAdvanceMs = nowMs;
                        newPose = true;
                    }
                    latest = snapshot;
                    hasLatest = true;
                }
            }
            if (benchMode) {
                if (newPose && benchActive) {
                    benchActive = false;
                    LogDriver("[Verantyx] Bench mode: headset poses arrived; synthetic poses stopped");
                } else if (!newPose && (lastAdvanceMs == 0 || nowMs - lastAdvanceMs >= 1000 || benchActive) &&
                           nowMs - lastBenchMs >= 11) {
                    if (!benchActive) {
                        benchActive = true;
                        LogDriver("[Verantyx] Bench mode: no headset poses; publishing synthetic head and hand poses");
                    }
                    MakeBenchHands(&latest, (double)nowMs / 1000.0);
                    lastBenchMs = nowMs;
                    lastTimestamp = latest.visionTimestamp;
                    lastAdvanceMs = nowMs;
                    hasLatest = true;
                    newInput = true;
                    newPose = true;
                }
            }
            const bool fresh = lastAdvanceMs != 0 && nowMs - lastAdvanceMs < 500;
            // Republish periodically so SteamVR sees hands drop out once the
            // headset stops sending, even when no new packet arrives.
            if (hasLatest && (newInput || nowMs - lastPublishMs >= 20)) {
                PublishTracking(latest, newPose, fresh);
                lastPublishMs = nowMs;
                if (newPose && (++publishedPoses == 1 || publishedPoses % 5400 == 0)) {
                    char message[128];
                    snprintf(message, sizeof(message), "[Verantyx] Forwarded Vision Pro pose #%llu to SteamVR",
                        (unsigned long long)publishedPoses);
                    LogDriver(message);
                }
            }
            if (m_pHmd && m_pHmd->m_ready.load()) m_pHmd->UpdatePresence(newInput ? &latest : nullptr);
            if (m_pHmd && m_pHmd->m_ready.load() && nowMs - lastIpdCheckMs >= 500) { lastIpdCheckMs = nowMs; m_pHmd->UpdateIpd(); }
            if (newPose && !benchActive) PersistLiveTangents(latest);
            PublishEffectiveTangents();
            Sleep(1);
        }
    }

    // The game's vibrations, into the map's tail for the encoder (an ALVR headset gets them; the Vision Pro app's
    // stream does not read them). SteamVR delivers them as events, polled here, on its thread.
    uint64_t m_hapticPulses = 0;
    void PassOnHaptics() {
        vr::VREvent_t event;
        while (vr::VRServerDriverHost()->PollNextEvent(&event, sizeof(event))) {
            if (event.eventType != vr::VREvent_Input_HapticVibration || !g_sharedMap) continue;
            const vr::VREvent_HapticVibration_t& v = event.data.hapticVibration;
            int hand = -1;
            if (m_pControllerLeft && v.componentHandle == m_pControllerLeft->m_compHaptic) hand = 0;
            else if (m_pControllerRight && v.componentHandle == m_pControllerRight->m_compHaptic) hand = 1;
            if (hand < 0) continue;
            SharedHaptic* h = (SharedHaptic*)((uint8_t*)g_sharedMap + kSharedHapticsOffset) + hand;
            h->durationSeconds = v.fDurationSeconds;
            h->frequency = v.fFrequency;
            h->amplitude = v.fAmplitude;
            MemoryBarrier();
            h->seq = h->seq + 1;
            if (++m_hapticPulses <= 5 || m_hapticPulses % 500 == 0) {
                char message[160];
                snprintf(message, sizeof(message), "[Verantyx] Vibration #%llu, %s hand: %.3f s, %.0f Hz, amplitude %.2f",
                    (unsigned long long)m_hapticPulses, hand ? "right" : "left", v.fDurationSeconds, v.fFrequency, v.fAmplitude);
                LogDriver(message);
            }
        }
    }
    virtual void RunFrame() override {
        PassOnHaptics();
        if (m_poseThreadRunning.load()) return;
        if (!InitSharedMemoryDriver()) return;
        SharedHands hands = {};
        if (!ReadSharedHandsSnapshot(&hands)) return;
        const bool newPose = std::isfinite(hands.visionTimestamp) && hands.visionTimestamp != m_lastRunFramePoseTimestamp;
        m_lastRunFramePoseTimestamp = hands.visionTimestamp;
        PublishTracking(hands, newPose, true);
        if (m_pHmd && m_pHmd->m_ready.load()) m_pHmd->UpdatePresence(&hands);
    }
    virtual bool ShouldBlockStandbyMode() override { return true; }
    virtual void EnterStandby() override {}
    virtual void LeaveStandby() override {}
};

CVerantyxServerProvider g_provider;
extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* pInterfaceName, int* pReturnCode) {
    LogDriver(pInterfaceName);
    if (0 == strcmp(vr::IServerTrackedDeviceProvider_Version, pInterfaceName)) {
        return &g_provider;
    }
    if (pReturnCode)
        *pReturnCode = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
