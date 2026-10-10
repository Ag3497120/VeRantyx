// kl_mediaplayer — the host audio player behind android.media.MediaPlayer.
//
// GTA Vice City (reVC) plays its intro/cutscene movies (movies/*.mpg) through
// android.media.MediaPlayer. On Android that decodes the whole MPEG-1 program
// stream — video to a Surface, audio to the mixer. Here the game renders the
// video itself; MediaPlayer is asked only for the AUDIO. Apple's AVFoundation
// cannot open an MPEG-1 program stream at all, so this does it by hand: demux
// the PS, pull the MPEG-1 Layer II audio out of it, and hand that to an
// AudioToolbox AudioQueue, which decodes and plays it against the app's audio
// session (mixing with the in-game sound).
//
// The contract mirrors the small slice of android.media.MediaPlayer the guest
// uses: construct, setDataSource, prepare, start/pause/stop, looping, volume,
// seek, and the three pollers (position, duration, isPlaying). A file we cannot
// open or that carries no MPEG audio leaves the player inert — every call is
// safe, isPlaying() reports false at once, and the guest moves on exactly as it
// did against the earlier no-op stub. Nothing here fabricates a duration or a
// position; they come from the decoded stream or are zero.
#ifndef KL_MEDIAPLAYER_H
#define KL_MEDIAPLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kl_mediaplayer kl_mediaplayer;

kl_mediaplayer *kl_mediaplayer_new(void);
// `real_path` is a host path (the caller has already run it through
// kl_guest_path). Returns 0 if the source was accepted, -1 otherwise; a -1 is
// not fatal — the player just stays silent.
int   kl_mediaplayer_set_source(kl_mediaplayer *mp, const char *real_path);
void  kl_mediaplayer_prepare(kl_mediaplayer *mp);
void  kl_mediaplayer_start(kl_mediaplayer *mp);
void  kl_mediaplayer_pause(kl_mediaplayer *mp);
void  kl_mediaplayer_stop(kl_mediaplayer *mp);
void  kl_mediaplayer_release(kl_mediaplayer *mp);   // stops and frees `mp`
void  kl_mediaplayer_set_looping(kl_mediaplayer *mp, int on);
void  kl_mediaplayer_set_volume(kl_mediaplayer *mp, float v);
void  kl_mediaplayer_seek_ms(kl_mediaplayer *mp, int ms);
int   kl_mediaplayer_position_ms(kl_mediaplayer *mp);
int   kl_mediaplayer_duration_ms(kl_mediaplayer *mp);
int   kl_mediaplayer_is_playing(kl_mediaplayer *mp);

#ifdef __cplusplus
}
#endif
#endif
