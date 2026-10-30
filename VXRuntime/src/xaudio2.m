// XAudio2 (2.8 / 2.9) on Core Audio.
//
// An engine mixes in passes of 10 ms on a thread of its own (a Windows thread: voice and engine callbacks are the
// program's code): each started source voice pulls from its queue of buffers (8/16/24/32-bit integer or float PCM,
// resampled by its frequency ratio with linear interpolation, loops and stream ends included), and is mixed
// through its output matrices into submix voices (in processing-stage order) and the mastering voice, whose output
// goes to an output AudioUnit. XAudio2's voices are not COM objects: their method tables have no IUnknown part.
#import <AudioToolbox/AudioToolbox.h>
#include "d3d_common.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "xaudio2_tables.h"
#pragma clang diagnostic pop

static const GUID IID_IUnknown_ = { 0, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID SUBTYPE_PCM = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID SUBTYPE_FLOAT = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
#define XAUDIO2_E_INVALID_CALL ((HRESULT)0x88960001)
#define XAUDIO2_END_OF_STREAM 0x40
#define XAUDIO2_LOOP_INFINITE 255
#define XAUDIO2_VOICE_NOSAMPLESPLAYED 0x100
#define MAX_CHANNELS 8
#define MAX_QUANTUM 1920                // frames per pass (10 ms at up to 192 kHz)

typedef struct __attribute__((packed)) { uint16_t tag, channels; DWORD rate, bytes_per_sec; uint16_t block, bits, extra; } WAVEFORMATEX;
// XAudio2's structures are packed (xaudio2.h has #pragma pack(1)).
typedef struct __attribute__((packed)) { DWORD flags, bytes; const uint8_t *data; DWORD play_begin, play_length, loop_begin, loop_length, loop_count; void *context; } XA_BUFFER;
// IXAudio2VoiceCallback and IXAudio2EngineCallback: method tables of the program's objects, no IUnknown.
typedef struct {
    void (WINAPI *pass_start)(void *, uint32_t);
    void (WINAPI *pass_end)(void *);
    void (WINAPI *stream_end)(void *);
    void (WINAPI *buffer_start)(void *, void *);
    void (WINAPI *buffer_end)(void *, void *);
    void (WINAPI *loop_end)(void *, void *);
    void (WINAPI *error)(void *, void *, HRESULT);
} voice_callback;
typedef struct {
    void (WINAPI *pass_start)(void *);
    void (WINAPI *pass_end)(void *);
    void (WINAPI *critical_error)(void *, HRESULT);
} engine_callback;
#define CALL(obj, fn, ...) (VX_X86FN((*(voice_callback **)(obj))->fn)((obj), ##__VA_ARGS__))

enum { V_SOURCE, V_SUBMIX, V_MASTER };
typedef struct engine engine;
typedef struct voice voice;
typedef struct { voice *to; float matrix[MAX_CHANNELS * MAX_CHANNELS]; } send;
typedef struct queued { XA_BUFFER b; uint32_t loops_left; struct queued *next; } queued;
struct voice {
    void **vtbl;
    int type;
    engine *eng;
    uint32_t channels, rate, stage, flags;
    // source voices
    WAVEFORMATEX fmt; int sample_kind;            // 1 integer PCM, 3 float
    void *callback;
    queued *queue; uint32_t queued_count;
    double pos;                                   // in frames of the current buffer
    int started, buffer_started;
    float ratio, max_ratio;
    double samples_played;                        // in the voice's own frames
    // all voices
    float volume, channel_volume[MAX_CHANNELS];
    send sends[8]; int nsends;
    float *mix;                                   // submix and mastering voices: this pass's input
    voice *next;
};
struct engine {
    d3d_object base;
    pthread_mutex_t lock;
    void *room;                                   // a dispatch semaphore (retained), signalled when the device takes frames (or the engine starts)
    voice *voices, *master;
    void *callbacks[8]; int ncallbacks;
    AudioUnit unit;
    float *ring; uint32_t ring_frames; volatile uint64_t written, read;
    uint32_t quantum, out_channels;               // frames per pass, the device's channels
    pthread_t thread; int thread_running, stop, engine_started;
};

#define ROOM(e) ((__bridge dispatch_semaphore_t)(e)->room)
static void **g_engine_vtbl, **g_source_vtbl, **g_submix_vtbl, **g_master_vtbl;
static void lock(engine *e) { pthread_mutex_lock(&e->lock); }
static void unlock(engine *e) { pthread_mutex_unlock(&e->lock); }

// VXRT_TRACE_AUDIO=1: what a program does with XAudio2 (engines, voices, buffers) and, every 10 s, how loud the
// output was (a game that plays nothing and one whose sound is lost on the way look the same from outside)
static int xa_trace(void) { static int on = -1; if (on < 0) { const char *t = getenv("VXRT_TRACE_AUDIO"); on = t && *t == '1'; } return on; }
static volatile uint64_t g_xa_submitted, g_xa_callbacks;
static volatile float g_xa_peak;

// ---- output -----------------------------------------------------------------------------------------------------------
static OSStatus output_callback(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *io) {
    (void)flags; (void)ts; (void)bus;
    engine *e = ctx;
    uint32_t ch = e->out_channels;
    float *out = io->mBuffers[0].mData;
    uint64_t avail = e->written - e->read;
    uint32_t take = (uint32_t)(avail < n ? avail : n);
    for (uint32_t i = 0; i < take; i++) memcpy(out + (size_t)i * ch, e->ring + (size_t)((e->read + i) % e->ring_frames) * ch, ch * sizeof(float));
    memset(out + (size_t)take * ch, 0, (size_t)(n - take) * ch * sizeof(float));
    static int muted = -1;                                      // VXRT_MUTE=1: silence (for unattended runs)
    if (muted < 0) muted = getenv("VXRT_MUTE") && atoi(getenv("VXRT_MUTE"));
    if (muted) memset(out, 0, (size_t)n * ch * sizeof(float));
    __atomic_add_fetch(&e->read, take, __ATOMIC_RELEASE);
    dispatch_semaphore_signal(ROOM(e));
    if (xa_trace()) {
        float peak = g_xa_peak;
        for (uint32_t i = 0; i < n * ch; i++) { float a = out[i] < 0 ? -out[i] : out[i]; if (a > peak) peak = a; }
        g_xa_peak = peak;
        static double last; double now = CFAbsoluteTimeGetCurrent();
        if (++g_xa_callbacks && now - last >= 10) {
            last = now;
            vx_log("XAudio2: output peak %.3f, %llu buffers submitted, %llu frames written in all%s", peak, (unsigned long long)g_xa_submitted,
                   (unsigned long long)e->written, e->engine_started ? "" : " (engine stopped)");
            g_xa_peak = 0;
        }
    }
    return noErr;
}

// ---- decoding and mixing -----------------------------------------------------------------------------------------------
static int sample_kind(const WAVEFORMATEX *w) {
    uint16_t tag = w->tag;
    if (tag == 0xFFFE && w->extra >= 22) {
        const GUID *sub = (const GUID *)((const uint8_t *)w + 24);
        tag = d3d_same_guid(sub, &SUBTYPE_FLOAT) ? 3 : d3d_same_guid(sub, &SUBTYPE_PCM) ? 1 : 0;
    }
    if (tag == 3 && w->bits == 32) return 3;
    if (tag == 1 && (w->bits == 8 || w->bits == 16 || w->bits == 24 || w->bits == 32)) return 1;
    return 0;
}
static float sample_at(const voice *v, const uint8_t *frame, uint32_t c) {
    const uint8_t *p = frame + c * (v->fmt.bits / 8);
    if (v->sample_kind == 3) { float f; memcpy(&f, p, 4); return f; }
    switch (v->fmt.bits) {
    case 8: return (p[0] - 128) / 128.0f;
    case 16: { int16_t s; memcpy(&s, p, 2); return s / 32768.0f; }
    case 24: { int32_t s = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24); return s / 2147483648.0f; }
    default: { int32_t s; memcpy(&s, p, 4); return s / 2147483648.0f; }
    }
}
// The frames [begin, end) a buffer plays, and its loop region.
static void regions(const voice *v, const XA_BUFFER *b, uint32_t *begin, uint32_t *end, uint32_t *lbegin, uint32_t *lend) {
    uint32_t total = b->bytes / (v->fmt.block ? v->fmt.block : 1);
    *begin = b->play_begin; *end = b->play_length ? b->play_begin + b->play_length : total;
    if (*end > total) *end = total;
    *lbegin = b->loop_begin; *lend = b->loop_length ? b->loop_begin + b->loop_length : *end;
    if (*lend > *end) *lend = *end;
}

// Renders `n` output frames of a source voice into `out` (channels interleaved), consuming its queue.
static void render_source(voice *v, float *out, uint32_t n, uint32_t out_rate) {
    double step = (double)v->ratio * v->rate / out_rate;
    for (uint32_t i = 0; i < n; i++) {
        for (;;) {
            queued *q = v->queue;
            if (!q) { memset(out + (size_t)i * v->channels, 0, v->channels * sizeof(float)); break; }
            uint32_t begin, end, lbegin, lend;
            regions(v, &q->b, &begin, &end, &lbegin, &lend);
            if (!v->buffer_started) {
                v->buffer_started = 1;
                v->pos = begin;
                if (v->callback) CALL(v->callback, buffer_start, q->b.context);
            }
            uint32_t limit = q->loops_left ? lend : end;
            if (v->pos >= limit) {
                if (q->loops_left) {                       // back to the loop's start
                    if (q->loops_left != XAUDIO2_LOOP_INFINITE) q->loops_left--;
                    v->pos = lbegin + (v->pos - limit);
                    if (v->callback) CALL(v->callback, loop_end, q->b.context);
                    continue;
                }
                v->queue = q->next;
                v->queued_count--;
                v->buffer_started = 0;
                void *ctx = q->b.context;
                int eos = q->b.flags & XAUDIO2_END_OF_STREAM;
                free(q);
                if (v->callback) { CALL(v->callback, buffer_end, ctx); if (eos) CALL(v->callback, stream_end); }
                continue;
            }
            uint32_t f0 = (uint32_t)v->pos, f1 = f0 + 1 < limit ? f0 + 1 : f0;
            float t = (float)(v->pos - f0);
            const uint8_t *data = q->b.data;
            for (uint32_t c = 0; c < v->channels; c++) {
                float a = sample_at(v, data + (size_t)f0 * v->fmt.block, c), b = sample_at(v, data + (size_t)f1 * v->fmt.block, c);
                out[(size_t)i * v->channels + c] = a + (b - a) * t;
            }
            v->pos += step;
            v->samples_played += step;
            break;
        }
    }
}
// Mixes a voice's output (n frames, `channels` wide) into its destinations through their matrices.
static void mix_into(voice *v, const float *frames, uint32_t n) {
    for (int s = 0; s < v->nsends; s++) {
        voice *d = v->sends[s].to;
        if (!d || !d->mix) continue;
        const float *m = v->sends[s].matrix;
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t oc = 0; oc < d->channels; oc++) {
                float acc = 0;
                for (uint32_t ic = 0; ic < v->channels; ic++) acc += frames[(size_t)i * v->channels + ic] * v->channel_volume[ic] * m[oc * v->channels + ic];
                d->mix[(size_t)i * d->channels + oc] += acc * v->volume;
            }
    }
}
// The default output matrix: channels map one to one; a mono voice feeds every output channel.
static void default_matrix(float *m, uint32_t in, uint32_t out) {
    memset(m, 0, sizeof(float) * MAX_CHANNELS * MAX_CHANNELS);
    for (uint32_t o = 0; o < out; o++)
        for (uint32_t i = 0; i < in; i++)
            m[o * in + i] = in == 1 ? 1.0f : (i == o ? 1.0f : 0.0f);
    if (in == 2 && out == 1) m[0] = m[1] = 0.5f;
}

static void one_pass(engine *e) {
    uint32_t n = e->quantum;
    for (int i = 0; i < e->ncallbacks; i++) ((*(engine_callback **)e->callbacks[i]))->pass_start(e->callbacks[i]);
    for (voice *v = e->voices; v; v = v->next) if (v->mix) memset(v->mix, 0, (size_t)n * v->channels * sizeof(float));
    float frames[MAX_QUANTUM * MAX_CHANNELS];
    for (voice *v = e->voices; v; v = v->next) {
        if (v->type != V_SOURCE || !v->started) continue;
        if (v->callback) {
            uint32_t needed = (uint32_t)((double)n * v->ratio * v->rate / e->master->rate + 1) * v->fmt.block;
            CALL(v->callback, pass_start, v->queue ? 0 : needed);
        }
        render_source(v, frames, n, e->master->rate);
        mix_into(v, frames, n);
        if (v->callback) CALL(v->callback, pass_end);
    }
    for (uint32_t stage = 0; stage < 4; stage++)               // submixes, lower processing stages first
        for (voice *v = e->voices; v; v = v->next)
            if (v->type == V_SUBMIX && v->stage == stage) mix_into(v, v->mix, n);
    voice *m = e->master;
    for (uint32_t i = 0; i < n; i++) {
        float *dst = e->ring + (size_t)((e->written + i) % e->ring_frames) * m->channels;
        for (uint32_t c = 0; c < m->channels; c++) dst[c] = m->mix[(size_t)i * m->channels + c] * m->volume * m->channel_volume[c];
    }
    __atomic_add_fetch(&e->written, n, __ATOMIC_RELEASE);
    for (int i = 0; i < e->ncallbacks; i++) ((*(engine_callback **)e->callbacks[i]))->pass_end(e->callbacks[i]);
}

// The engine thread: a Windows thread (callbacks run the program's code), mixing while the ring has room.
static void *engine_thread(void *arg) {
    engine *e = arg;
    vx_thread_attach();
    while (!e->stop) {
        lock(e);
        int ready = e->engine_started && e->master && e->written - __atomic_load_n(&e->read, __ATOMIC_ACQUIRE) + e->quantum <= e->ring_frames;
        if (ready) one_pass(e);
        unlock(e);
        if (!ready) dispatch_semaphore_wait(ROOM(e), dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_MSEC));
    }
    vx_thread_detach();
    return NULL;
}

// ---- voices ------------------------------------------------------------------------------------------------------------------
static voice *make_voice(engine *e, int type, uint32_t channels, uint32_t rate, void **vtbl) {
    voice *v = calloc(1, sizeof *v);
    v->vtbl = vtbl; v->type = type; v->eng = e; v->channels = channels; v->rate = rate;
    v->volume = 1.0f; v->ratio = 1.0f;
    for (int c = 0; c < MAX_CHANNELS; c++) v->channel_volume[c] = 1.0f;
    if (type != V_SOURCE) v->mix = calloc((size_t)MAX_QUANTUM * channels, sizeof(float));   // any mastering rate
    return v;
}
// XAUDIO2_VOICE_SENDS: a count and send descriptors (flags, voice).
typedef struct __attribute__((packed)) { DWORD flags; voice *to; } SEND_DESCRIPTOR;
typedef struct __attribute__((packed)) { uint32_t count; const SEND_DESCRIPTOR *sends; } VOICE_SENDS;
static void set_sends(voice *v, const VOICE_SENDS *sends) {
    v->nsends = 0;
    if (!sends) {                                               // the default: the mastering voice
        if (v->type != V_MASTER && v->eng->master) { v->sends[0].to = v->eng->master; default_matrix(v->sends[0].matrix, v->channels, v->eng->master->channels); v->nsends = 1; }
        return;
    }
    for (uint32_t i = 0; i < sends->count && i < 8; i++) {
        voice *to = sends->sends[i].to;
        v->sends[v->nsends].to = to;
        default_matrix(v->sends[v->nsends].matrix, v->channels, to ? to->channels : 2);
        v->nsends++;
    }
}
static voice *vself(void *this) { return this; }
static WINAPI void voice_details(void *this, uint32_t *d) { voice *v = vself(this); d[0] = v->flags; d[1] = v->flags; d[2] = v->channels; d[3] = v->rate; }
static WINAPI HRESULT voice_set_outputs(void *this, const VOICE_SENDS *s) { voice *v = vself(this); lock(v->eng); set_sends(v, s); unlock(v->eng); return S_OK; }
// Effects and filters are accepted but not applied yet.
static WINAPI HRESULT voice_set_effects(void *this, const void *chain) { (void)this; if (chain) d3d_not_implemented("IXAudio2Voice::SetEffectChain (effects)"); return S_OK; }
static WINAPI HRESULT voice_effect_toggle(void *this, uint32_t i, uint32_t op) { (void)this; (void)i; (void)op; return S_OK; }
static WINAPI HRESULT voice_set_filter(void *this, const void *p, uint32_t op) { (void)this; (void)p; (void)op; return S_OK; }
static WINAPI void voice_effect_state(void *this, uint32_t i, int *enabled) { (void)this; (void)i; *enabled = 0; }
static WINAPI HRESULT voice_effect_params(void *this, uint32_t i, const void *p, uint32_t n, uint32_t op) { (void)this; (void)i; (void)p; (void)n; (void)op; return S_OK; }
static WINAPI HRESULT voice_get_effect_params(void *this, uint32_t i, void *p, uint32_t n) { (void)this; (void)i; if (p) memset(p, 0, n); return S_OK; }
// XAUDIO2_FILTER_PARAMETERS: type (LowPassFilter), frequency, 1/Q; the default is a filter that passes everything.
static WINAPI void voice_get_filter(void *this, float *p) { (void)this; memset(p, 0, 4); p[1] = 1.0f; p[2] = 1.0f; }
static WINAPI HRESULT voice_set_output_filter(void *this, void *to, const void *p, uint32_t op) { (void)this; (void)to; (void)p; (void)op; return S_OK; }
static WINAPI void voice_get_output_filter(void *this, void *to, float *p) { (void)to; voice_get_filter(this, p); }
static WINAPI HRESULT voice_set_volume(void *this, float vol, uint32_t op) { (void)op; vself(this)->volume = vol; return S_OK; }
static WINAPI void voice_get_volume(void *this, float *vol) { *vol = vself(this)->volume; }
static WINAPI HRESULT voice_set_channel_volumes(void *this, uint32_t n, const float *vols, uint32_t op) {
    (void)op;
    voice *v = vself(this);
    if (n != v->channels) return E_INVALIDARG;
    for (uint32_t i = 0; i < n && i < MAX_CHANNELS; i++) v->channel_volume[i] = vols[i];
    return S_OK;
}
static WINAPI void voice_get_channel_volumes(void *this, uint32_t n, float *vols) { voice *v = vself(this); for (uint32_t i = 0; i < n && i < MAX_CHANNELS; i++) vols[i] = v->channel_volume[i]; }
static WINAPI HRESULT voice_set_matrix(void *this, voice *to, uint32_t in, uint32_t out, const float *m, uint32_t op) {
    (void)op;
    voice *v = vself(this);
    if (in != v->channels || in > MAX_CHANNELS || out > MAX_CHANNELS) return E_INVALIDARG;
    lock(v->eng);
    for (int s = 0; s < v->nsends; s++)
        if (!to || v->sends[s].to == to) memcpy(v->sends[s].matrix, m, sizeof(float) * in * out);
    unlock(v->eng);
    return S_OK;
}
static WINAPI void voice_get_matrix(void *this, voice *to, uint32_t in, uint32_t out, float *m) {
    voice *v = vself(this);
    for (int s = 0; s < v->nsends; s++) if (!to || v->sends[s].to == to) { memcpy(m, v->sends[s].matrix, sizeof(float) * in * out); return; }
}
static void stop_output(engine *e) {
    if (e->unit) { AudioOutputUnitStop(e->unit); AudioUnitUninitialize(e->unit); AudioComponentInstanceDispose(e->unit); }
    e->unit = NULL;
    free(e->ring); e->ring = NULL;
}
static WINAPI void voice_destroy(void *this) {
    voice *v = vself(this);
    engine *e = v->eng;
    lock(e);
    if (v == e->master) stop_output(e);                          // the device (and the mixer) stop before the voice goes
    for (voice **p = &e->voices; *p; p = &(*p)->next) if (*p == v) { *p = v->next; break; }
    for (voice *o = e->voices; o; o = o->next)                 // nothing sends to it any more
        for (int s = 0; s < o->nsends; s++) if (o->sends[s].to == v) o->sends[s].to = NULL;
    if (e->master == v) e->master = NULL;
    while (v->queue) { queued *q = v->queue; v->queue = q->next; free(q); }
    unlock(e);
    free(v->mix);
    free(v);
}
static WINAPI HRESULT source_start(void *this, uint32_t flags, uint32_t op) { (void)flags; (void)op; vself(this)->started = 1; return S_OK; }
static WINAPI HRESULT source_stop(void *this, uint32_t flags, uint32_t op) { (void)flags; (void)op; vself(this)->started = 0; return S_OK; }
static WINAPI HRESULT source_submit(void *this, const XA_BUFFER *b, const void *wma) {
    __atomic_add_fetch(&g_xa_submitted, 1, __ATOMIC_RELAXED);
    voice *v = vself(this);
    if (wma || !b || !b->data || !b->bytes) return XAUDIO2_E_INVALID_CALL;
    queued *q = calloc(1, sizeof *q);
    q->b = *b;
    q->loops_left = b->loop_count;
    lock(v->eng);
    queued **tail = &v->queue;
    while (*tail) tail = &(*tail)->next;
    *tail = q;
    v->queued_count++;
    unlock(v->eng);
    return S_OK;
}
static WINAPI HRESULT source_flush(void *this) {
    voice *v = vself(this);
    lock(v->eng);
    // The buffer playing (if started) stays; the rest are dropped with their end callbacks, as XAudio2 does.
    queued *keep = v->buffer_started ? v->queue : NULL, *q = keep ? keep->next : v->queue;
    if (keep) keep->next = NULL; else v->queue = NULL;
    while (q) {
        queued *n = q->next;
        if (v->callback) CALL(v->callback, buffer_end, q->b.context);
        free(q);
        v->queued_count--;
        q = n;
    }
    if (!keep) v->queue = NULL;
    unlock(v->eng);
    return S_OK;
}
static WINAPI HRESULT source_discontinuity(void *this) { (void)this; return S_OK; }
static WINAPI HRESULT source_exit_loop(void *this, uint32_t op) { (void)op; voice *v = vself(this); lock(v->eng); if (v->queue) v->queue->loops_left = 0; unlock(v->eng); return S_OK; }
// XAUDIO2_VOICE_STATE (packed): current buffer context, buffers queued, samples played.
static WINAPI void source_get_state(void *this, uint8_t *st, uint32_t flags) {
    voice *v = vself(this);
    lock(v->eng);
    void *ctx = v->queue ? v->queue->b.context : NULL;
    uint64_t n = flags & XAUDIO2_VOICE_NOSAMPLESPLAYED ? 0 : (uint64_t)(v->samples_played + 0.5);
    memcpy(st, &ctx, 8);                                        // packed: 8, 12
    memcpy(st + 8, &v->queued_count, 4);
    memcpy(st + 12, &n, 8);
    unlock(v->eng);
}
static WINAPI HRESULT source_set_ratio(void *this, float r, uint32_t op) { (void)op; voice *v = vself(this); v->ratio = r < 1.0f / 1024 ? 1.0f / 1024 : r > v->max_ratio ? v->max_ratio : r; return S_OK; }
static WINAPI void source_get_ratio(void *this, float *r) { *r = vself(this)->ratio; }
static WINAPI HRESULT source_set_rate(void *this, uint32_t rate) { voice *v = vself(this); if (v->queue) return XAUDIO2_E_INVALID_CALL; v->rate = rate; v->fmt.rate = rate; return S_OK; }
static WINAPI HRESULT master_channel_mask(void *this, DWORD *mask) { voice *v = vself(this); *mask = v->channels == 1 ? 0x4 : v->channels == 2 ? 0x3 : (1u << v->channels) - 1; return S_OK; }

// ---- the engine ---------------------------------------------------------------------------------------------------------------
static engine *eself(void *this) { return this; }
static WINAPI HRESULT engine_qi(void *this, const GUID *iid, void **out) {
    if (d3d_same_guid(iid, &IID_IUnknown_) || d3d_same_guid(iid, &IID_IXAudio2)) { d3d_addref(this); *out = this; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static WINAPI uint32_t engine_addref(void *this) { return d3d_addref(this); }
static WINAPI uint32_t engine_release(void *this) { return d3d_release(this); }
static void engine_destroy(d3d_object *o) {
    engine *e = (engine *)o;
    e->stop = 1;
    dispatch_semaphore_signal(ROOM(e));
    if (e->thread_running) pthread_join(e->thread, NULL);
    stop_output(e);
    while (e->voices) voice_destroy(e->voices);
    CFRelease(e->room);
}
static WINAPI HRESULT engine_register(void *this, void *cb) { engine *e = eself(this); lock(e); if (e->ncallbacks < 8) e->callbacks[e->ncallbacks++] = cb; unlock(e); return S_OK; }
static WINAPI void engine_unregister(void *this, void *cb) {
    engine *e = eself(this);
    lock(e);
    for (int i = 0; i < e->ncallbacks; i++) if (e->callbacks[i] == cb) { e->callbacks[i] = e->callbacks[--e->ncallbacks]; break; }
    unlock(e);
}
static WINAPI HRESULT engine_create_source(void *this, voice **out, const WAVEFORMATEX *fmt, uint32_t flags, float max_ratio, void *callback,
                                           const VOICE_SENDS *sends, const void *effects) {
    engine *e = eself(this);
    int kind = fmt ? sample_kind(fmt) : 0;
    if (!kind || !fmt->channels || fmt->channels > MAX_CHANNELS) { vx_log("XAudio2: source format (tag %#x, %u bits) is not supported yet", fmt ? fmt->tag : 0, fmt ? fmt->bits : 0); return XAUDIO2_E_INVALID_CALL; }
    if (!e->master) return XAUDIO2_E_INVALID_CALL;
    voice *v = make_voice(e, V_SOURCE, fmt->channels, fmt->rate, g_source_vtbl);
    v->fmt = *fmt; v->sample_kind = kind; v->callback = callback; v->flags = flags;
    if (!v->fmt.block) v->fmt.block = (uint16_t)(fmt->channels * fmt->bits / 8);
    v->max_ratio = max_ratio > 0 ? max_ratio : 2.0f;
    set_sends(v, sends);
    if (effects) voice_set_effects(v, effects);
    lock(e); v->next = e->voices; e->voices = v; unlock(e);
    if (xa_trace()) { static int n; if (n++ < 10) vx_log("XAudio2: source voice tag %#x %u ch %u Hz %u bits", fmt->tag, fmt->channels, fmt->rate, fmt->bits); }
    *out = v;
    return S_OK;
}
static WINAPI HRESULT engine_create_submix(void *this, voice **out, uint32_t channels, uint32_t rate, uint32_t flags, uint32_t stage, const VOICE_SENDS *sends, const void *effects) {
    engine *e = eself(this);
    if (!e->master || !channels || channels > MAX_CHANNELS) return XAUDIO2_E_INVALID_CALL;
    voice *v = make_voice(e, V_SUBMIX, channels, rate ? rate : e->master->rate, g_submix_vtbl);
    v->flags = flags; v->stage = stage < 4 ? stage : 3;
    set_sends(v, sends);
    if (effects) voice_set_effects(v, effects);
    lock(e); v->next = e->voices; e->voices = v; unlock(e);
    *out = v;
    return S_OK;
}
static WINAPI HRESULT engine_create_master(void *this, voice **out, uint32_t channels, uint32_t rate, uint32_t flags, const WCHAR *device, const void *effects, uint32_t category) {
    (void)device; (void)category;
    engine *e = eself(this);
    if (e->master) return XAUDIO2_E_INVALID_CALL;
    if (!channels) channels = 2;
    if (!rate) rate = 48000;
    if (channels > MAX_CHANNELS) return XAUDIO2_E_INVALID_CALL;
    e->quantum = rate / 100;
    if (e->quantum > MAX_QUANTUM) e->quantum = MAX_QUANTUM;
    voice *m = make_voice(e, V_MASTER, channels, rate, g_master_vtbl);
    m->flags = flags;
    if (effects) voice_set_effects(m, effects);
    e->ring_frames = e->quantum * 6;
    e->out_channels = channels;
    e->written = e->read = 0;
    e->ring = calloc((size_t)e->ring_frames * channels, sizeof(float));
    AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    AudioStreamBasicDescription asbd = { rate, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, channels * 4, 1, channels * 4, channels, 32, 0 };
    AURenderCallbackStruct cb = { output_callback, e };
    if (!comp || AudioComponentInstanceNew(comp, &e->unit) ||
        AudioUnitSetProperty(e->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &asbd, sizeof asbd) ||
        AudioUnitSetProperty(e->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) ||
        AudioUnitInitialize(e->unit) || AudioOutputUnitStart(e->unit)) {
        vx_log("XAudio2: the output device did not start");
        stop_output(e);
        free(m->mix); free(m);
        return (HRESULT)0x88960002;                              // XAUDIO2_E_DEVICE_INVALIDATED
    }
    lock(e); m->next = e->voices; e->voices = m; e->master = m; unlock(e);
    if (xa_trace()) vx_log("XAudio2: mastering voice %u ch %u Hz started", channels, rate);
    if (!e->thread_running) {
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 1 << 20);
        e->thread_running = pthread_create(&e->thread, &a, engine_thread, e) == 0;
        pthread_attr_destroy(&a);
    }
    *out = m;
    return S_OK;
}
static WINAPI HRESULT engine_start(void *this) { engine *e = eself(this); e->engine_started = 1; dispatch_semaphore_signal(ROOM(e)); return S_OK; }
static WINAPI void engine_stop(void *this) { engine *e = eself(this); lock(e); e->engine_started = 0; unlock(e); }
static WINAPI HRESULT engine_commit(void *this, uint32_t op) { (void)this; (void)op; return S_OK; }   // operation sets apply at once
static WINAPI void engine_perf(void *this, uint8_t *data) { (void)this; memset(data, 0, 64); }
static WINAPI void engine_debug(void *this, const void *cfg, void *r) { (void)this; (void)cfg; (void)r; }

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void make_tables(void) {
    static const d3d_method eng[] = { { "QueryInterface", (void *)engine_qi }, { "AddRef", (void *)engine_addref }, { "Release", (void *)engine_release },
        { "RegisterForCallbacks", (void *)engine_register }, { "UnregisterForCallbacks", (void *)engine_unregister },
        { "CreateSourceVoice", (void *)engine_create_source }, { "CreateSubmixVoice", (void *)engine_create_submix },
        { "CreateMasteringVoice", (void *)engine_create_master }, { "StartEngine", (void *)engine_start }, { "StopEngine", (void *)engine_stop },
        { "CommitChanges", (void *)engine_commit }, { "GetPerformanceData", (void *)engine_perf }, { "SetDebugConfiguration", (void *)engine_debug },
        { NULL, NULL } };
#define VOICE_METHODS \
        { "GetVoiceDetails", (void *)voice_details }, { "SetOutputVoices", (void *)voice_set_outputs }, { "SetEffectChain", (void *)voice_set_effects }, \
        { "EnableEffect", (void *)voice_effect_toggle }, { "DisableEffect", (void *)voice_effect_toggle }, { "SetFilterParameters", (void *)voice_set_filter }, \
        { "GetEffectState", (void *)voice_effect_state }, { "SetEffectParameters", (void *)voice_effect_params }, \
        { "GetEffectParameters", (void *)voice_get_effect_params }, { "GetFilterParameters", (void *)voice_get_filter }, \
        { "SetOutputFilterParameters", (void *)voice_set_output_filter }, { "GetOutputFilterParameters", (void *)voice_get_output_filter }, \
        { "SetVolume", (void *)voice_set_volume }, { "GetVolume", (void *)voice_get_volume }, { "SetChannelVolumes", (void *)voice_set_channel_volumes }, \
        { "GetChannelVolumes", (void *)voice_get_channel_volumes }, { "SetOutputMatrix", (void *)voice_set_matrix }, \
        { "GetOutputMatrix", (void *)voice_get_matrix }, { "DestroyVoice", (void *)voice_destroy }
    static const d3d_method source[] = { VOICE_METHODS, { "Start", (void *)source_start }, { "Stop", (void *)source_stop },
        { "SubmitSourceBuffer", (void *)source_submit }, { "FlushSourceBuffers", (void *)source_flush }, { "Discontinuity", (void *)source_discontinuity },
        { "ExitLoop", (void *)source_exit_loop }, { "GetState", (void *)source_get_state }, { "SetFrequencyRatio", (void *)source_set_ratio },
        { "GetFrequencyRatio", (void *)source_get_ratio }, { "SetSourceSampleRate", (void *)source_set_rate }, { NULL, NULL } };
    static const d3d_method submix[] = { VOICE_METHODS, { NULL, NULL } };
    static const d3d_method master[] = { VOICE_METHODS, { "GetChannelMask", (void *)master_channel_mask }, { NULL, NULL } };
    g_engine_vtbl = D3D_VTABLE(IXAudio2, eng);
    g_source_vtbl = D3D_VTABLE(IXAudio2SourceVoice, source);
    g_submix_vtbl = D3D_VTABLE(IXAudio2SubmixVoice, submix);
    g_master_vtbl = D3D_VTABLE(IXAudio2MasteringVoice, master);
}

static WINAPI HRESULT XAudio2Create(void **out, uint32_t flags, uint32_t processor) {
    (void)flags; (void)processor;
    if (xa_trace()) vx_log("XAudio2: XAudio2Create (flags %#x)", flags);
    pthread_once(&g_once, make_tables);
    engine *e = d3d_alloc(sizeof *e, K_STATE, g_engine_vtbl, NULL);
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&e->lock, &a);
    pthread_mutexattr_destroy(&a);
    e->room = (__bridge_retained void *)dispatch_semaphore_create(0);
    e->engine_started = 1;                                       // XAudio2Create returns a running engine
    e->base.destroy = engine_destroy;
    *out = e;
    return S_OK;
}
static WINAPI HRESULT XAudio2CreateWithVersionInfo(void **out, uint32_t flags, uint32_t processor, DWORD version) { (void)version; return XAudio2Create(out, flags, processor); }

static const vx_export xaudio2[] = { VX_EXPORT(XAudio2Create), VX_EXPORT(XAudio2CreateWithVersionInfo) };
void vx_xaudio2_init(void) {
    vx_register_module("xaudio2_9.dll", xaudio2, sizeof xaudio2 / sizeof xaudio2[0]);
    vx_register_module("xaudio2_9redist.dll", xaudio2, sizeof xaudio2 / sizeof xaudio2[0]);
    vx_register_module("xaudio2_8.dll", xaudio2, sizeof xaudio2 / sizeof xaudio2[0]);
}

#ifdef __aarch64__
#include "thunks/xaudio2.m.inc"
#endif
