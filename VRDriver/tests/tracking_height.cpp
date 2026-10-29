// Exercise the driver's origin publication without SteamVR or a headset.
#include <assert.h>
#include "../src/driver_verantyx.cpp"

static void Identity(float* matrix, float x, float y, float z) {
    memset(matrix, 0, 16 * sizeof(float));
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1;
    matrix[12] = x; matrix[13] = y; matrix[14] = z;
}

int main() {
    SetEnvironmentVariableA("VERANTYX_KEEP_HEIGHT", "1");
    PoseHistory history = {};
    g_poseHistory = &history;
    SharedHands raw = {};
    Identity(raw.headTransform, 0, 0.86f, 0);
    Identity(raw.leftTransform, -0.2f, 0.56f, -0.5f);
    Identity(raw.rightTransform, 0.3f, 0.66f, -0.4f);
    raw.headTracked = raw.leftTracked = raw.rightTracked = 1;
    raw.visionTimestamp = 1;
    SharedHands snapshot = raw;
    KeepHeightAcrossBreaks(&snapshot);
    assert(snapshot.headTransform[13] == 0.86f);

    // The last sample was on a table; tracking resumes 74 cm higher.
    g_lastHeadSampleMs = GetTickCount64() - 2000;
    raw.headTransform[13] += 0.74f;
    raw.leftTransform[13] += 0.74f;
    raw.rightTransform[13] += 0.74f;
    raw.visionTimestamp = 2;
    snapshot = raw;
    KeepHeightAcrossBreaks(&snapshot);
    assert(g_afterBreak);
    g_backSinceMs = GetTickCount64() - 600;
    raw.visionTimestamp = 3;
    snapshot = raw;
    KeepHeightAcrossBreaks(&snapshot);
    float published;
    const LONG bits = InterlockedCompareExchange(&history.heightOffsetBits, 0, 0);
    memcpy(&published, &bits, sizeof(published));
    assert(fabsf(published + 0.74f) < 0.0001f);
    assert(fabsf(snapshot.headTransform[13] - 0.86f) < 0.0001f);
    assert(fabsf(snapshot.leftTransform[13] - 0.56f) < 0.0001f);
    assert(fabsf(snapshot.rightTransform[13] - 0.66f) < 0.0001f);
    assert(raw.headTransform[13] > 1.59f);
    snapshot = raw;
    KeepHeightAcrossBreaks(&snapshot);
    assert(fabsf(snapshot.headTransform[13] - 0.86f) < 0.0001f);
    static_assert(offsetof(PoseHistory, heightMagic) == 8 && offsetof(PoseHistory, heightOffsetBits) == 12,
                  "Height origin must use the pose-history header's reserved words");
    puts("tracking height: resumed head/hands share the correction published for the proxy -> ok");
    return 0;
}
