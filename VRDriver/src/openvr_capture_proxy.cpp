#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
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
static_assert(sizeof(SharedFrame) == 88, "Shared frame layout mismatch");
static_assert(sizeof(SharedHands) == 287, "Shared hand layout mismatch");
static_assert(sizeof(SharedHeader) == 64, "Shared header layout mismatch");
static_assert(offsetof(SharedHeader, poseSequence) == 20, "Pose sequence offset mismatch");

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
    char message[512];
    snprintf(message, sizeof(message),
        "Capture metrics: submit_fps=%.2f capture_fps=%.2f queue_ms=%.2f map_ms=%.2f copy_ms=%.2f pair_age_ms=%.2f wait_poses_ms=%.2f readback_MBps=%.2f busy=%llu dropped=%llu contention=%ld",
        g_metrics.submitted * 1000.0 / elapsedMs, g_metrics.published * 1000.0 / elapsedMs,
        g_metrics.queuedEyes ? g_metrics.queueMs / g_metrics.queuedEyes : 0.0,
        g_metrics.mapAttempts ? g_metrics.mapMs / g_metrics.mapAttempts : 0.0,
        g_metrics.published ? g_metrics.copyMs / g_metrics.published : 0.0,
        g_metrics.published ? g_metrics.ageMs / g_metrics.published : 0.0,
        waitCalls ? TicksToMilliseconds((uint64_t)waitTicks) / waitCalls : 0.0,
        g_metrics.copiedBytes / (elapsedMs * 1000.0),
        (unsigned long long)g_metrics.mapBusy, (unsigned long long)g_metrics.dropped, contention);
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
        snprintf(message, sizeof(message), "Captured Alyx eye pair %u (%ux%u format=%u) using deferred readback",
            frame->sequenceNumber, frame->width, frame->height, frame->format);
        Log(message);
    }
    return true;
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
    if (texture->eType != vr::TextureType_DirectX || !texture->handle || !InitSharedMemory()) return;
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
        pair.renderHands = {};
        ReadSharedHandsSnapshot(&pair.renderHands);
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

typedef void* (VR_CALLTYPE *GetGenericInterfaceFn)(const char*, vr::EVRInitError*);
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
