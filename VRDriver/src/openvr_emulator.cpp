#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <algorithm>

void LogMsg(const char* msg) {
}
#include "openvr.h"
#include <cstring>
#include <iostream>
#include <fstream>
#include <map>
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <array>

struct SharedFrame {
    uint32_t sequenceNumber;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    float renderHeadTransform[16];
    double visionTimestamp;
};

const int MAX_PIXEL_DATA_SIZE = 4096 * 4096 * 8;
const int FRAME_BLOCK_SIZE = sizeof(SharedFrame) + MAX_PIXEL_DATA_SIZE;

struct DoubleBufferHeader {
    volatile LONG latestReadyIndex;
    volatile LONG pendingTextureHandle;
    volatile LONG captureRequestSequence;
    volatile LONG captureAckSequence;
    volatile LONG captureSourceMode;
    volatile LONG poseSequence;
    uint32_t padding[10];
};

#pragma pack(push, 1)
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

static_assert(sizeof(DoubleBufferHeader) == 64, "DoubleBufferHeader layout must match HardwareEncoder.swift");
static_assert(sizeof(SharedHands) == 287, "SharedHands layout must match HardwareEncoder.swift");

static HANDLE hMapFile = NULL;
static void* pBuf = NULL;
static DoubleBufferHeader* pDbHeader = NULL;
static SharedFrame* pHeaders[2] = {NULL, NULL};
static uint8_t* pPixelDatas[2] = {NULL, NULL};
static SharedHands* pSharedHands = NULL;
static uint32_t frameSeq = 1;

static bool ReadSharedHandsSnapshot(SharedHands* snapshot) {
    if (!snapshot || !pSharedHands || !pDbHeader) return false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG before = InterlockedCompareExchange(&pDbHeader->poseSequence, 0, 0);
        if (before & 1) continue;
        MemoryBarrier();
        memcpy(snapshot, pSharedHands, sizeof(*snapshot));
        MemoryBarrier();
        const LONG after = InterlockedCompareExchange(&pDbHeader->poseSequence, 0, 0);
        if (before == after && !(after & 1)) return true;
    }
    return false;
}

#define STAGING_BUFFER_SIZE 3
struct StagingFrame {
    ID3D11Texture2D* pColorLeft = nullptr;
    ID3D11Texture2D* pColorRight = nullptr;
    ID3D11Texture2D* pDepthLeft = nullptr;
    ID3D11Texture2D* pDepthRight = nullptr;
    
    float uMinL = 0.0f, vMinL = 0.0f, uMaxL = 1.0f, vMaxL = 1.0f;
    float uMinR = 0.0f, vMinR = 0.0f, uMaxR = 1.0f, vMaxR = 1.0f;
    double visionTimestamp = 0.0;
    float renderHeadTransform[16] = {0};
    bool bIsValid = false;
    
    ID3D11DeviceContext* pContext = nullptr;
    DXGI_FORMAT descFormat;
    uint32_t descWidth = 0;
    uint32_t descHeight = 0;
};
static StagingFrame g_stagingFrames[STAGING_BUFFER_SIZE];
static uint32_t g_stagingWriteIndex = 0;
static uint32_t g_stagingReadIndex = 0;

static std::thread g_copyThread;
static std::mutex g_copyMutex;
static std::condition_variable g_copyCv;
static bool g_copyThreadRunning = false;
static uint32_t g_pendingCopyIndex = -1;
static uint32_t g_copyFrameSeq = 1;

void BackgroundCopyThread();


float g_handVelocityL[3] = {0,0,0};
float g_handVelocityR[3] = {0,0,0};

static float g_lastLeftTransform[16] = {0};
static float g_lastRightTransform[16] = {0};
static float g_lastLeftVel[3] = {0,0,0};
static float g_lastRightVel[3] = {0,0,0};
static float g_lastLeftAngularVel[3] = {0,0,0};
static float g_lastRightAngularVel[3] = {0,0,0};

static float g_lastHeadTransform[16] = {0};
static float g_lastHeadVel[3] = {0,0,0};
static float g_lastHeadAngularVel[3] = {0,0,0};
static double g_lastHeadVisionTimestamp = 0.0;
static auto g_lastHeadArrivalMacTime = std::chrono::high_resolution_clock::now();

static std::mutex g_poseQueueMutex;
static std::queue<double> g_poseTimestampQueue;
static std::queue<std::array<float, 16>> g_poseTransformQueue;

static double g_lastPoppedVisionTimestamp = 0.0;
static float g_lastPoppedTransform[16] = {0};

void ApplyHeadExtrapolation(vr::TrackedDevicePose_t* pose, float* targetTransform, double visionTimestamp, float fPredictedSecondsToPhotonsFromNow) {
    // DISABLE PREDICTION
    float total_extrapolate = 0.0f;
    
    {
        std::lock_guard<std::mutex> lock(g_poseQueueMutex);
        if (g_poseTimestampQueue.size() >= 3) {
            g_poseTimestampQueue.pop();
            g_poseTransformQueue.pop();
        }
        g_poseTimestampQueue.push(visionTimestamp);
        std::array<float, 16> transform;
        for(int c=0; c<16; c++) transform[c] = targetTransform[c];
        g_poseTransformQueue.push(transform);
    }
    
    auto now = std::chrono::high_resolution_clock::now();
    
    // Initialize on first frame
    if (g_lastHeadTransform[0] == 0.0f && g_lastHeadTransform[5] == 0.0f) {
        for(int i=0; i<16; i++) g_lastHeadTransform[i] = targetTransform[i];
        g_lastHeadVisionTimestamp = visionTimestamp;
        g_lastHeadArrivalMacTime = now;
    }
    
    // New UDP packet arrived
    if (visionTimestamp > g_lastHeadVisionTimestamp) {
        float dt = (float)(visionTimestamp - g_lastHeadVisionTimestamp);
        if (dt < 0.001f) dt = 0.001f;
        if (dt > 0.1f) dt = 0.1f; // Cap dt to avoid huge spikes
        
        // Calculate Velocity
        float rawVel[3];
        rawVel[0] = (targetTransform[12] - g_lastHeadTransform[12]) / dt;
        rawVel[1] = (targetTransform[13] - g_lastHeadTransform[13]) / dt;
        rawVel[2] = (targetTransform[14] - g_lastHeadTransform[14]) / dt;
        
        float velSmooth = 10.0f * dt;
        if (velSmooth > 1.0f) velSmooth = 1.0f;
        g_lastHeadVel[0] += (rawVel[0] - g_lastHeadVel[0]) * velSmooth;
        g_lastHeadVel[1] += (rawVel[1] - g_lastHeadVel[1]) * velSmooth;
        g_lastHeadVel[2] += (rawVel[2] - g_lastHeadVel[2]) * velSmooth;
        
        // Calculate Angular Velocity
        float R_prev[3][3];
        float R_cur[3][3];
        for(int r=0; r<3; r++) {
            for(int c=0; c<3; c++) {
                R_prev[r][c] = g_lastHeadTransform[c*4 + r];
                R_cur[r][c] = targetTransform[c*4 + r];
            }
        }
        
        float R_delta[3][3];
        for(int r=0; r<3; r++) {
            for(int c=0; c<3; c++) {
                R_delta[r][c] = 0;
                for(int k=0; k<3; k++) {
                    R_delta[r][c] += R_cur[r][k] * R_prev[c][k];
                }
            }
        }
        
        float rawOmegaX = (R_delta[2][1] - R_delta[1][2]) / (2.0f * dt);
        float rawOmegaY = (R_delta[0][2] - R_delta[2][0]) / (2.0f * dt);
        float rawOmegaZ = (R_delta[1][0] - R_delta[0][1]) / (2.0f * dt);
        
        if (fabs(rawOmegaX) > 50.0f) rawOmegaX = 0;
        if (fabs(rawOmegaY) > 50.0f) rawOmegaY = 0;
        if (fabs(rawOmegaZ) > 50.0f) rawOmegaZ = 0;
        
        float angSmooth = 15.0f * dt;
        if (angSmooth > 1.0f) angSmooth = 1.0f;
        g_lastHeadAngularVel[0] += (rawOmegaX - g_lastHeadAngularVel[0]) * angSmooth;
        g_lastHeadAngularVel[1] += (rawOmegaY - g_lastHeadAngularVel[1]) * angSmooth;
        g_lastHeadAngularVel[2] += (rawOmegaZ - g_lastHeadAngularVel[2]) * angSmooth;
        
        for(int i=0; i<16; i++) g_lastHeadTransform[i] = targetTransform[i];
        g_lastHeadVisionTimestamp = visionTimestamp;
        g_lastHeadArrivalMacTime = now;
    }
    
    pose->bPoseIsValid = true;
    pose->bDeviceIsConnected = true;
    pose->eTrackingResult = vr::TrackingResult_Running_OK;
    
    // Copy base transform
    for(int r=0; r<3; r++) {
        for(int c=0; c<4; c++) {
            pose->mDeviceToAbsoluteTracking.m[r][c] = targetTransform[c*4 + r];
        }
    }
    
    // Extrapolate Position
    pose->mDeviceToAbsoluteTracking.m[0][3] += g_lastHeadVel[0] * total_extrapolate;
    pose->mDeviceToAbsoluteTracking.m[1][3] += g_lastHeadVel[1] * total_extrapolate;
    pose->mDeviceToAbsoluteTracking.m[2][3] += g_lastHeadVel[2] * total_extrapolate;
    
    // Extrapolate Rotation (Small Angle Approximation)
    float wx = g_lastHeadAngularVel[0] * total_extrapolate;
    float wy = g_lastHeadAngularVel[1] * total_extrapolate;
    float wz = g_lastHeadAngularVel[2] * total_extrapolate;
    
    float R_delta_extrapolate[3][3] = {
        { 1.0f, -wz, wy },
        { wz, 1.0f, -wx },
        { -wy, wx, 1.0f }
    };
    
    float currentR[3][3];
    for(int r=0; r<3; r++) {
        for(int c=0; c<3; c++) {
            currentR[r][c] = pose->mDeviceToAbsoluteTracking.m[r][c];
        }
    }
    
    for(int r=0; r<3; r++) {
        for(int c=0; c<3; c++) {
            pose->mDeviceToAbsoluteTracking.m[r][c] = 0;
            for(int k=0; k<3; k++) {
                pose->mDeviceToAbsoluteTracking.m[r][c] += R_delta_extrapolate[r][k] * currentR[k][c];
            }
        }
    }
    
    // Re-normalize rotation matrix to prevent distortion
    float col0_mag = sqrt(pose->mDeviceToAbsoluteTracking.m[0][0]*pose->mDeviceToAbsoluteTracking.m[0][0] + pose->mDeviceToAbsoluteTracking.m[1][0]*pose->mDeviceToAbsoluteTracking.m[1][0] + pose->mDeviceToAbsoluteTracking.m[2][0]*pose->mDeviceToAbsoluteTracking.m[2][0]);
    float col1_mag = sqrt(pose->mDeviceToAbsoluteTracking.m[0][1]*pose->mDeviceToAbsoluteTracking.m[0][1] + pose->mDeviceToAbsoluteTracking.m[1][1]*pose->mDeviceToAbsoluteTracking.m[1][1] + pose->mDeviceToAbsoluteTracking.m[2][1]*pose->mDeviceToAbsoluteTracking.m[2][1]);
    float col2_mag = sqrt(pose->mDeviceToAbsoluteTracking.m[0][2]*pose->mDeviceToAbsoluteTracking.m[0][2] + pose->mDeviceToAbsoluteTracking.m[1][2]*pose->mDeviceToAbsoluteTracking.m[1][2] + pose->mDeviceToAbsoluteTracking.m[2][2]*pose->mDeviceToAbsoluteTracking.m[2][2]);
    
    if (col0_mag > 0) { pose->mDeviceToAbsoluteTracking.m[0][0]/=col0_mag; pose->mDeviceToAbsoluteTracking.m[1][0]/=col0_mag; pose->mDeviceToAbsoluteTracking.m[2][0]/=col0_mag; }
    if (col1_mag > 0) { pose->mDeviceToAbsoluteTracking.m[0][1]/=col1_mag; pose->mDeviceToAbsoluteTracking.m[1][1]/=col1_mag; pose->mDeviceToAbsoluteTracking.m[2][1]/=col1_mag; }
    if (col2_mag > 0) { pose->mDeviceToAbsoluteTracking.m[0][2]/=col2_mag; pose->mDeviceToAbsoluteTracking.m[1][2]/=col2_mag; pose->mDeviceToAbsoluteTracking.m[2][2]/=col2_mag; }
    
    pose->vVelocity.v[0] = 0.0f;
    pose->vVelocity.v[1] = 0.0f;
    pose->vVelocity.v[2] = 0.0f;
    pose->vAngularVelocity.v[0] = 0.0f;
    pose->vAngularVelocity.v[1] = 0.0f;
    pose->vAngularVelocity.v[2] = 0.0f;
}

void ApplySmoothPose(vr::TrackedDevicePose_t* pose, float* targetTransform, float* lastTransform, float* lastVel, float* lastAngularVel, float fPredictedSecondsToPhotonsFromNow, float dt) {
    if (lastTransform[0] == 0.0f && lastTransform[5] == 0.0f && lastTransform[10] == 0.0f) {
        for(int i=0; i<16; i++) lastTransform[i] = targetTransform[i];
    }
    
    pose->bPoseIsValid = true;
    pose->bDeviceIsConnected = true;
    pose->eTrackingResult = vr::TrackingResult_Running_OK;

    // Apply a local rotation offset (Pitch ~ -60 degrees) because ARKit wrist origin points straight up the forearm
    // We need the laser pointer to shoot out of the "front" of the hand holding the JoyCon.
    float cosT = 0.5f;        // cos(-60 deg)
    float sinT = -0.866025f;  // sin(-60 deg)
    
    // Create offset matrix for X-axis rotation
    float offsetR[3][3] = {
        {1.0f, 0.0f, 0.0f},
        {0.0f, cosT, -sinT},
        {0.0f, sinT, cosT}
    };
    
    float correctedTransform[16];
    for(int r=0; r<3; r++) {
        for(int c=0; c<3; c++) {
            correctedTransform[c*4 + r] = 0.0f;
            for(int k=0; k<3; k++) {
                correctedTransform[c*4 + r] += targetTransform[k*4 + r] * offsetR[k][c];
            }
        }
        correctedTransform[3*4 + r] = targetTransform[3*4 + r]; // Translation remains unchanged
    }
    correctedTransform[3] = 0; correctedTransform[7] = 0; correctedTransform[11] = 0; correctedTransform[15] = 1;

    // Use absolute raw 1:1 tracking from Vision Pro with rotation offset
    for(int r=0; r<3; r++) {
        for(int c=0; c<4; c++) {
            pose->mDeviceToAbsoluteTracking.m[r][c] = correctedTransform[c*4 + r];
        }
    }

    // Calculate Velocity
    if (dt > 0.0001f) {
        float rawVel[3];
        rawVel[0] = (targetTransform[12] - lastTransform[12]) / dt;
        rawVel[1] = (targetTransform[13] - lastTransform[13]) / dt;
        rawVel[2] = (targetTransform[14] - lastTransform[14]) / dt;
        
        float velSmooth = 10.0f * dt;
        if (velSmooth > 1.0f) velSmooth = 1.0f;
        lastVel[0] += (rawVel[0] - lastVel[0]) * velSmooth;
        lastVel[1] += (rawVel[1] - lastVel[1]) * velSmooth;
        lastVel[2] += (rawVel[2] - lastVel[2]) * velSmooth;
        
        // Calculate Angular Velocity
        float R_prev[3][3];
        float R_cur[3][3];
        for(int r=0; r<3; r++) {
            for(int c=0; c<3; c++) {
                R_prev[r][c] = lastTransform[c*4 + r];
                R_cur[r][c] = targetTransform[c*4 + r];
            }
        }
        
        float R_delta[3][3];
        for(int r=0; r<3; r++) {
            for(int c=0; c<3; c++) {
                R_delta[r][c] = 0;
                for(int k=0; k<3; k++) {
                    R_delta[r][c] += R_cur[r][k] * R_prev[c][k];
                }
            }
        }
        
        float rawOmegaX = (R_delta[2][1] - R_delta[1][2]) / (2.0f * dt);
        float rawOmegaY = (R_delta[0][2] - R_delta[2][0]) / (2.0f * dt);
        float rawOmegaZ = (R_delta[1][0] - R_delta[0][1]) / (2.0f * dt);
        
        if (fabs(rawOmegaX) > 50.0f) rawOmegaX = 0;
        if (fabs(rawOmegaY) > 50.0f) rawOmegaY = 0;
        if (fabs(rawOmegaZ) > 50.0f) rawOmegaZ = 0;
        
        float angSmooth = 15.0f * dt;
        if (angSmooth > 1.0f) angSmooth = 1.0f;
        lastAngularVel[0] += (rawOmegaX - lastAngularVel[0]) * angSmooth;
        lastAngularVel[1] += (rawOmegaY - lastAngularVel[1]) * angSmooth;
        lastAngularVel[2] += (rawOmegaZ - lastAngularVel[2]) * angSmooth;
    }
    
    // Apply SteamVR's Native Forward Prediction (Photons)
    pose->mDeviceToAbsoluteTracking.m[0][3] += lastVel[0] * fPredictedSecondsToPhotonsFromNow;
    pose->mDeviceToAbsoluteTracking.m[1][3] += lastVel[1] * fPredictedSecondsToPhotonsFromNow;
    pose->mDeviceToAbsoluteTracking.m[2][3] += lastVel[2] * fPredictedSecondsToPhotonsFromNow;
    
    pose->vVelocity.v[0] = lastVel[0];
    pose->vVelocity.v[1] = lastVel[1];
    pose->vVelocity.v[2] = lastVel[2];
    pose->vAngularVelocity.v[0] = lastAngularVel[0];
    pose->vAngularVelocity.v[1] = lastAngularVel[1];
    pose->vAngularVelocity.v[2] = lastAngularVel[2];
    
    for(int i=0; i<16; i++) lastTransform[i] = correctedTransform[i];
}

static void InitSharedMemory() {
    if (hMapFile) return;
    
    const int mapSize = sizeof(DoubleBufferHeader) + (FRAME_BLOCK_SIZE * 2) + sizeof(SharedHands) + 1024;
    
    HANDLE hFile = CreateFileA("C:\\vr_shared_frame.dat", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER fsize;
        GetFileSizeEx(hFile, &fsize);
        if (fsize.QuadPart < mapSize) {
            SetFilePointer(hFile, mapSize - 1, NULL, FILE_BEGIN);
            DWORD written;
            WriteFile(hFile, "", 1, &written, NULL);
        }
        hMapFile = CreateFileMappingA(hFile, NULL, PAGE_READWRITE, 0, mapSize, NULL);
        if (hMapFile) {
            pBuf = MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, mapSize);
            if (pBuf) {
                pDbHeader = (DoubleBufferHeader*)pBuf;
                // Double buffering: two blocks of FRAME_BLOCK_SIZE
                uint8_t* base = (uint8_t*)pBuf + sizeof(DoubleBufferHeader);
                
                pHeaders[0] = (SharedFrame*)base;
                pPixelDatas[0] = base + sizeof(SharedFrame);
                
                pHeaders[1] = (SharedFrame*)(base + FRAME_BLOCK_SIZE);
                pPixelDatas[1] = base + FRAME_BLOCK_SIZE + sizeof(SharedFrame);
                
                pSharedHands = (SharedHands*)(base + (FRAME_BLOCK_SIZE * 2));
            }
        }
    }
}

using namespace vr;

class UniversalMock {
public:
    virtual void* Dummy0() { return nullptr; }
    virtual void* Dummy1() { return nullptr; }
    virtual void* Dummy2() { return nullptr; }
    virtual void* Dummy3() { return nullptr; }
    virtual void* Dummy4() { return nullptr; }
    virtual void* Dummy5() { return nullptr; }
    virtual void* Dummy6() { return nullptr; }
    virtual void* Dummy7() { return nullptr; }
    virtual void* Dummy8() { return nullptr; }
    virtual void* Dummy9() { return nullptr; }
    virtual void* Dummy10() { return nullptr; }
    virtual void* Dummy11() { return nullptr; }
    virtual void* Dummy12() { return nullptr; }
    virtual void* Dummy13() { return nullptr; }
    virtual void* Dummy14() { return nullptr; }
    virtual void* Dummy15() { return nullptr; }
    virtual void* Dummy16() { return nullptr; }
    virtual void* Dummy17() { return nullptr; }
    virtual void* Dummy18() { return nullptr; }
    virtual void* Dummy19() { return nullptr; }
    virtual void* Dummy20() { return nullptr; }
    virtual void* Dummy21() { return nullptr; }
    virtual void* Dummy22() { return nullptr; }
    virtual void* Dummy23() { return nullptr; }
    virtual void* Dummy24() { return nullptr; }
    virtual void* Dummy25() { return nullptr; }
    virtual void* Dummy26() { return nullptr; }
    virtual void* Dummy27() { return nullptr; }
    virtual void* Dummy28() { return nullptr; }
    virtual void* Dummy29() { return nullptr; }
    virtual void* Dummy30() { return nullptr; }
    virtual void* Dummy31() { return nullptr; }
    virtual void* Dummy32() { return nullptr; }
    virtual void* Dummy33() { return nullptr; }
    virtual void* Dummy34() { return nullptr; }
    virtual void* Dummy35() { return nullptr; }
    virtual void* Dummy36() { return nullptr; }
    virtual void* Dummy37() { return nullptr; }
    virtual void* Dummy38() { return nullptr; }
    virtual void* Dummy39() { return nullptr; }
    virtual void* Dummy40() { return nullptr; }
    virtual void* Dummy41() { return nullptr; }
    virtual void* Dummy42() { return nullptr; }
    virtual void* Dummy43() { return nullptr; }
    virtual void* Dummy44() { return nullptr; }
    virtual void* Dummy45() { return nullptr; }
    virtual void* Dummy46() { return nullptr; }
    virtual void* Dummy47() { return nullptr; }
    virtual void* Dummy48() { return nullptr; }
    virtual void* Dummy49() { return nullptr; }
    virtual void* Dummy50() { return nullptr; }
    virtual void* Dummy51() { return nullptr; }
    virtual void* Dummy52() { return nullptr; }
    virtual void* Dummy53() { return nullptr; }
    virtual void* Dummy54() { return nullptr; }
    virtual void* Dummy55() { return nullptr; }
    virtual void* Dummy56() { return nullptr; }
    virtual void* Dummy57() { return nullptr; }
    virtual void* Dummy58() { return nullptr; }
    virtual void* Dummy59() { return nullptr; }
    virtual void* Dummy60() { return nullptr; }
    virtual void* Dummy61() { return nullptr; }
    virtual void* Dummy62() { return nullptr; }
    virtual void* Dummy63() { return nullptr; }
    virtual void* Dummy64() { return nullptr; }
    virtual void* Dummy65() { return nullptr; }
    virtual void* Dummy66() { return nullptr; }
    virtual void* Dummy67() { return nullptr; }
    virtual void* Dummy68() { return nullptr; }
    virtual void* Dummy69() { return nullptr; }
    virtual void* Dummy70() { return nullptr; }
    virtual void* Dummy71() { return nullptr; }
    virtual void* Dummy72() { return nullptr; }
    virtual void* Dummy73() { return nullptr; }
    virtual void* Dummy74() { return nullptr; }
    virtual void* Dummy75() { return nullptr; }
    virtual void* Dummy76() { return nullptr; }
    virtual void* Dummy77() { return nullptr; }
    virtual void* Dummy78() { return nullptr; }
    virtual void* Dummy79() { return nullptr; }
    virtual void* Dummy80() { return nullptr; }
    virtual void* Dummy81() { return nullptr; }
    virtual void* Dummy82() { return nullptr; }
    virtual void* Dummy83() { return nullptr; }
    virtual void* Dummy84() { return nullptr; }
    virtual void* Dummy85() { return nullptr; }
    virtual void* Dummy86() { return nullptr; }
    virtual void* Dummy87() { return nullptr; }
    virtual void* Dummy88() { return nullptr; }
    virtual void* Dummy89() { return nullptr; }
    virtual void* Dummy90() { return nullptr; }
    virtual void* Dummy91() { return nullptr; }
    virtual void* Dummy92() { return nullptr; }
    virtual void* Dummy93() { return nullptr; }
    virtual void* Dummy94() { return nullptr; }
    virtual void* Dummy95() { return nullptr; }
    virtual void* Dummy96() { return nullptr; }
    virtual void* Dummy97() { return nullptr; }
    virtual void* Dummy98() { return nullptr; }
    virtual void* Dummy99() { return nullptr; }
};
UniversalMock g_universalMock;

#include <cmath>

class Mock_IVRSystem : public vr::IVRSystem {
public:
    virtual void GetRecommendedRenderTargetSize(uint32_t *pnWidth, uint32_t *pnHeight) override {
        LogMsg("Called: IVRSystem::GetRecommendedRenderTargetSize\n");
        SharedHands handsSnapshot = {};
        const bool haveHands = ReadSharedHandsSnapshot(&handsSnapshot);
        // Dynamically calculate the perfect aspect ratio directly from the physical tangents!
        if (haveHands && !std::isnan(handsSnapshot.leftTangents[0]) && handsSnapshot.leftTangents[0] != 0.0f) {
            float widthScale = (handsSnapshot.leftTangents[1] - handsSnapshot.leftTangents[0]);
            float heightScale = std::abs(handsSnapshot.leftTangents[2] - handsSnapshot.leftTangents[3]);
            // Apply 1.15x over-render margin to the base resolution
            if(pnWidth) *pnWidth = (uint32_t)(2000.0f * (widthScale / 2.0f) * 1.15f);
            if(pnHeight) *pnHeight = (uint32_t)(2000.0f * (heightScale / 2.0f) * 1.15f);
        } else {
            // Fallback
            if(pnWidth) *pnWidth = 1920;
            if(pnHeight) *pnHeight = 1920;
        }
    }
    virtual void GetProjectionMatrix(vr::HmdMatrix44_t *pRet, vr::EVREye eEye, float fNearZ, float fFarZ) override {
        if (pRet) {
            memset(pRet, 0, sizeof(*pRet));
            // Provide a valid 90-degree FOV perspective projection matrix to prevent zero-division math collapse
            float fov = 90.0f * (3.1415926535f / 180.0f);
            float y_scale = 1.0f / std::tan(fov / 2.0f);
            float x_scale = y_scale / (1920.0f / 2160.0f); // 1920x2160 aspect ratio
            pRet->m[0][0] = x_scale;
            pRet->m[1][1] = y_scale;
            pRet->m[2][2] = -fFarZ / (fFarZ - fNearZ);
            pRet->m[2][3] = -(fFarZ * fNearZ) / (fFarZ - fNearZ);
            pRet->m[3][2] = -1.0f;
            pRet->m[3][3] = 0.0f;
        }
    }
    virtual void GetProjectionRaw(EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom) override {
        // OVERRENDER MARGIN: Render 15% more FOV to hide black borders during fast head movements
        float overrender = 1.15f;
        SharedHands handsSnapshot = {};
        const bool haveHands = ReadSharedHandsSnapshot(&handsSnapshot);
        if (haveHands && !std::isnan(handsSnapshot.leftTangents[0]) && handsSnapshot.leftTangents[0] != 0.0f) {
            float* t = (eEye == vr::Eye_Left) ? handsSnapshot.leftTangents : handsSnapshot.rightTangents;
            // Vision Pro tangents: [0]=left, [1]=right, [2]=top, [3]=bottom
            if(pfLeft) *pfLeft = t[0] * overrender;
            if(pfRight) *pfRight = t[1] * overrender;
            if(pfTop) *pfTop = t[2] * overrender;
            if(pfBottom) *pfBottom = t[3] * overrender;
        } else {
            // Fallback
            if(pfLeft) *pfLeft = -1.0f * overrender; 
            if(pfRight) *pfRight = 1.0f * overrender; 
            if(pfTop) *pfTop = 1.0f * overrender; 
            if(pfBottom) *pfBottom = -1.0f * overrender;
        }
    }
    virtual bool ComputeDistortion(EVREye eEye, float fU, float fV, DistortionCoordinates_t *pDistortionCoordinates) override {
        if(pDistortionCoordinates) {
            pDistortionCoordinates->rfRed[0] = fU;
            pDistortionCoordinates->rfRed[1] = fV;
            pDistortionCoordinates->rfGreen[0] = fU;
            pDistortionCoordinates->rfGreen[1] = fV;
            pDistortionCoordinates->rfBlue[0] = fU;
            pDistortionCoordinates->rfBlue[1] = fV;
        }
        return true;
    }
    virtual void GetEyeToHeadTransform(HmdMatrix34_t *pRet, EVREye eEye) override {
        if(pRet) { memset(pRet, 0, sizeof(*pRet)); pRet->m[0][0] = 1; pRet->m[1][1] = 1; pRet->m[2][2] = 1; }
    }
    virtual bool GetTimeSinceLastVsync(float *pfSecondsSinceLastVsync, uint64_t *pulFrameCounter) override {
        static auto startTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - startTime).count();
        double frameDuration = 1.0 / 120.0;
        if (pulFrameCounter) *pulFrameCounter = (uint64_t)(elapsed / frameDuration);
        if (pfSecondsSinceLastVsync) {
            uint64_t fcount = (uint64_t)(elapsed / frameDuration);
            *pfSecondsSinceLastVsync = (float)(elapsed - (fcount * frameDuration));
        }
        return true;
    }
    virtual int32_t GetD3D9AdapterIndex() override {
        LogMsg("Called: IVRSystem::GetD3D9AdapterIndex\n");
        return (int32_t)0;
    }
    virtual void GetDXGIOutputInfo(int32_t *pnAdapterIndex) override {
        LogMsg("Called: IVRSystem::GetDXGIOutputInfo\n");
        if(pnAdapterIndex) *pnAdapterIndex = 0;
    }
    virtual void GetOutputDevice(uint64_t *pnDevice, ETextureType textureType, VkInstance_T *pInstance = nullptr) override {
        LogMsg("Called: IVRSystem::GetOutputDevice\n");
        if(pnDevice) *pnDevice = 0;
    }
    virtual bool IsDisplayOnDesktop() override {
        LogMsg("Called: IVRSystem::IsDisplayOnDesktop\n");
        return false;
    }
    virtual bool SetDisplayVisibility(bool bIsVisibleOnDesktop) override {
        LogMsg("Called: IVRSystem::SetDisplayVisibility\n");
        return false;
    }
    virtual void GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin eOrigin, float fPredictedSecondsToPhotonsFromNow, VR_ARRAY_COUNT(unTrackedDevicePoseArrayCount) TrackedDevicePose_t *pTrackedDevicePoseArray, uint32_t unTrackedDevicePoseArrayCount) override {
        LogMsg("Called: IVRSystem::GetDeviceToAbsoluteTrackingPose\n");
        if(pTrackedDevicePoseArray && unTrackedDevicePoseArrayCount > 0) { 
            SharedHands handsSnapshot = {};
            const bool haveHands = ReadSharedHandsSnapshot(&handsSnapshot);
            static auto lastTime = std::chrono::high_resolution_clock::now();
            auto now = std::chrono::high_resolution_clock::now();
            float dt = std::chrono::duration<float>(now - lastTime).count();
            if (dt < 0.0001f) dt = 0.0001f;
            lastTime = now;

            memset(pTrackedDevicePoseArray, 0, sizeof(vr::TrackedDevicePose_t) * unTrackedDevicePoseArrayCount); 
            for(uint32_t i=0; i<3 && i<unTrackedDevicePoseArrayCount; ++i) { 
                pTrackedDevicePoseArray[i].bDeviceIsConnected = true; 
                pTrackedDevicePoseArray[i].eTrackingResult = vr::TrackingResult_Running_OutOfRange;
                
                if (i == 0 && haveHands && handsSnapshot.headTracked) {
                    ApplyHeadExtrapolation(&pTrackedDevicePoseArray[i], handsSnapshot.headTransform, handsSnapshot.visionTimestamp, fPredictedSecondsToPhotonsFromNow);
                } else if (i == 1 || i == 2) {
                    const bool tracked = haveHands && (i == 1 ? handsSnapshot.leftTracked : handsSnapshot.rightTracked);
                    float* srcTransform = i == 1 ? handsSnapshot.leftTransform : handsSnapshot.rightTransform;
                    if (tracked) {
                        if (i == 1) ApplySmoothPose(&pTrackedDevicePoseArray[i], srcTransform, g_lastLeftTransform, g_lastLeftVel, g_lastLeftAngularVel, fPredictedSecondsToPhotonsFromNow, dt);
                        if (i == 2) ApplySmoothPose(&pTrackedDevicePoseArray[i], srcTransform, g_lastRightTransform, g_lastRightVel, g_lastRightAngularVel, fPredictedSecondsToPhotonsFromNow, dt);
                    }
                }
            } 
        }
    }
    virtual void ResetSeatedZeroPose() override {
        LogMsg("Called: IVRSystem::ResetSeatedZeroPose\n");
    }
    virtual void GetSeatedZeroPoseToStandingAbsoluteTrackingPose(HmdMatrix34_t *pRet) override {
        if(pRet) { memset(pRet, 0, sizeof(*pRet)); pRet->m[0][0] = 1; pRet->m[1][1] = 1; pRet->m[2][2] = 1; }
    }
    virtual void GetRawZeroPoseToStandingAbsoluteTrackingPose(HmdMatrix34_t *pRet) override {
        if(pRet) { memset(pRet, 0, sizeof(*pRet)); pRet->m[0][0] = 1; pRet->m[1][1] = 1; pRet->m[2][2] = 1; }
    }
    virtual uint32_t GetSortedTrackedDeviceIndicesOfClass(ETrackedDeviceClass eTrackedDeviceClass, VR_ARRAY_COUNT(unTrackedDeviceIndexArrayCount) vr::TrackedDeviceIndex_t *punTrackedDeviceIndexArray, uint32_t unTrackedDeviceIndexArrayCount, vr::TrackedDeviceIndex_t unRelativeToTrackedDeviceIndex = k_unTrackedDeviceIndex_Hmd) override {
        LogMsg("Called: IVRSystem::GetSortedTrackedDeviceIndicesOfClass\n");
        return (uint32_t)0;
    }
    virtual EDeviceActivityLevel GetTrackedDeviceActivityLevel(vr::TrackedDeviceIndex_t unDeviceId) override {
        return vr::k_EDeviceActivityLevel_UserInteraction;
    }
    virtual void ApplyTransform(TrackedDevicePose_t *pOutputPose, const TrackedDevicePose_t *pTrackedDevicePose, const HmdMatrix34_t *pTransform) override {
        LogMsg("Called: IVRSystem::ApplyTransform\n");
    }
    virtual vr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole unDeviceType) override {
        LogMsg("Called: IVRSystem::GetTrackedDeviceIndexForControllerRole\n");
        if (unDeviceType == vr::TrackedControllerRole_LeftHand) return 1;
        if (unDeviceType == vr::TrackedControllerRole_RightHand) return 2;
        return vr::k_unTrackedDeviceIndexInvalid;
    }
    virtual vr::ETrackedControllerRole GetControllerRoleForTrackedDeviceIndex(vr::TrackedDeviceIndex_t unDeviceIndex) override {
        LogMsg("Called: IVRSystem::GetControllerRoleForTrackedDeviceIndex\n");
        if (unDeviceIndex == 1) return vr::TrackedControllerRole_LeftHand;
        if (unDeviceIndex == 2) return vr::TrackedControllerRole_RightHand;
        return vr::TrackedControllerRole_Invalid;
    }
    virtual ETrackedDeviceClass GetTrackedDeviceClass(vr::TrackedDeviceIndex_t unDeviceIndex) override {
        if (unDeviceIndex == vr::k_unTrackedDeviceIndex_Hmd) return vr::TrackedDeviceClass_HMD;
        if (unDeviceIndex == 1 || unDeviceIndex == 2) return vr::TrackedDeviceClass_Controller;
        return vr::TrackedDeviceClass_Invalid;
    }
    virtual bool IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t unDeviceIndex) override {
        if(unDeviceIndex == 0 || unDeviceIndex == 1 || unDeviceIndex == 2) return true;
        return false;
    }
    virtual bool GetBoolTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError = 0L) override {
        FILE* f = fopen("vr_emulator_log.txt", "a");
        if(f) { fprintf(f, "Called: IVRSystem::GetBoolTrackedDeviceProperty prop=%d\n", (int)prop); fclose(f); }
        if (prop == vr::Prop_DeviceProvidesBatteryStatus_Bool || prop == vr::Prop_ContainsProximitySensor_Bool) return true;
        return false;
    }
    virtual float GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError = 0L) override {
        if(pError) *pError = vr::TrackedProp_Success;
        if(prop == vr::Prop_DisplayFrequency_Float) return 90.0f;
        return 0.0f;
    }
    virtual int32_t GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError = 0L) override {
        LogMsg("Called: IVRSystem::GetInt32TrackedDeviceProperty\n");
        if(pError) *pError = vr::TrackedProp_Success;
        if(prop == vr::Prop_DeviceClass_Int32) { if(unDeviceIndex == 0) return vr::TrackedDeviceClass_HMD; if(unDeviceIndex == 1 || unDeviceIndex == 2) return vr::TrackedDeviceClass_Controller; }
        if(prop == vr::Prop_ControllerRoleHint_Int32) { if(unDeviceIndex == 1) return vr::TrackedControllerRole_LeftHand; if(unDeviceIndex == 2) return vr::TrackedControllerRole_RightHand; }
        if(prop == vr::Prop_Axis0Type_Int32) return vr::k_eControllerAxis_Joystick;
        if(prop == vr::Prop_Axis1Type_Int32) return vr::k_eControllerAxis_Trigger;
        if(prop == vr::Prop_Axis2Type_Int32) return vr::k_eControllerAxis_Trigger;
        return 0;
    }
    virtual uint64_t GetUint64TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError = 0L) override {
        LogMsg("Called: IVRSystem::GetUint64TrackedDeviceProperty\n");
        if(pError) *pError = vr::TrackedProp_Success;
        if(prop == vr::Prop_CurrentUniverseId_Uint64) return 1;
        if(prop == vr::Prop_SupportedButtons_Uint64) return 0xFFFFFFFFFFFFFFFFULL;
        return 0;
    }
    virtual void GetMatrix34TrackedDeviceProperty(HmdMatrix34_t *pRet, vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError = 0L) override {
        LogMsg("Called: IVRSystem::GetMatrix34TrackedDeviceProperty\n");
        if(pRet) { memset(pRet, 0, sizeof(*pRet)); pRet->m[0][0] = 1; pRet->m[1][1] = 1; pRet->m[2][2] = 1; }
        if(pError) *pError = vr::TrackedProp_Success;
    }
    virtual uint32_t GetArrayTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, PropertyTypeTag_t propType, void *pBuffer, uint32_t unBufferSize, ETrackedPropertyError *pError = 0L) override {
        LogMsg("Called: IVRSystem::GetArrayTrackedDeviceProperty\n");
        return (uint32_t)0;
    }
    virtual uint32_t GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, VR_OUT_STRING() char *pchValue, uint32_t unBufferSize, ETrackedPropertyError *pError = 0L) override {
        const char* s = "";
        bool handled = true;
        if (prop == vr::Prop_RenderModelName_String) {
            if (unDeviceIndex == 0) s = "generic_hmd";
            else if (unDeviceIndex == 1) s = "oculus_rifts_controller_left";
            else if (unDeviceIndex == 2) s = "oculus_rifts_controller_right";
        }
        else if (prop == vr::Prop_ControllerType_String) s = "oculus_touch";
        else if (prop == vr::Prop_TrackingSystemName_String) s = "oculus";
        else if (prop == vr::Prop_ManufacturerName_String) s = "Oculus";
        else if (prop == vr::Prop_ResourceRoot_String) s = "oculus";
        else if (prop == vr::Prop_RegisteredDeviceType_String) {
            if (unDeviceIndex == 1) s = "oculus/oculus_touch_left";
            else if (unDeviceIndex == 2) s = "oculus/oculus_touch_right";
        }
        else if (prop == vr::Prop_SerialNumber_String) {
            if (unDeviceIndex == 1) s = "LHR-FFFFFFFF";
            else if (unDeviceIndex == 2) s = "LHR-EEEEEEEE";
            else s = "LHR-DDDDDDDD";
        }
        else if (prop == vr::Prop_ModelNumber_String) {
            if (unDeviceIndex == 1) s = "Oculus Rift S (Left Controller)";
            else if (unDeviceIndex == 2) s = "Oculus Rift S (Right Controller)";
            else s = "HMD";
        }
        else if (prop == vr::Prop_InputProfilePath_String) {
            s = "{oculus}/input/touch_profile.json";
        }
        else if (prop == vr::Prop_AttachedDeviceId_String) {
            if (unDeviceIndex == 1) s = "123456789";
            else if (unDeviceIndex == 2) s = "987654321";
        } else {
            handled = false;
            char msg[256];
            snprintf(msg, sizeof(msg), "Unhandled String Prop: %d for device %d\n", prop, unDeviceIndex);
            LogMsg(msg);
            s = "Generic";
        }

        if(pchValue && unBufferSize > 0) { strncpy(pchValue, s, unBufferSize - 1); pchValue[unBufferSize - 1] = '\0'; }
        if(pError) *pError = vr::TrackedProp_Success;
        return (uint32_t)strlen(s) + 1;
    }
    virtual const char * GetPropErrorNameFromEnum(ETrackedPropertyError error) override {
        LogMsg("Called: IVRSystem::GetPropErrorNameFromEnum\n");
        return "1.10.30";
    }
    virtual bool PollNextEvent(vr::VREvent_t *pEvent, uint32_t uncbVREvent) override {
        static int count = 0;
        if (count == 0) { count++; if(pEvent) { memset(pEvent, 0, uncbVREvent); pEvent->eventType = (vr::EVREventType)100; pEvent->trackedDeviceIndex = 0; } return true; }
        else if (count == 1) { count++; if(pEvent) { memset(pEvent, 0, uncbVREvent); pEvent->eventType = (vr::EVREventType)100; pEvent->trackedDeviceIndex = 1; } return true; }
        else if (count == 2) { count++; if(pEvent) { memset(pEvent, 0, uncbVREvent); pEvent->eventType = (vr::EVREventType)100; pEvent->trackedDeviceIndex = 2; } return true; }
        else if (count == 3) { count++; if(pEvent) { memset(pEvent, 0, uncbVREvent); pEvent->eventType = (vr::EVREventType)103; pEvent->trackedDeviceIndex = 0; } return true; } // UserInteractionStarted
        else if (count == 4) { count++; if(pEvent) { memset(pEvent, 0, uncbVREvent); pEvent->eventType = (vr::EVREventType)200; pEvent->trackedDeviceIndex = 0; pEvent->data.controller.button = 31; } return true; } // Proximity ButtonPress
        else if (count == 5) { count++; if(pEvent) { memset(pEvent, 0, uncbVREvent); pEvent->eventType = (vr::EVREventType)403; pEvent->trackedDeviceIndex = 0; } return true; }
        static uint64_t lastLeftVRButtons = 0;
        static uint64_t lastRightVRButtons = 0;
        
        SharedHands handsSnapshot = {};
        const bool haveHands = ReadSharedHandsSnapshot(&handsSnapshot);
        if (haveHands && pEvent) {
            uint32_t lb = handsSnapshot.leftButtons;
            uint32_t rb = handsSnapshot.rightButtons;
            uint64_t currentLeftVRButtons = 0;
            uint64_t currentRightVRButtons = 0;
            
            if (handsSnapshot.leftTracked && handsSnapshot.leftTrigger >= 128) currentLeftVRButtons |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger);
            if (handsSnapshot.leftTracked && handsSnapshot.leftGrip >= 150) currentLeftVRButtons |= vr::ButtonMaskFromId(vr::k_EButton_Grip);
            if (handsSnapshot.leftTracked && (lb & (1u << 8))) currentLeftVRButtons |= vr::ButtonMaskFromId(vr::k_EButton_A); // Vision Pro left primary (X)
            if (handsSnapshot.leftTracked && (lb & (1u << 9))) currentLeftVRButtons |= vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu); // Vision Pro left secondary (Y)
            if (lb & (1<<4)) currentLeftVRButtons |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger); // ZL
            if (lb & (1<<5)) currentLeftVRButtons |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad); // L3
            if (lb & (1<<6)) currentLeftVRButtons |= vr::ButtonMaskFromId(vr::k_EButton_System); // Minus

            if (handsSnapshot.rightTracked && handsSnapshot.rightTrigger >= 128) currentRightVRButtons |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger);
            if (handsSnapshot.rightTracked && handsSnapshot.rightGrip >= 150) currentRightVRButtons |= vr::ButtonMaskFromId(vr::k_EButton_Grip);
            if (rb & (1<<2)) currentRightVRButtons |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger); // ZR
            if (rb & (1<<0)) currentRightVRButtons |= vr::ButtonMaskFromId(vr::k_EButton_A); // A
            if (rb & (1<<1)) currentRightVRButtons |= vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu); // B -> App Menu
            if (rb & (1<<3)) currentRightVRButtons |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad); // R3
            if (rb & (1<<4)) currentRightVRButtons |= vr::ButtonMaskFromId(vr::k_EButton_System); // Plus

            uint64_t leftDiff = currentLeftVRButtons ^ lastLeftVRButtons;
            if (leftDiff != 0) {
                for (int i = 0; i < 64; i++) {
                    if (leftDiff & (1ULL << i)) {
                        memset(pEvent, 0, uncbVREvent);
                        pEvent->trackedDeviceIndex = 1;
                        pEvent->data.controller.button = i;
                        pEvent->eventType = (currentLeftVRButtons & (1ULL << i)) ? (vr::EVREventType)200 : (vr::EVREventType)201;
                        lastLeftVRButtons = (lastLeftVRButtons & ~(1ULL << i)) | (currentLeftVRButtons & (1ULL << i));
                        return true;
                    }
                }
            }

            uint64_t rightDiff = currentRightVRButtons ^ lastRightVRButtons;
            if (rightDiff != 0) {
                for (int i = 0; i < 64; i++) {
                    if (rightDiff & (1ULL << i)) {
                        memset(pEvent, 0, uncbVREvent);
                        pEvent->trackedDeviceIndex = 2;
                        pEvent->data.controller.button = i;
                        pEvent->eventType = (currentRightVRButtons & (1ULL << i)) ? (vr::EVREventType)200 : (vr::EVREventType)201;
                        lastRightVRButtons = (lastRightVRButtons & ~(1ULL << i)) | (currentRightVRButtons & (1ULL << i));
                        return true;
                    }
                }
            }
        }
        return false;
    }
    virtual bool PollNextEventWithPose(ETrackingUniverseOrigin eOrigin, vr::VREvent_t *pEvent, uint32_t uncbVREvent, vr::TrackedDevicePose_t *pTrackedDevicePose) override {
        return PollNextEvent(pEvent, uncbVREvent);
    }
    virtual const char * GetEventTypeNameFromEnum(EVREventType eType) override {
        LogMsg("Called: IVRSystem::GetEventTypeNameFromEnum\n");
        return "1.10.30";
    }
    virtual void GetHiddenAreaMesh(HiddenAreaMesh_t *pRet, EVREye eEye, EHiddenAreaMeshType type = k_eHiddenAreaMesh_Standard) override {
        LogMsg("Called: IVRSystem::GetHiddenAreaMesh\n");
        static vr::HmdVector2_t dummy_verts[3] = { { 2.0f, 2.0f }, { 2.0f, 2.1f }, { 2.1f, 2.0f } };
        if(pRet) { 
            pRet->pVertexData = dummy_verts; 
            pRet->unTriangleCount = 1; 
        }
    }
    virtual bool GetControllerState(vr::TrackedDeviceIndex_t unControllerDeviceIndex, vr::VRControllerState_t *pControllerState, uint32_t unControllerStateSize) override {
        if(pControllerState) {
            memset(pControllerState, 0, unControllerStateSize);
            pControllerState->unPacketNum = frameSeq;
            SharedHands handsSnapshot = {};
            const bool haveHands = ReadSharedHandsSnapshot(&handsSnapshot);
            if (haveHands) {
                uint32_t lb = handsSnapshot.leftButtons;
                uint32_t rb = handsSnapshot.rightButtons;
                if (unControllerDeviceIndex == 0) {
                    pControllerState->ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_ProximitySensor);
                } else if (unControllerDeviceIndex == 1) {
                    const bool tracked = handsSnapshot.leftTracked != 0;
                    const float trigger = tracked ? handsSnapshot.leftTrigger / 255.0f : 0.0f;
                    const float grip = tracked ? handsSnapshot.leftGrip / 255.0f : 0.0f;
                    // Left JoyCon Buttons
                    // Vision Pro pinch plus JoyCon ZL map to the SteamVR trigger.
                    pControllerState->rAxis[1].x = std::max(trigger, (lb & (1<<4)) ? 1.0f : 0.0f);
                    if (pControllerState->rAxis[1].x >= 0.5f) {
                        pControllerState->ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger);
                    }
                    // Vision Pro finger curl plus JoyCon SL/SR map to grip.
                    pControllerState->rAxis[2].x = std::max(grip, ((lb & (1<<7)) || (lb & (1<<8))) ? 1.0f : 0.0f);
                    if (pControllerState->rAxis[2].x >= 0.5f) {
                        pControllerState->ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_Grip);
                    }
                    // L3 (Stick Click)
                    if (lb & (1<<5)) pControllerState->ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad);
                    // Minus (System/Menu)
                    if (lb & (1<<6)) pControllerState->ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_System);
                    
                    pControllerState->rAxis[0].x = tracked ? handsSnapshot.leftStickX : 0.0f;
                    pControllerState->rAxis[0].y = tracked ? -handsSnapshot.leftStickY : 0.0f;
                } else if (unControllerDeviceIndex == 2) {
                    const bool tracked = handsSnapshot.rightTracked != 0;
                    const float trigger = tracked ? handsSnapshot.rightTrigger / 255.0f : 0.0f;
                    const float grip = tracked ? handsSnapshot.rightGrip / 255.0f : 0.0f;
                    // Right JoyCon Buttons
                    pControllerState->rAxis[1].x = std::max(trigger, (rb & (1<<2)) ? 1.0f : 0.0f);
                    if (pControllerState->rAxis[1].x >= 0.5f) {
                        pControllerState->ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger);
                    }
                    pControllerState->rAxis[2].x = std::max(grip, ((rb & (1<<5)) || (rb & (1<<6))) ? 1.0f : 0.0f);
                    if (pControllerState->rAxis[2].x >= 0.5f) {
                        pControllerState->ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_Grip);
                    }
                    // A
                    if (rb & (1<<0)) pControllerState->ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_A);
                    // B
                    if (rb & (1<<1)) pControllerState->ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu);
                    // R3
                    if (rb & (1<<3)) pControllerState->ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad);
                    // Plus
                    if (rb & (1<<4)) pControllerState->ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_System);
                    
                    pControllerState->rAxis[0].x = tracked ? handsSnapshot.rightStickX : 0.0f;
                    pControllerState->rAxis[0].y = tracked ? -handsSnapshot.rightStickY : 0.0f;
                }
            }
        }
        return true;
    }
    virtual bool GetControllerStateWithPose(ETrackingUniverseOrigin eOrigin, vr::TrackedDeviceIndex_t unControllerDeviceIndex, vr::VRControllerState_t *pControllerState, uint32_t unControllerStateSize, TrackedDevicePose_t *pTrackedDevicePose) override {
        GetControllerState(unControllerDeviceIndex, pControllerState, unControllerStateSize);
        if(pTrackedDevicePose) {
            memset(pTrackedDevicePose, 0, sizeof(vr::TrackedDevicePose_t));
            pTrackedDevicePose->bPoseIsValid = true;
            pTrackedDevicePose->bDeviceIsConnected = true;
            pTrackedDevicePose->eTrackingResult = vr::TrackingResult_Running_OK;
            SharedHands handsSnapshot = {};
            const bool haveHands = ReadSharedHandsSnapshot(&handsSnapshot);
            if (unControllerDeviceIndex == 1 && haveHands && handsSnapshot.leftTracked) {
                ApplySmoothPose(pTrackedDevicePose, handsSnapshot.leftTransform, g_lastLeftTransform, g_lastLeftVel, g_lastLeftAngularVel, 0.0f, 0.011f);
            } else if (unControllerDeviceIndex == 2 && haveHands && handsSnapshot.rightTracked) {
                ApplySmoothPose(pTrackedDevicePose, handsSnapshot.rightTransform, g_lastRightTransform, g_lastRightVel, g_lastRightAngularVel, 0.0f, 0.011f);
            } else {
                pTrackedDevicePose->bPoseIsValid = false;
                pTrackedDevicePose->eTrackingResult = vr::TrackingResult_Running_OutOfRange;
                pTrackedDevicePose->mDeviceToAbsoluteTracking.m[0][0] = 1; pTrackedDevicePose->mDeviceToAbsoluteTracking.m[1][1] = 1; pTrackedDevicePose->mDeviceToAbsoluteTracking.m[2][2] = 1;
            }
        }
        return true;
    }
    virtual void TriggerHapticPulse(vr::TrackedDeviceIndex_t unControllerDeviceIndex, uint32_t unAxisId, unsigned short usDurationMicroSec) override {
        LogMsg("Called: IVRSystem::TriggerHapticPulse\n");
    }
    virtual const char * GetButtonIdNameFromEnum(EVRButtonId eButtonId) override {
        LogMsg("Called: IVRSystem::GetButtonIdNameFromEnum\n");
        return "1.10.30";
    }
    virtual const char * GetControllerAxisTypeNameFromEnum(EVRControllerAxisType eAxisType) override {
        LogMsg("Called: IVRSystem::GetControllerAxisTypeNameFromEnum\n");
        return "1.10.30";
    }
    virtual bool IsInputAvailable() override {
        LogMsg("Called: IVRSystem::IsInputAvailable\n");
        return true;
    }
    virtual bool IsSteamVRDrawingControllers() override {
        LogMsg("Called: IVRSystem::IsSteamVRDrawingControllers\n");
        return false;
    }
    virtual bool ShouldApplicationPause() override {
        LogMsg("Called: IVRSystem::ShouldApplicationPause\n");
        return false;
    }
    virtual bool ShouldApplicationReduceRenderingWork() override {
        LogMsg("Called: IVRSystem::ShouldApplicationReduceRenderingWork\n");
        return false;
    }
    virtual vr::EVRFirmwareError PerformFirmwareUpdate(vr::TrackedDeviceIndex_t unDeviceIndex) override {
        LogMsg("Called: IVRSystem::PerformFirmwareUpdate\n");
        return (vr::EVRFirmwareError)1;
    }
    virtual void AcknowledgeQuit_Exiting() override {
        LogMsg("Called: IVRSystem::AcknowledgeQuit_Exiting\n");
    }
    virtual uint32_t GetAppContainerFilePaths(VR_OUT_STRING() char *pchBuffer, uint32_t unBufferSize) override {
        LogMsg("Called: IVRSystem::GetAppContainerFilePaths\n");
        return (uint32_t)0;
    }
    virtual const char * GetRuntimeVersion() override {
        LogMsg("Called: IVRSystem::GetRuntimeVersion\n");
        return "1.10.30";
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* DummyPadding42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* DummyPadding52() { return nullptr; }
    virtual void* DummyPadding53() { return nullptr; }
    virtual void* DummyPadding54() { return nullptr; }
    virtual void* DummyPadding55() { return nullptr; }
    virtual void* DummyPadding56() { return nullptr; }
    virtual void* DummyPadding57() { return nullptr; }
    virtual void* DummyPadding58() { return nullptr; }
    virtual void* DummyPadding59() { return nullptr; }
    virtual void* DummyPadding60() { return nullptr; }
    virtual void* DummyPadding61() { return nullptr; }
    virtual void* DummyPadding62() { return nullptr; }
    virtual void* DummyPadding63() { return nullptr; }
    virtual void* DummyPadding64() { return nullptr; }
    virtual void* DummyPadding65() { return nullptr; }
    virtual void* DummyPadding66() { return nullptr; }
    virtual void* DummyPadding67() { return nullptr; }
    virtual void* DummyPadding68() { return nullptr; }
    virtual void* DummyPadding69() { return nullptr; }
    virtual void* DummyPadding70() { return nullptr; }
    virtual void* DummyPadding71() { return nullptr; }
    virtual void* DummyPadding72() { return nullptr; }
    virtual void* DummyPadding73() { return nullptr; }
    virtual void* DummyPadding74() { return nullptr; }
    virtual void* DummyPadding75() { return nullptr; }
    virtual void* DummyPadding76() { return nullptr; }
    virtual void* DummyPadding77() { return nullptr; }
    virtual void* DummyPadding78() { return nullptr; }
    virtual void* DummyPadding79() { return nullptr; }
    virtual void* DummyPadding80() { return nullptr; }
    virtual void* DummyPadding81() { return nullptr; }
    virtual void* DummyPadding82() { return nullptr; }
    virtual void* DummyPadding83() { return nullptr; }
    virtual void* DummyPadding84() { return nullptr; }
    virtual void* DummyPadding85() { return nullptr; }
    virtual void* DummyPadding86() { return nullptr; }
    virtual void* DummyPadding87() { return nullptr; }
    virtual void* DummyPadding88() { return nullptr; }
    virtual void* DummyPadding89() { return nullptr; }
    virtual void* DummyPadding90() { return nullptr; }
    virtual void* DummyPadding91() { return nullptr; }
    virtual void* DummyPadding92() { return nullptr; }
    virtual void* Dummy93() { return nullptr; }
    virtual void* Dummy94() { return nullptr; }
    virtual void* Dummy95() { return nullptr; }
    virtual void* Dummy96() { return nullptr; }
    virtual void* Dummy97() { return nullptr; }
    virtual void* Dummy98() { return nullptr; }
    virtual void* Dummy99() { return nullptr; }
} g_mockSystem;

class Mock_IVRApplications : public vr::IVRApplications {
public:
    virtual EVRApplicationError AddApplicationManifest(const char *pchApplicationManifestFullPath, bool bTemporary = false) override {
        LogMsg("Called: IVRApplications::AddApplicationManifest\n");
        return (EVRApplicationError)1;
    }
    virtual EVRApplicationError RemoveApplicationManifest(const char *pchApplicationManifestFullPath) override {
        LogMsg("Called: IVRApplications::RemoveApplicationManifest\n");
        return (EVRApplicationError)1;
    }
    virtual bool IsApplicationInstalled(const char *pchAppKey) override {
        LogMsg("Called: IVRApplications::IsApplicationInstalled\n");
        return false;
    }
    virtual uint32_t GetApplicationCount() override {
        LogMsg("Called: IVRApplications::GetApplicationCount\n");
        return (uint32_t)0;
    }
    virtual EVRApplicationError GetApplicationKeyByIndex(uint32_t unApplicationIndex, VR_OUT_STRING() char *pchAppKeyBuffer, uint32_t unAppKeyBufferLen) override {
        LogMsg("Called: IVRApplications::GetApplicationKeyByIndex\n");
        if (pchAppKeyBuffer && unAppKeyBufferLen > 0) pchAppKeyBuffer[0] = '\0';
        return (EVRApplicationError)1;
    }
    virtual EVRApplicationError GetApplicationKeyByProcessId(uint32_t unProcessId, VR_OUT_STRING() char *pchAppKeyBuffer, uint32_t unAppKeyBufferLen) override {
        LogMsg("Called: IVRApplications::GetApplicationKeyByProcessId\n");
        if (pchAppKeyBuffer && unAppKeyBufferLen > 0) pchAppKeyBuffer[0] = '\0';
        return (EVRApplicationError)1;
    }
    virtual EVRApplicationError LaunchApplication(const char *pchAppKey) override {
        LogMsg("Called: IVRApplications::LaunchApplication\n");
        return (EVRApplicationError)1;
    }
    virtual EVRApplicationError LaunchTemplateApplication(const char *pchTemplateAppKey, const char *pchNewAppKey, VR_ARRAY_COUNT( unKeys ) const AppOverrideKeys_t *pKeys, uint32_t unKeys) override {
        LogMsg("Called: IVRApplications::LaunchTemplateApplication\n");
        return (EVRApplicationError)1;
    }
    virtual vr::EVRApplicationError LaunchApplicationFromMimeType(const char *pchMimeType, const char *pchArgs) override {
        LogMsg("Called: IVRApplications::LaunchApplicationFromMimeType\n");
        return (vr::EVRApplicationError)1;
    }
    virtual EVRApplicationError LaunchDashboardOverlay(const char *pchAppKey) override {
        LogMsg("Called: IVRApplications::LaunchDashboardOverlay\n");
        return (EVRApplicationError)1;
    }
    virtual bool CancelApplicationLaunch(const char *pchAppKey) override {
        LogMsg("Called: IVRApplications::CancelApplicationLaunch\n");
        return false;
    }
    virtual EVRApplicationError IdentifyApplication(uint32_t unProcessId, const char *pchAppKey) override {
        LogMsg("Called: IVRApplications::IdentifyApplication\n");
        return (EVRApplicationError)1;
    }
    virtual uint32_t GetApplicationProcessId(const char *pchAppKey) override {
        LogMsg("Called: IVRApplications::GetApplicationProcessId\n");
        return (uint32_t)0;
    }
    virtual const char * GetApplicationsErrorNameFromEnum(EVRApplicationError error) override {
        LogMsg("Called: IVRApplications::GetApplicationsErrorNameFromEnum\n");
        return "1.10.30";
    }
    virtual uint32_t GetApplicationPropertyString(const char *pchAppKey, EVRApplicationProperty eProperty, VR_OUT_STRING() char *pchPropertyValueBuffer, uint32_t unPropertyValueBufferLen, EVRApplicationError *peError = nullptr) override {
        LogMsg("Called: IVRApplications::GetApplicationPropertyString\n");
        if (pchPropertyValueBuffer && unPropertyValueBufferLen > 0) pchPropertyValueBuffer[0] = '\0';
        return (uint32_t)0;
    }
    virtual bool GetApplicationPropertyBool(const char *pchAppKey, EVRApplicationProperty eProperty, EVRApplicationError *peError = nullptr) override {
        LogMsg("Called: IVRApplications::GetApplicationPropertyBool\n");
        return false;
    }
    virtual uint64_t GetApplicationPropertyUint64(const char *pchAppKey, EVRApplicationProperty eProperty, EVRApplicationError *peError = nullptr) override {
        LogMsg("Called: IVRApplications::GetApplicationPropertyUint64\n");
        return (uint64_t)0;
    }
    virtual EVRApplicationError SetApplicationAutoLaunch(const char *pchAppKey, bool bAutoLaunch) override {
        LogMsg("Called: IVRApplications::SetApplicationAutoLaunch\n");
        return (EVRApplicationError)1;
    }
    virtual bool GetApplicationAutoLaunch(const char *pchAppKey) override {
        LogMsg("Called: IVRApplications::GetApplicationAutoLaunch\n");
        return false;
    }
    virtual EVRApplicationError SetDefaultApplicationForMimeType(const char *pchAppKey, const char *pchMimeType) override {
        LogMsg("Called: IVRApplications::SetDefaultApplicationForMimeType\n");
        return (EVRApplicationError)1;
    }
    virtual bool GetDefaultApplicationForMimeType(const char *pchMimeType, VR_OUT_STRING() char *pchAppKeyBuffer, uint32_t unAppKeyBufferLen) override {
        LogMsg("Called: IVRApplications::GetDefaultApplicationForMimeType\n");
        return false;
    }
    virtual bool GetApplicationSupportedMimeTypes(const char *pchAppKey, VR_OUT_STRING() char *pchMimeTypesBuffer, uint32_t unMimeTypesBuffer) override {
        LogMsg("Called: IVRApplications::GetApplicationSupportedMimeTypes\n");
        return false;
    }
    virtual uint32_t GetApplicationsThatSupportMimeType(const char *pchMimeType, VR_OUT_STRING() char *pchAppKeysThatSupportBuffer, uint32_t unAppKeysThatSupportBuffer) override {
        LogMsg("Called: IVRApplications::GetApplicationsThatSupportMimeType\n");
        return (uint32_t)0;
    }
    virtual uint32_t GetApplicationLaunchArguments(uint32_t unHandle, VR_OUT_STRING() char *pchArgs, uint32_t unArgs) override {
        LogMsg("Called: IVRApplications::GetApplicationLaunchArguments\n");
        return (uint32_t)0;
    }
    virtual EVRApplicationError GetStartingApplication(VR_OUT_STRING() char *pchAppKeyBuffer, uint32_t unAppKeyBufferLen) override {
        LogMsg("Called: IVRApplications::GetStartingApplication\n");
        return (EVRApplicationError)1;
    }
    virtual EVRSceneApplicationState GetSceneApplicationState() override {
        LogMsg("Called: IVRApplications::GetSceneApplicationState\n");
        return (EVRSceneApplicationState)0;
    }
    virtual EVRApplicationError PerformApplicationPrelaunchCheck(const char *pchAppKey) override {
        LogMsg("Called: IVRApplications::PerformApplicationPrelaunchCheck\n");
        return (EVRApplicationError)1;
    }
    virtual const char * GetSceneApplicationStateNameFromEnum(EVRSceneApplicationState state) override {
        LogMsg("Called: IVRApplications::GetSceneApplicationStateNameFromEnum\n");
        return "1.10.30";
    }
    virtual EVRApplicationError LaunchInternalProcess(const char *pchBinaryPath, const char *pchArguments, const char *pchWorkingDirectory) override {
        LogMsg("Called: IVRApplications::LaunchInternalProcess\n");
        return (EVRApplicationError)1;
    }
    virtual uint32_t GetCurrentSceneProcessId() override {
        LogMsg("Called: IVRApplications::GetCurrentSceneProcessId\n");
        return (uint32_t)0;
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* DummyPadding42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* DummyPadding52() { return nullptr; }
    virtual void* DummyPadding53() { return nullptr; }
    virtual void* DummyPadding54() { return nullptr; }
    virtual void* DummyPadding55() { return nullptr; }
    virtual void* DummyPadding56() { return nullptr; }
    virtual void* DummyPadding57() { return nullptr; }
    virtual void* DummyPadding58() { return nullptr; }
    virtual void* DummyPadding59() { return nullptr; }
    virtual void* DummyPadding60() { return nullptr; }
    virtual void* Dummy61() { return nullptr; }
    virtual void* Dummy62() { return nullptr; }
    virtual void* Dummy63() { return nullptr; }
    virtual void* Dummy64() { return nullptr; }
    virtual void* Dummy65() { return nullptr; }
    virtual void* Dummy66() { return nullptr; }
    virtual void* Dummy67() { return nullptr; }
    virtual void* Dummy68() { return nullptr; }
    virtual void* Dummy69() { return nullptr; }
    virtual void* Dummy70() { return nullptr; }
    virtual void* Dummy71() { return nullptr; }
    virtual void* Dummy72() { return nullptr; }
    virtual void* Dummy73() { return nullptr; }
    virtual void* Dummy74() { return nullptr; }
    virtual void* Dummy75() { return nullptr; }
    virtual void* Dummy76() { return nullptr; }
    virtual void* Dummy77() { return nullptr; }
    virtual void* Dummy78() { return nullptr; }
    virtual void* Dummy79() { return nullptr; }
    virtual void* Dummy80() { return nullptr; }
    virtual void* Dummy81() { return nullptr; }
    virtual void* Dummy82() { return nullptr; }
    virtual void* Dummy83() { return nullptr; }
    virtual void* Dummy84() { return nullptr; }
    virtual void* Dummy85() { return nullptr; }
    virtual void* Dummy86() { return nullptr; }
    virtual void* Dummy87() { return nullptr; }
    virtual void* Dummy88() { return nullptr; }
    virtual void* Dummy89() { return nullptr; }
    virtual void* Dummy90() { return nullptr; }
    virtual void* Dummy91() { return nullptr; }
    virtual void* Dummy92() { return nullptr; }
    virtual void* Dummy93() { return nullptr; }
    virtual void* Dummy94() { return nullptr; }
    virtual void* Dummy95() { return nullptr; }
    virtual void* Dummy96() { return nullptr; }
    virtual void* Dummy97() { return nullptr; }
    virtual void* Dummy98() { return nullptr; }
    virtual void* Dummy99() { return nullptr; }
} g_mockApplications;

class Mock_IVRSettings : public vr::IVRSettings {
public:
    virtual const char * GetSettingsErrorNameFromEnum(EVRSettingsError eError) override {
        LogMsg("Called: IVRSettings::GetSettingsErrorNameFromEnum\n");
        return "1.10.30";
    }
    virtual void SetBool(const char *pchSection, const char *pchSettingsKey, bool bValue, EVRSettingsError *peError = nullptr) override {
        LogMsg("Called: IVRSettings::SetBool\n");
    }
    virtual void SetInt32(const char *pchSection, const char *pchSettingsKey, int32_t nValue, EVRSettingsError *peError = nullptr) override {
        LogMsg("Called: IVRSettings::SetInt32\n");
    }
    virtual void SetFloat(const char *pchSection, const char *pchSettingsKey, float flValue, EVRSettingsError *peError = nullptr) override {
        LogMsg("Called: IVRSettings::SetFloat\n");
    }
    virtual void SetString(const char *pchSection, const char *pchSettingsKey, const char *pchValue, EVRSettingsError *peError = nullptr) override {
        LogMsg("Called: IVRSettings::SetString\n");
    }
    virtual bool GetBool(const char *pchSection, const char *pchSettingsKey, EVRSettingsError *peError = nullptr) override {
        LogMsg("Called: IVRSettings::GetBool\n");
        return false;
    }
    virtual int32_t GetInt32(const char *pchSection, const char *pchSettingsKey, EVRSettingsError *peError = nullptr) override {
        LogMsg("Called: IVRSettings::GetInt32\n");
        return (int32_t)0;
    }
    virtual float GetFloat(const char *pchSection, const char *pchSettingsKey, EVRSettingsError *peError = nullptr) override {
        LogMsg("Called: IVRSettings::GetFloat\n");
        return (float)0;
    }
    virtual void GetString(const char *pchSection, const char *pchSettingsKey, VR_OUT_STRING() char *pchValue, uint32_t unValueLen, EVRSettingsError *peError = nullptr) override {
        LogMsg("Called: IVRSettings::GetString\n");
        if(peError) *peError = vr::VRSettingsError_None;
        if(pchValue && unValueLen > 0) pchValue[0] = '\0';
    }
    virtual void RemoveSection(const char *pchSection, EVRSettingsError *peError = nullptr) override {
        LogMsg("Called: IVRSettings::RemoveSection\n");
    }
    virtual void RemoveKeyInSection(const char *pchSection, const char *pchSettingsKey, EVRSettingsError *peError = nullptr) override {
        LogMsg("Called: IVRSettings::RemoveKeyInSection\n");
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* DummyPadding42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* DummyPadding52() { return nullptr; }
    virtual void* DummyPadding53() { return nullptr; }
    virtual void* Dummy54() { return nullptr; }
    virtual void* Dummy55() { return nullptr; }
    virtual void* Dummy56() { return nullptr; }
    virtual void* Dummy57() { return nullptr; }
    virtual void* Dummy58() { return nullptr; }
    virtual void* Dummy59() { return nullptr; }
    virtual void* Dummy60() { return nullptr; }
    virtual void* Dummy61() { return nullptr; }
    virtual void* Dummy62() { return nullptr; }
    virtual void* Dummy63() { return nullptr; }
    virtual void* Dummy64() { return nullptr; }
    virtual void* Dummy65() { return nullptr; }
    virtual void* Dummy66() { return nullptr; }
    virtual void* Dummy67() { return nullptr; }
    virtual void* Dummy68() { return nullptr; }
    virtual void* Dummy69() { return nullptr; }
    virtual void* Dummy70() { return nullptr; }
    virtual void* Dummy71() { return nullptr; }
    virtual void* Dummy72() { return nullptr; }
    virtual void* Dummy73() { return nullptr; }
    virtual void* Dummy74() { return nullptr; }
    virtual void* Dummy75() { return nullptr; }
    virtual void* Dummy76() { return nullptr; }
    virtual void* Dummy77() { return nullptr; }
    virtual void* Dummy78() { return nullptr; }
    virtual void* Dummy79() { return nullptr; }
    virtual void* Dummy80() { return nullptr; }
    virtual void* Dummy81() { return nullptr; }
    virtual void* Dummy82() { return nullptr; }
    virtual void* Dummy83() { return nullptr; }
    virtual void* Dummy84() { return nullptr; }
    virtual void* Dummy85() { return nullptr; }
    virtual void* Dummy86() { return nullptr; }
    virtual void* Dummy87() { return nullptr; }
    virtual void* Dummy88() { return nullptr; }
    virtual void* Dummy89() { return nullptr; }
    virtual void* Dummy90() { return nullptr; }
    virtual void* Dummy91() { return nullptr; }
    virtual void* Dummy92() { return nullptr; }
    virtual void* Dummy93() { return nullptr; }
    virtual void* Dummy94() { return nullptr; }
    virtual void* Dummy95() { return nullptr; }
    virtual void* Dummy96() { return nullptr; }
    virtual void* Dummy97() { return nullptr; }
    virtual void* Dummy98() { return nullptr; }
    virtual void* Dummy99() { return nullptr; }
} g_mockSettings;

class Mock_IVRChaperone : public vr::IVRChaperone {
public:
    virtual vr::ChaperoneCalibrationState GetCalibrationState() override {
        return vr::ChaperoneCalibrationState_OK;
    }
    virtual bool GetPlayAreaSize(float *pSizeX, float *pSizeZ) override {
        if(pSizeX) *pSizeX = 1000.0f;
        if(pSizeZ) *pSizeZ = 1000.0f;
        return true;
    }
    virtual bool GetPlayAreaRect(vr::HmdQuad_t *rect) override {
        if(rect) {
            rect->vCorners[0].v[0] = -500.0f; rect->vCorners[0].v[1] = 0.0f; rect->vCorners[0].v[2] = -500.0f;
            rect->vCorners[1].v[0] = -500.0f; rect->vCorners[1].v[1] = 0.0f; rect->vCorners[1].v[2] =  500.0f;
            rect->vCorners[2].v[0] =  500.0f; rect->vCorners[2].v[1] = 0.0f; rect->vCorners[2].v[2] =  500.0f;
            rect->vCorners[3].v[0] =  500.0f; rect->vCorners[3].v[1] = 0.0f; rect->vCorners[3].v[2] = -500.0f;
        }
        return true;
    }
    virtual void ReloadInfo(void) override {
        LogMsg("Called: IVRChaperone::ReloadInfo\n");
    }
    virtual void SetSceneColor(HmdColor_t color) override {
        LogMsg("Called: IVRChaperone::SetSceneColor\n");
    }
    virtual void GetBoundsColor(vr::HmdColor_t *pOutputColorArray, int nNumOutputColors, float flCollisionBoundsFadeDistance, vr::HmdColor_t *pOutputCameraColor) override {
        if(pOutputColorArray) {
            for(int i = 0; i < nNumOutputColors; ++i) {
                pOutputColorArray[i].r = 0.0f;
                pOutputColorArray[i].g = 0.0f;
                pOutputColorArray[i].b = 0.0f;
                pOutputColorArray[i].a = 0.0f;
            }
        }
        if(pOutputCameraColor) {
            pOutputCameraColor->r = 0.0f;
            pOutputCameraColor->g = 0.0f;
            pOutputCameraColor->b = 0.0f;
            pOutputCameraColor->a = 0.0f;
        }
    }
    virtual bool AreBoundsVisible() override {
        LogMsg("Called: IVRChaperone::AreBoundsVisible\n");
        return false;
    }
    virtual void ForceBoundsVisible(bool bForce) override {
        LogMsg("Called: IVRChaperone::ForceBoundsVisible\n");
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* Dummy42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* Dummy52() { return nullptr; }
    virtual void* Dummy53() { return nullptr; }
    virtual void* Dummy54() { return nullptr; }
    virtual void* Dummy55() { return nullptr; }
    virtual void* Dummy56() { return nullptr; }
    virtual void* Dummy57() { return nullptr; }
    virtual void* Dummy58() { return nullptr; }
    virtual void* Dummy59() { return nullptr; }
    virtual void* Dummy60() { return nullptr; }
    virtual void* Dummy61() { return nullptr; }
    virtual void* Dummy62() { return nullptr; }
    virtual void* Dummy63() { return nullptr; }
    virtual void* Dummy64() { return nullptr; }
    virtual void* Dummy65() { return nullptr; }
    virtual void* Dummy66() { return nullptr; }
    virtual void* Dummy67() { return nullptr; }
    virtual void* Dummy68() { return nullptr; }
    virtual void* Dummy69() { return nullptr; }
    virtual void* Dummy70() { return nullptr; }
    virtual void* Dummy71() { return nullptr; }
    virtual void* Dummy72() { return nullptr; }
    virtual void* Dummy73() { return nullptr; }
    virtual void* Dummy74() { return nullptr; }
    virtual void* Dummy75() { return nullptr; }
    virtual void* Dummy76() { return nullptr; }
    virtual void* Dummy77() { return nullptr; }
    virtual void* Dummy78() { return nullptr; }
    virtual void* Dummy79() { return nullptr; }
    virtual void* Dummy80() { return nullptr; }
    virtual void* Dummy81() { return nullptr; }
    virtual void* Dummy82() { return nullptr; }
    virtual void* Dummy83() { return nullptr; }
    virtual void* Dummy84() { return nullptr; }
    virtual void* Dummy85() { return nullptr; }
    virtual void* Dummy86() { return nullptr; }
    virtual void* Dummy87() { return nullptr; }
    virtual void* Dummy88() { return nullptr; }
    virtual void* Dummy89() { return nullptr; }
    virtual void* Dummy90() { return nullptr; }
    virtual void* Dummy91() { return nullptr; }
    virtual void* Dummy92() { return nullptr; }
    virtual void* Dummy93() { return nullptr; }
    virtual void* Dummy94() { return nullptr; }
    virtual void* Dummy95() { return nullptr; }
    virtual void* Dummy96() { return nullptr; }
    virtual void* Dummy97() { return nullptr; }
    virtual void* Dummy98() { return nullptr; }
    virtual void* Dummy99() { return nullptr; }
} g_mockChaperone;

class Mock_IVRChaperoneSetup : public vr::IVRChaperoneSetup {
public:
    virtual bool CommitWorkingCopy(EChaperoneConfigFile configFile) override {
        LogMsg("Called: IVRChaperoneSetup::CommitWorkingCopy\n");
        return false;
    }
    virtual void RevertWorkingCopy() override {
        LogMsg("Called: IVRChaperoneSetup::RevertWorkingCopy\n");
    }
    virtual bool GetWorkingPlayAreaSize(float *pSizeX, float *pSizeZ) override {
        LogMsg("Called: IVRChaperoneSetup::GetWorkingPlayAreaSize\n");
        return false;
    }
    virtual bool GetWorkingPlayAreaRect(HmdQuad_t *rect) override {
        LogMsg("Called: IVRChaperoneSetup::GetWorkingPlayAreaRect\n");
        return false;
    }
    virtual bool GetWorkingCollisionBoundsInfo(VR_OUT_ARRAY_COUNT(punQuadsCount) HmdQuad_t *pQuadsBuffer, uint32_t* punQuadsCount) override {
        LogMsg("Called: IVRChaperoneSetup::GetWorkingCollisionBoundsInfo\n");
        return false;
    }
    virtual bool GetLiveCollisionBoundsInfo(VR_OUT_ARRAY_COUNT(punQuadsCount) HmdQuad_t *pQuadsBuffer, uint32_t* punQuadsCount) override {
        LogMsg("Called: IVRChaperoneSetup::GetLiveCollisionBoundsInfo\n");
        return false;
    }
    virtual bool GetWorkingSeatedZeroPoseToRawTrackingPose(HmdMatrix34_t *pmatSeatedZeroPoseToRawTrackingPose) override {
        LogMsg("Called: IVRChaperoneSetup::GetWorkingSeatedZeroPoseToRawTrackingPose\n");
        return false;
    }
    virtual bool GetWorkingStandingZeroPoseToRawTrackingPose(HmdMatrix34_t *pmatStandingZeroPoseToRawTrackingPose) override {
        LogMsg("Called: IVRChaperoneSetup::GetWorkingStandingZeroPoseToRawTrackingPose\n");
        return false;
    }
    virtual void SetWorkingPlayAreaSize(float sizeX, float sizeZ) override {
        LogMsg("Called: IVRChaperoneSetup::SetWorkingPlayAreaSize\n");
    }
    virtual void SetWorkingCollisionBoundsInfo(VR_ARRAY_COUNT(unQuadsCount) HmdQuad_t *pQuadsBuffer, uint32_t unQuadsCount) override {
        LogMsg("Called: IVRChaperoneSetup::SetWorkingCollisionBoundsInfo\n");
    }
    virtual void SetWorkingPerimeter(VR_ARRAY_COUNT( unPointCount ) HmdVector2_t *pPointBuffer, uint32_t unPointCount) override {
        LogMsg("Called: IVRChaperoneSetup::SetWorkingPerimeter\n");
    }
    virtual void SetWorkingSeatedZeroPoseToRawTrackingPose(const HmdMatrix34_t *pMatSeatedZeroPoseToRawTrackingPose) override {
        LogMsg("Called: IVRChaperoneSetup::SetWorkingSeatedZeroPoseToRawTrackingPose\n");
    }
    virtual void SetWorkingStandingZeroPoseToRawTrackingPose(const HmdMatrix34_t *pMatStandingZeroPoseToRawTrackingPose) override {
        LogMsg("Called: IVRChaperoneSetup::SetWorkingStandingZeroPoseToRawTrackingPose\n");
    }
    virtual void ReloadFromDisk(EChaperoneConfigFile configFile) override {
        LogMsg("Called: IVRChaperoneSetup::ReloadFromDisk\n");
    }
    virtual bool GetLiveSeatedZeroPoseToRawTrackingPose(HmdMatrix34_t *pmatSeatedZeroPoseToRawTrackingPose) override {
        LogMsg("Called: IVRChaperoneSetup::GetLiveSeatedZeroPoseToRawTrackingPose\n");
        return false;
    }
    virtual bool ExportLiveToBuffer(VR_OUT_STRING() char *pBuffer, uint32_t *pnBufferLength) override {
        LogMsg("Called: IVRChaperoneSetup::ExportLiveToBuffer\n");
        return false;
    }
    virtual bool ImportFromBufferToWorking(const char *pBuffer, uint32_t nImportFlags) override {
        LogMsg("Called: IVRChaperoneSetup::ImportFromBufferToWorking\n");
        return false;
    }
    virtual void ShowWorkingSetPreview() override {
        LogMsg("Called: IVRChaperoneSetup::ShowWorkingSetPreview\n");
    }
    virtual void HideWorkingSetPreview() override {
        LogMsg("Called: IVRChaperoneSetup::HideWorkingSetPreview\n");
    }
    virtual void RoomSetupStarting() override {
        LogMsg("Called: IVRChaperoneSetup::RoomSetupStarting\n");
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* DummyPadding42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* DummyPadding52() { return nullptr; }
    virtual void* DummyPadding53() { return nullptr; }
    virtual void* DummyPadding54() { return nullptr; }
    virtual void* DummyPadding55() { return nullptr; }
    virtual void* DummyPadding56() { return nullptr; }
    virtual void* Dummy57() { return nullptr; }
    virtual void* Dummy58() { return nullptr; }
    virtual void* Dummy59() { return nullptr; }
    virtual void* Dummy60() { return nullptr; }
    virtual void* Dummy61() { return nullptr; }
    virtual void* Dummy62() { return nullptr; }
    virtual void* Dummy63() { return nullptr; }
    virtual void* Dummy64() { return nullptr; }
    virtual void* Dummy65() { return nullptr; }
    virtual void* Dummy66() { return nullptr; }
    virtual void* Dummy67() { return nullptr; }
    virtual void* Dummy68() { return nullptr; }
    virtual void* Dummy69() { return nullptr; }
    virtual void* Dummy70() { return nullptr; }
    virtual void* Dummy71() { return nullptr; }
    virtual void* Dummy72() { return nullptr; }
    virtual void* Dummy73() { return nullptr; }
    virtual void* Dummy74() { return nullptr; }
    virtual void* Dummy75() { return nullptr; }
    virtual void* Dummy76() { return nullptr; }
    virtual void* Dummy77() { return nullptr; }
    virtual void* Dummy78() { return nullptr; }
    virtual void* Dummy79() { return nullptr; }
    virtual void* Dummy80() { return nullptr; }
    virtual void* Dummy81() { return nullptr; }
    virtual void* Dummy82() { return nullptr; }
    virtual void* Dummy83() { return nullptr; }
    virtual void* Dummy84() { return nullptr; }
    virtual void* Dummy85() { return nullptr; }
    virtual void* Dummy86() { return nullptr; }
    virtual void* Dummy87() { return nullptr; }
    virtual void* Dummy88() { return nullptr; }
    virtual void* Dummy89() { return nullptr; }
    virtual void* Dummy90() { return nullptr; }
    virtual void* Dummy91() { return nullptr; }
    virtual void* Dummy92() { return nullptr; }
    virtual void* Dummy93() { return nullptr; }
    virtual void* Dummy94() { return nullptr; }
    virtual void* Dummy95() { return nullptr; }
    virtual void* Dummy96() { return nullptr; }
    virtual void* Dummy97() { return nullptr; }
    virtual void* Dummy98() { return nullptr; }
    virtual void* Dummy99() { return nullptr; }
} g_mockChaperoneSetup;

class Mock_IVRCompositor : public vr::IVRCompositor {
public:
    virtual void SetTrackingSpace(ETrackingUniverseOrigin eOrigin) override {
        LogMsg("Called: IVRCompositor::SetTrackingSpace\n");
    }
    virtual ETrackingUniverseOrigin GetTrackingSpace() override {
        LogMsg("Called: IVRCompositor::GetTrackingSpace\n");
        return (ETrackingUniverseOrigin)0;
    }
    virtual EVRCompositorError WaitGetPoses(VR_ARRAY_COUNT( unRenderPoseArrayCount ) TrackedDevicePose_t* pRenderPoseArray, uint32_t unRenderPoseArrayCount, VR_ARRAY_COUNT( unGamePoseArrayCount ) TrackedDevicePose_t* pGamePoseArray, uint32_t unGamePoseArrayCount) override {
        SharedHands handsSnapshot = {};
        const bool haveHands = ReadSharedHandsSnapshot(&handsSnapshot);
        if(pRenderPoseArray && unRenderPoseArrayCount > 0) {
            memset(pRenderPoseArray, 0, sizeof(vr::TrackedDevicePose_t) * unRenderPoseArrayCount);
            for(uint32_t i=0; i<3 && i<unRenderPoseArrayCount; ++i) {
                pRenderPoseArray[i].bDeviceIsConnected = true;
                pRenderPoseArray[i].eTrackingResult = vr::TrackingResult_Running_OutOfRange;
                const bool tracked = haveHands && (i == 0 ? handsSnapshot.headTracked != 0 :
                    (i == 1 ? handsSnapshot.leftTracked != 0 : handsSnapshot.rightTracked != 0));
                if (!tracked) continue;
                pRenderPoseArray[i].bPoseIsValid = true;
                pRenderPoseArray[i].eTrackingResult = vr::TrackingResult_Running_OK;
                float* srcTransform = i == 0 ? handsSnapshot.headTransform :
                    (i == 1 ? handsSnapshot.leftTransform : handsSnapshot.rightTransform);
                if (i == 0) {
                    ApplyHeadExtrapolation(&pRenderPoseArray[i], srcTransform, handsSnapshot.visionTimestamp, 0.0f);
                } else {
                    for(int r=0;r<3;r++) for(int c=0;c<4;c++) pRenderPoseArray[i].mDeviceToAbsoluteTracking.m[r][c] = srcTransform[c*4 + r];
                }
            }
        }
        if(pGamePoseArray && unGamePoseArrayCount > 0) {
            memset(pGamePoseArray, 0, sizeof(vr::TrackedDevicePose_t) * unGamePoseArrayCount);
            for(uint32_t i=0; i<3 && i<unGamePoseArrayCount; ++i) {
                pGamePoseArray[i].bDeviceIsConnected = true;
                pGamePoseArray[i].eTrackingResult = vr::TrackingResult_Running_OutOfRange;
                const bool tracked = haveHands && (i == 0 ? handsSnapshot.headTracked != 0 :
                    (i == 1 ? handsSnapshot.leftTracked != 0 : handsSnapshot.rightTracked != 0));
                if (!tracked) continue;
                pGamePoseArray[i].bPoseIsValid = true;
                pGamePoseArray[i].eTrackingResult = vr::TrackingResult_Running_OK;
                float* srcTransform = i == 0 ? handsSnapshot.headTransform :
                    (i == 1 ? handsSnapshot.leftTransform : handsSnapshot.rightTransform);
                if (i == 0) {
                    ApplyHeadExtrapolation(&pGamePoseArray[i], srcTransform, handsSnapshot.visionTimestamp, 0.0f);
                } else {
                    for(int r=0;r<3;r++) for(int c=0;c<4;c++) pGamePoseArray[i].mDeviceToAbsoluteTracking.m[r][c] = srcTransform[c*4 + r];
                }
            }
        }
        
// Removed local velocity declaration
        static float lastLeft[3] = {0,0,0};
        static float lastRight[3] = {0,0,0};
        static DWORD lastTime = GetTickCount();
        DWORD now = GetTickCount();
        float dt = (now - lastTime) / 1000.0f;
        if (dt < 0.001f) dt = 0.001f;
        if (dt > 0.1f) dt = 0.1f;
        
        if (haveHands) {
            float curLeft[3] = { handsSnapshot.leftTransform[3], handsSnapshot.leftTransform[7], handsSnapshot.leftTransform[11] }; // Retained for compatibility with the old velocity path.
            float cL[3] = { handsSnapshot.leftTransform[12], handsSnapshot.leftTransform[13], handsSnapshot.leftTransform[14] };
            float cR[3] = { handsSnapshot.rightTransform[12], handsSnapshot.rightTransform[13], handsSnapshot.rightTransform[14] };
            
            // Set velocities to zero. Vision Pro sends data at 250Hz.
            // Allowing SteamVR to predict motion causes extreme jitter in Alyx.
            g_handVelocityL[0] = 0.0f; g_handVelocityL[1] = 0.0f; g_handVelocityL[2] = 0.0f;
            g_handVelocityR[0] = 0.0f; g_handVelocityR[1] = 0.0f; g_handVelocityR[2] = 0.0f;
            
            if (pRenderPoseArray && unRenderPoseArrayCount > 1) {
                pRenderPoseArray[1].vVelocity.v[0] = g_handVelocityL[0]; pRenderPoseArray[1].vVelocity.v[1] = g_handVelocityL[1]; pRenderPoseArray[1].vVelocity.v[2] = g_handVelocityL[2];
            }
            if (pRenderPoseArray && unRenderPoseArrayCount > 2) {
                pRenderPoseArray[2].vVelocity.v[0] = g_handVelocityR[0]; pRenderPoseArray[2].vVelocity.v[1] = g_handVelocityR[1]; pRenderPoseArray[2].vVelocity.v[2] = g_handVelocityR[2];
            }
            if (pGamePoseArray && unGamePoseArrayCount > 1) {
                pGamePoseArray[1].vVelocity.v[0] = g_handVelocityL[0]; pGamePoseArray[1].vVelocity.v[1] = g_handVelocityL[1]; pGamePoseArray[1].vVelocity.v[2] = g_handVelocityL[2];
            }
            if (pGamePoseArray && unGamePoseArrayCount > 2) {
                pGamePoseArray[2].vVelocity.v[0] = g_handVelocityR[0]; pGamePoseArray[2].vVelocity.v[1] = g_handVelocityR[1]; pGamePoseArray[2].vVelocity.v[2] = g_handVelocityR[2];
            }
            
            lastLeft[0] = cL[0]; lastLeft[1] = cL[1]; lastLeft[2] = cL[2];
            lastRight[0] = cR[0]; lastRight[1] = cR[1]; lastRight[2] = cR[2];
        }
        lastTime = now;
        
        static LARGE_INTEGER freq;
        static bool initFreq = false;
        if(!initFreq) { QueryPerformanceFrequency(&freq); initFreq = true; }
        static LARGE_INTEGER lastWait;
        
        LARGE_INTEGER nowWait;
        QueryPerformanceCounter(&nowWait);
        
        // Wait for exactly 11.1111ms (90Hz)
        double targetWaitMs = 1000.0 / 90.0;
        
        if (lastWait.QuadPart != 0) {
            double elapsedMs = (double)(nowWait.QuadPart - lastWait.QuadPart) * 1000.0 / (double)freq.QuadPart;
            while(elapsedMs < targetWaitMs) {
                QueryPerformanceCounter(&nowWait);
                elapsedMs = (double)(nowWait.QuadPart - lastWait.QuadPart) * 1000.0 / (double)freq.QuadPart;
            }
        }
        lastWait = nowWait;
        
        return vr::VRCompositorError_None;
    }
    virtual EVRCompositorError GetLastPoses(VR_ARRAY_COUNT( unRenderPoseArrayCount ) TrackedDevicePose_t* pRenderPoseArray, uint32_t unRenderPoseArrayCount, VR_ARRAY_COUNT( unGamePoseArrayCount ) TrackedDevicePose_t* pGamePoseArray, uint32_t unGamePoseArrayCount) override {
        LogMsg("Called: IVRCompositor::GetLastPoses\n");
        return (EVRCompositorError)1;
    }
    virtual EVRCompositorError GetLastPoseForTrackedDeviceIndex(TrackedDeviceIndex_t unDeviceIndex, TrackedDevicePose_t *pOutputPose, TrackedDevicePose_t *pOutputGamePose) override {
        LogMsg("Called: IVRCompositor::GetLastPoseForTrackedDeviceIndex\n");
        return (EVRCompositorError)1;
    }
    virtual EVRCompositorError Submit(vr::EVREye eEye, const vr::Texture_t *pTexture, const vr::VRTextureBounds_t* pBounds, vr::EVRSubmitFlags nSubmitFlags) override {
        FILE* f2 = fopen("C:\\VR_Depth_Steal_Log.txt", "a");
        if (f2 && pTexture && pTexture->handle && pTexture->eType == vr::TextureType_DirectX) {
            ID3D11Texture2D* pTex = (ID3D11Texture2D*)pTexture->handle;
            ID3D11Device* pDev = nullptr;
            pTex->GetDevice(&pDev);
            if (pDev) {
                ID3D11DeviceContext* pCtx = nullptr;
                pDev->GetImmediateContext(&pCtx);
                if (pCtx) {
                    ID3D11RenderTargetView* rtv = nullptr;
                    ID3D11DepthStencilView* dsv = nullptr;
                    pCtx->OMGetRenderTargets(1, &rtv, &dsv);
                    fprintf(f2, "OMGetRenderTargets: rtv=%p, dsv=%p\n", rtv, dsv);
                    if (dsv) dsv->Release();
                    if (rtv) rtv->Release();
                    pCtx->Release();
                }
                pDev->Release();
            }
            fclose(f2);
        }

        FILE* f = fopen("C:\\VR_Depth_Log.txt", "a");
        if (f) {
            fprintf(f, "Submit called! eEye=%d, nSubmitFlags=%d\n", eEye, nSubmitFlags);
            if (nSubmitFlags & vr::Submit_TextureWithDepth) {
                fprintf(f, "HAS DEPTH!\n");
            }
            fclose(f);
        }

        if(pTexture && pTexture->handle && pTexture->eType == vr::TextureType_DirectX) {
            ID3D11Texture2D* pGameTex = (ID3D11Texture2D*)pTexture->handle;
            D3D11_TEXTURE2D_DESC desc;
            pGameTex->GetDesc(&desc);
            
            if ((eEye == vr::Eye_Left || eEye == vr::Eye_Right) && hMapFile) {
                ID3D11Device* pDevice = nullptr;
                pGameTex->GetDevice(&pDevice);
                if (pDevice) {
                    ID3D11DeviceContext* pContext = nullptr;
                    pDevice->GetImmediateContext(&pContext);

                    if (pContext) {

                        ID3D11Multithread* pMt = nullptr;
                        pContext->QueryInterface(__uuidof(ID3D11Multithread), (void**)&pMt);
                        if (pMt) { pMt->SetMultithreadProtected(TRUE); pMt->Enter(); }

                        StagingFrame& currentStaging = g_stagingFrames[g_stagingWriteIndex];
                        
                        ID3D11Texture2D** ppColorStaging = (eEye == vr::Eye_Left) ? &currentStaging.pColorLeft : &currentStaging.pColorRight;
                        ID3D11Texture2D** ppDepthStaging = (eEye == vr::Eye_Left) ? &currentStaging.pDepthLeft : &currentStaging.pDepthRight;
                        
                        if (*ppColorStaging) {
                            D3D11_TEXTURE2D_DESC existingDesc;
                            (*ppColorStaging)->GetDesc(&existingDesc);
                            if (existingDesc.Width != desc.Width || existingDesc.Height != desc.Height || existingDesc.Format != desc.Format) {
                                (*ppColorStaging)->Release();
                                *ppColorStaging = nullptr;
                            }
                        }

                        if (!(*ppColorStaging)) {
                            D3D11_TEXTURE2D_DESC stDesc = desc;
                            stDesc.Usage = D3D11_USAGE_STAGING;
                            stDesc.BindFlags = 0;
                            stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                            stDesc.MiscFlags = 0;
                            pDevice->CreateTexture2D(&stDesc, nullptr, ppColorStaging);
                        }
                        
                        if (*ppColorStaging) {
                            pContext->CopyResource(*ppColorStaging, pGameTex);
                        }
                        
                        // Capture bounds and transform for the current frame
                        float uMin = pBounds ? pBounds->uMin : 0.0f;
                        float vMin = pBounds ? pBounds->vMin : 0.0f;
                        float uMax = pBounds ? pBounds->uMax : 1.0f;
                        float vMax = pBounds ? pBounds->vMax : 1.0f;
                        
                        if (vMin > vMax) { float tmp = vMin; vMin = vMax; vMax = tmp; }
                        if (uMin > uMax) { float tmp = uMin; uMin = uMax; uMax = tmp; }
                        
                        if (eEye == vr::Eye_Left) {
                            currentStaging.uMinL = uMin; currentStaging.vMinL = vMin;
                            currentStaging.uMaxL = uMax; currentStaging.vMaxL = vMax;
                            {
                                std::lock_guard<std::mutex> lock(g_poseQueueMutex);
                                if (!g_poseTimestampQueue.empty()) {
                                    g_lastPoppedVisionTimestamp = g_poseTimestampQueue.front();
                                    for(int i=0; i<16; i++) g_lastPoppedTransform[i] = g_poseTransformQueue.front()[i];
                                    g_poseTimestampQueue.pop();
                                    g_poseTransformQueue.pop();
                                }
                            }
                            currentStaging.visionTimestamp = g_lastPoppedVisionTimestamp;
                            for(int i=0; i<16; i++) currentStaging.renderHeadTransform[i] = g_lastPoppedTransform[i];
                            currentStaging.bIsValid = true;
                        } else {
                            currentStaging.uMinR = uMin; currentStaging.vMinR = vMin;
                            currentStaging.uMaxR = uMax; currentStaging.vMaxR = vMax;
                            // Do not pop on right eye, because multiple layers or passes might submit to right eye and drain the queue.
                        }
                        
                        // DEPTH CAPTURE
                        typedef ID3D11Resource* (*GetDepthResource_t)();
                        HMODULE hD3D11 = GetModuleHandle("d3d11.dll");
                        if (hD3D11) {
                            GetDepthResource_t pGetDepth = (GetDepthResource_t)GetProcAddress(hD3D11, "VerantyxGetDepthResource");
                            if (pGetDepth) {
                                ID3D11Resource* pDepthRes = pGetDepth();
                                if (pDepthRes) {
                                    ID3D11Texture2D* pDepthTex = nullptr;
                                    if (SUCCEEDED(pDepthRes->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pDepthTex))) {
                                        D3D11_TEXTURE2D_DESC dDesc;
                                        pDepthTex->GetDesc(&dDesc);
                                        
                                        if (*ppDepthStaging) {
                                            D3D11_TEXTURE2D_DESC existingDDesc;
                                            (*ppDepthStaging)->GetDesc(&existingDDesc);
                                            if (existingDDesc.Width != dDesc.Width || existingDDesc.Height != dDesc.Height || existingDDesc.Format != dDesc.Format) {
                                                (*ppDepthStaging)->Release();
                                                *ppDepthStaging = nullptr;
                                            }
                                        }
                                        
                                        if (!(*ppDepthStaging)) {
                                            D3D11_TEXTURE2D_DESC stDDesc = dDesc;
                                            stDDesc.Usage = D3D11_USAGE_STAGING;
                                            stDDesc.BindFlags = 0;
                                            stDDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                                            stDDesc.MiscFlags = 0;
                                            pDevice->CreateTexture2D(&stDDesc, nullptr, ppDepthStaging);
                                        }
                                        
                                        if (*ppDepthStaging) {
                                            pContext->CopyResource(*ppDepthStaging, pDepthRes);
                                        }
                                        pDepthTex->Release();
                                    }
                                }
                            }
                        }
                        
                        // Wait until Right Eye is submitted, then read the OLD frame (2 frames ago)
                        if (eEye == vr::Eye_Right) {
                            uint32_t readIndex = (g_stagingWriteIndex + 1) % STAGING_BUFFER_SIZE;
                            StagingFrame& readFrame = g_stagingFrames[readIndex];
                            
                            if (readFrame.bIsValid && readFrame.pColorLeft && readFrame.pColorRight) {
                                if (!g_copyThreadRunning) {
                                    g_copyThreadRunning = true;
                                    g_copyThread = std::thread(BackgroundCopyThread);
                                    g_copyThread.detach();
                                }
                                
                                readFrame.pContext = pContext;
                                readFrame.descFormat = desc.Format;
                                readFrame.descWidth = desc.Width;
                                readFrame.descHeight = desc.Height;
                                
                                {
                                    std::lock_guard<std::mutex> lock(g_copyMutex);
                                    g_pendingCopyIndex = readIndex;
                                }
                                g_copyCv.notify_one();
                            }
                            
                            // Advance write index
                            g_stagingWriteIndex = (g_stagingWriteIndex + 1) % STAGING_BUFFER_SIZE;
                        }
                        if (pMt) { pMt->Leave(); pMt->Release(); }
                        pContext->Release();
                    }
                    pDevice->Release();
                }
            }
        }
        return vr::VRCompositorError_None;
    }
    virtual void ClearLastSubmittedFrame() override {
        LogMsg("Called: IVRCompositor::ClearLastSubmittedFrame\n");
    }
    virtual void PostPresentHandoff() override {
        LogMsg("Called: IVRCompositor::PostPresentHandoff\n");
    }
    virtual bool GetFrameTiming(Compositor_FrameTiming *pTiming, uint32_t unFramesAgo = 0) override {
        LogMsg("Called: IVRCompositor::GetFrameTiming\n");
        return false;
    }
    virtual uint32_t GetFrameTimings(VR_ARRAY_COUNT( nFrames ) Compositor_FrameTiming *pTiming, uint32_t nFrames) override {
        LogMsg("Called: IVRCompositor::GetFrameTimings\n");
        return (uint32_t)0;
    }
    virtual float GetFrameTimeRemaining() override {
        LogMsg("Called: IVRCompositor::GetFrameTimeRemaining\n");
        return (float)0;
    }
    virtual void GetCumulativeStats(Compositor_CumulativeStats *pStats, uint32_t nStatsSizeInBytes) override {
        LogMsg("Called: IVRCompositor::GetCumulativeStats\n");
    }
    virtual void FadeToColor(float fSeconds, float fRed, float fGreen, float fBlue, float fAlpha, bool bBackground = false) override {
        LogMsg("Called: IVRCompositor::FadeToColor\n");
    }
    virtual void GetCurrentFadeColor(HmdColor_t *pRet, bool bBackground = false) override { if(pRet) { memset(pRet, 0, sizeof(*pRet)); } }
    virtual void FadeGrid(float fSeconds, bool bFadeIn) override {
        LogMsg("Called: IVRCompositor::FadeGrid\n");
    }
    virtual float GetCurrentGridAlpha() override {
        LogMsg("Called: IVRCompositor::GetCurrentGridAlpha\n");
        return (float)0;
    }
    virtual EVRCompositorError SetSkyboxOverride(VR_ARRAY_COUNT( unTextureCount ) const Texture_t *pTextures, uint32_t unTextureCount) override {
        LogMsg("Called: IVRCompositor::SetSkyboxOverride\n");
        return vr::VRCompositorError_None;
    }
    virtual void ClearSkyboxOverride() override {
        LogMsg("Called: IVRCompositor::ClearSkyboxOverride\n");
    }
    virtual void CompositorBringToFront() override {
        LogMsg("Called: IVRCompositor::CompositorBringToFront\n");
    }
    virtual void CompositorGoToBack() override {
        LogMsg("Called: IVRCompositor::CompositorGoToBack\n");
    }
    virtual void CompositorQuit() override {
        LogMsg("Called: IVRCompositor::CompositorQuit\n");
    }
    virtual bool IsFullscreen() override {
        LogMsg("Called: IVRCompositor::IsFullscreen\n");
        return false;
    }
    virtual uint32_t GetCurrentSceneFocusProcess() override {
        LogMsg("Called: IVRCompositor::GetCurrentSceneFocusProcess\n");
        return GetCurrentProcessId();
    }
    virtual uint32_t GetLastFrameRenderer() override {
        LogMsg("Called: IVRCompositor::GetLastFrameRenderer\n");
        return (uint32_t)0;
    }
    virtual bool CanRenderScene() override {
        LogMsg("Called: IVRCompositor::CanRenderScene\n");
        return true;
    }
    virtual void ShowMirrorWindow() override {
        LogMsg("Called: IVRCompositor::ShowMirrorWindow\n");
    }
    virtual void HideMirrorWindow() override {
        LogMsg("Called: IVRCompositor::HideMirrorWindow\n");
    }
    virtual bool IsMirrorWindowVisible() override {
        LogMsg("Called: IVRCompositor::IsMirrorWindowVisible\n");
        return false;
    }
    virtual void CompositorDumpImages() override {
        LogMsg("Called: IVRCompositor::CompositorDumpImages\n");
    }
    virtual bool ShouldAppRenderWithLowResources() override {
        LogMsg("Called: IVRCompositor::ShouldAppRenderWithLowResources\n");
        return false;
    }
    virtual void ForceInterleavedReprojectionOn(bool bOverride) override {
        LogMsg("Called: IVRCompositor::ForceInterleavedReprojectionOn\n");
    }
    virtual void ForceReconnectProcess() override {
        LogMsg("Called: IVRCompositor::ForceReconnectProcess\n");
    }
    virtual void SuspendRendering(bool bSuspend) override {
        LogMsg("Called: IVRCompositor::SuspendRendering\n");
    }
    virtual vr::EVRCompositorError GetMirrorTextureD3D11(vr::EVREye eEye, void *pD3D11DeviceOrResource, void **ppD3D11ShaderResourceView) override {
        LogMsg("Called: IVRCompositor::GetMirrorTextureD3D11\n");
        return (vr::EVRCompositorError)1;
    }
    virtual void ReleaseMirrorTextureD3D11(void *pD3D11ShaderResourceView) override {
        LogMsg("Called: IVRCompositor::ReleaseMirrorTextureD3D11\n");
    }
    virtual vr::EVRCompositorError GetMirrorTextureGL(vr::EVREye eEye, vr::glUInt_t *pglTextureId, vr::glSharedTextureHandle_t *pglSharedTextureHandle) override {
        LogMsg("Called: IVRCompositor::GetMirrorTextureGL\n");
        return (vr::EVRCompositorError)1;
    }
    virtual bool ReleaseSharedGLTexture(vr::glUInt_t glTextureId, vr::glSharedTextureHandle_t glSharedTextureHandle) override {
        LogMsg("Called: IVRCompositor::ReleaseSharedGLTexture\n");
        return false;
    }
    virtual void LockGLSharedTextureForAccess(vr::glSharedTextureHandle_t glSharedTextureHandle) override {
        LogMsg("Called: IVRCompositor::LockGLSharedTextureForAccess\n");
    }
    virtual void UnlockGLSharedTextureForAccess(vr::glSharedTextureHandle_t glSharedTextureHandle) override {
        LogMsg("Called: IVRCompositor::UnlockGLSharedTextureForAccess\n");
    }
    virtual uint32_t GetVulkanInstanceExtensionsRequired(VR_OUT_STRING() char *pchValue, uint32_t unBufferSize) override {
        LogMsg("Called: IVRCompositor::GetVulkanInstanceExtensionsRequired\n");
        if(pchValue && unBufferSize > 0) pchValue[0] = '\0';
        return 0;
    }
    virtual uint32_t GetVulkanDeviceExtensionsRequired(VkPhysicalDevice_T *pPhysicalDevice, VR_OUT_STRING() char *pchValue, uint32_t unBufferSize) override {
        LogMsg("Called: IVRCompositor::GetVulkanDeviceExtensionsRequired\n");
        if(pchValue && unBufferSize > 0) pchValue[0] = '\0';
        return 0;
    }
    virtual void SetExplicitTimingMode(EVRCompositorTimingMode eTimingMode) override {
        LogMsg("Called: IVRCompositor::SetExplicitTimingMode\n");
    }
    virtual EVRCompositorError SubmitExplicitTimingData() override {
        LogMsg("Called: IVRCompositor::SubmitExplicitTimingData\n");
        return (EVRCompositorError)1;
    }
    virtual bool IsMotionSmoothingEnabled() override {
        LogMsg("Called: IVRCompositor::IsMotionSmoothingEnabled\n");
        return false;
    }
    virtual bool IsMotionSmoothingSupported() override {
        LogMsg("Called: IVRCompositor::IsMotionSmoothingSupported\n");
        return false;
    }
    virtual bool IsCurrentSceneFocusAppLoading() override {
        LogMsg("Called: IVRCompositor::IsCurrentSceneFocusAppLoading\n");
        return false;
    }
    virtual EVRCompositorError SetStageOverride_Async(const char *pchRenderModelPath, const HmdMatrix34_t *pTransform = 0, const Compositor_StageRenderSettings *pRenderSettings = 0, uint32_t nSizeOfRenderSettings = 0) override {
        LogMsg("Called: IVRCompositor::SetStageOverride_Async\n");
        return vr::VRCompositorError_None;
    }
    virtual void ClearStageOverride() override {
        LogMsg("Called: IVRCompositor::ClearStageOverride\n");
    }
    virtual bool GetCompositorBenchmarkResults(Compositor_BenchmarkResults *pBenchmarkResults, uint32_t nSizeOfBenchmarkResults) override {
        LogMsg("Called: IVRCompositor::GetCompositorBenchmarkResults\n");
        return false;
    }
    virtual EVRCompositorError GetLastPosePredictionIDs(uint32_t *pRenderPosePredictionID, uint32_t *pGamePosePredictionID) override {
        LogMsg("Called: IVRCompositor::GetLastPosePredictionIDs\n");
        return (EVRCompositorError)1;
    }
    virtual EVRCompositorError GetPosesForFrame(uint32_t unPosePredictionID, VR_ARRAY_COUNT( unPoseArrayCount ) TrackedDevicePose_t* pPoseArray, uint32_t unPoseArrayCount) override {
        LogMsg("Called: IVRCompositor::GetPosesForFrame\n");
        return (EVRCompositorError)1;
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* DummyPadding42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* DummyPadding52() { return nullptr; }
    virtual void* DummyPadding53() { return nullptr; }
    virtual void* DummyPadding54() { return nullptr; }
    virtual void* DummyPadding55() { return nullptr; }
    virtual void* DummyPadding56() { return nullptr; }
    virtual void* DummyPadding57() { return nullptr; }
    virtual void* DummyPadding58() { return nullptr; }
    virtual void* DummyPadding59() { return nullptr; }
    virtual void* DummyPadding60() { return nullptr; }
    virtual void* DummyPadding61() { return nullptr; }
    virtual void* DummyPadding62() { return nullptr; }
    virtual void* DummyPadding63() { return nullptr; }
    virtual void* DummyPadding64() { return nullptr; }
    virtual void* DummyPadding65() { return nullptr; }
    virtual void* DummyPadding66() { return nullptr; }
    virtual void* DummyPadding67() { return nullptr; }
    virtual void* DummyPadding68() { return nullptr; }
    virtual void* DummyPadding69() { return nullptr; }
    virtual void* DummyPadding70() { return nullptr; }
    virtual void* DummyPadding71() { return nullptr; }
    virtual void* DummyPadding72() { return nullptr; }
    virtual void* DummyPadding73() { return nullptr; }
    virtual void* DummyPadding74() { return nullptr; }
    virtual void* DummyPadding75() { return nullptr; }
    virtual void* DummyPadding76() { return nullptr; }
    virtual void* DummyPadding77() { return nullptr; }
    virtual void* DummyPadding78() { return nullptr; }
    virtual void* DummyPadding79() { return nullptr; }
    virtual void* DummyPadding80() { return nullptr; }
    virtual void* DummyPadding81() { return nullptr; }
    virtual void* DummyPadding82() { return nullptr; }
    virtual void* DummyPadding83() { return nullptr; }
    virtual void* DummyPadding84() { return nullptr; }
    virtual void* DummyPadding85() { return nullptr; }
    virtual void* DummyPadding86() { return nullptr; }
    virtual void* DummyPadding87() { return nullptr; }
    virtual void* DummyPadding88() { return nullptr; }
    virtual void* DummyPadding89() { return nullptr; }
    virtual void* DummyPadding90() { return nullptr; }
    virtual void* DummyPadding91() { return nullptr; }
    virtual void* DummyPadding92() { return nullptr; }
    virtual void* DummyPadding93() { return nullptr; }
    virtual void* DummyPadding94() { return nullptr; }
    virtual void* DummyPadding95() { return nullptr; }
    virtual void* DummyPadding96() { return nullptr; }
    virtual void* DummyPadding97() { return nullptr; }
    virtual void* DummyPadding98() { return nullptr; }
    virtual void* DummyPadding99() { return nullptr; }
} g_mockCompositor;

class Mock_IVRHeadsetView : public vr::IVRHeadsetView {
public:
    virtual void SetHeadsetViewSize(uint32_t nWidth, uint32_t nHeight) override {
        LogMsg("Called: IVRHeadsetView::SetHeadsetViewSize\n");
    }
    virtual void GetHeadsetViewSize(uint32_t *pnWidth, uint32_t *pnHeight) override {
        LogMsg("Called: IVRHeadsetView::GetHeadsetViewSize\n");
    }
    virtual void SetHeadsetViewMode(HeadsetViewMode_t eHeadsetViewMode) override {
        LogMsg("Called: IVRHeadsetView::SetHeadsetViewMode\n");
    }
    virtual HeadsetViewMode_t GetHeadsetViewMode() override {
        LogMsg("Called: IVRHeadsetView::GetHeadsetViewMode\n");
        return (HeadsetViewMode_t)0;
    }
    virtual void SetHeadsetViewCropped(bool bCropped) override {
        LogMsg("Called: IVRHeadsetView::SetHeadsetViewCropped\n");
    }
    virtual bool GetHeadsetViewCropped() override {
        LogMsg("Called: IVRHeadsetView::GetHeadsetViewCropped\n");
        return false;
    }
    virtual float GetHeadsetViewAspectRatio() override {
        LogMsg("Called: IVRHeadsetView::GetHeadsetViewAspectRatio\n");
        return (float)0;
    }
    virtual void SetHeadsetViewBlendRange(float flStartPct, float flEndPct) override {
        LogMsg("Called: IVRHeadsetView::SetHeadsetViewBlendRange\n");
    }
    virtual void GetHeadsetViewBlendRange(float *pStartPct, float *pEndPct) override {
        LogMsg("Called: IVRHeadsetView::GetHeadsetViewBlendRange\n");
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* DummyPadding42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* DummyPadding52() { return nullptr; }
    virtual void* DummyPadding53() { return nullptr; }
    virtual void* DummyPadding54() { return nullptr; }
    virtual void* DummyPadding55() { return nullptr; }
    virtual void* DummyPadding56() { return nullptr; }
    virtual void* DummyPadding57() { return nullptr; }
    virtual void* DummyPadding58() { return nullptr; }
    virtual void* DummyPadding59() { return nullptr; }
    virtual void* DummyPadding60() { return nullptr; }
    virtual void* DummyPadding61() { return nullptr; }
    virtual void* DummyPadding62() { return nullptr; }
    virtual void* DummyPadding63() { return nullptr; }
    virtual void* DummyPadding64() { return nullptr; }
    virtual void* DummyPadding65() { return nullptr; }
    virtual void* DummyPadding66() { return nullptr; }
    virtual void* DummyPadding67() { return nullptr; }
    virtual void* DummyPadding68() { return nullptr; }
    virtual void* DummyPadding69() { return nullptr; }
    virtual void* DummyPadding70() { return nullptr; }
    virtual void* DummyPadding71() { return nullptr; }
    virtual void* DummyPadding72() { return nullptr; }
    virtual void* DummyPadding73() { return nullptr; }
    virtual void* DummyPadding74() { return nullptr; }
    virtual void* DummyPadding75() { return nullptr; }
    virtual void* DummyPadding76() { return nullptr; }
    virtual void* DummyPadding77() { return nullptr; }
    virtual void* DummyPadding78() { return nullptr; }
    virtual void* DummyPadding79() { return nullptr; }
    virtual void* DummyPadding80() { return nullptr; }
    virtual void* DummyPadding81() { return nullptr; }
    virtual void* DummyPadding82() { return nullptr; }
    virtual void* DummyPadding83() { return nullptr; }
    virtual void* DummyPadding84() { return nullptr; }
    virtual void* DummyPadding85() { return nullptr; }
    virtual void* DummyPadding86() { return nullptr; }
    virtual void* DummyPadding87() { return nullptr; }
    virtual void* DummyPadding88() { return nullptr; }
    virtual void* DummyPadding89() { return nullptr; }
    virtual void* DummyPadding90() { return nullptr; }
    virtual void* DummyPadding91() { return nullptr; }
    virtual void* DummyPadding92() { return nullptr; }
    virtual void* DummyPadding93() { return nullptr; }
    virtual void* DummyPadding94() { return nullptr; }
    virtual void* DummyPadding95() { return nullptr; }
    virtual void* DummyPadding96() { return nullptr; }
    virtual void* DummyPadding97() { return nullptr; }
    virtual void* DummyPadding98() { return nullptr; }
    virtual void* DummyPadding99() { return nullptr; }
} g_mockHeadsetView;

class Mock_IVRNotifications : public vr::IVRNotifications {
public:
    virtual EVRNotificationError CreateNotification(VROverlayHandle_t ulOverlayHandle, uint64_t ulUserValue, EVRNotificationType type, const char *pchText, EVRNotificationStyle style, const NotificationBitmap_t *pImage, VRNotificationId *pNotificationId) override {
        LogMsg("Called: IVRNotifications::CreateNotification\n");
        return (EVRNotificationError)1;
    }
    virtual EVRNotificationError RemoveNotification(VRNotificationId notificationId) override {
        LogMsg("Called: IVRNotifications::RemoveNotification\n");
        return (EVRNotificationError)1;
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* DummyPadding42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* DummyPadding52() { return nullptr; }
    virtual void* DummyPadding53() { return nullptr; }
    virtual void* DummyPadding54() { return nullptr; }
    virtual void* DummyPadding55() { return nullptr; }
    virtual void* DummyPadding56() { return nullptr; }
    virtual void* DummyPadding57() { return nullptr; }
    virtual void* DummyPadding58() { return nullptr; }
    virtual void* DummyPadding59() { return nullptr; }
    virtual void* DummyPadding60() { return nullptr; }
    virtual void* DummyPadding61() { return nullptr; }
    virtual void* DummyPadding62() { return nullptr; }
    virtual void* DummyPadding63() { return nullptr; }
    virtual void* DummyPadding64() { return nullptr; }
    virtual void* DummyPadding65() { return nullptr; }
    virtual void* DummyPadding66() { return nullptr; }
    virtual void* DummyPadding67() { return nullptr; }
    virtual void* DummyPadding68() { return nullptr; }
    virtual void* DummyPadding69() { return nullptr; }
    virtual void* DummyPadding70() { return nullptr; }
    virtual void* DummyPadding71() { return nullptr; }
    virtual void* DummyPadding72() { return nullptr; }
    virtual void* DummyPadding73() { return nullptr; }
    virtual void* DummyPadding74() { return nullptr; }
    virtual void* DummyPadding75() { return nullptr; }
    virtual void* DummyPadding76() { return nullptr; }
    virtual void* DummyPadding77() { return nullptr; }
    virtual void* DummyPadding78() { return nullptr; }
    virtual void* DummyPadding79() { return nullptr; }
    virtual void* DummyPadding80() { return nullptr; }
    virtual void* DummyPadding81() { return nullptr; }
    virtual void* DummyPadding82() { return nullptr; }
    virtual void* DummyPadding83() { return nullptr; }
    virtual void* DummyPadding84() { return nullptr; }
    virtual void* DummyPadding85() { return nullptr; }
    virtual void* DummyPadding86() { return nullptr; }
    virtual void* DummyPadding87() { return nullptr; }
    virtual void* DummyPadding88() { return nullptr; }
    virtual void* DummyPadding89() { return nullptr; }
    virtual void* DummyPadding90() { return nullptr; }
    virtual void* DummyPadding91() { return nullptr; }
    virtual void* DummyPadding92() { return nullptr; }
    virtual void* DummyPadding93() { return nullptr; }
    virtual void* DummyPadding94() { return nullptr; }
    virtual void* DummyPadding95() { return nullptr; }
    virtual void* DummyPadding96() { return nullptr; }
    virtual void* DummyPadding97() { return nullptr; }
    virtual void* DummyPadding98() { return nullptr; }
    virtual void* DummyPadding99() { return nullptr; }
} g_mockNotifications;

class Mock_IVROverlay : public vr::IVROverlay {
public:
    virtual EVROverlayError FindOverlay(const char *pchOverlayKey, VROverlayHandle_t * pOverlayHandle) override {
        LogMsg("Called: IVROverlay::FindOverlay\n");
        if (pOverlayHandle) *pOverlayHandle = 100;
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError CreateOverlay(const char *pchOverlayKey, const char *pchOverlayName, VROverlayHandle_t * pOverlayHandle) override {
        LogMsg("Called: IVROverlay::CreateOverlay\n");
        if (pOverlayHandle) *pOverlayHandle = 100;
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError DestroyOverlay(VROverlayHandle_t ulOverlayHandle) override {
        LogMsg("Called: IVROverlay::DestroyOverlay\n");
        return vr::VROverlayError_None;
    }
    virtual uint32_t GetOverlayKey(VROverlayHandle_t ulOverlayHandle, VR_OUT_STRING() char *pchValue, uint32_t unBufferSize, EVROverlayError *pError = 0L) override {
        LogMsg("Called: IVROverlay::GetOverlayKey\n");
        if (pchValue && unBufferSize > 0) pchValue[0] = '\0';
        return (uint32_t)0;
    }
    virtual uint32_t GetOverlayName(VROverlayHandle_t ulOverlayHandle, VR_OUT_STRING() char *pchValue, uint32_t unBufferSize, EVROverlayError *pError = 0L) override {
        LogMsg("Called: IVROverlay::GetOverlayName\n");
        if (pchValue && unBufferSize > 0) pchValue[0] = '\0';
        return (uint32_t)0;
    }
    virtual EVROverlayError SetOverlayName(VROverlayHandle_t ulOverlayHandle, const char *pchName) override {
        LogMsg("Called: IVROverlay::SetOverlayName\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError GetOverlayImageData(VROverlayHandle_t ulOverlayHandle, void *pvBuffer, uint32_t unBufferSize, uint32_t *punWidth, uint32_t *punHeight) override {
        LogMsg("Called: IVROverlay::GetOverlayImageData\n");
        return vr::VROverlayError_None;
    }
    virtual const char * GetOverlayErrorNameFromEnum(EVROverlayError error) override {
        LogMsg("Called: IVROverlay::GetOverlayErrorNameFromEnum\n");
        return "1.10.30";
    }
    virtual EVROverlayError SetOverlayRenderingPid(VROverlayHandle_t ulOverlayHandle, uint32_t unPID) override {
        LogMsg("Called: IVROverlay::SetOverlayRenderingPid\n");
        return vr::VROverlayError_None;
    }
    virtual uint32_t GetOverlayRenderingPid(VROverlayHandle_t ulOverlayHandle) override {
        LogMsg("Called: IVROverlay::GetOverlayRenderingPid\n");
        return (uint32_t)0;
    }
    virtual EVROverlayError SetOverlayFlag(VROverlayHandle_t ulOverlayHandle, VROverlayFlags eOverlayFlag, bool bEnabled) override {
        LogMsg("Called: IVROverlay::SetOverlayFlag\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError GetOverlayFlag(VROverlayHandle_t ulOverlayHandle, VROverlayFlags eOverlayFlag, bool *pbEnabled) override {
        LogMsg("Called: IVROverlay::GetOverlayFlag\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError GetOverlayFlags(VROverlayHandle_t ulOverlayHandle, uint32_t *pFlags) override {
        LogMsg("Called: IVROverlay::GetOverlayFlags\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError SetOverlayColor(VROverlayHandle_t ulOverlayHandle, float fRed, float fGreen, float fBlue) override {
        LogMsg("Called: IVROverlay::SetOverlayColor\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError GetOverlayColor(VROverlayHandle_t ulOverlayHandle, float *pfRed, float *pfGreen, float *pfBlue) override {
        LogMsg("Called: IVROverlay::GetOverlayColor\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError SetOverlayAlpha(VROverlayHandle_t ulOverlayHandle, float fAlpha) override {
        LogMsg("Called: IVROverlay::SetOverlayAlpha\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError GetOverlayAlpha(VROverlayHandle_t ulOverlayHandle, float *pfAlpha) override {
        LogMsg("Called: IVROverlay::GetOverlayAlpha\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError SetOverlayTexelAspect(VROverlayHandle_t ulOverlayHandle, float fTexelAspect) override {
        LogMsg("Called: IVROverlay::SetOverlayTexelAspect\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError GetOverlayTexelAspect(VROverlayHandle_t ulOverlayHandle, float *pfTexelAspect) override {
        LogMsg("Called: IVROverlay::GetOverlayTexelAspect\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError SetOverlaySortOrder(VROverlayHandle_t ulOverlayHandle, uint32_t unSortOrder) override {
        LogMsg("Called: IVROverlay::SetOverlaySortOrder\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError GetOverlaySortOrder(VROverlayHandle_t ulOverlayHandle, uint32_t *punSortOrder) override {
        LogMsg("Called: IVROverlay::GetOverlaySortOrder\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError SetOverlayWidthInMeters(VROverlayHandle_t ulOverlayHandle, float fWidthInMeters) override {
        LogMsg("Called: IVROverlay::SetOverlayWidthInMeters\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError GetOverlayWidthInMeters(VROverlayHandle_t ulOverlayHandle, float *pfWidthInMeters) override {
        LogMsg("Called: IVROverlay::GetOverlayWidthInMeters\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError SetOverlayCurvature(VROverlayHandle_t ulOverlayHandle, float fCurvature) override {
        LogMsg("Called: IVROverlay::SetOverlayCurvature\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError GetOverlayCurvature(VROverlayHandle_t ulOverlayHandle, float *pfCurvature) override {
        LogMsg("Called: IVROverlay::GetOverlayCurvature\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError SetOverlayTextureColorSpace(VROverlayHandle_t ulOverlayHandle, EColorSpace eTextureColorSpace) override {
        LogMsg("Called: IVROverlay::SetOverlayTextureColorSpace\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError GetOverlayTextureColorSpace(VROverlayHandle_t ulOverlayHandle, EColorSpace *peTextureColorSpace) override {
        LogMsg("Called: IVROverlay::GetOverlayTextureColorSpace\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError SetOverlayTextureBounds(VROverlayHandle_t ulOverlayHandle, const VRTextureBounds_t *pOverlayTextureBounds) override {
        LogMsg("Called: IVROverlay::SetOverlayTextureBounds\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError GetOverlayTextureBounds(VROverlayHandle_t ulOverlayHandle, VRTextureBounds_t *pOverlayTextureBounds) override {
        LogMsg("Called: IVROverlay::GetOverlayTextureBounds\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError GetOverlayTransformType(VROverlayHandle_t ulOverlayHandle, VROverlayTransformType *peTransformType) override {
        LogMsg("Called: IVROverlay::GetOverlayTransformType\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError SetOverlayTransformAbsolute(VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t *pmatTrackingOriginToOverlayTransform) override {
        LogMsg("Called: IVROverlay::SetOverlayTransformAbsolute\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError GetOverlayTransformAbsolute(VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin *peTrackingOrigin, HmdMatrix34_t *pmatTrackingOriginToOverlayTransform) override {
        LogMsg("Called: IVROverlay::GetOverlayTransformAbsolute\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError SetOverlayTransformTrackedDeviceRelative(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unTrackedDevice, const HmdMatrix34_t *pmatTrackedDeviceToOverlayTransform) override {
        LogMsg("Called: IVROverlay::SetOverlayTransformTrackedDeviceRelative\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError GetOverlayTransformTrackedDeviceRelative(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t *punTrackedDevice, HmdMatrix34_t *pmatTrackedDeviceToOverlayTransform) override {
        LogMsg("Called: IVROverlay::GetOverlayTransformTrackedDeviceRelative\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError SetOverlayTransformTrackedDeviceComponent(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unDeviceIndex, const char *pchComponentName) override {
        LogMsg("Called: IVROverlay::SetOverlayTransformTrackedDeviceComponent\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError GetOverlayTransformTrackedDeviceComponent(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t *punDeviceIndex, VR_OUT_STRING() char *pchComponentName, uint32_t unComponentNameSize) override {
        LogMsg("Called: IVROverlay::GetOverlayTransformTrackedDeviceComponent\n");
        if (pchComponentName && unComponentNameSize > 0) pchComponentName[0] = '\0';
        return vr::VROverlayError_None;
    }
    virtual vr::EVROverlayError GetOverlayTransformOverlayRelative(VROverlayHandle_t ulOverlayHandle, VROverlayHandle_t *ulOverlayHandleParent, HmdMatrix34_t *pmatParentOverlayToOverlayTransform) override {
        LogMsg("Called: IVROverlay::GetOverlayTransformOverlayRelative\n");
        return vr::VROverlayError_None;
    }
    virtual vr::EVROverlayError SetOverlayTransformOverlayRelative(VROverlayHandle_t ulOverlayHandle, VROverlayHandle_t ulOverlayHandleParent, const HmdMatrix34_t *pmatParentOverlayToOverlayTransform) override {
        LogMsg("Called: IVROverlay::SetOverlayTransformOverlayRelative\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError SetOverlayTransformCursor(VROverlayHandle_t ulCursorOverlayHandle, const HmdVector2_t *pvHotspot) override {
        LogMsg("Called: IVROverlay::SetOverlayTransformCursor\n");
        return vr::VROverlayError_None;
    }
    virtual vr::EVROverlayError GetOverlayTransformCursor(VROverlayHandle_t ulOverlayHandle, HmdVector2_t *pvHotspot) override {
        LogMsg("Called: IVROverlay::GetOverlayTransformCursor\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError ShowOverlay(VROverlayHandle_t ulOverlayHandle) override {
        LogMsg("Called: IVROverlay::ShowOverlay\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError HideOverlay(VROverlayHandle_t ulOverlayHandle) override {
        LogMsg("Called: IVROverlay::HideOverlay\n");
        return vr::VROverlayError_None;
    }
    virtual bool IsOverlayVisible(VROverlayHandle_t ulOverlayHandle) override {
        LogMsg("Called: IVROverlay::IsOverlayVisible\n");
        return false;
    }
    virtual EVROverlayError GetTransformForOverlayCoordinates(VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin eTrackingOrigin, HmdVector2_t coordinatesInOverlay, HmdMatrix34_t *pmatTransform) override {
        LogMsg("Called: IVROverlay::GetTransformForOverlayCoordinates\n");
        return vr::VROverlayError_None;
    }
    virtual bool PollNextOverlayEvent(VROverlayHandle_t ulOverlayHandle, VREvent_t *pEvent, uint32_t uncbVREvent) override {
        LogMsg("Called: IVROverlay::PollNextOverlayEvent\n");
        return false;
    }
    virtual EVROverlayError GetOverlayInputMethod(VROverlayHandle_t ulOverlayHandle, VROverlayInputMethod *peInputMethod) override {
        LogMsg("Called: IVROverlay::GetOverlayInputMethod\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError SetOverlayInputMethod(VROverlayHandle_t ulOverlayHandle, VROverlayInputMethod eInputMethod) override {
        LogMsg("Called: IVROverlay::SetOverlayInputMethod\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError GetOverlayMouseScale(VROverlayHandle_t ulOverlayHandle, HmdVector2_t *pvecMouseScale) override {
        LogMsg("Called: IVROverlay::GetOverlayMouseScale\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError SetOverlayMouseScale(VROverlayHandle_t ulOverlayHandle, const HmdVector2_t *pvecMouseScale) override {
        LogMsg("Called: IVROverlay::SetOverlayMouseScale\n");
        return vr::VROverlayError_None;
    }
    virtual bool ComputeOverlayIntersection(VROverlayHandle_t ulOverlayHandle, const VROverlayIntersectionParams_t *pParams, VROverlayIntersectionResults_t *pResults) override {
        LogMsg("Called: IVROverlay::ComputeOverlayIntersection\n");
        return false;
    }
    virtual bool IsHoverTargetOverlay(VROverlayHandle_t ulOverlayHandle) override {
        LogMsg("Called: IVROverlay::IsHoverTargetOverlay\n");
        return false;
    }
    virtual EVROverlayError SetOverlayIntersectionMask(VROverlayHandle_t ulOverlayHandle, VROverlayIntersectionMaskPrimitive_t *pMaskPrimitives, uint32_t unNumMaskPrimitives, uint32_t unPrimitiveSize = sizeof( VROverlayIntersectionMaskPrimitive_t )) override {
        LogMsg("Called: IVROverlay::SetOverlayIntersectionMask\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError TriggerLaserMouseHapticVibration(VROverlayHandle_t ulOverlayHandle, float fDurationSeconds, float fFrequency, float fAmplitude) override {
        LogMsg("Called: IVROverlay::TriggerLaserMouseHapticVibration\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError SetOverlayCursor(VROverlayHandle_t ulOverlayHandle, VROverlayHandle_t ulCursorHandle) override {
        LogMsg("Called: IVROverlay::SetOverlayCursor\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError SetOverlayCursorPositionOverride(VROverlayHandle_t ulOverlayHandle, const HmdVector2_t *pvCursor) override {
        LogMsg("Called: IVROverlay::SetOverlayCursorPositionOverride\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError ClearOverlayCursorPositionOverride(VROverlayHandle_t ulOverlayHandle) override {
        LogMsg("Called: IVROverlay::ClearOverlayCursorPositionOverride\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError SetOverlayTexture(VROverlayHandle_t ulOverlayHandle, const Texture_t *pTexture) override {
        LogMsg("Called: IVROverlay::SetOverlayTexture\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError ClearOverlayTexture(VROverlayHandle_t ulOverlayHandle) override {
        LogMsg("Called: IVROverlay::ClearOverlayTexture\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError SetOverlayRaw(VROverlayHandle_t ulOverlayHandle, void *pvBuffer, uint32_t unWidth, uint32_t unHeight, uint32_t unBytesPerPixel) override {
        LogMsg("Called: IVROverlay::SetOverlayRaw\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError SetOverlayFromFile(VROverlayHandle_t ulOverlayHandle, const char *pchFilePath) override {
        LogMsg("Called: IVROverlay::SetOverlayFromFile\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError GetOverlayTexture(VROverlayHandle_t ulOverlayHandle, void **pNativeTextureHandle, void *pNativeTextureRef, uint32_t *pWidth, uint32_t *pHeight, uint32_t *pNativeFormat, ETextureType *pAPIType, EColorSpace *pColorSpace, VRTextureBounds_t *pTextureBounds) override {
        LogMsg("Called: IVROverlay::GetOverlayTexture\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError ReleaseNativeOverlayHandle(VROverlayHandle_t ulOverlayHandle, void *pNativeTextureHandle) override {
        LogMsg("Called: IVROverlay::ReleaseNativeOverlayHandle\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError GetOverlayTextureSize(VROverlayHandle_t ulOverlayHandle, uint32_t *pWidth, uint32_t *pHeight) override {
        LogMsg("Called: IVROverlay::GetOverlayTextureSize\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError CreateDashboardOverlay(const char *pchOverlayKey, const char *pchOverlayFriendlyName, VROverlayHandle_t * pMainHandle, VROverlayHandle_t *pThumbnailHandle) override {
        LogMsg("Called: IVROverlay::CreateDashboardOverlay\n");
        if (pMainHandle) *pMainHandle = 101;
        if (pThumbnailHandle) *pThumbnailHandle = 102;
        return vr::VROverlayError_None;
    }
    virtual bool IsDashboardVisible() override {
        LogMsg("Called: IVROverlay::IsDashboardVisible\n");
        return false;
    }
    virtual bool IsActiveDashboardOverlay(VROverlayHandle_t ulOverlayHandle) override {
        LogMsg("Called: IVROverlay::IsActiveDashboardOverlay\n");
        return false;
    }
    virtual EVROverlayError SetDashboardOverlaySceneProcess(VROverlayHandle_t ulOverlayHandle, uint32_t unProcessId) override {
        LogMsg("Called: IVROverlay::SetDashboardOverlaySceneProcess\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError GetDashboardOverlaySceneProcess(VROverlayHandle_t ulOverlayHandle, uint32_t *punProcessId) override {
        LogMsg("Called: IVROverlay::GetDashboardOverlaySceneProcess\n");
        return vr::VROverlayError_None;
    }
    virtual void ShowDashboard(const char *pchOverlayToShow) override {
        LogMsg("Called: IVROverlay::ShowDashboard\n");
    }
    virtual vr::TrackedDeviceIndex_t GetPrimaryDashboardDevice() override {
        LogMsg("Called: IVROverlay::GetPrimaryDashboardDevice\n");
        return (vr::TrackedDeviceIndex_t)0;
    }
    virtual EVROverlayError ShowKeyboard(EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode, uint32_t unFlags, const char *pchDescription, uint32_t unCharMax, const char *pchExistingText, uint64_t uUserValue) override {
        LogMsg("Called: IVROverlay::ShowKeyboard\n");
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError ShowKeyboardForOverlay(VROverlayHandle_t ulOverlayHandle, EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode, uint32_t unFlags, const char *pchDescription, uint32_t unCharMax, const char *pchExistingText, uint64_t uUserValue) override {
        LogMsg("Called: IVROverlay::ShowKeyboardForOverlay\n");
        return vr::VROverlayError_None;
    }
    virtual uint32_t GetKeyboardText(VR_OUT_STRING() char *pchText, uint32_t cchText) override {
        LogMsg("Called: IVROverlay::GetKeyboardText\n");
        if (pchText && cchText > 0) pchText[0] = '\0';
        return (uint32_t)0;
    }
    virtual void HideKeyboard() override {
        LogMsg("Called: IVROverlay::HideKeyboard\n");
    }
    virtual void SetKeyboardTransformAbsolute(ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t *pmatTrackingOriginToKeyboardTransform) override {
        LogMsg("Called: IVROverlay::SetKeyboardTransformAbsolute\n");
    }
    virtual void SetKeyboardPositionForOverlay(VROverlayHandle_t ulOverlayHandle, HmdRect2_t avoidRect) override {
        LogMsg("Called: IVROverlay::SetKeyboardPositionForOverlay\n");
    }
    virtual VRMessageOverlayResponse ShowMessageOverlay(const char* pchText, const char* pchCaption, const char* pchButton0Text, const char* pchButton1Text = nullptr, const char* pchButton2Text = nullptr, const char* pchButton3Text = nullptr) override {
        LogMsg("Called: IVROverlay::ShowMessageOverlay\n");
        return (VRMessageOverlayResponse)0;
    }
    virtual void CloseMessageOverlay() override {
        LogMsg("Called: IVROverlay::CloseMessageOverlay\n");
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* DummyPadding42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* DummyPadding52() { return nullptr; }
    virtual void* DummyPadding53() { return nullptr; }
    virtual void* DummyPadding54() { return nullptr; }
    virtual void* DummyPadding55() { return nullptr; }
    virtual void* DummyPadding56() { return nullptr; }
    virtual void* DummyPadding57() { return nullptr; }
    virtual void* DummyPadding58() { return nullptr; }
    virtual void* DummyPadding59() { return nullptr; }
    virtual void* DummyPadding60() { return nullptr; }
    virtual void* DummyPadding61() { return nullptr; }
    virtual void* DummyPadding62() { return nullptr; }
    virtual void* DummyPadding63() { return nullptr; }
    virtual void* DummyPadding64() { return nullptr; }
    virtual void* DummyPadding65() { return nullptr; }
    virtual void* DummyPadding66() { return nullptr; }
    virtual void* DummyPadding67() { return nullptr; }
    virtual void* DummyPadding68() { return nullptr; }
    virtual void* DummyPadding69() { return nullptr; }
    virtual void* DummyPadding70() { return nullptr; }
    virtual void* DummyPadding71() { return nullptr; }
    virtual void* DummyPadding72() { return nullptr; }
    virtual void* DummyPadding73() { return nullptr; }
    virtual void* DummyPadding74() { return nullptr; }
    virtual void* DummyPadding75() { return nullptr; }
    virtual void* DummyPadding76() { return nullptr; }
    virtual void* DummyPadding77() { return nullptr; }
    virtual void* DummyPadding78() { return nullptr; }
    virtual void* DummyPadding79() { return nullptr; }
    virtual void* DummyPadding80() { return nullptr; }
    virtual void* DummyPadding81() { return nullptr; }
    virtual void* DummyPadding82() { return nullptr; }
    virtual void* DummyPadding83() { return nullptr; }
    virtual void* DummyPadding84() { return nullptr; }
    virtual void* DummyPadding85() { return nullptr; }
    virtual void* DummyPadding86() { return nullptr; }
    virtual void* DummyPadding87() { return nullptr; }
    virtual void* DummyPadding88() { return nullptr; }
    virtual void* DummyPadding89() { return nullptr; }
    virtual void* DummyPadding90() { return nullptr; }
    virtual void* DummyPadding91() { return nullptr; }
    virtual void* DummyPadding92() { return nullptr; }
    virtual void* DummyPadding93() { return nullptr; }
    virtual void* DummyPadding94() { return nullptr; }
    virtual void* DummyPadding95() { return nullptr; }
    virtual void* DummyPadding96() { return nullptr; }
    virtual void* DummyPadding97() { return nullptr; }
    virtual void* DummyPadding98() { return nullptr; }
    virtual void* DummyPadding99() { return nullptr; }
} g_mockOverlay;

class Mock_IVROverlayView : public vr::IVROverlayView {
public:
    virtual EVROverlayError AcquireOverlayView(VROverlayHandle_t ulOverlayHandle, VRNativeDevice_t *pNativeDevice, VROverlayView_t *pOverlayView, uint32_t unOverlayViewSize) override {
        LogMsg("Called: IVROverlayView::AcquireOverlayView\n");
        if (pOverlayView && unOverlayViewSize >= sizeof(VROverlayView_t)) {
            memset(pOverlayView, 0, unOverlayViewSize);
        }
        return vr::VROverlayError_None;
    }
    virtual EVROverlayError ReleaseOverlayView(VROverlayView_t *pOverlayView) override {
        LogMsg("Called: IVROverlayView::ReleaseOverlayView\n");
        return vr::VROverlayError_None;
    }
    virtual void PostOverlayEvent(VROverlayHandle_t ulOverlayHandle, const VREvent_t *pvrEvent) override {
        LogMsg("Called: IVROverlayView::PostOverlayEvent\n");
    }
    virtual bool IsViewingPermitted(VROverlayHandle_t ulOverlayHandle) override {
        LogMsg("Called: IVROverlayView::IsViewingPermitted\n");
        return false;
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* DummyPadding42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* DummyPadding52() { return nullptr; }
    virtual void* DummyPadding53() { return nullptr; }
    virtual void* DummyPadding54() { return nullptr; }
    virtual void* DummyPadding55() { return nullptr; }
    virtual void* DummyPadding56() { return nullptr; }
    virtual void* DummyPadding57() { return nullptr; }
    virtual void* DummyPadding58() { return nullptr; }
    virtual void* DummyPadding59() { return nullptr; }
    virtual void* DummyPadding60() { return nullptr; }
    virtual void* DummyPadding61() { return nullptr; }
    virtual void* DummyPadding62() { return nullptr; }
    virtual void* DummyPadding63() { return nullptr; }
    virtual void* DummyPadding64() { return nullptr; }
    virtual void* DummyPadding65() { return nullptr; }
    virtual void* DummyPadding66() { return nullptr; }
    virtual void* DummyPadding67() { return nullptr; }
    virtual void* DummyPadding68() { return nullptr; }
    virtual void* DummyPadding69() { return nullptr; }
    virtual void* DummyPadding70() { return nullptr; }
    virtual void* DummyPadding71() { return nullptr; }
    virtual void* DummyPadding72() { return nullptr; }
    virtual void* DummyPadding73() { return nullptr; }
    virtual void* DummyPadding74() { return nullptr; }
    virtual void* DummyPadding75() { return nullptr; }
    virtual void* DummyPadding76() { return nullptr; }
    virtual void* DummyPadding77() { return nullptr; }
    virtual void* DummyPadding78() { return nullptr; }
    virtual void* DummyPadding79() { return nullptr; }
    virtual void* DummyPadding80() { return nullptr; }
    virtual void* DummyPadding81() { return nullptr; }
    virtual void* DummyPadding82() { return nullptr; }
    virtual void* DummyPadding83() { return nullptr; }
    virtual void* DummyPadding84() { return nullptr; }
    virtual void* DummyPadding85() { return nullptr; }
    virtual void* DummyPadding86() { return nullptr; }
    virtual void* DummyPadding87() { return nullptr; }
    virtual void* DummyPadding88() { return nullptr; }
    virtual void* DummyPadding89() { return nullptr; }
    virtual void* DummyPadding90() { return nullptr; }
    virtual void* DummyPadding91() { return nullptr; }
    virtual void* DummyPadding92() { return nullptr; }
    virtual void* DummyPadding93() { return nullptr; }
    virtual void* DummyPadding94() { return nullptr; }
    virtual void* DummyPadding95() { return nullptr; }
    virtual void* DummyPadding96() { return nullptr; }
    virtual void* DummyPadding97() { return nullptr; }
    virtual void* DummyPadding98() { return nullptr; }
    virtual void* DummyPadding99() { return nullptr; }
} g_mockOverlayView;

class Mock_IVRRenderModels : public vr::IVRRenderModels {
public:
    virtual EVRRenderModelError LoadRenderModel_Async(const char *pchRenderModelName, RenderModel_t **ppRenderModel) override {
        LogMsg("Called: IVRRenderModels::LoadRenderModel_Async\n");
        if (ppRenderModel) *ppRenderModel = nullptr;
        return vr::VRRenderModelError_NotSupported;
    }
    virtual void FreeRenderModel(RenderModel_t *pRenderModel) override {
        LogMsg("Called: IVRRenderModels::FreeRenderModel\n");
    }
    virtual EVRRenderModelError LoadTexture_Async(TextureID_t textureId, RenderModel_TextureMap_t **ppTexture) override {
        LogMsg("Called: IVRRenderModels::LoadTexture_Async\n");
        return vr::VRRenderModelError_NotSupported;
    }
    virtual void FreeTexture(RenderModel_TextureMap_t *pTexture) override {
        LogMsg("Called: IVRRenderModels::FreeTexture\n");
    }
    virtual EVRRenderModelError LoadTextureD3D11_Async(TextureID_t textureId, void *pD3D11Device, void **ppD3D11Texture2D) override {
        LogMsg("Called: IVRRenderModels::LoadTextureD3D11_Async\n");
        return vr::VRRenderModelError_NotSupported;
    }
    virtual EVRRenderModelError LoadIntoTextureD3D11_Async(TextureID_t textureId, void *pDstTexture) override {
        LogMsg("Called: IVRRenderModels::LoadIntoTextureD3D11_Async\n");
        return vr::VRRenderModelError_NotSupported;
    }
    virtual void FreeTextureD3D11(void *pD3D11Texture2D) override {
        LogMsg("Called: IVRRenderModels::FreeTextureD3D11\n");
    }
    virtual uint32_t GetRenderModelName(uint32_t unRenderModelIndex, VR_OUT_STRING() char *pchRenderModelName, uint32_t unRenderModelNameLen) override {
        LogMsg("Called: IVRRenderModels::GetRenderModelName\n");
        return (uint32_t)0;
    }
    virtual uint32_t GetRenderModelCount() override {
        LogMsg("Called: IVRRenderModels::GetRenderModelCount\n");
        return (uint32_t)0;
    }
    virtual uint32_t GetComponentCount(const char *pchRenderModelName) override {
        LogMsg("Called: IVRRenderModels::GetComponentCount\n");
        return (uint32_t)0;
    }
    virtual uint32_t GetComponentName(const char *pchRenderModelName, uint32_t unComponentIndex, VR_OUT_STRING( ) char *pchComponentName, uint32_t unComponentNameLen) override {
        LogMsg("Called: IVRRenderModels::GetComponentName\n");
        return (uint32_t)0;
    }
    virtual uint64_t GetComponentButtonMask(const char *pchRenderModelName, const char *pchComponentName) override {
        LogMsg("Called: IVRRenderModels::GetComponentButtonMask\n");
        return (uint64_t)0;
    }
    virtual uint32_t GetComponentRenderModelName(const char *pchRenderModelName, const char *pchComponentName, VR_OUT_STRING( ) char *pchComponentRenderModelName, uint32_t unComponentRenderModelNameLen) override {
        LogMsg("Called: IVRRenderModels::GetComponentRenderModelName\n");
        return (uint32_t)0;
    }
    virtual bool GetComponentStateForDevicePath(const char *pchRenderModelName, const char *pchComponentName, vr::VRInputValueHandle_t devicePath, const vr::RenderModel_ControllerMode_State_t *pState, vr::RenderModel_ComponentState_t *pComponentState) override {
        LogMsg("Called: IVRRenderModels::GetComponentStateForDevicePath\n");
        return false;
    }
    virtual bool GetComponentState(const char *pchRenderModelName, const char *pchComponentName, const vr::VRControllerState_t *pControllerState, const RenderModel_ControllerMode_State_t *pState, RenderModel_ComponentState_t *pComponentState) override {
        LogMsg("Called: IVRRenderModels::GetComponentState\n");
        return false;
    }
    virtual bool RenderModelHasComponent(const char *pchRenderModelName, const char *pchComponentName) override {
        LogMsg("Called: IVRRenderModels::RenderModelHasComponent\n");
        return false;
    }
    virtual uint32_t GetRenderModelThumbnailURL(const char *pchRenderModelName, VR_OUT_STRING() char *pchThumbnailURL, uint32_t unThumbnailURLLen, vr::EVRRenderModelError *peError) override {
        LogMsg("Called: IVRRenderModels::GetRenderModelThumbnailURL\n");
        return (uint32_t)0;
    }
    virtual uint32_t GetRenderModelOriginalPath(const char *pchRenderModelName, VR_OUT_STRING() char *pchOriginalPath, uint32_t unOriginalPathLen, vr::EVRRenderModelError *peError) override {
        LogMsg("Called: IVRRenderModels::GetRenderModelOriginalPath\n");
        return (uint32_t)0;
    }
    virtual const char * GetRenderModelErrorNameFromEnum(vr::EVRRenderModelError error) override {
        LogMsg("Called: IVRRenderModels::GetRenderModelErrorNameFromEnum\n");
        return "1.10.30";
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* DummyPadding42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* DummyPadding52() { return nullptr; }
    virtual void* DummyPadding53() { return nullptr; }
    virtual void* DummyPadding54() { return nullptr; }
    virtual void* DummyPadding55() { return nullptr; }
    virtual void* DummyPadding56() { return nullptr; }
    virtual void* DummyPadding57() { return nullptr; }
    virtual void* DummyPadding58() { return nullptr; }
    virtual void* DummyPadding59() { return nullptr; }
    virtual void* DummyPadding60() { return nullptr; }
    virtual void* DummyPadding61() { return nullptr; }
    virtual void* DummyPadding62() { return nullptr; }
    virtual void* DummyPadding63() { return nullptr; }
    virtual void* DummyPadding64() { return nullptr; }
    virtual void* DummyPadding65() { return nullptr; }
    virtual void* DummyPadding66() { return nullptr; }
    virtual void* DummyPadding67() { return nullptr; }
    virtual void* DummyPadding68() { return nullptr; }
    virtual void* DummyPadding69() { return nullptr; }
    virtual void* DummyPadding70() { return nullptr; }
    virtual void* DummyPadding71() { return nullptr; }
    virtual void* DummyPadding72() { return nullptr; }
    virtual void* DummyPadding73() { return nullptr; }
    virtual void* DummyPadding74() { return nullptr; }
    virtual void* DummyPadding75() { return nullptr; }
    virtual void* DummyPadding76() { return nullptr; }
    virtual void* DummyPadding77() { return nullptr; }
    virtual void* DummyPadding78() { return nullptr; }
    virtual void* DummyPadding79() { return nullptr; }
    virtual void* DummyPadding80() { return nullptr; }
    virtual void* DummyPadding81() { return nullptr; }
    virtual void* DummyPadding82() { return nullptr; }
    virtual void* DummyPadding83() { return nullptr; }
    virtual void* DummyPadding84() { return nullptr; }
    virtual void* DummyPadding85() { return nullptr; }
    virtual void* DummyPadding86() { return nullptr; }
    virtual void* DummyPadding87() { return nullptr; }
    virtual void* DummyPadding88() { return nullptr; }
    virtual void* DummyPadding89() { return nullptr; }
    virtual void* DummyPadding90() { return nullptr; }
    virtual void* DummyPadding91() { return nullptr; }
    virtual void* DummyPadding92() { return nullptr; }
    virtual void* DummyPadding93() { return nullptr; }
    virtual void* DummyPadding94() { return nullptr; }
    virtual void* DummyPadding95() { return nullptr; }
    virtual void* DummyPadding96() { return nullptr; }
    virtual void* DummyPadding97() { return nullptr; }
    virtual void* DummyPadding98() { return nullptr; }
    virtual void* DummyPadding99() { return nullptr; }
} g_mockRenderModels;

class Mock_IVRExtendedDisplay : public vr::IVRExtendedDisplay {
public:
    virtual void GetWindowBounds(int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight) override {
        LogMsg("Called: IVRExtendedDisplay::GetWindowBounds\n");
    }
    virtual void GetEyeOutputViewport(EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight) override {
        LogMsg("Called: IVRExtendedDisplay::GetEyeOutputViewport\n");
        if(pnX) *pnX = 0; if(pnY) *pnY = 0; if(pnWidth) *pnWidth = 1920; if(pnHeight) *pnHeight = 1080;
    }
    virtual void GetDXGIOutputInfo(int32_t *pnAdapterIndex, int32_t *pnAdapterOutputIndex) override {
        LogMsg("Called: IVRExtendedDisplay::GetDXGIOutputInfo\n");
        if(pnAdapterIndex) *pnAdapterIndex = 0;
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* DummyPadding42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* DummyPadding52() { return nullptr; }
    virtual void* DummyPadding53() { return nullptr; }
    virtual void* DummyPadding54() { return nullptr; }
    virtual void* DummyPadding55() { return nullptr; }
    virtual void* DummyPadding56() { return nullptr; }
    virtual void* DummyPadding57() { return nullptr; }
    virtual void* DummyPadding58() { return nullptr; }
    virtual void* DummyPadding59() { return nullptr; }
    virtual void* DummyPadding60() { return nullptr; }
    virtual void* DummyPadding61() { return nullptr; }
    virtual void* DummyPadding62() { return nullptr; }
    virtual void* DummyPadding63() { return nullptr; }
    virtual void* DummyPadding64() { return nullptr; }
    virtual void* DummyPadding65() { return nullptr; }
    virtual void* DummyPadding66() { return nullptr; }
    virtual void* DummyPadding67() { return nullptr; }
    virtual void* DummyPadding68() { return nullptr; }
    virtual void* DummyPadding69() { return nullptr; }
    virtual void* DummyPadding70() { return nullptr; }
    virtual void* DummyPadding71() { return nullptr; }
    virtual void* DummyPadding72() { return nullptr; }
    virtual void* DummyPadding73() { return nullptr; }
    virtual void* DummyPadding74() { return nullptr; }
    virtual void* DummyPadding75() { return nullptr; }
    virtual void* DummyPadding76() { return nullptr; }
    virtual void* DummyPadding77() { return nullptr; }
    virtual void* DummyPadding78() { return nullptr; }
    virtual void* DummyPadding79() { return nullptr; }
    virtual void* DummyPadding80() { return nullptr; }
    virtual void* DummyPadding81() { return nullptr; }
    virtual void* DummyPadding82() { return nullptr; }
    virtual void* DummyPadding83() { return nullptr; }
    virtual void* DummyPadding84() { return nullptr; }
    virtual void* DummyPadding85() { return nullptr; }
    virtual void* DummyPadding86() { return nullptr; }
    virtual void* DummyPadding87() { return nullptr; }
    virtual void* DummyPadding88() { return nullptr; }
    virtual void* DummyPadding89() { return nullptr; }
    virtual void* DummyPadding90() { return nullptr; }
    virtual void* DummyPadding91() { return nullptr; }
    virtual void* DummyPadding92() { return nullptr; }
    virtual void* DummyPadding93() { return nullptr; }
    virtual void* DummyPadding94() { return nullptr; }
    virtual void* DummyPadding95() { return nullptr; }
    virtual void* DummyPadding96() { return nullptr; }
    virtual void* DummyPadding97() { return nullptr; }
    virtual void* DummyPadding98() { return nullptr; }
    virtual void* DummyPadding99() { return nullptr; }
} g_mockExtendedDisplay;

class Mock_IVRTrackedCamera : public vr::IVRTrackedCamera {
public:
    virtual const char * GetCameraErrorNameFromEnum(vr::EVRTrackedCameraError eCameraError) override {
        LogMsg("Called: IVRTrackedCamera::GetCameraErrorNameFromEnum\n");
        return "1.10.30";
    }
    virtual vr::EVRTrackedCameraError HasCamera(vr::TrackedDeviceIndex_t nDeviceIndex, bool *pHasCamera) override {
        LogMsg("Called: IVRTrackedCamera::HasCamera\n");
        return vr::VRTrackedCameraError_OperationFailed;
    }
    virtual vr::EVRTrackedCameraError GetCameraFrameSize(vr::TrackedDeviceIndex_t nDeviceIndex, vr::EVRTrackedCameraFrameType eFrameType, uint32_t *pnWidth, uint32_t *pnHeight, uint32_t *pnFrameBufferSize) override {
        LogMsg("Called: IVRTrackedCamera::GetCameraFrameSize\n");
        return vr::VRTrackedCameraError_OperationFailed;
    }
    virtual vr::EVRTrackedCameraError GetCameraIntrinsics(vr::TrackedDeviceIndex_t nDeviceIndex, uint32_t nCameraIndex, vr::EVRTrackedCameraFrameType eFrameType, vr::HmdVector2_t *pFocalLength, vr::HmdVector2_t *pCenter) override {
        LogMsg("Called: IVRTrackedCamera::GetCameraIntrinsics\n");
        return vr::VRTrackedCameraError_OperationFailed;
    }
    virtual vr::EVRTrackedCameraError GetCameraProjection(vr::TrackedDeviceIndex_t nDeviceIndex, uint32_t nCameraIndex, vr::EVRTrackedCameraFrameType eFrameType, float flZNear, float flZFar, vr::HmdMatrix44_t *pProjection) override {
        LogMsg("Called: IVRTrackedCamera::GetCameraProjection\n");
        return vr::VRTrackedCameraError_OperationFailed;
    }
    virtual vr::EVRTrackedCameraError AcquireVideoStreamingService(vr::TrackedDeviceIndex_t nDeviceIndex, vr::TrackedCameraHandle_t *pHandle) override {
        LogMsg("Called: IVRTrackedCamera::AcquireVideoStreamingService\n");
        return vr::VRTrackedCameraError_OperationFailed;
    }
    virtual vr::EVRTrackedCameraError ReleaseVideoStreamingService(vr::TrackedCameraHandle_t hTrackedCamera) override {
        LogMsg("Called: IVRTrackedCamera::ReleaseVideoStreamingService\n");
        return vr::VRTrackedCameraError_OperationFailed;
    }
    virtual vr::EVRTrackedCameraError GetVideoStreamFrameBuffer(vr::TrackedCameraHandle_t hTrackedCamera, vr::EVRTrackedCameraFrameType eFrameType, void *pFrameBuffer, uint32_t nFrameBufferSize, vr::CameraVideoStreamFrameHeader_t *pFrameHeader, uint32_t nFrameHeaderSize) override {
        LogMsg("Called: IVRTrackedCamera::GetVideoStreamFrameBuffer\n");
        return vr::VRTrackedCameraError_OperationFailed;
    }
    virtual vr::EVRTrackedCameraError GetVideoStreamTextureSize(vr::TrackedDeviceIndex_t nDeviceIndex, vr::EVRTrackedCameraFrameType eFrameType, vr::VRTextureBounds_t *pTextureBounds, uint32_t *pnWidth, uint32_t *pnHeight) override {
        LogMsg("Called: IVRTrackedCamera::GetVideoStreamTextureSize\n");
        return vr::VRTrackedCameraError_OperationFailed;
    }
    virtual vr::EVRTrackedCameraError GetVideoStreamTextureD3D11(vr::TrackedCameraHandle_t hTrackedCamera, vr::EVRTrackedCameraFrameType eFrameType, void *pD3D11DeviceOrResource, void **ppD3D11ShaderResourceView, vr::CameraVideoStreamFrameHeader_t *pFrameHeader, uint32_t nFrameHeaderSize) override {
        LogMsg("Called: IVRTrackedCamera::GetVideoStreamTextureD3D11\n");
        return vr::VRTrackedCameraError_OperationFailed;
    }
    virtual vr::EVRTrackedCameraError GetVideoStreamTextureGL(vr::TrackedCameraHandle_t hTrackedCamera, vr::EVRTrackedCameraFrameType eFrameType, vr::glUInt_t *pglTextureId, vr::CameraVideoStreamFrameHeader_t *pFrameHeader, uint32_t nFrameHeaderSize) override {
        LogMsg("Called: IVRTrackedCamera::GetVideoStreamTextureGL\n");
        return vr::VRTrackedCameraError_OperationFailed;
    }
    virtual vr::EVRTrackedCameraError ReleaseVideoStreamTextureGL(vr::TrackedCameraHandle_t hTrackedCamera, vr::glUInt_t glTextureId) override {
        LogMsg("Called: IVRTrackedCamera::ReleaseVideoStreamTextureGL\n");
        return vr::VRTrackedCameraError_OperationFailed;
    }
    virtual void SetCameraTrackingSpace(vr::ETrackingUniverseOrigin eUniverse) override {
        LogMsg("Called: IVRTrackedCamera::SetCameraTrackingSpace\n");
    }
    virtual vr::ETrackingUniverseOrigin GetCameraTrackingSpace() override {
        LogMsg("Called: IVRTrackedCamera::GetCameraTrackingSpace\n");
        return (vr::ETrackingUniverseOrigin)0;
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* DummyPadding42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* DummyPadding52() { return nullptr; }
    virtual void* DummyPadding53() { return nullptr; }
    virtual void* DummyPadding54() { return nullptr; }
    virtual void* DummyPadding55() { return nullptr; }
    virtual void* DummyPadding56() { return nullptr; }
    virtual void* DummyPadding57() { return nullptr; }
    virtual void* DummyPadding58() { return nullptr; }
    virtual void* DummyPadding59() { return nullptr; }
    virtual void* DummyPadding60() { return nullptr; }
    virtual void* DummyPadding61() { return nullptr; }
    virtual void* DummyPadding62() { return nullptr; }
    virtual void* DummyPadding63() { return nullptr; }
    virtual void* DummyPadding64() { return nullptr; }
    virtual void* DummyPadding65() { return nullptr; }
    virtual void* DummyPadding66() { return nullptr; }
    virtual void* DummyPadding67() { return nullptr; }
    virtual void* DummyPadding68() { return nullptr; }
    virtual void* DummyPadding69() { return nullptr; }
    virtual void* DummyPadding70() { return nullptr; }
    virtual void* DummyPadding71() { return nullptr; }
    virtual void* DummyPadding72() { return nullptr; }
    virtual void* DummyPadding73() { return nullptr; }
    virtual void* DummyPadding74() { return nullptr; }
    virtual void* DummyPadding75() { return nullptr; }
    virtual void* DummyPadding76() { return nullptr; }
    virtual void* DummyPadding77() { return nullptr; }
    virtual void* DummyPadding78() { return nullptr; }
    virtual void* DummyPadding79() { return nullptr; }
    virtual void* DummyPadding80() { return nullptr; }
    virtual void* DummyPadding81() { return nullptr; }
    virtual void* DummyPadding82() { return nullptr; }
    virtual void* DummyPadding83() { return nullptr; }
    virtual void* DummyPadding84() { return nullptr; }
    virtual void* DummyPadding85() { return nullptr; }
    virtual void* DummyPadding86() { return nullptr; }
    virtual void* DummyPadding87() { return nullptr; }
    virtual void* DummyPadding88() { return nullptr; }
    virtual void* DummyPadding89() { return nullptr; }
    virtual void* DummyPadding90() { return nullptr; }
    virtual void* DummyPadding91() { return nullptr; }
    virtual void* DummyPadding92() { return nullptr; }
    virtual void* DummyPadding93() { return nullptr; }
    virtual void* DummyPadding94() { return nullptr; }
    virtual void* DummyPadding95() { return nullptr; }
    virtual void* DummyPadding96() { return nullptr; }
    virtual void* DummyPadding97() { return nullptr; }
    virtual void* DummyPadding98() { return nullptr; }
    virtual void* DummyPadding99() { return nullptr; }
} g_mockTrackedCamera;

class Mock_IVRScreenshots : public vr::IVRScreenshots {
public:
    virtual vr::EVRScreenshotError RequestScreenshot(vr::ScreenshotHandle_t *pOutScreenshotHandle, vr::EVRScreenshotType type, const char *pchPreviewFilename, const char *pchVRFilename) override {
        LogMsg("Called: IVRScreenshots::RequestScreenshot\n");
        return (vr::EVRScreenshotError)1;
    }
    virtual vr::EVRScreenshotError HookScreenshot(VR_ARRAY_COUNT( numTypes ) const vr::EVRScreenshotType *pSupportedTypes, int numTypes) override {
        LogMsg("Called: IVRScreenshots::HookScreenshot\n");
        return (vr::EVRScreenshotError)1;
    }
    virtual vr::EVRScreenshotType GetScreenshotPropertyType(vr::ScreenshotHandle_t screenshotHandle, vr::EVRScreenshotError *pError) override {
        LogMsg("Called: IVRScreenshots::GetScreenshotPropertyType\n");
        return (vr::EVRScreenshotType)0;
    }
    virtual uint32_t GetScreenshotPropertyFilename(vr::ScreenshotHandle_t screenshotHandle, vr::EVRScreenshotPropertyFilenames filenameType, VR_OUT_STRING() char *pchFilename, uint32_t cchFilename, vr::EVRScreenshotError *pError) override {
        LogMsg("Called: IVRScreenshots::GetScreenshotPropertyFilename\n");
        return (uint32_t)0;
    }
    virtual vr::EVRScreenshotError UpdateScreenshotProgress(vr::ScreenshotHandle_t screenshotHandle, float flProgress) override {
        LogMsg("Called: IVRScreenshots::UpdateScreenshotProgress\n");
        return (vr::EVRScreenshotError)1;
    }
    virtual vr::EVRScreenshotError TakeStereoScreenshot(vr::ScreenshotHandle_t *pOutScreenshotHandle, const char *pchPreviewFilename, const char *pchVRFilename) override {
        LogMsg("Called: IVRScreenshots::TakeStereoScreenshot\n");
        return (vr::EVRScreenshotError)1;
    }
    virtual vr::EVRScreenshotError SubmitScreenshot(vr::ScreenshotHandle_t screenshotHandle, vr::EVRScreenshotType type, const char *pchSourcePreviewFilename, const char *pchSourceVRFilename) override {
        LogMsg("Called: IVRScreenshots::SubmitScreenshot\n");
        return (vr::EVRScreenshotError)1;
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* DummyPadding42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* DummyPadding52() { return nullptr; }
    virtual void* DummyPadding53() { return nullptr; }
    virtual void* DummyPadding54() { return nullptr; }
    virtual void* DummyPadding55() { return nullptr; }
    virtual void* DummyPadding56() { return nullptr; }
    virtual void* DummyPadding57() { return nullptr; }
    virtual void* DummyPadding58() { return nullptr; }
    virtual void* DummyPadding59() { return nullptr; }
    virtual void* DummyPadding60() { return nullptr; }
    virtual void* DummyPadding61() { return nullptr; }
    virtual void* DummyPadding62() { return nullptr; }
    virtual void* DummyPadding63() { return nullptr; }
    virtual void* DummyPadding64() { return nullptr; }
    virtual void* DummyPadding65() { return nullptr; }
    virtual void* DummyPadding66() { return nullptr; }
    virtual void* DummyPadding67() { return nullptr; }
    virtual void* DummyPadding68() { return nullptr; }
    virtual void* DummyPadding69() { return nullptr; }
    virtual void* DummyPadding70() { return nullptr; }
    virtual void* DummyPadding71() { return nullptr; }
    virtual void* DummyPadding72() { return nullptr; }
    virtual void* DummyPadding73() { return nullptr; }
    virtual void* DummyPadding74() { return nullptr; }
    virtual void* DummyPadding75() { return nullptr; }
    virtual void* DummyPadding76() { return nullptr; }
    virtual void* DummyPadding77() { return nullptr; }
    virtual void* DummyPadding78() { return nullptr; }
    virtual void* DummyPadding79() { return nullptr; }
    virtual void* DummyPadding80() { return nullptr; }
    virtual void* DummyPadding81() { return nullptr; }
    virtual void* DummyPadding82() { return nullptr; }
    virtual void* DummyPadding83() { return nullptr; }
    virtual void* DummyPadding84() { return nullptr; }
    virtual void* DummyPadding85() { return nullptr; }
    virtual void* DummyPadding86() { return nullptr; }
    virtual void* DummyPadding87() { return nullptr; }
    virtual void* DummyPadding88() { return nullptr; }
    virtual void* DummyPadding89() { return nullptr; }
    virtual void* DummyPadding90() { return nullptr; }
    virtual void* DummyPadding91() { return nullptr; }
    virtual void* DummyPadding92() { return nullptr; }
    virtual void* DummyPadding93() { return nullptr; }
    virtual void* DummyPadding94() { return nullptr; }
    virtual void* DummyPadding95() { return nullptr; }
    virtual void* DummyPadding96() { return nullptr; }
    virtual void* DummyPadding97() { return nullptr; }
    virtual void* DummyPadding98() { return nullptr; }
    virtual void* DummyPadding99() { return nullptr; }
} g_mockScreenshots;

class Mock_IVRResources : public vr::IVRResources {
public:
    virtual uint32_t LoadSharedResource(const char *pchResourceName, char *pchBuffer, uint32_t unBufferLen) override {
        LogMsg("Called: IVRResources::LoadSharedResource\n");
        return (uint32_t)0;
    }
    virtual uint32_t GetResourceFullPath(const char *pchResourceName, const char *pchResourceTypeDirectory, VR_OUT_STRING() char *pchPathBuffer, uint32_t unBufferLen) override {
        char msg[512];
        snprintf(msg, sizeof(msg), "Called: IVRResources::GetResourceFullPath: %s\n", pchResourceName ? pchResourceName : "null");
        LogMsg(msg);
        
        if (!pchResourceName) return 0;
        
        std::string resName(pchResourceName);
        std::string fullPath = "";
        
        std::string baseIndex = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\drivers\\indexcontroller\\resources";
        std::string baseSteamVR = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\resources";
        
        if (resName.find("{indexcontroller}") == 0) {
            fullPath = baseIndex + resName.substr(17);
        } else if (resName.find("{steamvr}") == 0) {
            fullPath = baseSteamVR + resName.substr(9);
        } else {
            fullPath = resName;
        }
        
        for (int i = 0; i < fullPath.length(); ++i) {
            if (fullPath[i] == '/') fullPath[i] = '\\';
        }
        
        if (pchPathBuffer && unBufferLen > fullPath.length()) {
            strcpy(pchPathBuffer, fullPath.c_str());
            return fullPath.length() + 1;
        }
        return fullPath.length() + 1;
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* DummyPadding42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* DummyPadding52() { return nullptr; }
    virtual void* DummyPadding53() { return nullptr; }
    virtual void* DummyPadding54() { return nullptr; }
    virtual void* DummyPadding55() { return nullptr; }
    virtual void* DummyPadding56() { return nullptr; }
    virtual void* DummyPadding57() { return nullptr; }
    virtual void* DummyPadding58() { return nullptr; }
    virtual void* DummyPadding59() { return nullptr; }
    virtual void* DummyPadding60() { return nullptr; }
    virtual void* DummyPadding61() { return nullptr; }
    virtual void* DummyPadding62() { return nullptr; }
    virtual void* DummyPadding63() { return nullptr; }
    virtual void* DummyPadding64() { return nullptr; }
    virtual void* DummyPadding65() { return nullptr; }
    virtual void* DummyPadding66() { return nullptr; }
    virtual void* DummyPadding67() { return nullptr; }
    virtual void* DummyPadding68() { return nullptr; }
    virtual void* DummyPadding69() { return nullptr; }
    virtual void* DummyPadding70() { return nullptr; }
    virtual void* DummyPadding71() { return nullptr; }
    virtual void* DummyPadding72() { return nullptr; }
    virtual void* DummyPadding73() { return nullptr; }
    virtual void* DummyPadding74() { return nullptr; }
    virtual void* DummyPadding75() { return nullptr; }
    virtual void* DummyPadding76() { return nullptr; }
    virtual void* DummyPadding77() { return nullptr; }
    virtual void* DummyPadding78() { return nullptr; }
    virtual void* DummyPadding79() { return nullptr; }
    virtual void* DummyPadding80() { return nullptr; }
    virtual void* DummyPadding81() { return nullptr; }
    virtual void* DummyPadding82() { return nullptr; }
    virtual void* DummyPadding83() { return nullptr; }
    virtual void* DummyPadding84() { return nullptr; }
    virtual void* DummyPadding85() { return nullptr; }
    virtual void* DummyPadding86() { return nullptr; }
    virtual void* DummyPadding87() { return nullptr; }
    virtual void* DummyPadding88() { return nullptr; }
    virtual void* DummyPadding89() { return nullptr; }
    virtual void* DummyPadding90() { return nullptr; }
    virtual void* DummyPadding91() { return nullptr; }
    virtual void* DummyPadding92() { return nullptr; }
    virtual void* DummyPadding93() { return nullptr; }
    virtual void* DummyPadding94() { return nullptr; }
    virtual void* DummyPadding95() { return nullptr; }
    virtual void* DummyPadding96() { return nullptr; }
    virtual void* DummyPadding97() { return nullptr; }
    virtual void* DummyPadding98() { return nullptr; }
    virtual void* DummyPadding99() { return nullptr; }
} g_mockResources;

class Mock_IVRDriverManager : public vr::IVRDriverManager {
public:
    virtual uint32_t GetDriverCount() const override {
        LogMsg("Called: IVRDriverManager::GetDriverCount\n");
        return (uint32_t)0;
    }
    virtual uint32_t GetDriverName(vr::DriverId_t nDriver, VR_OUT_STRING() char *pchValue, uint32_t unBufferSize) override {
        LogMsg("Called: IVRDriverManager::GetDriverName\n");
        return (uint32_t)0;
    }
    virtual DriverHandle_t GetDriverHandle(const char *pchDriverName) override {
        LogMsg("Called: IVRDriverManager::GetDriverHandle\n");
        return (DriverHandle_t)0;
    }
    virtual bool IsEnabled(vr::DriverId_t nDriver) const override {
        LogMsg("Called: IVRDriverManager::IsEnabled\n");
        return false;
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* DummyPadding42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* DummyPadding52() { return nullptr; }
    virtual void* DummyPadding53() { return nullptr; }
    virtual void* DummyPadding54() { return nullptr; }
    virtual void* DummyPadding55() { return nullptr; }
    virtual void* DummyPadding56() { return nullptr; }
    virtual void* DummyPadding57() { return nullptr; }
    virtual void* DummyPadding58() { return nullptr; }
    virtual void* DummyPadding59() { return nullptr; }
    virtual void* DummyPadding60() { return nullptr; }
    virtual void* DummyPadding61() { return nullptr; }
    virtual void* DummyPadding62() { return nullptr; }
    virtual void* DummyPadding63() { return nullptr; }
    virtual void* DummyPadding64() { return nullptr; }
    virtual void* DummyPadding65() { return nullptr; }
    virtual void* DummyPadding66() { return nullptr; }
    virtual void* DummyPadding67() { return nullptr; }
    virtual void* DummyPadding68() { return nullptr; }
    virtual void* DummyPadding69() { return nullptr; }
    virtual void* DummyPadding70() { return nullptr; }
    virtual void* DummyPadding71() { return nullptr; }
    virtual void* DummyPadding72() { return nullptr; }
    virtual void* DummyPadding73() { return nullptr; }
    virtual void* DummyPadding74() { return nullptr; }
    virtual void* DummyPadding75() { return nullptr; }
    virtual void* DummyPadding76() { return nullptr; }
    virtual void* DummyPadding77() { return nullptr; }
    virtual void* DummyPadding78() { return nullptr; }
    virtual void* DummyPadding79() { return nullptr; }
    virtual void* DummyPadding80() { return nullptr; }
    virtual void* DummyPadding81() { return nullptr; }
    virtual void* DummyPadding82() { return nullptr; }
    virtual void* DummyPadding83() { return nullptr; }
    virtual void* DummyPadding84() { return nullptr; }
    virtual void* DummyPadding85() { return nullptr; }
    virtual void* DummyPadding86() { return nullptr; }
    virtual void* DummyPadding87() { return nullptr; }
    virtual void* DummyPadding88() { return nullptr; }
    virtual void* DummyPadding89() { return nullptr; }
    virtual void* DummyPadding90() { return nullptr; }
    virtual void* DummyPadding91() { return nullptr; }
    virtual void* DummyPadding92() { return nullptr; }
    virtual void* DummyPadding93() { return nullptr; }
    virtual void* DummyPadding94() { return nullptr; }
    virtual void* DummyPadding95() { return nullptr; }
    virtual void* DummyPadding96() { return nullptr; }
    virtual void* DummyPadding97() { return nullptr; }
    virtual void* DummyPadding98() { return nullptr; }
    virtual void* DummyPadding99() { return nullptr; }
} g_mockDriverManager;

static std::map<vr::VRActionHandle_t, std::string> g_actionNames;

class Mock_IVRInput : public vr::IVRInput {
public:
    virtual EVRInputError SetActionManifestPath(const char *pchActionManifestPath) override {
        LogMsg("Called: IVRInput::SetActionManifestPath\n");
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetActionSetHandle(const char *pchActionSetName, VRActionSetHandle_t *pHandle) override {
        LogMsg("Called: IVRInput::GetActionSetHandle\n");
        static uint64_t nextSetHandle = 100;
        if(pHandle) *pHandle = nextSetHandle++;
        return vr::VRInputError_None;
    }
    
    virtual EVRInputError GetActionHandle(const char *pchActionName, VRActionHandle_t *pHandle) override {
        static uint64_t nextHandle = 10;
        if(pHandle) { 
            *pHandle = nextHandle++; 
            if (pchActionName) g_actionNames[*pHandle] = pchActionName;
            char buf[256];
            snprintf(buf, sizeof(buf), "GetActionHandle: %s -> %llu\n", pchActionName, *pHandle);
            LogMsg(buf);
        }
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetInputSourceHandle(const char *pchInputSourcePath, VRInputValueHandle_t *pHandle) override {
        LogMsg("Called: IVRInput::GetInputSourceHandle\n");
        static uint64_t nextSourceHandle = 10000;
        if(pHandle) { *pHandle = nextSourceHandle++; if(pchInputSourcePath) { if(strstr(pchInputSourcePath, "left")) *pHandle = 1; else if(strstr(pchInputSourcePath, "right")) *pHandle = 2; } }
        return vr::VRInputError_None;
    }
    virtual EVRInputError UpdateActionState(VR_ARRAY_COUNT( unSetCount ) VRActiveActionSet_t *pSets, uint32_t unSizeOfVRSelectedActionSet_t, uint32_t unSetCount) override {
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetDigitalActionData(VRActionHandle_t action, InputDigitalActionData_t *pActionData, uint32_t unActionDataSize, VRInputValueHandle_t ulRestrictToDevice) override {
        if(pActionData && unActionDataSize > 0) {
            vr::InputDigitalActionData_t temp = {0};
            SharedHands handsSnapshot = {};
            const bool haveHands = ReadSharedHandsSnapshot(&handsSnapshot);
            if (haveHands) {
                bool pressed = false;
                uint64_t origin = ulRestrictToDevice;
                if (origin == 0) origin = 2;
                const bool tracked = origin == 1 ? handsSnapshot.leftTracked != 0 : handsSnapshot.rightTracked != 0;
                temp.bActive = tracked;
                
                std::string name = g_actionNames[action];
                
                static bool leftGripToggle = false;
                static bool rightGripToggle = false;
                static uint32_t lastLeftBtns = 0;
                static uint32_t lastRightBtns = 0;
                
                uint32_t lb = handsSnapshot.leftButtons;
                uint32_t rb = handsSnapshot.rightButtons;
                
                if ((lb & (1<<7)) && !(lastLeftBtns & (1<<7))) leftGripToggle = !leftGripToggle;
                if ((lb & (1<<8)) && !(lastLeftBtns & (1<<8))) leftGripToggle = !leftGripToggle;
                if ((rb & (1<<5)) && !(lastRightBtns & (1<<5))) rightGripToggle = !rightGripToggle;
                if ((rb & (1<<6)) && !(lastRightBtns & (1<<6))) rightGripToggle = !rightGripToggle;
                
                lastLeftBtns = lb;
                lastRightBtns = rb;
                
                if (name.find("Interact") != std::string::npos || name.find("Use") != std::string::npos) {
                    if (origin == 1) pressed = (tracked && handsSnapshot.leftTrigger >= 128) || leftGripToggle || ((lb & (1<<4)) != 0) || ((lb & (1<<0)) != 0);
                    else pressed = (tracked && handsSnapshot.rightTrigger >= 128) || rightGripToggle || ((rb & (1<<2)) != 0) || ((rb & (1<<0)) != 0);
                }
                else if (name.find("GrabGrip") != std::string::npos || 
                    name.find("Grip") != std::string::npos ||
                    name.find("Grab") != std::string::npos) {
                    if (origin == 1) pressed = (tracked && handsSnapshot.leftGrip >= 150) || leftGripToggle;
                    else pressed = (tracked && handsSnapshot.rightGrip >= 150) || rightGripToggle;
                }
                else if (name.find("GrabPinch") != std::string::npos ||
                    name.find("Pinch") != std::string::npos ||
                    name.find("Trigger") != std::string::npos || 
                    name.find("Fire") != std::string::npos) {
                    if (origin == 1) pressed = (tracked && handsSnapshot.leftTrigger >= 128) || ((lb & (1<<4)) != 0);
                    else pressed = (tracked && handsSnapshot.rightTrigger >= 128) || ((rb & (1<<2)) != 0);
                }
                else if (name.find("Teleport") != std::string::npos) {
                    if (origin == 2) pressed = (rb & (1<<3)) != 0;
                    else pressed = (lb & (1<<5)) != 0;
                }
                else if (name.find("a_button") != std::string::npos || name.find("AButton") != std::string::npos) {
                    if (origin == 2) pressed = (rb & (1<<0)) != 0;
                }
                else if (name.find("b_button") != std::string::npos || name.find("BButton") != std::string::npos) {
                    if (origin == 2) pressed = (rb & (1<<1)) != 0;
                }
                else if (name.find("x_button") != std::string::npos || name.find("XButton") != std::string::npos) {
                    if (origin == 1) pressed = tracked && (lb & (1<<8)) != 0;
                }
                else if (name.find("y_button") != std::string::npos || name.find("YButton") != std::string::npos) {
                    if (origin == 1) pressed = tracked && (lb & (1<<9)) != 0;
                }
                else if (name.find("Pause") != std::string::npos || name.find("System") != std::string::npos || name.find("Menu") != std::string::npos) {
                    if (origin == 2) pressed = (rb & (1<<4)) != 0;
                    else pressed = (lb & (1<<6)) != 0;
                }
                
                static std::map<std::pair<VRActionHandle_t, uint64_t>, bool> s_lastState;
                std::pair<VRActionHandle_t, uint64_t> key = {action, origin};
                temp.bState = pressed;
                temp.bChanged = (s_lastState[key] != pressed);
                temp.activeOrigin = origin;
                temp.fUpdateTime = (float)GetTickCount() / 1000.0f;
                s_lastState[key] = pressed;
                
                if (temp.bChanged || temp.bState) {
                    char msg[256];
                    snprintf(msg, 256, "Action '%s' (origin=%llu): bState=%d, bChanged=%d\n", name.c_str(), origin, temp.bState, temp.bChanged);
                    LogMsg(msg);
                }
            }
            memcpy(pActionData, &temp, unActionDataSize > sizeof(temp) ? sizeof(temp) : unActionDataSize);
        }
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetAnalogActionData(VRActionHandle_t action, InputAnalogActionData_t *pActionData, uint32_t unActionDataSize, VRInputValueHandle_t ulRestrictToDevice) override {
        if(pActionData && unActionDataSize > 0) {
            vr::InputAnalogActionData_t temp = {0};
            SharedHands handsSnapshot = {};
            const bool haveHands = ReadSharedHandsSnapshot(&handsSnapshot);
            if (haveHands) {
                float x = 0.0f;
                float y = 0.0f;
                uint64_t origin = ulRestrictToDevice;
                if (origin == 0) origin = 2;
                const bool tracked = origin == 1 ? handsSnapshot.leftTracked != 0 : handsSnapshot.rightTracked != 0;
                temp.bActive = tracked;

                std::string name = g_actionNames[action];
                uint32_t lb = handsSnapshot.leftButtons;
                uint32_t rb = handsSnapshot.rightButtons;

                if (name.find("Move") != std::string::npos || name.find("Trackpad") != std::string::npos || name.find("Thumbstick") != std::string::npos || name.find("Turn") != std::string::npos || name.find("Scroll") != std::string::npos) {
                    if (origin == 1) {
                        x = tracked ? handsSnapshot.leftStickX : 0.0f;
                        y = tracked ? -handsSnapshot.leftStickY : 0.0f;
                    } else {
                        x = tracked ? handsSnapshot.rightStickX : 0.0f;
                        y = tracked ? -handsSnapshot.rightStickY : 0.0f;
                    }
                }
                else if (name.find("Trigger") != std::string::npos || name.find("Pull") != std::string::npos || name.find("Pinch") != std::string::npos || name.find("Use") != std::string::npos) {
                    if (origin == 1) {
                        x = std::max(tracked ? handsSnapshot.leftTrigger / 255.0f : 0.0f, (lb & (1<<4)) ? 1.0f : 0.0f);
                    } else {
                        x = std::max(tracked ? handsSnapshot.rightTrigger / 255.0f : 0.0f, (rb & (1<<2)) ? 1.0f : 0.0f);
                    }
                }
                else if (name.find("Grip") != std::string::npos || name.find("GrabGrip") != std::string::npos) {
                    if (origin == 1) x = tracked ? handsSnapshot.leftGrip / 255.0f : 0.0f;
                    else x = tracked ? handsSnapshot.rightGrip / 255.0f : 0.0f;
                }
                
                static std::map<std::pair<VRActionHandle_t, uint64_t>, std::pair<float, float>> s_lastAnalog;
                std::pair<VRActionHandle_t, uint64_t> key = {action, origin};
                float lastX = s_lastAnalog[key].first;
                float lastY = s_lastAnalog[key].second;
                
                temp.x = x;
                temp.y = y;
                temp.deltaX = x - lastX;
                temp.deltaY = y - lastY;
                temp.activeOrigin = origin;
                temp.fUpdateTime = (float)GetTickCount() / 1000.0f;
                
                s_lastAnalog[key] = {x, y};
            }
            memcpy(pActionData, &temp, unActionDataSize > sizeof(temp) ? sizeof(temp) : unActionDataSize);
        }
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetPoseActionDataRelativeToNow(VRActionHandle_t action, ETrackingUniverseOrigin eOrigin, float fPredictedSecondsFromNow, InputPoseActionData_t *pActionData, uint32_t unActionDataSize, VRInputValueHandle_t ulRestrictToDevice) override {
        if(pActionData && unActionDataSize > 0) {
            vr::InputPoseActionData_t temp = {0};
            temp.pose.bDeviceIsConnected = true;
            temp.pose.eTrackingResult = vr::TrackingResult_Running_OutOfRange;
            uint64_t origin = ulRestrictToDevice;
            if (origin == 0) origin = 2;
            temp.activeOrigin = origin;
            SharedHands handsSnapshot = {};
            const bool haveHands = ReadSharedHandsSnapshot(&handsSnapshot);
            const bool tracked = haveHands && (origin == 1 ? handsSnapshot.leftTracked != 0 : handsSnapshot.rightTracked != 0);
            temp.bActive = tracked;
            temp.pose.bPoseIsValid = tracked;
            if (origin == 1 && tracked) {
                for(int r=0;r<3;r++) for(int c=0;c<4;c++) temp.pose.mDeviceToAbsoluteTracking.m[r][c] = handsSnapshot.leftTransform[c*4 + r];
                temp.pose.vVelocity.v[0] = g_handVelocityL[0]; temp.pose.vVelocity.v[1] = g_handVelocityL[1]; temp.pose.vVelocity.v[2] = g_handVelocityL[2];
            } else if (origin == 2 && tracked) {
                for(int r=0;r<3;r++) for(int c=0;c<4;c++) temp.pose.mDeviceToAbsoluteTracking.m[r][c] = handsSnapshot.rightTransform[c*4 + r];
                temp.pose.vVelocity.v[0] = g_handVelocityR[0]; temp.pose.vVelocity.v[1] = g_handVelocityR[1]; temp.pose.vVelocity.v[2] = g_handVelocityR[2];
            } else {
                temp.pose.mDeviceToAbsoluteTracking.m[0][0] = 1; temp.pose.mDeviceToAbsoluteTracking.m[1][1] = 1; temp.pose.mDeviceToAbsoluteTracking.m[2][2] = 1;
            }
            memcpy(pActionData, &temp, unActionDataSize > sizeof(temp) ? sizeof(temp) : unActionDataSize);
        }
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetPoseActionDataForNextFrame(VRActionHandle_t action, ETrackingUniverseOrigin eOrigin, InputPoseActionData_t *pActionData, uint32_t unActionDataSize, VRInputValueHandle_t ulRestrictToDevice) override {
        if(pActionData && unActionDataSize > 0) {
            vr::InputPoseActionData_t temp = {0};
            temp.pose.bDeviceIsConnected = true;
            temp.pose.eTrackingResult = vr::TrackingResult_Running_OutOfRange;
            uint64_t origin = ulRestrictToDevice;
            if (origin == 0) origin = 2;
            temp.activeOrigin = origin;
            SharedHands handsSnapshot = {};
            const bool haveHands = ReadSharedHandsSnapshot(&handsSnapshot);
            const bool tracked = haveHands && (origin == 1 ? handsSnapshot.leftTracked != 0 : handsSnapshot.rightTracked != 0);
            temp.bActive = tracked;
            temp.pose.bPoseIsValid = tracked;
            if (origin == 1 && tracked) {
                for(int r=0;r<3;r++) for(int c=0;c<4;c++) temp.pose.mDeviceToAbsoluteTracking.m[r][c] = handsSnapshot.leftTransform[c*4 + r];
                temp.pose.vVelocity.v[0] = g_handVelocityL[0]; temp.pose.vVelocity.v[1] = g_handVelocityL[1]; temp.pose.vVelocity.v[2] = g_handVelocityL[2];
            } else if (origin == 2 && tracked) {
                for(int r=0;r<3;r++) for(int c=0;c<4;c++) temp.pose.mDeviceToAbsoluteTracking.m[r][c] = handsSnapshot.rightTransform[c*4 + r];
                temp.pose.vVelocity.v[0] = g_handVelocityR[0]; temp.pose.vVelocity.v[1] = g_handVelocityR[1]; temp.pose.vVelocity.v[2] = g_handVelocityR[2];
            } else {
                temp.pose.mDeviceToAbsoluteTracking.m[0][0] = 1; temp.pose.mDeviceToAbsoluteTracking.m[1][1] = 1; temp.pose.mDeviceToAbsoluteTracking.m[2][2] = 1;
            }
            memcpy(pActionData, &temp, unActionDataSize > sizeof(temp) ? sizeof(temp) : unActionDataSize);
        }
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetSkeletalActionData(VRActionHandle_t action, InputSkeletalActionData_t *pActionData, uint32_t unActionDataSize) override {
        if (pActionData && unActionDataSize >= sizeof(InputSkeletalActionData_t)) {
            pActionData->bActive = true;
            pActionData->activeOrigin = 2;
        }
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetDominantHand(ETrackedControllerRole *peDominantHand) override {
        if(peDominantHand) *peDominantHand = vr::TrackedControllerRole_RightHand;
        return vr::VRInputError_None;
    }
    virtual EVRInputError SetDominantHand(ETrackedControllerRole eDominantHand) override {
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetBoneCount(VRActionHandle_t action, uint32_t* pBoneCount) override {
        if(pBoneCount) *pBoneCount = 31;
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetBoneHierarchy(VRActionHandle_t action, BoneIndex_t* pParentIndices, uint32_t unIndexArayCount) override {
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetBoneName(VRActionHandle_t action, BoneIndex_t nBoneIndex, char* pchBoneName, uint32_t unNameBufferSize) override {
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetSkeletalReferenceTransforms(VRActionHandle_t action, EVRSkeletalTransformSpace eTransformSpace, EVRSkeletalReferencePose eReferencePose, VRBoneTransform_t *pTransformArray, uint32_t unTransformArrayCount) override {
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetSkeletalTrackingLevel(VRActionHandle_t action, EVRSkeletalTrackingLevel* pSkeletalTrackingLevel) override {
        if(pSkeletalTrackingLevel) *pSkeletalTrackingLevel = vr::VRSkeletalTracking_Partial;
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetSkeletalBoneData(VRActionHandle_t action, EVRSkeletalTransformSpace eTransformSpace, EVRSkeletalMotionRange eMotionRange, VRBoneTransform_t *pTransformArray, uint32_t unTransformArrayCount) override {
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetSkeletalSummaryData(VRActionHandle_t action, EVRSummaryType eSummaryType, VRSkeletalSummaryData_t * pSkeletalSummaryData) override {
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetSkeletalBoneDataCompressed(VRActionHandle_t action, EVRSkeletalMotionRange eMotionRange, void *pvCompressedData, uint32_t unCompressedSize, uint32_t *punRequiredCompressedSize) override {
        return vr::VRInputError_None;
    }
    virtual EVRInputError DecompressSkeletalBoneData(const void *pvCompressedBuffer, uint32_t unCompressedBufferSize, EVRSkeletalTransformSpace eTransformSpace, VRBoneTransform_t *pTransformArray, uint32_t unTransformArrayCount) override {
        return vr::VRInputError_None;
    }
    virtual EVRInputError TriggerHapticVibrationAction(VRActionHandle_t action, float fStartSecondsFromNow, float fDurationSeconds, float fFrequency, float fAmplitude, VRInputValueHandle_t ulRestrictToDevice) override {
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetActionOrigins(VRActionSetHandle_t actionSetHandle, VRActionHandle_t digitalActionHandle, VRInputValueHandle_t *originsOut, uint32_t originOutCount) override {
        if (originsOut && originOutCount > 0) {
            memset(originsOut, 0, sizeof(vr::VRInputValueHandle_t) * originOutCount);
            if (originOutCount >= 2) {
                originsOut[0] = 1; // Left Hand
                originsOut[1] = 2; // Right Hand
            } else {
                originsOut[0] = 1;
            }
        }
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetOriginLocalizedName(VRInputValueHandle_t origin, char *pchNameArray, uint32_t unNameArraySize, int32_t unStringSectionsToInclude) override {
        if(pchNameArray && unNameArraySize > 0) {
            strncpy(pchNameArray, "Trigger", unNameArraySize - 1);
            pchNameArray[unNameArraySize - 1] = '\0';
        }
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetOriginTrackedDeviceInfo(VRInputValueHandle_t origin, InputOriginInfo_t *pOriginInfo, uint32_t unOriginInfoSize) override {
        if(pOriginInfo && unOriginInfoSize > 0) {
            memset(pOriginInfo, 0, unOriginInfoSize);
            if (unOriginInfoSize >= sizeof(vr::VRInputValueHandle_t)) {
                pOriginInfo->devicePath = origin;
            }
            if (unOriginInfoSize >= offsetof(InputOriginInfo_t, trackedDeviceIndex) + sizeof(vr::TrackedDeviceIndex_t)) {
                pOriginInfo->trackedDeviceIndex = origin;
            }
        }
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetActionBindingInfo(VRActionHandle_t action, InputBindingInfo_t *pOriginInfo, uint32_t unBindingInfoSize, uint32_t unBindingInfoCount, uint32_t *punReturnedBindingInfoCount) override {
        if (pOriginInfo && unBindingInfoCount > 0 && unBindingInfoSize > 0) {
            memset(pOriginInfo, 0, unBindingInfoSize * unBindingInfoCount);
            
            // Write only within the allowed bounds
            auto safe_strcpy = [unBindingInfoSize](char* dst, const char* src, size_t offset, size_t max_len) {
                if (offset < unBindingInfoSize) {
                    size_t available = unBindingInfoSize - offset;
                    size_t copy_len = (max_len < available) ? max_len : available;
                    strncpy(dst, src, copy_len - 1);
                    dst[copy_len - 1] = '\0';
                }
            };
            
            // Fallback to simple copying if we know the struct matches our definition
            if (unBindingInfoSize >= sizeof(InputBindingInfo_t)) {
                strncpy(pOriginInfo[0].rchDevicePathName, "/user/hand/right", 127);
                strncpy(pOriginInfo[0].rchInputPathName, "/user/hand/right/input/trigger", 127);
                strncpy(pOriginInfo[0].rchModeName, "trigger", 127);
                strncpy(pOriginInfo[0].rchSlotName, "click", 127);
                strncpy(pOriginInfo[0].rchInputSourceType, "trackpad", 31);
            }
        }
        if (punReturnedBindingInfoCount) *punReturnedBindingInfoCount = 1;
        return vr::VRInputError_None;
    }
    virtual EVRInputError ShowActionOrigins(VRActionSetHandle_t actionSetHandle, VRActionHandle_t ulActionHandle) override {
        return vr::VRInputError_None;
    }
    virtual EVRInputError ShowBindingsForActionSet(VR_ARRAY_COUNT( unSetCount ) VRActiveActionSet_t *pSets, uint32_t unSizeOfVRSelectedActionSet_t, uint32_t unSetCount, VRInputValueHandle_t originToHighlight) override {
        return vr::VRInputError_None;
    }
    virtual EVRInputError GetComponentStateForBinding(const char *pchRenderModelName, const char *pchComponentName, const InputBindingInfo_t *pOriginInfo, uint32_t unBindingInfoSize, uint32_t unBindingInfoCount, vr::RenderModel_ComponentState_t *pComponentState) override {
        return vr::VRInputError_None;
    }
    virtual bool IsUsingLegacyInput() override {
        return false;
    }
    virtual EVRInputError OpenBindingUI(const char* pchAppKey, VRActionSetHandle_t ulActionSetHandle, VRInputValueHandle_t ulDeviceHandle, bool bShowOnDesktop) override {
        LogMsg("Called: IVRInput::OpenBindingUI\n");
        return vr::VRInputError_InvalidHandle;
    }
    virtual EVRInputError GetBindingVariant(vr::VRInputValueHandle_t ulDevicePath, VR_OUT_STRING() char *pchVariantArray, uint32_t unVariantArraySize) override {
        LogMsg("Called: IVRInput::GetBindingVariant\n");
        return vr::VRInputError_InvalidHandle;
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* DummyPadding42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* DummyPadding52() { return nullptr; }
    virtual void* DummyPadding53() { return nullptr; }
    virtual void* DummyPadding54() { return nullptr; }
    virtual void* DummyPadding55() { return nullptr; }
    virtual void* DummyPadding56() { return nullptr; }
    virtual void* DummyPadding57() { return nullptr; }
    virtual void* DummyPadding58() { return nullptr; }
    virtual void* DummyPadding59() { return nullptr; }
    virtual void* DummyPadding60() { return nullptr; }
    virtual void* DummyPadding61() { return nullptr; }
    virtual void* DummyPadding62() { return nullptr; }
    virtual void* DummyPadding63() { return nullptr; }
    virtual void* DummyPadding64() { return nullptr; }
    virtual void* DummyPadding65() { return nullptr; }
    virtual void* DummyPadding66() { return nullptr; }
    virtual void* DummyPadding67() { return nullptr; }
    virtual void* DummyPadding68() { return nullptr; }
    virtual void* DummyPadding69() { return nullptr; }
    virtual void* DummyPadding70() { return nullptr; }
    virtual void* DummyPadding71() { return nullptr; }
    virtual void* DummyPadding72() { return nullptr; }
    virtual void* DummyPadding73() { return nullptr; }
    virtual void* DummyPadding74() { return nullptr; }
    virtual void* DummyPadding75() { return nullptr; }
    virtual void* DummyPadding76() { return nullptr; }
    virtual void* DummyPadding77() { return nullptr; }
    virtual void* DummyPadding78() { return nullptr; }
    virtual void* DummyPadding79() { return nullptr; }
    virtual void* DummyPadding80() { return nullptr; }
    virtual void* DummyPadding81() { return nullptr; }
    virtual void* DummyPadding82() { return nullptr; }
    virtual void* DummyPadding83() { return nullptr; }
    virtual void* DummyPadding84() { return nullptr; }
    virtual void* DummyPadding85() { return nullptr; }
    virtual void* DummyPadding86() { return nullptr; }
    virtual void* DummyPadding87() { return nullptr; }
    virtual void* DummyPadding88() { return nullptr; }
    virtual void* DummyPadding89() { return nullptr; }
    virtual void* DummyPadding90() { return nullptr; }
    virtual void* DummyPadding91() { return nullptr; }
    virtual void* DummyPadding92() { return nullptr; }
    virtual void* DummyPadding93() { return nullptr; }
    virtual void* DummyPadding94() { return nullptr; }
    virtual void* DummyPadding95() { return nullptr; }
    virtual void* DummyPadding96() { return nullptr; }
    virtual void* DummyPadding97() { return nullptr; }
    virtual void* DummyPadding98() { return nullptr; }
    virtual void* DummyPadding99() { return nullptr; }
} g_mockInput;

class Mock_IVRIOBuffer : public vr::IVRIOBuffer {
public:
    virtual vr::EIOBufferError Open(const char *pchPath, vr::EIOBufferMode mode, uint32_t unElementSize, uint32_t unElements, vr::IOBufferHandle_t *pulBuffer) override {
        LogMsg("Called: IVRIOBuffer::Open\n");
        return (vr::EIOBufferError)1;
    }
    virtual vr::EIOBufferError Close(vr::IOBufferHandle_t ulBuffer) override {
        LogMsg("Called: IVRIOBuffer::Close\n");
        return (vr::EIOBufferError)1;
    }
    virtual vr::EIOBufferError Read(vr::IOBufferHandle_t ulBuffer, void *pDst, uint32_t unBytes, uint32_t *punRead) override {
        LogMsg("Called: IVRIOBuffer::Read\n");
        return (vr::EIOBufferError)1;
    }
    virtual vr::EIOBufferError Write(vr::IOBufferHandle_t ulBuffer, void *pSrc, uint32_t unBytes) override {
        LogMsg("Called: IVRIOBuffer::Write\n");
        return (vr::EIOBufferError)1;
    }
    virtual vr::PropertyContainerHandle_t PropertyContainer(vr::IOBufferHandle_t ulBuffer) override {
        LogMsg("Called: IVRIOBuffer::PropertyContainer\n");
        return (vr::PropertyContainerHandle_t)0;
    }
    virtual bool HasReaders(vr::IOBufferHandle_t ulBuffer) override {
        LogMsg("Called: IVRIOBuffer::HasReaders\n");
        return false;
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* DummyPadding42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* DummyPadding52() { return nullptr; }
    virtual void* DummyPadding53() { return nullptr; }
    virtual void* DummyPadding54() { return nullptr; }
    virtual void* DummyPadding55() { return nullptr; }
    virtual void* DummyPadding56() { return nullptr; }
    virtual void* DummyPadding57() { return nullptr; }
    virtual void* DummyPadding58() { return nullptr; }
    virtual void* DummyPadding59() { return nullptr; }
    virtual void* DummyPadding60() { return nullptr; }
    virtual void* DummyPadding61() { return nullptr; }
    virtual void* DummyPadding62() { return nullptr; }
    virtual void* DummyPadding63() { return nullptr; }
    virtual void* DummyPadding64() { return nullptr; }
    virtual void* DummyPadding65() { return nullptr; }
    virtual void* DummyPadding66() { return nullptr; }
    virtual void* DummyPadding67() { return nullptr; }
    virtual void* DummyPadding68() { return nullptr; }
    virtual void* DummyPadding69() { return nullptr; }
    virtual void* DummyPadding70() { return nullptr; }
    virtual void* DummyPadding71() { return nullptr; }
    virtual void* DummyPadding72() { return nullptr; }
    virtual void* DummyPadding73() { return nullptr; }
    virtual void* DummyPadding74() { return nullptr; }
    virtual void* DummyPadding75() { return nullptr; }
    virtual void* DummyPadding76() { return nullptr; }
    virtual void* DummyPadding77() { return nullptr; }
    virtual void* DummyPadding78() { return nullptr; }
    virtual void* DummyPadding79() { return nullptr; }
    virtual void* DummyPadding80() { return nullptr; }
    virtual void* DummyPadding81() { return nullptr; }
    virtual void* DummyPadding82() { return nullptr; }
    virtual void* DummyPadding83() { return nullptr; }
    virtual void* DummyPadding84() { return nullptr; }
    virtual void* DummyPadding85() { return nullptr; }
    virtual void* DummyPadding86() { return nullptr; }
    virtual void* DummyPadding87() { return nullptr; }
    virtual void* DummyPadding88() { return nullptr; }
    virtual void* DummyPadding89() { return nullptr; }
    virtual void* DummyPadding90() { return nullptr; }
    virtual void* DummyPadding91() { return nullptr; }
    virtual void* DummyPadding92() { return nullptr; }
    virtual void* DummyPadding93() { return nullptr; }
    virtual void* DummyPadding94() { return nullptr; }
    virtual void* DummyPadding95() { return nullptr; }
    virtual void* DummyPadding96() { return nullptr; }
    virtual void* DummyPadding97() { return nullptr; }
    virtual void* DummyPadding98() { return nullptr; }
    virtual void* DummyPadding99() { return nullptr; }
} g_mockIOBuffer;

class Mock_IVRSpatialAnchors : public vr::IVRSpatialAnchors {
public:
    virtual EVRSpatialAnchorError CreateSpatialAnchorFromDescriptor(const char *pchDescriptor, SpatialAnchorHandle_t *pHandleOut) override {
        LogMsg("Called: IVRSpatialAnchors::CreateSpatialAnchorFromDescriptor\n");
        return (EVRSpatialAnchorError)1;
    }
    virtual EVRSpatialAnchorError CreateSpatialAnchorFromPose(TrackedDeviceIndex_t unDeviceIndex, ETrackingUniverseOrigin eOrigin, SpatialAnchorPose_t *pPose, SpatialAnchorHandle_t *pHandleOut) override {
        LogMsg("Called: IVRSpatialAnchors::CreateSpatialAnchorFromPose\n");
        return (EVRSpatialAnchorError)1;
    }
    virtual EVRSpatialAnchorError GetSpatialAnchorPose(SpatialAnchorHandle_t unHandle, ETrackingUniverseOrigin eOrigin, SpatialAnchorPose_t *pPoseOut) override {
        LogMsg("Called: IVRSpatialAnchors::GetSpatialAnchorPose\n");
        return (EVRSpatialAnchorError)1;
    }
    virtual EVRSpatialAnchorError GetSpatialAnchorDescriptor(SpatialAnchorHandle_t unHandle, VR_OUT_STRING() char *pchDescriptorOut, uint32_t *punDescriptorBufferLenInOut) override {
        LogMsg("Called: IVRSpatialAnchors::GetSpatialAnchorDescriptor\n");
        return (EVRSpatialAnchorError)1;
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* DummyPadding42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* DummyPadding52() { return nullptr; }
    virtual void* DummyPadding53() { return nullptr; }
    virtual void* DummyPadding54() { return nullptr; }
    virtual void* DummyPadding55() { return nullptr; }
    virtual void* DummyPadding56() { return nullptr; }
    virtual void* DummyPadding57() { return nullptr; }
    virtual void* DummyPadding58() { return nullptr; }
    virtual void* DummyPadding59() { return nullptr; }
    virtual void* DummyPadding60() { return nullptr; }
    virtual void* DummyPadding61() { return nullptr; }
    virtual void* DummyPadding62() { return nullptr; }
    virtual void* DummyPadding63() { return nullptr; }
    virtual void* DummyPadding64() { return nullptr; }
    virtual void* DummyPadding65() { return nullptr; }
    virtual void* DummyPadding66() { return nullptr; }
    virtual void* DummyPadding67() { return nullptr; }
    virtual void* DummyPadding68() { return nullptr; }
    virtual void* DummyPadding69() { return nullptr; }
    virtual void* DummyPadding70() { return nullptr; }
    virtual void* DummyPadding71() { return nullptr; }
    virtual void* DummyPadding72() { return nullptr; }
    virtual void* DummyPadding73() { return nullptr; }
    virtual void* DummyPadding74() { return nullptr; }
    virtual void* DummyPadding75() { return nullptr; }
    virtual void* DummyPadding76() { return nullptr; }
    virtual void* DummyPadding77() { return nullptr; }
    virtual void* DummyPadding78() { return nullptr; }
    virtual void* DummyPadding79() { return nullptr; }
    virtual void* DummyPadding80() { return nullptr; }
    virtual void* DummyPadding81() { return nullptr; }
    virtual void* DummyPadding82() { return nullptr; }
    virtual void* DummyPadding83() { return nullptr; }
    virtual void* DummyPadding84() { return nullptr; }
    virtual void* DummyPadding85() { return nullptr; }
    virtual void* DummyPadding86() { return nullptr; }
    virtual void* DummyPadding87() { return nullptr; }
    virtual void* DummyPadding88() { return nullptr; }
    virtual void* DummyPadding89() { return nullptr; }
    virtual void* DummyPadding90() { return nullptr; }
    virtual void* DummyPadding91() { return nullptr; }
    virtual void* DummyPadding92() { return nullptr; }
    virtual void* DummyPadding93() { return nullptr; }
    virtual void* DummyPadding94() { return nullptr; }
    virtual void* DummyPadding95() { return nullptr; }
    virtual void* DummyPadding96() { return nullptr; }
    virtual void* DummyPadding97() { return nullptr; }
    virtual void* DummyPadding98() { return nullptr; }
    virtual void* DummyPadding99() { return nullptr; }
} g_mockSpatialAnchors;

class Mock_IVRDebug : public vr::IVRDebug {
public:
    virtual EVRDebugError EmitVrProfilerEvent(const char *pchMessage) override {
        LogMsg("Called: IVRDebug::EmitVrProfilerEvent\n");
        return (EVRDebugError)1;
    }
    virtual EVRDebugError BeginVrProfilerEvent(VrProfilerEventHandle_t *pHandleOut) override {
        LogMsg("Called: IVRDebug::BeginVrProfilerEvent\n");
        return (EVRDebugError)1;
    }
    virtual EVRDebugError FinishVrProfilerEvent(VrProfilerEventHandle_t hHandle, const char *pchMessage) override {
        LogMsg("Called: IVRDebug::FinishVrProfilerEvent\n");
        return (EVRDebugError)1;
    }
    virtual uint32_t DriverDebugRequest(vr::TrackedDeviceIndex_t unDeviceIndex, const char *pchRequest, VR_OUT_STRING() char *pchResponseBuffer, uint32_t unResponseBufferSize) override {
        LogMsg("Called: IVRDebug::DriverDebugRequest\n");
        return (uint32_t)0;
    }
    virtual void* DummyPadding0() { return nullptr; }
    virtual void* DummyPadding1() { return nullptr; }
    virtual void* DummyPadding2() { return nullptr; }
    virtual void* DummyPadding3() { return nullptr; }
    virtual void* DummyPadding4() { return nullptr; }
    virtual void* DummyPadding5() { return nullptr; }
    virtual void* DummyPadding6() { return nullptr; }
    virtual void* DummyPadding7() { return nullptr; }
    virtual void* DummyPadding8() { return nullptr; }
    virtual void* DummyPadding9() { return nullptr; }
    virtual void* DummyPadding10() { return nullptr; }
    virtual void* DummyPadding11() { return nullptr; }
    virtual void* DummyPadding12() { return nullptr; }
    virtual void* DummyPadding13() { return nullptr; }
    virtual void* DummyPadding14() { return nullptr; }
    virtual void* DummyPadding15() { return nullptr; }
    virtual void* DummyPadding16() { return nullptr; }
    virtual void* DummyPadding17() { return nullptr; }
    virtual void* DummyPadding18() { return nullptr; }
    virtual void* DummyPadding19() { return nullptr; }
    virtual void* DummyPadding20() { return nullptr; }
    virtual void* DummyPadding21() { return nullptr; }
    virtual void* DummyPadding22() { return nullptr; }
    virtual void* DummyPadding23() { return nullptr; }
    virtual void* DummyPadding24() { return nullptr; }
    virtual void* DummyPadding25() { return nullptr; }
    virtual void* DummyPadding26() { return nullptr; }
    virtual void* DummyPadding27() { return nullptr; }
    virtual void* DummyPadding28() { return nullptr; }
    virtual void* DummyPadding29() { return nullptr; }
    virtual void* DummyPadding30() { return nullptr; }
    virtual void* DummyPadding31() { return nullptr; }
    virtual void* DummyPadding32() { return nullptr; }
    virtual void* DummyPadding33() { return nullptr; }
    virtual void* DummyPadding34() { return nullptr; }
    virtual void* DummyPadding35() { return nullptr; }
    virtual void* DummyPadding36() { return nullptr; }
    virtual void* DummyPadding37() { return nullptr; }
    virtual void* DummyPadding38() { return nullptr; }
    virtual void* DummyPadding39() { return nullptr; }
    virtual void* DummyPadding40() { return nullptr; }
    virtual void* DummyPadding41() { return nullptr; }
    virtual void* DummyPadding42() { return nullptr; }
    virtual void* DummyPadding43() { return nullptr; }
    virtual void* DummyPadding44() { return nullptr; }
    virtual void* DummyPadding45() { return nullptr; }
    virtual void* DummyPadding46() { return nullptr; }
    virtual void* DummyPadding47() { return nullptr; }
    virtual void* DummyPadding48() { return nullptr; }
    virtual void* DummyPadding49() { return nullptr; }
    virtual void* DummyPadding50() { return nullptr; }
    virtual void* DummyPadding51() { return nullptr; }
    virtual void* DummyPadding52() { return nullptr; }
    virtual void* DummyPadding53() { return nullptr; }
    virtual void* DummyPadding54() { return nullptr; }
    virtual void* DummyPadding55() { return nullptr; }
    virtual void* DummyPadding56() { return nullptr; }
    virtual void* DummyPadding57() { return nullptr; }
    virtual void* DummyPadding58() { return nullptr; }
    virtual void* DummyPadding59() { return nullptr; }
    virtual void* DummyPadding60() { return nullptr; }
    virtual void* DummyPadding61() { return nullptr; }
    virtual void* DummyPadding62() { return nullptr; }
    virtual void* DummyPadding63() { return nullptr; }
    virtual void* DummyPadding64() { return nullptr; }
    virtual void* DummyPadding65() { return nullptr; }
    virtual void* DummyPadding66() { return nullptr; }
    virtual void* DummyPadding67() { return nullptr; }
    virtual void* DummyPadding68() { return nullptr; }
    virtual void* DummyPadding69() { return nullptr; }
    virtual void* DummyPadding70() { return nullptr; }
    virtual void* DummyPadding71() { return nullptr; }
    virtual void* DummyPadding72() { return nullptr; }
    virtual void* DummyPadding73() { return nullptr; }
    virtual void* DummyPadding74() { return nullptr; }
    virtual void* DummyPadding75() { return nullptr; }
    virtual void* DummyPadding76() { return nullptr; }
    virtual void* DummyPadding77() { return nullptr; }
    virtual void* DummyPadding78() { return nullptr; }
    virtual void* DummyPadding79() { return nullptr; }
    virtual void* DummyPadding80() { return nullptr; }
    virtual void* DummyPadding81() { return nullptr; }
    virtual void* DummyPadding82() { return nullptr; }
    virtual void* DummyPadding83() { return nullptr; }
    virtual void* DummyPadding84() { return nullptr; }
    virtual void* DummyPadding85() { return nullptr; }
    virtual void* DummyPadding86() { return nullptr; }
    virtual void* DummyPadding87() { return nullptr; }
    virtual void* DummyPadding88() { return nullptr; }
    virtual void* DummyPadding89() { return nullptr; }
    virtual void* DummyPadding90() { return nullptr; }
    virtual void* DummyPadding91() { return nullptr; }
    virtual void* DummyPadding92() { return nullptr; }
    virtual void* DummyPadding93() { return nullptr; }
    virtual void* DummyPadding94() { return nullptr; }
    virtual void* DummyPadding95() { return nullptr; }
    virtual void* DummyPadding96() { return nullptr; }
    virtual void* DummyPadding97() { return nullptr; }
    virtual void* DummyPadding98() { return nullptr; }
    virtual void* DummyPadding99() { return nullptr; }
} g_mockDebug;


extern "C" __declspec(dllexport) void* VR_GetGenericInterface(const char *pchInterfaceVersion, vr::EVRInitError *peError) {
    if (peError) *peError = vr::VRInitError_None;
    {
    char _buf[256];
    snprintf(_buf, sizeof(_buf), "Requested interface: %s\n", pchInterfaceVersion);
    LogMsg(_buf);
}

    if (strstr(pchInterfaceVersion, "IVRMailbox")) return &g_universalMock;

    if (strstr(pchInterfaceVersion, "System")) return &g_mockSystem;
    if (strstr(pchInterfaceVersion, "Applications")) return &g_mockApplications;
    if (strstr(pchInterfaceVersion, "Settings")) return &g_mockSettings;
    if (strstr(pchInterfaceVersion, "Chaperone")) return &g_mockChaperone;
    if (strstr(pchInterfaceVersion, "ChaperoneSetup")) return &g_mockChaperoneSetup;
    if (strstr(pchInterfaceVersion, "Compositor")) return &g_mockCompositor;
    if (strstr(pchInterfaceVersion, "HeadsetView")) return &g_mockHeadsetView;
    if (strstr(pchInterfaceVersion, "Notifications")) return &g_mockNotifications;
    if (strstr(pchInterfaceVersion, "Overlay")) return &g_mockOverlay;
    if (strstr(pchInterfaceVersion, "OverlayView")) return &g_mockOverlayView;
    if (strstr(pchInterfaceVersion, "RenderModels")) return &g_mockRenderModels;
    if (strstr(pchInterfaceVersion, "ExtendedDisplay")) return &g_mockExtendedDisplay;
    if (strstr(pchInterfaceVersion, "TrackedCamera")) return &g_mockTrackedCamera;
    if (strstr(pchInterfaceVersion, "Screenshots")) return &g_mockScreenshots;
    if (strstr(pchInterfaceVersion, "Resources")) return &g_mockResources;
    if (strstr(pchInterfaceVersion, "DriverManager")) return &g_mockDriverManager;
    if (strstr(pchInterfaceVersion, "Input")) return &g_mockInput;
    if (strstr(pchInterfaceVersion, "IOBuffer")) return &g_mockIOBuffer;
    if (strstr(pchInterfaceVersion, "SpatialAnchors")) return &g_mockSpatialAnchors;
    if (strstr(pchInterfaceVersion, "Debug")) return &g_mockDebug;

    return nullptr;
}

void ApplyCOMHooks();

extern "C" __declspec(dllexport) uint32_t VR_InitInternal2(vr::EVRInitError *peError, vr::EVRApplicationType eType, const char *pStartupInfo) {
    if (peError) *peError = vr::VRInitError_None;
    LogMsg("Called: VR_InitInternal2\n");

    // Inject COM VTable hooks to capture rendering context safely
    ApplyCOMHooks();

    InitSharedMemory();
    return 1;
}

extern "C" __declspec(dllexport) void VR_ShutdownInternal() {
}

extern "C" __declspec(dllexport) bool VR_IsHmdPresent() {
    return true;
}

extern "C" __declspec(dllexport) const char* VR_GetVRInitErrorAsSymbol(vr::EVRInitError error) {
    return "";
}

extern "C" __declspec(dllexport) const char* VR_GetVRInitErrorAsEnglishDescription(vr::EVRInitError error) {
    return "";
}

extern "C" __declspec(dllexport) uint32_t VR_GetInitToken() {
    return 1;
}

extern "C" __declspec(dllexport) void* VRControlPanel() { return nullptr; }
extern "C" __declspec(dllexport) bool VR_GetRuntimePath(char *pchPathBuffer, uint32_t unBufferSize, uint32_t *punRequiredBufferSize) { 
    if (punRequiredBufferSize) *punRequiredBufferSize = 1; 
    if (pchPathBuffer && unBufferSize > 0) pchPathBuffer[0] = '\0';
    return true; 
}
extern "C" __declspec(dllexport) const char* VR_GetStringForHmdError(vr::EVRInitError error) { return ""; }
extern "C" __declspec(dllexport) uint32_t VR_InitInternal(vr::EVRInitError *peError, vr::EVRApplicationType eType) {
    if (peError) *peError = vr::VRInitError_None;
    InitSharedMemory();
    return 1;
}
extern "C" __declspec(dllexport) bool VR_IsInterfaceVersionValid(const char *pchInterfaceVersion) { return true; }
extern "C" __declspec(dllexport) bool VR_IsRuntimeInstalled() { return true; }

#include "iat_hook.h"
#include <d3d11_4.h> // Include proper header for ID3D11Multithread


void BackgroundCopyThread() {
    while (true) {
        uint32_t processIndex = -1;
        {
            std::unique_lock<std::mutex> lock(g_copyMutex);
            g_copyCv.wait(lock, []{ return g_pendingCopyIndex != (uint32_t)-1; });
            processIndex = g_pendingCopyIndex;
            g_pendingCopyIndex = -1;
        }

        StagingFrame& readFrame = g_stagingFrames[processIndex];
        if (readFrame.pContext) {
            ID3D11Multithread* pMt = nullptr;
            readFrame.pContext->QueryInterface(__uuidof(ID3D11Multithread), (void**)&pMt);
            if (pMt) { pMt->SetMultithreadProtected(TRUE); pMt->Enter(); }

            uint32_t srcWidthL = (uint32_t)(readFrame.descWidth * (readFrame.uMaxL - readFrame.uMinL));
            uint32_t srcHeightL = (uint32_t)(readFrame.descHeight * (readFrame.vMaxL - readFrame.vMinL));
            uint32_t srcXL = (uint32_t)(readFrame.descWidth * readFrame.uMinL);
            uint32_t srcYL = (uint32_t)(readFrame.descHeight * readFrame.vMinL);

            uint32_t srcWidthR = (uint32_t)(readFrame.descWidth * (readFrame.uMaxR - readFrame.uMinR));
            uint32_t srcHeightR = (uint32_t)(readFrame.descHeight * (readFrame.vMaxR - readFrame.vMinR));
            uint32_t srcXR = (uint32_t)(readFrame.descWidth * readFrame.uMinR);
            uint32_t srcYR = (uint32_t)(readFrame.descHeight * readFrame.vMinR);

            uint32_t dbWriteIndex = (g_copyFrameSeq + 1) % 2;
            SharedFrame* pHeader = pHeaders[dbWriteIndex];
            uint8_t* pPixelData = pPixelDatas[dbWriteIndex];

            if (pHeader && pPixelData) {
                pHeader->width = srcWidthL * 2;
                pHeader->height = srcHeightL * 2;
                pHeader->format = readFrame.descFormat;
                if (pSharedHands) {
                    pHeader->visionTimestamp = readFrame.visionTimestamp;
                    for(int i=0; i<16; i++) pHeader->renderHeadTransform[i] = readFrame.renderHeadTransform[i];
                }

                uint32_t copyWidthL = (srcWidthL > 4096) ? 4096 : srcWidthL;
                uint32_t copyHeightL = (srcHeightL > 4096) ? 4096 : srcHeightL;
                uint32_t copyWidthR = (srcWidthR > 4096) ? 4096 : srcWidthR;
                uint32_t copyHeightR = (srcHeightR > 4096) ? 4096 : srcHeightR;

                D3D11_MAPPED_SUBRESOURCE mappedColorL, mappedColorR;
                D3D11_MAPPED_SUBRESOURCE mappedDepthL, mappedDepthR;

                bool hasColorL = SUCCEEDED(readFrame.pContext->Map(readFrame.pColorLeft, 0, D3D11_MAP_READ, 0, &mappedColorL));
                bool hasColorR = SUCCEEDED(readFrame.pContext->Map(readFrame.pColorRight, 0, D3D11_MAP_READ, 0, &mappedColorR));
                bool hasDepthL = readFrame.pDepthLeft && SUCCEEDED(readFrame.pContext->Map(readFrame.pDepthLeft, 0, D3D11_MAP_READ, 0, &mappedDepthL));
                bool hasDepthR = readFrame.pDepthRight && SUCCEEDED(readFrame.pContext->Map(readFrame.pDepthRight, 0, D3D11_MAP_READ, 0, &mappedDepthR));

                if (hasColorL) {
                    uint8_t* dst = pPixelData;
                    uint8_t* src = ((uint8_t*)mappedColorL.pData) + srcYL * mappedColorL.RowPitch + srcXL * 4;
                    for(uint32_t y=0; y<copyHeightL; ++y) {
                        memcpy(dst + y * (copyWidthL * 2) * 4, src + y * mappedColorL.RowPitch, copyWidthL * 4);
                    }
                    readFrame.pContext->Unmap(readFrame.pColorLeft, 0);
                }
                if (hasColorR) {
                    uint8_t* dst = pPixelData;
                    uint8_t* src = ((uint8_t*)mappedColorR.pData) + srcYR * mappedColorR.RowPitch + srcXR * 4;
                    for(uint32_t y=0; y<copyHeightR; ++y) {
                        memcpy(dst + y * (copyWidthL * 2) * 4 + copyWidthL * 4, src + y * mappedColorR.RowPitch, copyWidthR * 4);
                    }
                    readFrame.pContext->Unmap(readFrame.pColorRight, 0);
                }

                uint8_t* depthDstOffset = pPixelData + (copyHeightL * (copyWidthL * 2) * 4);
                if (hasDepthL) {
                    uint8_t* src = ((uint8_t*)mappedDepthL.pData) + srcYL * mappedDepthL.RowPitch + srcXL * 4;
                    for(uint32_t y=0; y<copyHeightL; ++y) {
                        memcpy(depthDstOffset + y * (copyWidthL * 2) * 4, src + y * mappedDepthL.RowPitch, copyWidthL * 4);
                    }
                    readFrame.pContext->Unmap(readFrame.pDepthLeft, 0);
                }
                if (hasDepthR) {
                    uint8_t* src = ((uint8_t*)mappedDepthR.pData) + srcYR * mappedDepthR.RowPitch + srcXR * 4;
                    for(uint32_t y=0; y<copyHeightR; ++y) {
                        memcpy(depthDstOffset + y * (copyWidthL * 2) * 4 + copyWidthL * 4, src + y * mappedDepthR.RowPitch, copyWidthR * 4);
                    }
                    readFrame.pContext->Unmap(readFrame.pDepthRight, 0);
                }

                pHeader->sequenceNumber = ++g_copyFrameSeq;
                if (pDbHeader) pDbHeader->latestReadyIndex = dbWriteIndex;
            }

            if (pMt) { pMt->Leave(); pMt->Release(); }
        }
    }
}

void LogInitMsg(const char* msg) {}


// --- COM VTABLE HOOKING ---
void ApplyCOMHooks() { }
// --- END COM VTABLE HOOKING ---

void InjectMinHook() {
    // Keep empty to avoid hooking LoadLibrary
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        InjectMinHook();
    }
    return TRUE;
}
