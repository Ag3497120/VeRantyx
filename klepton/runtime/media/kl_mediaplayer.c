// kl_mediaplayer — see kl_mediaplayer.h.
//
// Three stages, all here:
//   1. Read the .mpg and DEMUX the MPEG-1 program stream: pack headers, a system
//      header, then PES packets. The audio lives in the first audio stream id
//      (0xC0..0xDF); its PES payloads, concatenated, are an MPEG-1 Layer II
//      elementary stream.
//   2. Walk that elementary stream frame by frame (each Layer II frame is a self
//      describing unit: an 0xFFF sync, then bitrate/rate/padding that give its
//      byte length and its 1152 samples) to build the packet table AudioQueue
//      needs and to learn the sample rate and channel count.
//   3. Hand the frames to an AudioToolbox AudioQueue declared as
//      kAudioFormatMPEGLayer2. The queue owns the decoder and the output; its
//      callback refills from the packet table, looping or stopping at the end.
//
// Everything is plain C over the AudioToolbox C API — no AVFoundation, which is
// the whole reason this exists (it cannot open an MPEG-1 program stream).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <AudioToolbox/AudioToolbox.h>

#include "kl_mediaplayer.h"

#define KL_MP_NBUF   3
#define KL_MP_PKTS   48            // MPEG frames per queue buffer

struct kl_mediaplayer {
    char        path[1024];
    int         have_source;

    // The demuxed Layer II elementary stream and its frame table.
    unsigned char *es;
    long           es_len;
    AudioStreamPacketDescription *pkts;
    long           npkts;
    double         sample_rate;
    int            channels;
    int            fmt_id;          // kAudioFormatMPEGLayer1/2/3
    int            fpp;             // samples (frames) per packet
    long long      duration_ms;

    AudioQueueRef  queue;
    AudioQueueBufferRef bufs[KL_MP_NBUF];
    int            inflight;

    pthread_mutex_t mu;
    long           next_pkt;        // index of the next frame to enqueue
    int            looping;
    int            started;         // AudioQueueStart has been called
    int            paused;
    int            complete;        // playback drained (and not looping)
    float          volume;
};

// ---------------------------------------------------------------------------
// MPEG audio framing — any version (1 / 2 / 2.5), any layer (I / II / III).
// GTA Vice City feeds MediaPlayer two very different things through the same
// class: raw MP3 files (Layer III) for music/ambient, and the Layer II audio
// carved out of the MPEG-1 program-stream cutscenes. One parser handles both.

// [version_is_mpeg1][layer_index] -> bitrate table (kbps), index 0..15.
// layer_index: 0=Layer III, 1=Layer II, 2=Layer I (matches (h1>>1&3)-1).
static const int kl_mpa_br[2][3][16] = {
  { // MPEG-2 / 2.5
    {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0},   // Layer III
    {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0},   // Layer II
    {0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,0}, // Layer I
  },
  { // MPEG-1
    {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0},  // Layer III
    {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0}, // Layer II
    {0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0}, // Layer I
  },
};
static const int kl_mpa_sr[4][3] = {   // [version][srate_index]; version enc below
    {11025, 12000, 8000},   // 0 = MPEG-2.5
    {0, 0, 0},              // 1 = reserved
    {22050, 24000, 16000},  // 2 = MPEG-2
    {44100, 48000, 32000},  // 3 = MPEG-1
};

// Parse one frame header at `h` (>= 4 bytes). Returns the frame length in bytes
// and, via out-params, its rate / channels / AudioQueue format id / samples per
// packet; 0 if `h` is not a valid MPEG-audio frame sync.
static int kl_mpa_frame(const unsigned char *h, int *rate, int *channels,
                        int *fmt_id, int *fpp) {
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return 0;    // sync
    int ver = (h[1] >> 3) & 3;          // 3=MPEG1 2=MPEG2 0=MPEG2.5 (1=reserved)
    int layer = (h[1] >> 1) & 3;        // 3=Layer I 2=Layer II 1=Layer III
    if (ver == 1 || layer == 0) return 0;
    int br_i = (h[2] >> 4) & 0xF, sr_i = (h[2] >> 2) & 3, pad = (h[2] >> 1) & 1;
    if (br_i == 0 || br_i == 15 || sr_i == 3) return 0;     // free/bad/reserved
    int mpeg1 = (ver == 3);
    int br = kl_mpa_br[mpeg1][layer - 1][br_i] * 1000;      // bps
    int sr = kl_mpa_sr[ver][sr_i];
    if (br == 0 || sr == 0) return 0;

    int len, samples, fmt;
    if (layer == 3) {                                       // Layer I
        len = (12 * br / sr + pad) * 4; samples = 384;  fmt = kAudioFormatMPEGLayer1;
    } else if (layer == 2) {                               // Layer II
        len = 144 * br / sr + pad;      samples = 1152; fmt = kAudioFormatMPEGLayer2;
    } else {                                               // Layer III (MP3)
        int co = mpeg1 ? 144 : 72;
        len = co * br / sr + pad;
        samples = mpeg1 ? 1152 : 576;  fmt = kAudioFormatMPEGLayer3;
    }
    if (rate)     *rate = sr;
    if (channels) *channels = ((h[3] >> 6) & 3) == 3 ? 1 : 2;   // 3 = mono
    if (fmt_id)   *fmt_id = fmt;
    if (fpp)      *fpp = samples;
    return len;
}

// ---------------------------------------------------------------------------
// MPEG-1 program-stream demux: append the first audio stream's PES payloads.

static void kl_mp_demux(const unsigned char *f, long n,
                        unsigned char **out, long *out_len) {
    unsigned char *es = malloc(n);        // the ES is never larger than the file
    long len = 0;
    int  audio_id = -1;
    long p = 0;
    while (p + 4 <= n) {
        if (!(f[p] == 0 && f[p + 1] == 0 && f[p + 2] == 1)) { p++; continue; }
        int id = f[p + 3];
        if (id == 0xBA) { p += 12; continue; }   // MPEG-1 pack header is 12 bytes
        if (id == 0xB9) break;                     // program end
        if (p + 6 > n) break;
        long plen = (f[p + 4] << 8) | f[p + 5];
        long payload = p + 6, end = payload + plen;
        if (end > n) end = n;
        int is_audio = id >= 0xC0 && id <= 0xDF;
        if (is_audio && (audio_id < 0 || id == audio_id)) {
            audio_id = id;
            long q = payload;
            while (q < end && f[q] == 0xFF) q++;              // stuffing
            if (q < end && (f[q] & 0xC0) == 0x40) q += 2;     // STD buffer scale/size
            if (q < end) {
                int c = f[q];
                if      ((c & 0xF0) == 0x20) q += 5;          // PTS
                else if ((c & 0xF0) == 0x30) q += 10;         // PTS + DTS
                else if (c == 0x0F)          q += 1;          // no timestamps
            }
            if (q < end && len + (end - q) <= n) {
                memcpy(es + len, f + q, end - q);
                len += end - q;
            }
        }
        p = end;
    }
    *out = es; *out_len = len;
}

// Build the frame table over the demuxed elementary stream.
static int kl_mp_index(kl_mediaplayer *mp) {
    long cap = 4096;
    mp->pkts = malloc(cap * sizeof *mp->pkts);
    mp->npkts = 0;
    long p = 0, samples = 0; int rate = 0, ch = 0, fmt = 0, fpp = 0;
    while (p + 4 <= mp->es_len) {
        int r = 0, c = 0, ff = 0, pp = 0;
        int flen = kl_mpa_frame(mp->es + p, &r, &c, &ff, &pp);
        if (flen < 4 || p + flen > mp->es_len) { p++; continue; }  // resync
        if (!rate) { rate = r; ch = c; fmt = ff; fpp = pp; }
        else if (ff != fmt) { p += flen; continue; }  // ignore a stray other-layer frame
        if (mp->npkts == cap) { cap *= 2; mp->pkts = realloc(mp->pkts, cap * sizeof *mp->pkts); }
        mp->pkts[mp->npkts].mStartOffset = p;
        mp->pkts[mp->npkts].mVariableFramesInPacket = 0;
        mp->pkts[mp->npkts].mDataByteSize = flen;
        mp->npkts++;
        samples += fpp;
        p += flen;
    }
    if (mp->npkts == 0 || rate == 0) return -1;
    mp->sample_rate = rate;
    mp->channels    = ch;
    mp->fmt_id      = fmt;
    mp->fpp         = fpp;
    mp->duration_ms = (long long)samples * 1000 / rate;
    return 0;
}

// ---------------------------------------------------------------------------
// AudioQueue.

// Fill `buf` with as many whole frames as fit, from mp->next_pkt onward, and
// enqueue it. Caller holds mp->mu. Returns 1 if something was enqueued.
static int kl_mp_fill(kl_mediaplayer *mp, AudioQueueBufferRef buf) {
    AudioStreamPacketDescription descs[KL_MP_PKTS];
    UInt32 nb = 0, np = 0;
    while (np < KL_MP_PKTS && mp->next_pkt < mp->npkts) {
        AudioStreamPacketDescription *s = &mp->pkts[mp->next_pkt];
        if (nb + s->mDataByteSize > buf->mAudioDataBytesCapacity) break;
        memcpy((unsigned char *)buf->mAudioData + nb, mp->es + s->mStartOffset,
               s->mDataByteSize);
        descs[np].mStartOffset = nb;
        descs[np].mVariableFramesInPacket = 0;
        descs[np].mDataByteSize = s->mDataByteSize;
        nb += s->mDataByteSize; np++;
        mp->next_pkt++;
    }
    if (np == 0) return 0;
    buf->mAudioDataByteSize = nb;
    return AudioQueueEnqueueBuffer(mp->queue, buf, np, descs) == noErr;
}

static void kl_mp_cb(void *ref, AudioQueueRef q, AudioQueueBufferRef buf) {
    (void)q;
    kl_mediaplayer *mp = ref;
    pthread_mutex_lock(&mp->mu);
    mp->inflight--;
    if (mp->next_pkt >= mp->npkts) {
        if (mp->looping) mp->next_pkt = 0;      // wrap and keep going
    }
    if (mp->next_pkt < mp->npkts && !mp->paused) {
        if (kl_mp_fill(mp, buf)) mp->inflight++;
    }
    if (mp->inflight == 0 && !mp->looping) {     // drained to the end
        mp->complete = 1;
        AudioQueueStop(mp->queue, false);        // stop once buffers finish
    }
    pthread_mutex_unlock(&mp->mu);
}

// ---------------------------------------------------------------------------

kl_mediaplayer *kl_mediaplayer_new(void) {
    kl_mediaplayer *mp = calloc(1, sizeof *mp);
    if (!mp) return NULL;
    pthread_mutex_init(&mp->mu, NULL);
    mp->volume = 1.0f;
    return mp;
}

int kl_mediaplayer_set_source(kl_mediaplayer *mp, const char *real_path) {
    if (!mp || !real_path) return -1;
    snprintf(mp->path, sizeof mp->path, "%s", real_path);
    mp->have_source = 1;
    return 0;
}

void kl_mediaplayer_prepare(kl_mediaplayer *mp) {
    if (!mp || !mp->have_source || mp->queue) return;   // already prepared

    FILE *fp = fopen(mp->path, "rb");
    if (!fp) { fprintf(stderr, "[mp] cannot open %s\n", mp->path); return; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (n <= 0) { fclose(fp); return; }
    unsigned char *f = malloc(n);
    if (!f) { fclose(fp); return; }
    long got = fread(f, 1, n, fp);
    fclose(fp);
    if (got != n) { free(f); return; }

    // Two shapes reach MediaPlayer: a raw MPEG-audio elementary file (an .mp3,
    // possibly with an ID3v2 tag up front) and an MPEG-1 program stream (the
    // .mpg cutscenes, whose audio must be demuxed out). Tell them apart and
    // produce the elementary stream `es` either way.
    long start = 0;
    if (n > 10 && f[0] == 'I' && f[1] == 'D' && f[2] == '3') {   // skip ID3v2
        long sz = ((long)(f[6] & 0x7F) << 21) | ((f[7] & 0x7F) << 14) |
                  ((f[8] & 0x7F) << 7) | (f[9] & 0x7F);
        start = 10 + sz;
        if (start > n) start = n;
    }
    int is_ps = 0;
    for (long i = start; i + 4 <= n && i < start + 65536; i++)
        if (f[i] == 0 && f[i + 1] == 0 && f[i + 2] == 1 &&
            (f[i + 3] == 0xBA || f[i + 3] == 0xBB)) { is_ps = 1; break; }
    if (is_ps) {
        kl_mp_demux(f, n, &mp->es, &mp->es_len);
    } else {                                    // raw elementary: keep it as-is
        mp->es_len = n - start;
        mp->es = malloc(mp->es_len > 0 ? mp->es_len : 1);
        if (mp->es) memcpy(mp->es, f + start, mp->es_len);
    }
    free(f);
    if (!mp->es || mp->es_len <= 0 || kl_mp_index(mp) != 0) {
        fprintf(stderr, "[mp] %s: no MPEG audio found\n", mp->path);
        free(mp->es); mp->es = NULL;
        free(mp->pkts); mp->pkts = NULL; mp->npkts = 0;
        return;
    }

    AudioStreamBasicDescription asbd = {0};
    asbd.mSampleRate       = mp->sample_rate;
    asbd.mFormatID         = mp->fmt_id;
    asbd.mFramesPerPacket  = mp->fpp;
    asbd.mChannelsPerFrame = mp->channels;
    OSStatus st = AudioQueueNewOutput(&asbd, kl_mp_cb, mp, NULL, NULL, 0, &mp->queue);
    if (st != noErr || !mp->queue) {
        // Most likely a layer this platform has no decoder for (Layer II is the
        // suspect; Layer III / MP3 is universal). The player stays inert; the
        // guest sees isPlaying()==false and moves on.
        const char *lyr = mp->fmt_id == kAudioFormatMPEGLayer3 ? "MP3"
                        : mp->fmt_id == kAudioFormatMPEGLayer2 ? "Layer II" : "Layer I";
        fprintf(stderr, "[mp] AudioQueue(%s) unavailable (OSStatus %d) — %s silent\n",
                lyr, (int)st, mp->path);
        mp->queue = NULL;
        return;
    }
    AudioQueueSetParameter(mp->queue, kAudioQueueParam_Volume, mp->volume);
    // Big enough for KL_MP_PKTS worst-case frames (448 kbps @ 32 kHz ≈ 2016 B).
    UInt32 cap = KL_MP_PKTS * 2048;
    for (int i = 0; i < KL_MP_NBUF; i++)
        AudioQueueAllocateBuffer(mp->queue, cap, &mp->bufs[i]);
    fprintf(stderr, "[mp] %s: %ld frames, fmt %.4s, %.0f Hz, %d ch, %lld ms\n",
            mp->path, mp->npkts, (const char *)&mp->fmt_id, mp->sample_rate,
            mp->channels, mp->duration_ms);
}

void kl_mediaplayer_start(kl_mediaplayer *mp) {
    if (!mp) return;
    if (!mp->queue) kl_mediaplayer_prepare(mp);   // start() implies prepare()
    if (!mp->queue) return;
    pthread_mutex_lock(&mp->mu);
    mp->paused = 0; mp->complete = 0;
    if (!mp->started) {
        for (int i = 0; i < KL_MP_NBUF; i++)
            if (kl_mp_fill(mp, mp->bufs[i])) mp->inflight++;
        mp->started = 1;
    }
    AudioQueueStart(mp->queue, NULL);
    pthread_mutex_unlock(&mp->mu);
}

void kl_mediaplayer_pause(kl_mediaplayer *mp) {
    if (!mp || !mp->queue) return;
    pthread_mutex_lock(&mp->mu); mp->paused = 1; pthread_mutex_unlock(&mp->mu);
    AudioQueuePause(mp->queue);
}

void kl_mediaplayer_stop(kl_mediaplayer *mp) {
    if (!mp || !mp->queue) return;
    AudioQueueStop(mp->queue, true);
    pthread_mutex_lock(&mp->mu);
    mp->started = 0; mp->inflight = 0; mp->next_pkt = 0; mp->paused = 0;
    pthread_mutex_unlock(&mp->mu);
}

void kl_mediaplayer_release(kl_mediaplayer *mp) {
    if (!mp) return;
    if (mp->queue) { AudioQueueStop(mp->queue, true); AudioQueueDispose(mp->queue, true); }
    free(mp->es);
    free(mp->pkts);
    pthread_mutex_destroy(&mp->mu);
    free(mp);
}

void kl_mediaplayer_set_looping(kl_mediaplayer *mp, int on) {
    if (!mp) return;
    pthread_mutex_lock(&mp->mu); mp->looping = on ? 1 : 0; pthread_mutex_unlock(&mp->mu);
}

void kl_mediaplayer_set_volume(kl_mediaplayer *mp, float v) {
    if (!mp) return;
    mp->volume = v;
    if (mp->queue) AudioQueueSetParameter(mp->queue, kAudioQueueParam_Volume, v);
}

void kl_mediaplayer_seek_ms(kl_mediaplayer *mp, int ms) {
    if (!mp || mp->sample_rate <= 0 || mp->npkts == 0) return;
    // Frame-granular seek: land on the frame whose start time is nearest ms.
    long target = (long)((double)ms * mp->sample_rate / 1000.0 / (mp->fpp ? mp->fpp : 1152));
    if (target < 0) target = 0;
    if (target > mp->npkts) target = mp->npkts;
    int wasplaying = mp->queue && !mp->paused && mp->started;
    if (mp->queue) AudioQueueStop(mp->queue, true);
    pthread_mutex_lock(&mp->mu);
    mp->next_pkt = target; mp->inflight = 0; mp->started = 0; mp->complete = 0;
    pthread_mutex_unlock(&mp->mu);
    if (wasplaying) kl_mediaplayer_start(mp);
}

int kl_mediaplayer_position_ms(kl_mediaplayer *mp) {
    if (!mp || !mp->queue || mp->sample_rate <= 0) return 0;
    AudioTimeStamp ts;
    if (AudioQueueGetCurrentTime(mp->queue, NULL, &ts, NULL) == noErr &&
        (ts.mFlags & kAudioTimeStampSampleTimeValid))
        return (int)(ts.mSampleTime * 1000.0 / mp->sample_rate);
    return 0;
}

int kl_mediaplayer_duration_ms(kl_mediaplayer *mp) {
    return mp ? (int)mp->duration_ms : 0;
}

int kl_mediaplayer_is_playing(kl_mediaplayer *mp) {
    if (!mp || !mp->queue) return 0;
    int r;
    pthread_mutex_lock(&mp->mu);
    r = mp->started && !mp->paused && !mp->complete;
    pthread_mutex_unlock(&mp->mu);
    return r;
}
