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
#include <stddef.h>

void LogDriver(const char* msg) {
    FILE* f = fopen("C:\\vr_shared_frame.log", "a");
    if (f) {
        fprintf(f, "%s\n", msg);
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

static_assert(sizeof(SharedFrame) == 88, "SharedFrame layout must match HardwareEncoder.swift");
static_assert(sizeof(DoubleBufferHeader) == 64, "DoubleBufferHeader layout must match HardwareEncoder.swift");
static_assert(sizeof(SharedHands) == 287, "SharedHands layout must match HardwareEncoder.swift offsets");
static_assert(offsetof(SharedHands, visionTimestamp) == 244, "SharedHands timestamp offset changed");
static_assert(offsetof(SharedHands, leftTangents) == 252, "SharedHands tangent offsets changed");
static_assert(offsetof(SharedHands, leftGrip) == 284, "SharedHands grip offsets changed");
static_assert(offsetof(DoubleBufferHeader, poseSequence) == 20, "Pose sequence offset changed");

static const size_t kMaxPixelBytes = 4096ull * 4096ull * 8ull;
static const size_t kFrameBlockSize = sizeof(SharedFrame) + kMaxPixelBytes;
static const size_t kSharedMapSize = sizeof(DoubleBufferHeader) +
    (kFrameBlockSize * 2) + sizeof(SharedHands) + 1024;
static HANDLE hMapFile = NULL;
static void* g_sharedMap = nullptr;
static DoubleBufferHeader* g_doubleBufferHeader = nullptr;
static SharedFrame* g_frameHeaders[2] = { nullptr, nullptr };
static uint8_t* g_framePixels[2] = { nullptr, nullptr };
static SharedHands* pSharedHands = NULL;
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

static bool ReadSharedHandsSnapshot(SharedHands* snapshot) {
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
    if (InterlockedCompareExchange(&g_doubleBufferHeader->captureSourceMode, 0, 0) == 2) return;

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
        sourceDesc.Width > 4096 || sourceDesc.Height > 4096 ||
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

    // The file can survive a reboot or be copied from another machine. Reset
    // only the small metadata area so the encoder never treats an old frame as
    // the first live SteamVR image; the two large pixel buffers stay untouched.
    memset(g_doubleBufferHeader, 0, sizeof(*g_doubleBufferHeader));
    memset(g_frameHeaders[0], 0, sizeof(SharedFrame));
    memset(g_frameHeaders[1], 0, sizeof(SharedFrame));
    memset(pSharedHands, 0, sizeof(SharedHands));
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
    return q;
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
        std::this_thread::sleep_for(std::chrono::milliseconds(11));
    }
    
    virtual bool GetTimeSinceLastVsync(float *pfSecondsSinceLastVsync, uint64_t *pulFrameCounter) override {
        static auto startTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - startTime).count();
        double frameDuration = 1.0 / 90.0;
        const uint64_t frameCounter = (uint64_t)(elapsed / frameDuration);
        if (pulFrameCounter) *pulFrameCounter = frameCounter;
        if (pfSecondsSinceLastVsync) {
            *pfSecondsSinceLastVsync = (float)(elapsed - (frameCounter * frameDuration));
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

static EyeRenderSize GetEyeRenderSize() {
    constexpr float kFallbackEyeAspect = 8.0f / 9.0f;
    constexpr double kTargetPixelsPerEye = 800.0 * 900.0;
    float eyeAspect = kFallbackEyeAspect;

    InitSharedMemoryDriver();
    SharedHands snapshot = {};
    if (ReadSharedHandsSnapshot(&snapshot) &&
        HasValidProjectionTangents(snapshot.leftTangents)) {
        const float horizontal = snapshot.leftTangents[1] - snapshot.leftTangents[0];
        const float vertical = snapshot.leftTangents[3] - snapshot.leftTangents[2];
        if (horizontal > 0.01f && vertical > 0.01f) {
            eyeAspect = horizontal / vertical;
        }
    }
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

    virtual vr::EVRInitError Activate(uint32_t unObjectId) override {
        LogDriver("[Verantyx] CVerantyxDeviceDriver Activated!");
        m_unObjectId = unObjectId;
        m_ulPropertyContainer = vr::VRProperties()->TrackedDeviceToPropertyContainer(m_unObjectId);
        
        vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_ManufacturerName_String, "Verantyx");
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
        vr::VRProperties()->SetFloatProperty(m_ulPropertyContainer, vr::Prop_DisplayFrequency_Float, 90.0f);
        vr::VRProperties()->SetFloatProperty(m_ulPropertyContainer, vr::Prop_SecondsFromVsyncToPhotons_Float, 0.011f);
        LogDriver("[Verantyx] HMD Activate: properties set");

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
        return vr::VRInitError_None;
    }
    void UpdatePresence() {
        if (!m_proximityComponent) return;
        const uint64_t nowMs = GetTickCount64();
        bool present = m_headPresence.IsFresh(nowMs);
        SharedHands hands = {};
        if (ReadSharedHandsSnapshot(&hands)) {
            bool validHead = hands.headTracked != 0;
            for (float value : hands.headTransform) validHead = validHead && std::isfinite(value);
            present = m_headPresence.Update(hands.visionTimestamp, validHead, nowMs);
        }
        if (present != m_userPresent) {
            m_userPresent = present;
            LogDriver(present ? "[Verantyx] HMD proximity: present (fresh Vision Pro tracking)"
                : "[Verantyx] HMD proximity: absent (tracking stopped or older than 500ms)");
        }
        vr::VRDriverInput()->UpdateBooleanComponent(m_proximityComponent, present, 0.0);
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
        vr::DriverPose_t pose = { 0 };
        pose.poseIsValid = false;
        pose.result = vr::TrackingResult_Running_OutOfRange;
        pose.deviceIsConnected = true;
        pose.qWorldFromDriverRotation = { 1, 0, 0, 0 };
        pose.qDriverFromHeadRotation = { 1, 0, 0, 0 };
        pose.qRotation = { 1, 0, 0, 0 };
        InitSharedMemoryDriver();
        SharedHands handsSnapshot = {};
        if (ReadSharedHandsSnapshot(&handsSnapshot) && handsSnapshot.headTracked) {
            pose.poseIsValid = true;
            pose.result = vr::TrackingResult_Running_OK;
            pose.qRotation = ExtractQuaternion(handsSnapshot.headTransform);
            pose.vecPosition[0] = handsSnapshot.headTransform[12];
            pose.vecPosition[1] = handsSnapshot.headTransform[13];
            pose.vecPosition[2] = handsSnapshot.headTransform[14];
        }

#if defined(__GNUC__) && !defined(__clang__)
        if (ret) *ret = pose;
        return ret;
    }
#else
        return pose;
    }
#endif
    virtual void GetWindowBounds(int32_t* pnX, int32_t* pnY, uint32_t* pnWidth, uint32_t* pnHeight) override {
        const EyeRenderSize eye = GetEyeRenderSize();
        static LONG logged = 0;
        if (InterlockedCompareExchange(&logged, 1, 0) == 0) {
            char message[160];
            snprintf(message, sizeof(message),
                "[Verantyx] Stereo target initialized: %ux%u per eye at 90Hz",
                eye.width, eye.height);
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
        const EyeRenderSize eye = GetEyeRenderSize();
        *pnY = 0; *pnWidth = eye.width; *pnHeight = eye.height;
        if (eEye == vr::Eye_Left) *pnX = 0; else *pnX = eye.width;
    }
    virtual void GetProjectionRaw(vr::EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom) override {
        // OpenVR names these values from the clipped plane: pfTop is the
        // tangent for the bottom (-Y) edge and pfBottom is for the top (+Y).
        constexpr float fallbackTangents[4] = { -8.0f / 9.0f, 8.0f / 9.0f, -1.0f, 1.0f };
        SharedHands snapshot = {};
        const float* tangents = nullptr;
        if (InitSharedMemoryDriver() && ReadSharedHandsSnapshot(&snapshot)) {
            tangents = eEye == vr::Eye_Left ? snapshot.leftTangents : snapshot.rightTangents;
        }
        if (!HasValidProjectionTangents(tangents)) tangents = fallbackTangents;

        *pfLeft = tangents[0];
        *pfRight = tangents[1];
        *pfTop = tangents[2];
        *pfBottom = tangents[3];
        if (tangents != fallbackTangents) {
            static LONG logged = 0;
            if (InterlockedCompareExchange(&logged, 1, 0) == 0) {
                char message[192];
                snprintf(message, sizeof(message),
                    "[Verantyx] Vision Pro frustum received: left=%.3f right=%.3f bottom=%.3f top=%.3f",
                    tangents[0], tangents[1], tangents[2], tangents[3]);
                LogDriver(message);
            }
        }
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

    CVerantyxControllerDriver(bool isLeft) : m_bIsLeft(isLeft) {}

    virtual vr::EVRInitError Activate(uint32_t unObjectId) override {
        m_unObjectId = unObjectId;
        m_ulPropertyContainer = vr::VRProperties()->TrackedDeviceToPropertyContainer(m_unObjectId);

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
        vr::DriverPose_t pose = { 0 };
        pose.poseIsValid = false;
        pose.result = vr::TrackingResult_Running_OutOfRange;
        pose.deviceIsConnected = true;
        pose.qWorldFromDriverRotation = { 1, 0, 0, 0 };
        pose.qDriverFromHeadRotation = { 1, 0, 0, 0 };
        pose.qRotation = { 1, 0, 0, 0 };
        
        InitSharedMemoryDriver();
        SharedHands handsSnapshot = {};
        if (ReadSharedHandsSnapshot(&handsSnapshot)) {
            const bool tracked = m_bIsLeft ? handsSnapshot.leftTracked != 0 : handsSnapshot.rightTracked != 0;
            const float* srcTransform = m_bIsLeft ? handsSnapshot.leftTransform : handsSnapshot.rightTransform;
            if (tracked) {
                pose.poseIsValid = true;
                pose.result = vr::TrackingResult_Running_OK;
                pose.qRotation = ExtractQuaternion(srcTransform);
                pose.vecPosition[0] = srcTransform[12];
                pose.vecPosition[1] = srcTransform[13];
                pose.vecPosition[2] = srcTransform[14];
            }
        }

#if defined(__GNUC__) && !defined(__clang__)
        if (ret) *ret = pose;
        return ret;
    }
#else
        return pose;
    }
#endif

    void UpdateInputs() {
        if (m_unObjectId == vr::k_unTrackedDeviceIndexInvalid) return;
        SharedHands handsSnapshot = {};
        const bool hasSnapshot = ReadSharedHandsSnapshot(&handsSnapshot);
        const bool tracked = hasSnapshot && (m_bIsLeft ? handsSnapshot.leftTracked != 0 : handsSnapshot.rightTracked != 0);
        const uint8_t triggerValue = tracked ? (m_bIsLeft ? handsSnapshot.leftTrigger : handsSnapshot.rightTrigger) : 0;
        const uint8_t gripValue = tracked ? (m_bIsLeft ? handsSnapshot.leftGrip : handsSnapshot.rightGrip) : 0;
        vr::VRDriverInput()->UpdateBooleanComponent(m_compTrigger, triggerValue >= 128, 0.0);
        vr::VRDriverInput()->UpdateScalarComponent(m_compTriggerValue, (float)triggerValue / 255.0f, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_compTriggerTouch, tracked && triggerValue > 0, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_compGrip, gripValue >= 150, 0.0);
        vr::VRDriverInput()->UpdateScalarComponent(m_compGripValue, (float)gripValue / 255.0f, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_compGripTouch, tracked && gripValue > 0, 0.0);

        const float stickX = tracked ? (m_bIsLeft ? handsSnapshot.leftStickX : handsSnapshot.rightStickX) : 0.0f;
        const float stickY = tracked ? (m_bIsLeft ? handsSnapshot.leftStickY : handsSnapshot.rightStickY) : 0.0f;
        vr::VRDriverInput()->UpdateScalarComponent(m_compJoystickX, stickX, 0.0);
        vr::VRDriverInput()->UpdateScalarComponent(m_compJoystickY, stickY, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_compJoystickClick, tracked && gripValue >= 230, 0.0);
        // Half-Life: Alyx's Oculus Touch locomotion binding uses a joystick
        // d-pad in "touch" mode. Without this component SteamVR silently
        // ignores those move/turn bindings even when the axes change.
        vr::VRDriverInput()->UpdateBooleanComponent(
            m_compJoystickTouch,
            tracked && (fabsf(stickX) >= 0.02f || fabsf(stickY) >= 0.02f),
            0.0
        );

        const uint32_t buttons = tracked ? (m_bIsLeft ? handsSnapshot.leftButtons : handsSnapshot.rightButtons) : 0;
        const uint32_t primaryMask = m_bIsLeft ? (1u << 8) : (1u << 0);
        const uint32_t secondaryMask = m_bIsLeft ? (1u << 9) : (1u << 1);
        const bool primaryPressed = (buttons & primaryMask) != 0;
        const bool secondaryPressed = (buttons & secondaryMask) != 0;
        vr::VRDriverInput()->UpdateBooleanComponent(m_compA, primaryPressed, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_compB, secondaryPressed, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_compATouch, primaryPressed, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_compBTouch, secondaryPressed, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_compThumbrestTouch, tracked && gripValue > 0, 0.0);

        static ULONGLONG lastInputLogAt = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - lastInputLogAt >= 5000) {
            lastInputLogAt = now;
            char message[224];
            snprintf(message, sizeof(message),
                "[Verantyx] %s input: tracked=%d trigger=%u grip=%u joystick=(%.2f,%.2f)",
                m_bIsLeft ? "left" : "right", tracked ? 1 : 0,
                (unsigned)triggerValue, (unsigned)gripValue, stickX, stickY);
            LogDriver(message);
        }
    }
};

class CVerantyxServerProvider : public vr::IServerTrackedDeviceProvider {
public:
    CVerantyxDeviceDriver* m_pHmd = nullptr;
    CVerantyxControllerDriver* m_pControllerLeft = nullptr;
    CVerantyxControllerDriver* m_pControllerRight = nullptr;

    virtual vr::EVRInitError Init(vr::IVRDriverContext* pDriverContext) override { 
        VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);
        LogDriver("[Verantyx] ServerProvider Init called!");
        
        m_pHmd = new CVerantyxDeviceDriver();
        vr::VRServerDriverHost()->TrackedDeviceAdded("verantyx_hmd", vr::TrackedDeviceClass_HMD, m_pHmd);

        m_pControllerLeft = new CVerantyxControllerDriver(true);
        vr::VRServerDriverHost()->TrackedDeviceAdded("verantyx_ctrl_l", vr::TrackedDeviceClass_Controller, m_pControllerLeft);
        
        m_pControllerRight = new CVerantyxControllerDriver(false);
        vr::VRServerDriverHost()->TrackedDeviceAdded("verantyx_ctrl_r", vr::TrackedDeviceClass_Controller, m_pControllerRight);

        return vr::VRInitError_None; 
    }
    virtual void Cleanup() override {
        if (m_pHmd) { delete m_pHmd; m_pHmd = nullptr; }
        if (m_pControllerLeft) { delete m_pControllerLeft; m_pControllerLeft = nullptr; }
        if (m_pControllerRight) { delete m_pControllerRight; m_pControllerRight = nullptr; }
    }
    virtual const char* const* GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
    virtual void RunFrame() override {
        if (m_pHmd && m_pHmd->m_unObjectId != vr::k_unTrackedDeviceIndexInvalid) {
            vr::DriverPose_t pose;
#if defined(__GNUC__) && !defined(__clang__)
            m_pHmd->GetPose(&pose);
#else
            pose = m_pHmd->GetPose();
#endif
            vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_pHmd->m_unObjectId, pose, sizeof(vr::DriverPose_t));
            m_pHmd->UpdatePresence();
        }
        
        if (m_pControllerLeft && m_pControllerLeft->m_unObjectId != vr::k_unTrackedDeviceIndexInvalid) {
            vr::DriverPose_t pose;
#if defined(__GNUC__) && !defined(__clang__)
            m_pControllerLeft->GetPose(&pose);
#else
            pose = m_pControllerLeft->GetPose();
#endif
            vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_pControllerLeft->m_unObjectId, pose, sizeof(vr::DriverPose_t));
            m_pControllerLeft->UpdateInputs();
        }

        if (m_pControllerRight && m_pControllerRight->m_unObjectId != vr::k_unTrackedDeviceIndexInvalid) {
            vr::DriverPose_t pose;
#if defined(__GNUC__) && !defined(__clang__)
            m_pControllerRight->GetPose(&pose);
#else
            pose = m_pControllerRight->GetPose();
#endif
            vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_pControllerRight->m_unObjectId, pose, sizeof(vr::DriverPose_t));
            m_pControllerRight->UpdateInputs();
        }
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
