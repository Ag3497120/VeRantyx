// WASAPI (the MMDevice API and IAudioClient) on Core Audio.
//
// The default render endpoint is the Mac's default output device. An audio client keeps the program's samples in
// a ring buffer the size WASAPI promises (GetBufferSize); an output AudioUnit pulls from it on Core Audio's thread,
// set to the program's own format so Core Audio converts sample rate, channels and sample type. Event-driven
// clients get their event signalled every time Core Audio takes data. Capture endpoints are not offered yet.
//
// The COM objects use the same machinery as Direct3D (d3d11.m): generated method tables with stubs.
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#include "d3d_common.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "audio_tables.h"
#pragma clang diagnostic pop

static const GUID IID_IUnknown_ = { 0, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID CLSID_MMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID SUBTYPE_PCM = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID SUBTYPE_FLOAT = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID PKEY_FriendlyName = { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } };
#define AUDCLNT_E_NOT_INITIALIZED ((HRESULT)0x88890001)
#define AUDCLNT_E_ALREADY_INITIALIZED ((HRESULT)0x88890002)
#define AUDCLNT_E_BUFFER_TOO_LARGE ((HRESULT)0x88890006)
#define AUDCLNT_E_UNSUPPORTED_FORMAT ((HRESULT)0x88890008)
#define AUDCLNT_E_EVENTHANDLE_NOT_SET ((HRESULT)0x88890014)
#define E_NOTFOUND ((HRESULT)0x80070490)
#define AUDCLNT_STREAMFLAGS_EVENTCALLBACK 0x00040000

static void **g_enum_vtbl, **g_device_vtbl, **g_collection_vtbl, **g_client_vtbl, **g_render_vtbl, **g_clock_vtbl, **g_volume_vtbl, **g_props_vtbl;
static d3d_object *self_of(void *this) { return (d3d_object *)this; }
static WINAPI uint32_t obj_addref(void *this) { return d3d_addref(self_of(this)); }
static WINAPI uint32_t obj_release(void *this) { return d3d_release(self_of(this)); }
static HRESULT query(void *this, const GUID *iid, void **out, const GUID *iface) {
    if (!out) return E_POINTER;
    if (d3d_same_guid(iid, &IID_IUnknown_) || d3d_same_guid(iid, iface)) { d3d_addref(self_of(this)); *out = this; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
typedef void *(WINAPI *task_alloc_fn)(size_t);
static void *task_alloc(size_t n) {
    static task_alloc_fn fn;
    if (!fn) fn = (task_alloc_fn)vx_find_export("ole32.dll", "CoTaskMemAlloc");
    return fn(n);
}

// ---- the output device ------------------------------------------------------------------------------------------------
static AudioObjectID default_output(void) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectID id = 0; UInt32 size = sizeof id;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, &id);
    return id;
}
static double device_rate(void) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    Float64 rate = 0; UInt32 size = sizeof rate;
    AudioObjectID dev = default_output();
    if (!dev || AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &rate) || rate < 8000) rate = 48000;
    return rate;
}
static void device_name(char *out, size_t cap) {
    AudioObjectPropertyAddress a = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef name = NULL; UInt32 size = sizeof name;
    AudioObjectID dev = default_output();
    if (dev && !AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &name) && name) {
        CFStringGetCString(name, out, (CFIndex)cap, kCFStringEncodingUTF8);
        CFRelease(name);
    } else snprintf(out, cap, "Mac output");
}
static const char *g_device_id = "{0.0.0.00000000}.{56585254-0000-0000-0000-6f7574707574}";

// WAVEFORMATEX(TENSIBLE): tag, channels, rate, bytes per second, block align, bits, extra size; then valid bits,
// channel mask, sub format.
typedef struct __attribute__((packed)) { uint16_t tag, channels; DWORD rate, bytes_per_sec; uint16_t block, bits, extra; } WAVEFORMATEX;
static WAVEFORMATEX *mix_format(void) {
    uint8_t *f = task_alloc(40);
    memset(f, 0, 40);
    WAVEFORMATEX *w = (WAVEFORMATEX *)f;
    w->tag = 0xFFFE; w->channels = 2; w->rate = (DWORD)device_rate(); w->bits = 32; w->block = 8; w->bytes_per_sec = w->rate * 8; w->extra = 22;
    *(uint16_t *)(f + 18) = 32;                               // valid bits
    *(DWORD *)(f + 20) = 3;                                   // front left | front right
    memcpy(f + 24, &SUBTYPE_FLOAT, 16);
    return w;
}
// Whether a format is one Core Audio can take as given: integer or float PCM, interleaved.
static int format_kind(const WAVEFORMATEX *w) {          // 1 integer PCM, 3 float, 0 unsupported
    uint16_t tag = w->tag;
    if (tag == 0xFFFE && w->extra >= 22) {
        const GUID *sub = (const GUID *)((const uint8_t *)w + 24);
        tag = d3d_same_guid(sub, &SUBTYPE_FLOAT) ? 3 : d3d_same_guid(sub, &SUBTYPE_PCM) ? 1 : 0;
    }
    if (tag == 3 && w->bits == 32) return 3;
    if (tag == 1 && (w->bits == 16 || w->bits == 24 || w->bits == 32 || w->bits == 8)) return 1;
    return 0;
}

// ---- the audio client --------------------------------------------------------------------------------------------------
typedef struct {
    d3d_object base;
    WAVEFORMATEX fmt;
    AudioUnit unit;
    uint8_t *ring, *scratch;
    uint32_t frames, frame_bytes;                 // ring size in frames
    volatile uint64_t written, read;              // frame counters (written - read = padding)
    HANDLE event;
    int initialized, started, event_driven;
    float volume; int mute;
    d3d_object *render, *clock, *vol;
} client;

static OSStatus render_callback(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes, AudioBufferList *io) {
    (void)flags; (void)ts; (void)bus;
    client *c = ctx;
    uint8_t *out = io->mBuffers[0].mData;
    uint64_t avail = c->written - c->read;
    uint32_t n = (uint32_t)(avail < nframes ? avail : nframes);
    for (uint32_t i = 0; i < n;) {
        uint32_t pos = (uint32_t)((c->read + i) % c->frames), run = MIN(n - i, c->frames - pos);
        memcpy(out + (size_t)i * c->frame_bytes, c->ring + (size_t)pos * c->frame_bytes, (size_t)run * c->frame_bytes);
        i += run;
    }
    memset(out + (size_t)n * c->frame_bytes, 0, (size_t)(nframes - n) * c->frame_bytes);
    if (n < nframes && c->written) {                            // ran dry: a gap of silence (heard as crackle)
        static uint64_t gaps, gap_frames, since;
        gaps++; gap_frames += nframes - n;
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (!since) since = now;
        if (now - since >= 10000000000ull) {
            vx_log("audio: %llu gaps of silence in the last 10 s (%llu ms missing): the program wrote too late",
                   (unsigned long long)gaps, (unsigned long long)(gap_frames * 1000 / (c->fmt.rate ? c->fmt.rate : 48000)));
            gaps = gap_frames = 0; since = now;
        }
    }
    static int muted = -1;                                      // VXRT_MUTE=1: silence (for unattended runs)
    if (muted < 0) muted = getenv("VXRT_MUTE") && atoi(getenv("VXRT_MUTE"));
    if (muted) memset(out, 0, (size_t)nframes * c->frame_bytes);
    else if (c->mute || c->volume != 1.0f) {
        if (format_kind(&c->fmt) == 3) {
            float *f = (float *)out, v = c->mute ? 0 : c->volume;
            for (size_t i = 0; i < (size_t)n * c->fmt.channels; i++) f[i] *= v;
        } else if (c->mute) memset(out, 0, (size_t)n * c->frame_bytes);
    }
    __atomic_add_fetch(&c->read, n, __ATOMIC_RELEASE);
    if (c->event) vx_set_event(c->event);
    return noErr;
}

static WINAPI HRESULT client_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_IAudioClient); }
static void client_destroy(d3d_object *o) {
    client *c = (client *)o;
    if (c->unit) { AudioOutputUnitStop(c->unit); AudioUnitUninitialize(c->unit); AudioComponentInstanceDispose(c->unit); }
    free(c->ring); free(c->scratch);
}
static WINAPI HRESULT client_initialize(void *this, uint32_t share, DWORD flags, int64_t duration, int64_t period, const WAVEFORMATEX *fmt, const GUID *session) {
    (void)share; (void)period; (void)session;
    client *c = this;
    if (c->initialized) return AUDCLNT_E_ALREADY_INITIALIZED;
    int kind = fmt ? format_kind(fmt) : 0;
    if (!kind || !fmt->channels || !fmt->rate) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    c->fmt = *fmt;
    c->frame_bytes = fmt->block ? fmt->block : (uint32_t)fmt->channels * fmt->bits / 8;
    // At least 60 ms: Core Audio takes ~11 ms a time, and with 20 ms a program woken a little late (Rosetta, a
    // busy Mac) left it short every few callbacks: gaps of silence, heard as crackle in Half-Life: Alyx.
    int64_t min = 600000;
    c->frames = (uint32_t)(((duration > min ? duration : min) * (int64_t)fmt->rate + 9999999) / 10000000);
    c->ring = calloc(c->frames, c->frame_bytes);
    c->scratch = calloc(c->frames, c->frame_bytes);
    c->event_driven = (flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0;
    AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if (!comp || AudioComponentInstanceNew(comp, &c->unit)) return E_FAIL;
    AudioStreamBasicDescription asbd = { 0 };
    asbd.mSampleRate = fmt->rate; asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsPacked | (kind == 3 ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger);
    if (kind == 1 && fmt->bits == 8) asbd.mFormatFlags = kAudioFormatFlagIsPacked;   // 8-bit PCM is unsigned
    asbd.mFramesPerPacket = 1; asbd.mChannelsPerFrame = fmt->channels; asbd.mBitsPerChannel = fmt->bits;
    asbd.mBytesPerFrame = c->frame_bytes; asbd.mBytesPerPacket = c->frame_bytes;
    AURenderCallbackStruct cb = { render_callback, c };
    if (AudioUnitSetProperty(c->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &asbd, sizeof asbd) ||
        AudioUnitSetProperty(c->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) ||
        AudioUnitInitialize(c->unit)) {
        AudioComponentInstanceDispose(c->unit); c->unit = NULL;
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    c->initialized = 1;
    return S_OK;
}
static WINAPI HRESULT client_get_buffer_size(void *this, uint32_t *n) { client *c = this; if (!c->initialized) return AUDCLNT_E_NOT_INITIALIZED; *n = c->frames; return S_OK; }
static WINAPI HRESULT client_get_latency(void *this, int64_t *l) { (void)this; *l = 100000; return S_OK; }
static WINAPI HRESULT client_get_padding(void *this, uint32_t *n) {
    client *c = this;
    if (!c->initialized) return AUDCLNT_E_NOT_INITIALIZED;
    *n = (uint32_t)(c->written - __atomic_load_n(&c->read, __ATOMIC_ACQUIRE));
    return S_OK;
}
static WINAPI HRESULT client_is_format_supported(void *this, uint32_t share, const WAVEFORMATEX *fmt, WAVEFORMATEX **closest) {
    (void)this; (void)share;
    if (closest) *closest = NULL;
    if (fmt && format_kind(fmt)) return S_OK;
    if (closest) *closest = mix_format();
    return closest ? S_FALSE : AUDCLNT_E_UNSUPPORTED_FORMAT;
}
static WINAPI HRESULT client_get_mix_format(void *this, WAVEFORMATEX **out) { (void)this; *out = mix_format(); return S_OK; }
static WINAPI HRESULT client_get_period(void *this, int64_t *def, int64_t *min) { (void)this; if (def) *def = 100000; if (min) *min = 30000; return S_OK; }
static WINAPI HRESULT client_start(void *this) {
    client *c = this;
    if (!c->initialized) return AUDCLNT_E_NOT_INITIALIZED;
    if (c->event_driven && !c->event) return AUDCLNT_E_EVENTHANDLE_NOT_SET;
    if (!c->started && AudioOutputUnitStart(c->unit)) return E_FAIL;
    c->started = 1;
    return S_OK;
}
static WINAPI HRESULT client_stop(void *this) { client *c = this; if (c->started) AudioOutputUnitStop(c->unit); c->started = 0; return S_OK; }
static WINAPI HRESULT client_reset(void *this) { client *c = this; if (c->started) return (HRESULT)0x8889000B; c->read = c->written = 0; return S_OK; }   // AUDCLNT_E_NOT_STOPPED
static WINAPI HRESULT client_set_event(void *this, HANDLE e) { ((client *)this)->event = e; return S_OK; }

// The client's services share it: render client, clock, volume.
typedef struct { d3d_object base; client *owner; } service;
static void service_destroy(d3d_object *o) { d3d_release(&((service *)o)->owner->base); }
static d3d_object *make_service(client *c, void **vtbl) {
    service *s = d3d_alloc(sizeof *s, K_STATE, vtbl, NULL);
    s->owner = c;
    d3d_addref(&c->base);
    s->base.destroy = service_destroy;
    return &s->base;
}
static WINAPI HRESULT client_get_service(void *this, const GUID *iid, void **out) {
    client *c = this;
    if (!c->initialized) return AUDCLNT_E_NOT_INITIALIZED;
    void **vtbl = d3d_same_guid(iid, &IID_IAudioRenderClient) ? g_render_vtbl : d3d_same_guid(iid, &IID_IAudioClock) ? g_clock_vtbl :
                  d3d_same_guid(iid, &IID_ISimpleAudioVolume) ? g_volume_vtbl : NULL;
    if (!vtbl) { *out = NULL; return E_NOINTERFACE; }
    *out = make_service(c, vtbl);
    return S_OK;
}
static client *owner(void *this) { return ((service *)this)->owner; }
static WINAPI HRESULT render_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_IAudioRenderClient); }
static WINAPI HRESULT render_get_buffer(void *this, uint32_t n, uint8_t **data) {
    client *c = owner(this);
    if (n > c->frames - (uint32_t)(c->written - __atomic_load_n(&c->read, __ATOMIC_ACQUIRE))) { *data = NULL; return AUDCLNT_E_BUFFER_TOO_LARGE; }
    *data = c->scratch;
    return S_OK;
}
#define AUDCLNT_BUFFERFLAGS_SILENT 0x2
static WINAPI HRESULT render_release_buffer(void *this, uint32_t n, DWORD flags) {
    client *c = owner(this);
    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) memset(c->scratch, c->fmt.bits == 8 ? 0x80 : 0, (size_t)n * c->frame_bytes);
    for (uint32_t i = 0; i < n;) {
        uint32_t pos = (uint32_t)((c->written + i) % c->frames), run = MIN(n - i, c->frames - pos);
        memcpy(c->ring + (size_t)pos * c->frame_bytes, c->scratch + (size_t)i * c->frame_bytes, (size_t)run * c->frame_bytes);
        i += run;
    }
    __atomic_add_fetch(&c->written, n, __ATOMIC_RELEASE);
    return S_OK;
}
// IAudioClock: positions in frames played.
static WINAPI HRESULT clock_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_IAudioClock); }
static WINAPI HRESULT clock_frequency(void *this, uint64_t *f) { *f = owner(this)->fmt.rate; return S_OK; }
static WINAPI HRESULT clock_position(void *this, uint64_t *pos, uint64_t *qpc) {
    *pos = __atomic_load_n(&owner(this)->read, __ATOMIC_ACQUIRE);
    if (qpc) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); *qpc = (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100; }
    return S_OK;
}
static WINAPI HRESULT clock_characteristics(void *this, DWORD *c) { (void)this; *c = 0; return S_OK; }
static WINAPI HRESULT volume_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_ISimpleAudioVolume); }
static WINAPI HRESULT volume_set(void *this, float v, const GUID *ctx) { (void)ctx; owner(this)->volume = v < 0 ? 0 : v > 1 ? 1 : v; return S_OK; }
static WINAPI HRESULT volume_get(void *this, float *v) { *v = owner(this)->volume; return S_OK; }
static WINAPI HRESULT volume_set_mute(void *this, int m, const GUID *ctx) { (void)ctx; owner(this)->mute = m; return S_OK; }
static WINAPI HRESULT volume_get_mute(void *this, int *m) { *m = owner(this)->mute; return S_OK; }

// ---- devices, the enumerator, the property store ----------------------------------------------------------------------------
static d3d_object *g_device;                                   // the one render endpoint
// IMMEndpoint (the device's direction): Unity asks the device for it and uses the result unchecked.
static const GUID IID_IMMEndpoint = { 0x1be09788, 0x6894, 0x4089, { 0x85, 0x86, 0x9a, 0x2a, 0x6c, 0x26, 0x5a, 0xc5 } };
static d3d_object *g_endpoint;
static WINAPI HRESULT endpoint_qi(void *this, const GUID *iid, void **out) {
    if (out && d3d_same_guid(iid, &IID_IMMDevice)) { d3d_addref(g_device); *out = g_device; return S_OK; }
    return query(this, iid, out, &IID_IMMEndpoint);
}
static WINAPI HRESULT endpoint_get_data_flow(void *this, int *flow) { (void)this; if (!flow) return E_POINTER; *flow = 0; return S_OK; }   // eRender
static void *g_endpoint_vtbl[] = { (void *)endpoint_qi, (void *)obj_addref, (void *)obj_release, (void *)endpoint_get_data_flow };
static WINAPI HRESULT device_qi(void *this, const GUID *iid, void **out) {
    if (out && d3d_same_guid(iid, &IID_IMMEndpoint)) { d3d_addref(g_endpoint); *out = g_endpoint; return S_OK; }
    return query(this, iid, out, &IID_IMMDevice);
}
static WINAPI HRESULT device_activate(void *this, const GUID *iid, DWORD ctx, void *params, void **out) {
    (void)this; (void)ctx; (void)params;
    if (!d3d_same_guid(iid, &IID_IAudioClient) && !d3d_same_guid(iid, &IID_IUnknown_)) { *out = NULL; return E_NOINTERFACE; }
    client *c = d3d_alloc(sizeof *c, K_STATE, g_client_vtbl, NULL);
    c->volume = 1.0f;
    c->base.destroy = client_destroy;
    *out = c;
    return S_OK;
}
static WINAPI HRESULT device_get_id(void *this, WCHAR **out) {
    (void)this;
    size_t n = strlen(g_device_id) + 1;
    *out = task_alloc(n * sizeof(WCHAR));
    vx_utf8_to_utf16(g_device_id, n, *out, n);
    return S_OK;
}
static WINAPI HRESULT device_get_state(void *this, DWORD *state) { (void)this; *state = 1; return S_OK; }   // DEVICE_STATE_ACTIVE
// PROPERTYKEY: format GUID, property id. PROPVARIANT: type, reserved, value.
typedef struct { GUID fmtid; DWORD pid; } PROPERTYKEY;
static WINAPI HRESULT props_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_IPropertyStore); }
static WINAPI HRESULT props_count(void *this, DWORD *n) { (void)this; *n = 1; return S_OK; }
static WINAPI HRESULT props_get_at(void *this, DWORD i, PROPERTYKEY *key) { (void)this; if (i) return E_INVALIDARG; key->fmtid = PKEY_FriendlyName; key->pid = 14; return S_OK; }
static WINAPI HRESULT props_get_value(void *this, const PROPERTYKEY *key, uint8_t *pv) {
    (void)this;
    memset(pv, 0, 24);
    if (!d3d_same_guid(&key->fmtid, &PKEY_FriendlyName) || (key->pid != 14 && key->pid != 2)) return S_OK;   // VT_EMPTY
    char name[256];
    device_name(name, sizeof name);
    size_t n = vx_utf8_to_utf16(name, strlen(name), NULL, 0);
    WCHAR *w = task_alloc((n + 1) * sizeof(WCHAR));
    vx_utf8_to_utf16(name, strlen(name), w, n); w[n] = 0;
    *(uint16_t *)pv = 31;                                       // VT_LPWSTR
    *(WCHAR **)(pv + 8) = w;
    return S_OK;
}
static WINAPI HRESULT device_open_props(void *this, DWORD access, void **out) {
    (void)this; (void)access;
    *out = d3d_alloc(sizeof(d3d_object), K_STATE, g_props_vtbl, NULL);
    return S_OK;
}
typedef struct { d3d_object base; int count; } collection;
static WINAPI HRESULT collection_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_IMMDeviceCollection); }
static WINAPI HRESULT collection_count(void *this, uint32_t *n) { *n = (uint32_t)((collection *)this)->count; return S_OK; }
static WINAPI HRESULT collection_item(void *this, uint32_t i, void **out) {
    if ((int)i >= ((collection *)this)->count) { *out = NULL; return E_INVALIDARG; }
    d3d_addref(g_device); *out = g_device;
    return S_OK;
}
static WINAPI HRESULT enum_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_IMMDeviceEnumerator); }
static WINAPI HRESULT enum_endpoints(void *this, uint32_t flow, DWORD mask, void **out) {
    (void)this;
    collection *c = d3d_alloc(sizeof *c, K_STATE, g_collection_vtbl, NULL);
    c->count = flow != 1 && (mask & 1) ? 1 : 0;                  // eRender (or eAll) and DEVICE_STATE_ACTIVE
    *out = c;
    return S_OK;
}
static WINAPI HRESULT enum_default(void *this, uint32_t flow, uint32_t role, void **out) {
    (void)this; (void)role;
    if (flow == 1) { *out = NULL; return E_NOTFOUND; }          // no capture endpoint yet
    d3d_addref(g_device); *out = g_device;
    return S_OK;
}
static WINAPI HRESULT enum_get_device(void *this, const WCHAR *id, void **out) {
    (void)this;
    char *a = vx_utf8_dup(id);
    int match = a && !strcmp(a, g_device_id);
    free(a);
    if (!match) { *out = NULL; return E_NOTFOUND; }
    d3d_addref(g_device); *out = g_device;
    return S_OK;
}
static WINAPI HRESULT enum_register(void *this, void *client) { (void)this; (void)client; return S_OK; }

static HRESULT create_enumerator(const void *iid, void **out) {
    d3d_object *e = d3d_alloc(sizeof(d3d_object), K_STATE, g_enum_vtbl, NULL);
    HRESULT r = enum_qi(e, iid, out);
    d3d_release(e);
    return r;
}

void vx_com_register_class(const void *clsid, HRESULT (*create)(const void *iid, void **out));
void vx_audio_init(void) {
    static const d3d_method enumerator[] = { { "QueryInterface", (void *)enum_qi }, { "AddRef", (void *)obj_addref }, { "Release", (void *)obj_release },
        { "EnumAudioEndpoints", (void *)enum_endpoints }, { "GetDefaultAudioEndpoint", (void *)enum_default }, { "GetDevice", (void *)enum_get_device },
        { "RegisterEndpointNotificationCallback", (void *)enum_register }, { "UnregisterEndpointNotificationCallback", (void *)enum_register }, { NULL, NULL } };
    static const d3d_method device[] = { { "QueryInterface", (void *)device_qi }, { "AddRef", (void *)obj_addref }, { "Release", (void *)obj_release },
        { "Activate", (void *)device_activate }, { "OpenPropertyStore", (void *)device_open_props }, { "GetId", (void *)device_get_id },
        { "GetState", (void *)device_get_state }, { NULL, NULL } };
    static const d3d_method coll[] = { { "QueryInterface", (void *)collection_qi }, { "AddRef", (void *)obj_addref }, { "Release", (void *)obj_release },
        { "GetCount", (void *)collection_count }, { "Item", (void *)collection_item }, { NULL, NULL } };
    static const d3d_method cl[] = { { "QueryInterface", (void *)client_qi }, { "AddRef", (void *)obj_addref }, { "Release", (void *)obj_release },
        { "Initialize", (void *)client_initialize }, { "GetBufferSize", (void *)client_get_buffer_size }, { "GetStreamLatency", (void *)client_get_latency },
        { "GetCurrentPadding", (void *)client_get_padding }, { "IsFormatSupported", (void *)client_is_format_supported },
        { "GetMixFormat", (void *)client_get_mix_format }, { "GetDevicePeriod", (void *)client_get_period }, { "Start", (void *)client_start },
        { "Stop", (void *)client_stop }, { "Reset", (void *)client_reset }, { "SetEventHandle", (void *)client_set_event },
        { "GetService", (void *)client_get_service }, { NULL, NULL } };
    static const d3d_method render[] = { { "QueryInterface", (void *)render_qi }, { "AddRef", (void *)obj_addref }, { "Release", (void *)obj_release },
        { "GetBuffer", (void *)render_get_buffer }, { "ReleaseBuffer", (void *)render_release_buffer }, { NULL, NULL } };
    static const d3d_method clock[] = { { "QueryInterface", (void *)clock_qi }, { "AddRef", (void *)obj_addref }, { "Release", (void *)obj_release },
        { "GetFrequency", (void *)clock_frequency }, { "GetPosition", (void *)clock_position }, { "GetCharacteristics", (void *)clock_characteristics }, { NULL, NULL } };
    static const d3d_method volume[] = { { "QueryInterface", (void *)volume_qi }, { "AddRef", (void *)obj_addref }, { "Release", (void *)obj_release },
        { "SetMasterVolume", (void *)volume_set }, { "GetMasterVolume", (void *)volume_get }, { "SetMute", (void *)volume_set_mute },
        { "GetMute", (void *)volume_get_mute }, { NULL, NULL } };
    static const d3d_method props[] = { { "QueryInterface", (void *)props_qi }, { "AddRef", (void *)obj_addref }, { "Release", (void *)obj_release },
        { "GetCount", (void *)props_count }, { "GetAt", (void *)props_get_at }, { "GetValue", (void *)props_get_value }, { NULL, NULL } };
    g_enum_vtbl = D3D_VTABLE(IMMDeviceEnumerator, enumerator);
    g_device_vtbl = D3D_VTABLE(IMMDevice, device);
    g_collection_vtbl = D3D_VTABLE(IMMDeviceCollection, coll);
    g_client_vtbl = D3D_VTABLE(IAudioClient, cl);
    g_render_vtbl = D3D_VTABLE(IAudioRenderClient, render);
    g_clock_vtbl = D3D_VTABLE(IAudioClock, clock);
    g_volume_vtbl = D3D_VTABLE(ISimpleAudioVolume, volume);
    g_props_vtbl = D3D_VTABLE(IPropertyStore, props);
    g_device = d3d_alloc(sizeof(d3d_object), K_STATE, g_device_vtbl, NULL);
    g_endpoint = d3d_alloc(sizeof(d3d_object), K_STATE, g_endpoint_vtbl, NULL);
    vx_com_register_class(&CLSID_MMDeviceEnumerator, create_enumerator);
}

#ifdef __aarch64__
#include "thunks/audio.m.inc"
#endif
