// winmm's MIDI functions with no MIDI devices (midiOutGetNumDevs is in mm.c and answers 0 too), and the waveOut calls
// that pause and restart playback. No device can be opened here (waveOutOpen answers MMSYSERR_NODRIVER, midiInOpen and
// midiOutOpen fail below), so no handle a program passes to these can be one: each answers MMSYSERR_INVALHANDLE.
#include "vx.h"

enum {
    MMSYSERR_NOERROR = 0, MMSYSERR_BADDEVICEID = 2, MMSYSERR_INVALHANDLE = 5, MIDIERR_NODEVICE = 68,   // MIDIERR_BASE + 4
};
#define MIDI_MAPPER 0xffffffffu

static WINAPI uint32_t midiInGetNumDevs(void) { return 0; }
// Capabilities of a device that is not there; a zero size copies nothing and succeeds (documented).
static WINAPI uint32_t midi_caps(uintptr_t device, void *caps, uint32_t size) {
    (void)device; (void)caps;
    return size ? MMSYSERR_BADDEVICEID : MMSYSERR_NOERROR;
}
static WINAPI uint32_t midiInOpen(HANDLE *midi, uint32_t device, uintptr_t callback, uintptr_t instance, DWORD flags) {
    (void)midi; (void)device; (void)callback; (void)instance; (void)flags;
    return MMSYSERR_BADDEVICEID;
}
// The mapper finds no port to map to: MIDIERR_NODEVICE, which only the mapper returns; any other id is out of range.
static WINAPI uint32_t midiOutOpen(HANDLE *midi, uint32_t device, uintptr_t callback, uintptr_t instance, DWORD flags) {
    (void)midi; (void)callback; (void)instance; (void)flags;
    return device == MIDI_MAPPER ? MIDIERR_NODEVICE : MMSYSERR_BADDEVICEID;
}
static WINAPI uint32_t no_handle(HANDLE h) { (void)h; return MMSYSERR_INVALHANDLE; }
static WINAPI uint32_t no_handle_header(HANDLE h, void *header, uint32_t size) { (void)h; (void)header; (void)size; return MMSYSERR_INVALHANDLE; }
static WINAPI uint32_t midiOutShortMsg(HANDLE h, DWORD message) { (void)h; (void)message; return MMSYSERR_INVALHANDLE; }

static const vx_export winmm[] = {
    VX_EXPORT(midiInGetNumDevs), { "midiInGetDevCapsW", (void *)midi_caps }, { "midiInGetDevCapsA", (void *)midi_caps },
    { "midiOutGetDevCapsW", (void *)midi_caps }, { "midiOutGetDevCapsA", (void *)midi_caps },
    VX_EXPORT(midiInOpen), VX_EXPORT(midiOutOpen), VX_EXPORT(midiOutShortMsg),
    { "midiInClose", (void *)no_handle }, { "midiInStart", (void *)no_handle }, { "midiInStop", (void *)no_handle },
    { "midiInReset", (void *)no_handle }, { "midiInAddBuffer", (void *)no_handle_header },
    { "midiInPrepareHeader", (void *)no_handle_header }, { "midiInUnprepareHeader", (void *)no_handle_header },
    { "midiOutClose", (void *)no_handle }, { "midiOutReset", (void *)no_handle }, { "midiOutLongMsg", (void *)no_handle_header },
    { "midiOutPrepareHeader", (void *)no_handle_header }, { "midiOutUnprepareHeader", (void *)no_handle_header },
    { "waveOutPause", (void *)no_handle }, { "waveOutRestart", (void *)no_handle },
};
void vx_midi_init(void) {
    vx_register_module("winmm.dll", winmm, sizeof winmm / sizeof winmm[0]);
}

#ifdef __aarch64__
#include "thunks/midi.c.inc"
#endif
