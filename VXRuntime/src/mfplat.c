// Media Foundation (MFPlat, MF, MFReadWrite) and DXVA2 as a system where Media Foundation cannot be started: MFStartup
// fails with MF_E_DISABLED_IN_SAFEMODE, one of the results Microsoft documents for it, so a program finds out at its
// first call that there is no platform to decode with; the other functions fail with E_NOTIMPL and set their out
// pointers to NULL.
// Steam's web helper loads Valve's winh264.dll (an H.264 decoder on Media Foundation) after sign-in; its first call,
// MFStartup, was a stub that stopped the program, and Steam restarted the helper again and again.
#include <stdint.h>
#include <string.h>
#include "vx.h"

typedef int32_t HRESULT;
#define S_OK 0
#define E_NOTIMPL ((HRESULT)0x80004001)
#define E_POINTER ((HRESULT)0x80004003)
#define MF_E_DISABLED_IN_SAFEMODE ((HRESULT)0xC00D36EF)

static void report(const char *what) {
    static int seen;
    if (!__atomic_exchange_n(&seen, 1, __ATOMIC_RELAXED)) vx_log("Media Foundation: %s fails, as on a system where it cannot be started", what);
}
static WINAPI HRESULT MFStartup(DWORD version, DWORD flags) { (void)version; (void)flags; report("MFStartup"); return MF_E_DISABLED_IN_SAFEMODE; }
static WINAPI HRESULT MFShutdown(void) { return S_OK; }
static HRESULT none(void **out) { if (!out) return E_POINTER; *out = NULL; return E_NOTIMPL; }
static WINAPI HRESULT MFCreateAttributes(void **out, uint32_t size) { (void)size; return none(out); }
static WINAPI HRESULT MFCreateMediaType(void **out) { return none(out); }
static WINAPI HRESULT MFCreateSample(void **out) { return none(out); }
static WINAPI HRESULT MFCreateMemoryBuffer(DWORD max, void **out) { (void)max; return none(out); }
static WINAPI HRESULT MFCreateEventQueue(void **out) { return none(out); }
static WINAPI HRESULT MFCreateStreamDescriptor(DWORD id, DWORD count, void **types, void **out) { (void)id; (void)count; (void)types; return none(out); }
static WINAPI HRESULT MFCreatePresentationDescriptor(DWORD count, void **streams, void **out) { (void)count; (void)streams; return none(out); }
static WINAPI HRESULT MFGetService(void *object, const void *service, const void *iid, void **out) { (void)object; (void)service; (void)iid; return none(out); }
static WINAPI HRESULT MFCreateSourceReaderFromMediaSource(void *source, void *attributes, void **out) { (void)source; (void)attributes; return none(out); }
static WINAPI HRESULT DXVA2CreateDirect3DDeviceManager9(uint32_t *token, void **out) { if (token) *token = 0; return none(out); }

static const vx_export mfplat[] = {
    VX_EXPORT(MFStartup), VX_EXPORT(MFShutdown), VX_EXPORT(MFCreateAttributes), VX_EXPORT(MFCreateMediaType), VX_EXPORT(MFCreateSample),
    VX_EXPORT(MFCreateMemoryBuffer), VX_EXPORT(MFCreateEventQueue), VX_EXPORT(MFCreateStreamDescriptor),
    VX_EXPORT(MFCreatePresentationDescriptor),
};
static const vx_export mf[] = { VX_EXPORT(MFGetService) };
static const vx_export mfreadwrite[] = { VX_EXPORT(MFCreateSourceReaderFromMediaSource) };
static const vx_export dxva2[] = { VX_EXPORT(DXVA2CreateDirect3DDeviceManager9) };
void vx_mfplat_init(void) {
    vx_register_module("mfplat.dll", mfplat, sizeof mfplat / sizeof mfplat[0]);
    vx_register_module("mf.dll", mf, sizeof mf / sizeof mf[0]);
    vx_register_module("mfreadwrite.dll", mfreadwrite, sizeof mfreadwrite / sizeof mfreadwrite[0]);
    vx_register_module("dxva2.dll", dxva2, sizeof dxva2 / sizeof dxva2[0]);
}

#ifdef __aarch64__
#include "thunks/mfplat.c.inc"
#endif
