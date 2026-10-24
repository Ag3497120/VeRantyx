// winmm with no MIDI devices: none counted, none opened (the mapper finds no port), capabilities of none, and every
// call on a handle refused as an invalid handle; waveOutPause/waveOutRestart likewise, as no waveOut device opens.
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)

int main(void) {
    CHECK(midiInGetNumDevs() == 0);
    CHECK(midiOutGetNumDevs() == 0);

    HMIDIOUT out = NULL;
    HMIDIIN in = NULL;
    CHECK(midiOutOpen(&out, 0, 0, 0, CALLBACK_NULL) == MMSYSERR_BADDEVICEID);
    CHECK(midiOutOpen(&out, MIDI_MAPPER, 0, 0, CALLBACK_NULL) == MIDIERR_NODEVICE);
    CHECK(midiInOpen(&in, 0, 0, 0, CALLBACK_NULL) == MMSYSERR_BADDEVICEID);

    MIDIOUTCAPSW out_caps;
    MIDIINCAPSW in_caps;
    MIDIOUTCAPSA out_caps_a;
    CHECK(midiOutGetDevCapsW(0, &out_caps, sizeof out_caps) == MMSYSERR_BADDEVICEID);
    CHECK(midiOutGetDevCapsA(0, &out_caps_a, sizeof out_caps_a) == MMSYSERR_BADDEVICEID);
    CHECK(midiOutGetDevCapsW(0, &out_caps, 0) == MMSYSERR_NOERROR);   // a zero size copies nothing (documented)
    CHECK(midiInGetDevCapsW(0, &in_caps, sizeof in_caps) == MMSYSERR_BADDEVICEID);

    MIDIHDR header = { 0 };
    char data[4] = { 0 };
    header.lpData = data;
    header.dwBufferLength = sizeof data;
    CHECK(midiOutShortMsg(out, 0x00403c90) == MMSYSERR_INVALHANDLE);
    CHECK(midiOutPrepareHeader(out, &header, sizeof header) == MMSYSERR_INVALHANDLE);
    CHECK(midiOutLongMsg(out, &header, sizeof header) == MMSYSERR_INVALHANDLE);
    CHECK(midiOutUnprepareHeader(out, &header, sizeof header) == MMSYSERR_INVALHANDLE);
    CHECK(midiOutReset(out) == MMSYSERR_INVALHANDLE);
    CHECK(midiOutClose(out) == MMSYSERR_INVALHANDLE);
    CHECK(midiInPrepareHeader(in, &header, sizeof header) == MMSYSERR_INVALHANDLE);
    CHECK(midiInAddBuffer(in, &header, sizeof header) == MMSYSERR_INVALHANDLE);
    CHECK(midiInStart(in) == MMSYSERR_INVALHANDLE);
    CHECK(midiInStop(in) == MMSYSERR_INVALHANDLE);
    CHECK(midiInReset(in) == MMSYSERR_INVALHANDLE);
    CHECK(midiInUnprepareHeader(in, &header, sizeof header) == MMSYSERR_INVALHANDLE);
    CHECK(midiInClose(in) == MMSYSERR_INVALHANDLE);

    // waveOut: no device opens, so there is nothing to pause or restart
    HWAVEOUT wave = NULL;
    WAVEFORMATEX format = { WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0 };
    CHECK(waveOutOpen(&wave, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR);
    CHECK(waveOutPause(wave) == MMSYSERR_INVALHANDLE);
    CHECK(waveOutRestart(wave) == MMSYSERR_INVALHANDLE);

    if (failures) { printf("midi: %d checks failed\n", failures); return 1; }
    puts("midi: no MIDI devices, opens refused as documented, handles invalid, waveOutPause/Restart: ok");
    return 0;
}
