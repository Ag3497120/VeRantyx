// Milestone 7c: WASAPI on Core Audio. The default endpoint and its name, the mix format, an event-driven shared
// client: the device takes the queued frames (silence, so the test makes no sound), signals the event, and the
// clock advances. "audio.exe --tone" plays a quiet 440 Hz tone for a second instead.
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)

int main(int argc, char **argv) {
    int tone = argc > 1 && !strcmp(argv[1], "--tone");
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL; IMMDevice *dev = NULL; IPropertyStore *props = NULL; IAudioClient *ac = NULL;
    CHECK(SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en)));
    if (!en) { printf("audio: no enumerator -> FAILED\n"); return 1; }
    CHECK(SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)));
    char name[256] = "?";
    if (dev && SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &props))) {
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(IPropertyStore_GetValue(props, &PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
            WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, name, sizeof name, NULL, NULL);
        PropVariantClear(&pv);
        IPropertyStore_Release(props);
    }
    CHECK(SUCCEEDED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac)));
    WAVEFORMATEX *mix = NULL;
    CHECK(SUCCEEDED(IAudioClient_GetMixFormat(ac, &mix)) && mix->nChannels == 2 && mix->wBitsPerSample == 32);
    CHECK(SUCCEEDED(IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 1000000, 0, mix, NULL)));
    HANDLE event = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHECK(SUCCEEDED(IAudioClient_SetEventHandle(ac, event)));
    UINT32 frames = 0;
    CHECK(SUCCEEDED(IAudioClient_GetBufferSize(ac, &frames)) && frames >= mix->nSamplesPerSec / 10);
    IAudioRenderClient *rc = NULL; IAudioClock *clock = NULL;
    CHECK(SUCCEEDED(IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc)));
    CHECK(SUCCEEDED(IAudioClient_GetService(ac, &IID_IAudioClock, (void **)&clock)));
    BYTE *data;
    CHECK(IAudioRenderClient_GetBuffer(rc, frames + 1, &data) == AUDCLNT_E_BUFFER_TOO_LARGE);
    CHECK(SUCCEEDED(IAudioRenderClient_GetBuffer(rc, frames, &data)));
    IAudioRenderClient_ReleaseBuffer(rc, frames, AUDCLNT_BUFFERFLAGS_SILENT);
    UINT32 padding = 0;
    IAudioClient_GetCurrentPadding(ac, &padding);
    CHECK(padding == frames);
    CHECK(SUCCEEDED(IAudioClient_Start(ac)));
    CHECK(WaitForSingleObject(event, 2000) == WAIT_OBJECT_0);               // the device asked for data
    double phase = 0;
    DWORD until = GetTickCount() + (tone ? 1000 : 150);
    while (GetTickCount() < until) {                                        // keep the buffer filled, as a real client does
        if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0) break;
        IAudioClient_GetCurrentPadding(ac, &padding);
        UINT32 room = frames - padding;
        if (!room || FAILED(IAudioRenderClient_GetBuffer(rc, room, &data))) continue;
        float *f = (float *)data;
        for (UINT32 i = 0; i < room; i++) {
            float v = tone ? 0.05f * (float)sin(phase) : 0.0f;
            phase += 2 * 3.14159265 * 440.0 / mix->nSamplesPerSec;
            f[i * 2] = f[i * 2 + 1] = v;
        }
        IAudioRenderClient_ReleaseBuffer(rc, room, 0);
    }
    UINT64 pos = 0, freq = 0;
    IAudioClock_GetFrequency(clock, &freq);
    IAudioClock_GetPosition(clock, &pos, NULL);
    CHECK(freq == mix->nSamplesPerSec && pos > 0);
    CHECK(SUCCEEDED(IAudioClient_Stop(ac)));
    printf("audio: \"%s\", %lu Hz float stereo, buffer %u frames, played %llu frames: %d failed checks -> %s\n",
           name, mix->nSamplesPerSec, frames, (unsigned long long)pos, failures, failures ? "FAILED" : "ok");
    CoTaskMemFree(mix);
    IAudioClock_Release(clock); IAudioRenderClient_Release(rc); IAudioClient_Release(ac); IMMDevice_Release(dev); IMMDeviceEnumerator_Release(en);
    CoUninitialize();
    return failures;
}
