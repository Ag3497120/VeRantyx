// SDL2 audio for the Linux game, played through Core Audio.
//
// libsoundsystem drives SDL2's audio API through tier0's SDL table
// (SDL_OpenAudioDevice, SDL_QueueAudio, SDL_GetQueuedAudioSize, ...). The
// Linux libSDL2 has no backend that exists on macOS (PulseAudio, ALSA), so its
// audio init fails and the engine falls back to a null device: no sound. These
// entry points are interposed (tier0 resolves them with dlsym, which consults
// the interposers): a default-output AudioUnit plays a ring buffer the game
// fills. The Mac's audio is what MacEncoder streams to the headset.
//
// Queue mode (no callback) never runs game code on the Core Audio thread. In
// callback mode the callback is pumped from a thread created through the
// guest's own pthread_create, so it runs with the guest's thread state.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <AudioToolbox/AudioToolbox.h>
#include "klepton.h"
#include "kl_sdlaudio.h"

#define SDL_INIT_AUDIO 0x00000010u
#define AUDIO_U8     0x0008
#define AUDIO_S16LSB 0x8010
#define AUDIO_S32LSB 0x8020
#define AUDIO_F32LSB 0x8120

typedef struct {               // SDL2's SDL_AudioSpec (32 bytes on LP64)
    int32_t freq;
    uint16_t format;
    uint8_t channels;
    uint8_t silence;
    uint16_t samples;
    uint16_t padding;
    uint32_t size;
    void (*callback)(void *userdata, uint8_t *stream, int len);
    void *userdata;
} sdl_spec;

#define RING_FRAMES (1u << 16)   // ~1.4 s at 48 kHz, stereo float
static float g_ring[RING_FRAMES * 2];
static _Atomic uint32_t g_head, g_tail;   // frames written / read (wrapping)
static sdl_spec g_spec;
static AudioUnit g_unit;
static int g_open, g_running;
static _Atomic uint64_t g_underrunFrames, g_playedFrames, g_queuedFrames;
static pthread_t g_pump;
static _Atomic int g_pumpRun;

static unsigned bytes_per_sample(uint16_t f) { return (f & 0xFF) / 8; }
static uint32_t ring_count(void) { return atomic_load(&g_head) - atomic_load(&g_tail); }

// One frame of `ch` input channels to stereo (SDL channel order).
static void downmix(const float *in, unsigned ch, float *l, float *r) {
    const float k = 0.7071f;
    switch (ch) {
    case 1: *l = *r = in[0]; break;
    case 2: *l = in[0]; *r = in[1]; break;
    case 4: *l = in[0] + k * in[2]; *r = in[1] + k * in[3]; break;              // FL FR BL BR
    case 6: *l = in[0] + k * in[2] + k * in[4]; *r = in[1] + k * in[2] + k * in[5]; break;   // FL FR FC LFE BL BR
    case 8: *l = in[0] + k * in[2] + k * (in[4] + in[6]); *r = in[1] + k * in[2] + k * (in[5] + in[7]); break;
    default: *l = in[0]; *r = ch > 1 ? in[1] : in[0]; break;
    }
}

static float sample_at(const uint8_t *p, uint16_t fmt) {
    switch (fmt) {
    case AUDIO_F32LSB: { float v; memcpy(&v, p, 4); return v; }
    case AUDIO_S16LSB: { int16_t v; memcpy(&v, p, 2); return v / 32768.0f; }
    case AUDIO_S32LSB: { int32_t v; memcpy(&v, p, 4); return v / 2147483648.0f; }
    case AUDIO_U8: return (p[0] - 128) / 128.0f;
    default: return 0;
    }
}

// Append `len` bytes in the device's format; drops what does not fit.
static void ring_push(const uint8_t *data, uint32_t len) {
    const unsigned ch = g_spec.channels ? g_spec.channels : 2;
    const unsigned bps = bytes_per_sample(g_spec.format);
    const uint32_t frameBytes = ch * bps;
    if (!frameBytes) return;
    uint32_t frames = len / frameBytes;
    uint32_t space = RING_FRAMES - ring_count();
    if (frames > space) frames = space;
    uint32_t head = atomic_load(&g_head);
    float in[8];
    for (uint32_t i = 0; i < frames; i++) {
        const uint8_t *f = data + (size_t)i * frameBytes;
        for (unsigned c = 0; c < ch && c < 8; c++) in[c] = sample_at(f + c * bps, g_spec.format);
        float l, r; downmix(in, ch > 8 ? 8 : ch, &l, &r);
        uint32_t slot = (head + i) & (RING_FRAMES - 1);
        g_ring[slot * 2] = l; g_ring[slot * 2 + 1] = r;
    }
    atomic_store(&g_head, head + frames);
    atomic_fetch_add(&g_queuedFrames, frames);
}

static OSStatus render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                       UInt32 bus, UInt32 nframes, AudioBufferList *io) {
    (void)ref; (void)flags; (void)ts; (void)bus;
    float *out = (float *)io->mBuffers[0].mData;
    uint32_t avail = ring_count(), n = nframes < avail ? nframes : avail;
    uint32_t tail = atomic_load(&g_tail);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = (tail + i) & (RING_FRAMES - 1);
        out[i * 2] = g_ring[slot * 2]; out[i * 2 + 1] = g_ring[slot * 2 + 1];
    }
    if (n < nframes) {
        memset(out + n * 2, 0, (size_t)(nframes - n) * 2 * sizeof(float));
        if (g_running) atomic_fetch_add(&g_underrunFrames, nframes - n);
    }
    atomic_store(&g_tail, tail + n);
    atomic_fetch_add(&g_playedFrames, n);
    return noErr;
}

static int unit_create(int rate) {
    AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    if (!c || AudioComponentInstanceNew(c, &g_unit) != noErr) return -1;
    AudioStreamBasicDescription f = { 0 };
    f.mSampleRate = rate; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = 2; f.mBitsPerChannel = 32; f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 8; f.mBytesPerPacket = 8;
    if (AudioUnitSetProperty(g_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) != noErr) return -1;
    AURenderCallbackStruct cb = { render, NULL };
    if (AudioUnitSetProperty(g_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) != noErr) return -1;
    return AudioUnitInitialize(g_unit) == noErr ? 0 : -1;
}

// Callback mode: keep ~3 device buffers queued by calling the game.
static void *pump(void *arg) {
    (void)arg;
    const uint32_t frameBytes = g_spec.channels * bytes_per_sample(g_spec.format);
    uint8_t *buf = malloc(g_spec.size ? g_spec.size : 4096);
    while (atomic_load(&g_pumpRun)) {
        if (g_running && ring_count() < (uint32_t)g_spec.samples * 3 && buf) {
            memset(buf, g_spec.silence, g_spec.size);
            g_spec.callback(g_spec.userdata, buf, (int)g_spec.size);
            ring_push(buf, g_spec.size);
            (void)frameBytes;
        } else {
            usleep(2000);
        }
    }
    free(buf);
    return NULL;
}

// ---- interposed SDL entry points ----
typedef int (*init_fn)(uint32_t);
static void *sdl_real(const char *name) {
    kl_image *sdl = kl_find_image("libSDL2-2.0.so.0");
    if (!sdl) sdl = kl_find_image("libSDL2.so");
    return sdl ? kl_sym(sdl, name) : NULL;
}
static int shim_SDL_InitSubSystem(uint32_t flags) {
    if (!(flags & SDL_INIT_AUDIO)) { init_fn real = (init_fn)sdl_real("SDL_InitSubSystem"); return real ? real(flags) : -1; }
    uint32_t rest = flags & ~SDL_INIT_AUDIO;
    fprintf(stderr, "  [audio] SDL_InitSubSystem(%#x): audio served by Core Audio\n", flags);
    if (!rest) return 0;
    init_fn real = (init_fn)sdl_real("SDL_InitSubSystem");
    return real ? real(rest) : -1;
}
static int shim_SDL_GetNumAudioDevices(int iscapture) { return iscapture ? 0 : 1; }
static const char *shim_SDL_GetAudioDeviceName(int index, int iscapture) {
    return (!iscapture && index == 0) ? "VeRantyx Core Audio" : NULL;
}
static uint32_t shim_SDL_OpenAudioDevice(const char *device, int iscapture, const sdl_spec *want, sdl_spec *have, int allowed) {
    (void)allowed;
    if (iscapture || !want) return 0;
    if (g_open) { fprintf(stderr, "  [audio] second output device refused\n"); return 0; }
    sdl_spec s = *want;
    if (s.format != AUDIO_F32LSB && s.format != AUDIO_S16LSB && s.format != AUDIO_S32LSB && s.format != AUDIO_U8) s.format = AUDIO_F32LSB;
    if (!s.channels) s.channels = 2;
    if (s.freq <= 0) s.freq = 48000;
    if (!s.samples) s.samples = 1024;
    s.silence = s.format == AUDIO_U8 ? 0x80 : 0;
    s.size = (uint32_t)s.samples * s.channels * bytes_per_sample(s.format);
    g_spec = s;
    if (unit_create(s.freq) != 0) { fprintf(stderr, "  [audio] Core Audio output could not be created\n"); return 0; }
    if (have) *have = s;
    g_open = 1;
    atomic_store(&g_head, 0); atomic_store(&g_tail, 0);
    if (s.callback) {
        int (*guest_create)(pthread_t *, const void *, void *(*)(void *), void *) =
            (int (*)(pthread_t *, const void *, void *(*)(void *), void *))kl_shim_lookup("pthread_create");
        atomic_store(&g_pumpRun, 1);
        if (!guest_create || guest_create(&g_pump, NULL, pump, NULL) != 0) {
            fprintf(stderr, "  [audio] callback thread could not be started\n");
            atomic_store(&g_pumpRun, 0);
        }
    }
    fprintf(stderr, "  [audio] SDL_OpenAudioDevice(\"%s\") -> Core Audio: %d Hz, %u ch (played as stereo), format %#x, %u-frame buffers, %s mode\n",
            device ? device : "default", s.freq, s.channels, s.format, s.samples, s.callback ? "callback" : "queue");
    return 2;
}
static void shim_SDL_PauseAudioDevice(uint32_t dev, int pause_on) {
    if (dev != 2 || !g_open) return;
    if (!pause_on && !g_running) { g_running = 1; AudioOutputUnitStart(g_unit); fprintf(stderr, "  [audio] playing\n"); }
    else if (pause_on && g_running) { g_running = 0; AudioOutputUnitStop(g_unit); }
}
static int shim_SDL_QueueAudio(uint32_t dev, const void *data, uint32_t len) {
    if (dev != 2 || !g_open || !data) return -1;
    ring_push((const uint8_t *)data, len);
    static double last; static int reports;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    double now = t.tv_sec + t.tv_nsec * 1e-9;
    if (reports < 1000 && now - last > 30) {
        last = now; reports++;
        fprintf(stderr, "  [audio] played %.1f s, queued %u frames, underrun %.2f s\n",
                atomic_load(&g_playedFrames) / (double)g_spec.freq, ring_count(), atomic_load(&g_underrunFrames) / (double)g_spec.freq);
    }
    return 0;
}
static uint32_t shim_SDL_GetQueuedAudioSize(uint32_t dev) {
    if (dev != 2 || !g_open) return 0;
    return ring_count() * g_spec.channels * bytes_per_sample(g_spec.format);
}
static void shim_SDL_ClearQueuedAudio(uint32_t dev) {
    if (dev != 2 || !g_open) return;
    atomic_store(&g_tail, atomic_load(&g_head));
}
static void shim_SDL_CloseAudioDevice(uint32_t dev) {
    if (dev != 2 || !g_open) return;
    atomic_store(&g_pumpRun, 0);
    if (g_running) AudioOutputUnitStop(g_unit);
    g_running = 0;
    AudioUnitUninitialize(g_unit);
    AudioComponentInstanceDispose(g_unit);
    g_open = 0;
}
static void shim_SDL_LockAudioDevice(uint32_t dev) { (void)dev; }
static void shim_SDL_UnlockAudioDevice(uint32_t dev) { (void)dev; }
static int shim_SDL_GetAudioDeviceStatus(uint32_t dev) { return (dev == 2 && g_open) ? (g_running ? 1 : 2) : 0; }

void kl_sdlaudio_install(void) {
    if (getenv("ALYX_SDL_AUDIO") && !strcmp(getenv("ALYX_SDL_AUDIO"), "0")) return;
    kl_interpose("SDL_InitSubSystem", (void *)shim_SDL_InitSubSystem);
    kl_interpose("SDL_GetNumAudioDevices", (void *)shim_SDL_GetNumAudioDevices);
    kl_interpose("SDL_GetAudioDeviceName", (void *)shim_SDL_GetAudioDeviceName);
    kl_interpose("SDL_OpenAudioDevice", (void *)shim_SDL_OpenAudioDevice);
    kl_interpose("SDL_PauseAudioDevice", (void *)shim_SDL_PauseAudioDevice);
    kl_interpose("SDL_QueueAudio", (void *)shim_SDL_QueueAudio);
    kl_interpose("SDL_GetQueuedAudioSize", (void *)shim_SDL_GetQueuedAudioSize);
    kl_interpose("SDL_ClearQueuedAudio", (void *)shim_SDL_ClearQueuedAudio);
    kl_interpose("SDL_CloseAudioDevice", (void *)shim_SDL_CloseAudioDevice);
    kl_interpose("SDL_LockAudioDevice", (void *)shim_SDL_LockAudioDevice);
    kl_interpose("SDL_UnlockAudioDevice", (void *)shim_SDL_UnlockAudioDevice);
    kl_interpose("SDL_GetAudioDeviceStatus", (void *)shim_SDL_GetAudioDeviceStatus);
}
