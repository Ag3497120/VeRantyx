// Milestone 8a: XAudio2 on Core Audio. A mastering voice on the default device; source voices of 16-bit and float
// PCM play queued buffers (silence, so the test makes no sound) and report their buffers, loops and stream end
// through voice callbacks on the engine's thread; samples played follow the frequency ratio; a submix voice
// passes a source on; flushing returns the queued buffers. "xaudio2.exe --tone" plays a quiet 440 Hz tone.
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <xaudio2.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#ifndef XAUDIO2_END_OF_STREAM
#define XAUDIO2_END_OF_STREAM 0x40
#endif
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)

typedef struct {
    IXAudio2VoiceCallback iface;
    volatile LONG pass_start, pass_end, buffer_start, buffer_end, loop_end, stream_end, errors;
    DWORD thread;
    HANDLE done;
} callback;
static callback *self(IXAudio2VoiceCallback *i) { return (callback *)i; }
static void WINAPI on_pass_start(IXAudio2VoiceCallback *i, UINT32 bytes) { (void)bytes; InterlockedIncrement(&self(i)->pass_start); self(i)->thread = GetCurrentThreadId(); }
static void WINAPI on_pass_end(IXAudio2VoiceCallback *i) { InterlockedIncrement(&self(i)->pass_end); }
static void WINAPI on_stream_end(IXAudio2VoiceCallback *i) { InterlockedIncrement(&self(i)->stream_end); SetEvent(self(i)->done); }
static void WINAPI on_buffer_start(IXAudio2VoiceCallback *i, void *ctx) { (void)ctx; InterlockedIncrement(&self(i)->buffer_start); }
static void WINAPI on_buffer_end(IXAudio2VoiceCallback *i, void *ctx) { (void)ctx; InterlockedIncrement(&self(i)->buffer_end); }
static void WINAPI on_loop_end(IXAudio2VoiceCallback *i, void *ctx) { (void)ctx; InterlockedIncrement(&self(i)->loop_end); }
static void WINAPI on_error(IXAudio2VoiceCallback *i, void *ctx, HRESULT hr) { (void)ctx; (void)hr; InterlockedIncrement(&self(i)->errors); }
static IXAudio2VoiceCallbackVtbl callback_vtbl = { on_pass_start, on_pass_end, on_stream_end, on_buffer_start, on_buffer_end, on_loop_end, on_error };
static void init_callback(callback *c) { memset(c, 0, sizeof *c); c->iface.lpVtbl = &callback_vtbl; c->done = CreateEventA(NULL, FALSE, FALSE, NULL); }

typedef struct { IXAudio2EngineCallback iface; volatile LONG passes; } engine_callback;
static void WINAPI on_engine_pass_start(IXAudio2EngineCallback *i) { InterlockedIncrement(&((engine_callback *)i)->passes); }
static void WINAPI on_engine_pass_end(IXAudio2EngineCallback *i) { (void)i; }
static void WINAPI on_critical(IXAudio2EngineCallback *i, HRESULT hr) { (void)i; (void)hr; }
static IXAudio2EngineCallbackVtbl engine_vtbl = { on_engine_pass_start, on_engine_pass_end, on_critical };

static WAVEFORMATEX format(WORD tag, WORD channels, DWORD rate, WORD bits) {
    WAVEFORMATEX w = { tag, channels, rate, rate * channels * bits / 8, (WORD)(channels * bits / 8), bits, 0 };
    return w;
}
static UINT64 played(IXAudio2SourceVoice *v) { XAUDIO2_VOICE_STATE st; IXAudio2SourceVoice_GetState(v, &st, 0); return st.SamplesPlayed; }

int main(int argc, char **argv) {
    int tone = argc > 1 && !strcmp(argv[1], "--tone");
    IXAudio2 *xa = NULL;
    CHECK(SUCCEEDED(XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR)));
    if (!xa) { printf("xaudio2: no engine -> FAILED\n"); return 1; }
    engine_callback ec = { { &engine_vtbl }, 0 };
    CHECK(SUCCEEDED(IXAudio2_RegisterForCallbacks(xa, &ec.iface)));
    IXAudio2MasteringVoice *master = NULL;
    CHECK(SUCCEEDED(IXAudio2_CreateMasteringVoice(xa, &master, 2, 48000, 0, NULL, NULL, AudioCategory_GameEffects)));
    if (!master) { printf("xaudio2: no mastering voice -> FAILED\n"); return 1; }
    XAUDIO2_VOICE_DETAILS details;
    IXAudio2MasteringVoice_GetVoiceDetails(master, &details);
    CHECK(details.InputChannels == 2 && details.InputSampleRate == 48000);

    // 16-bit mono at the device rate: two buffers, the second ending the stream.
    callback cb; init_callback(&cb);
    WAVEFORMATEX w16 = format(WAVE_FORMAT_PCM, 1, 48000, 16);
    IXAudio2SourceVoice *src = NULL;
    CHECK(SUCCEEDED(IXAudio2_CreateSourceVoice(xa, &src, &w16, 0, 2.0f, &cb.iface, NULL, NULL)));
    static short pcm[4800];
    XAUDIO2_BUFFER b = { 0 };
    b.AudioBytes = sizeof pcm; b.pAudioData = (const BYTE *)pcm;
    CHECK(SUCCEEDED(IXAudio2SourceVoice_SubmitSourceBuffer(src, &b, NULL)));
    b.Flags = XAUDIO2_END_OF_STREAM;
    CHECK(SUCCEEDED(IXAudio2SourceVoice_SubmitSourceBuffer(src, &b, NULL)));
    XAUDIO2_VOICE_STATE st;
    IXAudio2SourceVoice_GetState(src, &st, 0);
    CHECK(st.BuffersQueued == 2 && st.SamplesPlayed == 0);
    CHECK(SUCCEEDED(IXAudio2SourceVoice_Start(src, 0, XAUDIO2_COMMIT_NOW)));
    CHECK(WaitForSingleObject(cb.done, 3000) == WAIT_OBJECT_0);
    IXAudio2SourceVoice_GetState(src, &st, 0);
    CHECK(st.BuffersQueued == 0);
    CHECK(st.SamplesPlayed == 9600);
    CHECK(cb.buffer_start == 2 && cb.buffer_end == 2 && cb.stream_end == 1 && cb.errors == 0);
    CHECK(cb.pass_start > 0 && cb.pass_end > 0);
    CHECK(cb.thread && cb.thread != GetCurrentThreadId());                  // callbacks come from the engine's thread
    CHECK(ec.passes > 0);

    // A loop: 480 frames played three times (two loops back).
    LONG before = cb.buffer_end;
    UINT64 base = played(src);
    XAUDIO2_BUFFER lb = { XAUDIO2_END_OF_STREAM, 480 * 2, (const BYTE *)pcm, 0, 0, 0, 0, 2, NULL };
    CHECK(SUCCEEDED(IXAudio2SourceVoice_SubmitSourceBuffer(src, &lb, NULL)));
    CHECK(WaitForSingleObject(cb.done, 3000) == WAIT_OBJECT_0);
    CHECK(cb.loop_end == 2 && cb.buffer_end == before + 1);
    CHECK(played(src) - base == 1440);
    IXAudio2SourceVoice_DestroyVoice(src);

    // Float stereo at half the device rate, through a submix voice: 2400 frames last 100 ms and count as 2400 samples.
    IXAudio2SubmixVoice *sub = NULL;
    CHECK(SUCCEEDED(IXAudio2_CreateSubmixVoice(xa, &sub, 2, 48000, 0, 0, NULL, NULL)));
    XAUDIO2_SEND_DESCRIPTOR send = { 0, (IXAudio2Voice *)sub };
    XAUDIO2_VOICE_SENDS sends = { 1, &send };
    callback cf; init_callback(&cf);
    WAVEFORMATEX wf = format(WAVE_FORMAT_IEEE_FLOAT, 2, 24000, 32);
    CHECK(SUCCEEDED(IXAudio2_CreateSourceVoice(xa, &src, &wf, 0, 2.0f, &cf.iface, &sends, NULL)));
    static float fl[2400 * 2];
    XAUDIO2_BUFFER fb = { XAUDIO2_END_OF_STREAM, sizeof fl, (const BYTE *)fl, 0, 0, 0, 0, 0, NULL };
    CHECK(SUCCEEDED(IXAudio2SourceVoice_SubmitSourceBuffer(src, &fb, NULL)));
    DWORD t0 = GetTickCount();
    CHECK(SUCCEEDED(IXAudio2SourceVoice_Start(src, 0, XAUDIO2_COMMIT_NOW)));
    CHECK(WaitForSingleObject(cf.done, 3000) == WAIT_OBJECT_0);
    DWORD took = GetTickCount() - t0;
    CHECK(played(src) == 2400);
    CHECK(took >= 40);                                                       // it took real time (100 ms of audio, less the buffered head start)
    float vol = 0;
    CHECK(SUCCEEDED(IXAudio2SourceVoice_SetVolume(src, 0.5f, XAUDIO2_COMMIT_NOW)));
    IXAudio2SourceVoice_GetVolume(src, &vol);
    CHECK(vol == 0.5f);
    float m[4] = { 1, 0, 0, 1 }, got[4] = { 0 };
    CHECK(SUCCEEDED(IXAudio2SourceVoice_SetOutputMatrix(src, (IXAudio2Voice *)sub, 2, 2, m, XAUDIO2_COMMIT_NOW)));
    IXAudio2SourceVoice_GetOutputMatrix(src, (IXAudio2Voice *)sub, 2, 2, got);
    CHECK(!memcmp(m, got, sizeof m));
    CHECK(SUCCEEDED(IXAudio2SourceVoice_SetFrequencyRatio(src, 1.5f, XAUDIO2_COMMIT_NOW)));
    float ratio = 0;
    IXAudio2SourceVoice_GetFrequencyRatio(src, &ratio);
    CHECK(ratio == 1.5f);

    // Flushing a stopped voice returns its buffers.
    CHECK(SUCCEEDED(IXAudio2SourceVoice_Stop(src, 0, XAUDIO2_COMMIT_NOW)));
    before = cf.buffer_end;
    fb.Flags = 0;
    for (int i = 0; i < 3; i++) IXAudio2SourceVoice_SubmitSourceBuffer(src, &fb, NULL);
    IXAudio2SourceVoice_GetState(src, &st, 0);
    CHECK(st.BuffersQueued == 3);
    CHECK(SUCCEEDED(IXAudio2SourceVoice_FlushSourceBuffers(src)));
    IXAudio2SourceVoice_GetState(src, &st, 0);
    CHECK(st.BuffersQueued == 0 && cf.buffer_end == before + 3);
    IXAudio2SourceVoice_DestroyVoice(src);
    IXAudio2SubmixVoice_DestroyVoice(sub);

    // A format it does not decode is refused.
    WAVEFORMATEX adpcm = format(2, 1, 22050, 4);
    CHECK(FAILED(IXAudio2_CreateSourceVoice(xa, &src, &adpcm, 0, 2.0f, NULL, NULL, NULL)));

    if (tone) {
        static short wave[48000];
        for (int i = 0; i < 48000; i++) wave[i] = (short)(0.05 * 32767 * sin(2 * 3.14159265358979 * 440 * i / 48000));
        callback ct; init_callback(&ct);
        CHECK(SUCCEEDED(IXAudio2_CreateSourceVoice(xa, &src, &w16, 0, 2.0f, &ct.iface, NULL, NULL)));
        XAUDIO2_BUFFER tb = { XAUDIO2_END_OF_STREAM, sizeof wave, (const BYTE *)wave, 0, 0, 0, 0, 0, NULL };
        IXAudio2SourceVoice_SubmitSourceBuffer(src, &tb, NULL);
        IXAudio2SourceVoice_Start(src, 0, XAUDIO2_COMMIT_NOW);
        CHECK(WaitForSingleObject(ct.done, 3000) == WAIT_OBJECT_0);
        IXAudio2SourceVoice_DestroyVoice(src);
    }
    IXAudio2_UnregisterForCallbacks(xa, &ec.iface);
    IXAudio2MasteringVoice_DestroyVoice(master);
    IXAudio2_Release(xa);
    printf("xaudio2: %s\n", failures ? "FAILED" : "all checks passed");
    return failures != 0;
}
