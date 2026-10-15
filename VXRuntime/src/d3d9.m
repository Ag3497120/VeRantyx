// Direct3D 9 (IDirect3D9 / IDirect3D9Ex), the object programs ask about adapters and display modes: one adapter
// (this Mac's GPU), the main display's modes, Direct3D 9 capabilities of a shader model 3 part, format and
// multisample checks. Devices are not available (Direct3D 11 renders here), so CreateDevice fails cleanly.
#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>
#include "vx.h"
#include <stdlib.h>
#include <string.h>

typedef int32_t HRESULT;
#define D3D_OK 0
#define D3DERR_NOTAVAILABLE ((HRESULT)0x8876086A)
#define D3DERR_INVALIDCALL ((HRESULT)0x8876086C)
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define D3DFMT_X8R8G8B8 22
#define D3DFMT_A8R8G8B8 21

typedef struct { void **vtbl; LONG refs; int ex; } d3d9;
typedef struct { uint32_t width, height, refresh; } mode;
static mode g_modes[256];
static int g_nmodes;

// The main display's modes (at least its current one).
static void load_modes(void) {
    if (g_nmodes) return;
    CGDirectDisplayID d = CGMainDisplayID();
    CFArrayRef all = CGDisplayCopyAllDisplayModes(d, NULL);
    for (CFIndex i = 0; all && i < CFArrayGetCount(all) && g_nmodes < 256; i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(all, i);
        mode v = { (uint32_t)CGDisplayModeGetWidth(m), (uint32_t)CGDisplayModeGetHeight(m), (uint32_t)(CGDisplayModeGetRefreshRate(m) + 0.5) };
        if (!v.refresh) v.refresh = 60;
        int dup = 0;
        for (int k = 0; k < g_nmodes && !dup; k++) dup = !memcmp(&g_modes[k], &v, sizeof v);
        if (!dup) g_modes[g_nmodes++] = v;
    }
    if (all) CFRelease(all);
    if (!g_nmodes) { g_modes[0] = (mode){ (uint32_t)CGDisplayPixelsWide(d), (uint32_t)CGDisplayPixelsHigh(d), 60 }; g_nmodes = 1; }
}
static mode current_mode(void) {
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID());
    mode v = { (uint32_t)CGDisplayModeGetWidth(m), (uint32_t)CGDisplayModeGetHeight(m), (uint32_t)(CGDisplayModeGetRefreshRate(m) + 0.5) };
    CGDisplayModeRelease(m);
    if (!v.refresh) v.refresh = 60;
    return v;
}

static const uint8_t IID_IUnknown_[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46 };
static const uint8_t IID_IDirect3D9_[16] = { 0xca, 0xcb, 0xbd, 0x81, 0xd4, 0x64, 0x6d, 0x42, 0xae, 0x8d, 0xad, 0x01, 0x47, 0xf4, 0x27, 0x5c };
static const uint8_t IID_IDirect3D9Ex_[16] = { 0x41, 0x72, 0x17, 0x02, 0xfc, 0x69, 0x0c, 0x40, 0x8f, 0xf1, 0x93, 0xa4, 0x4d, 0xf6, 0x86, 0x1d };
static WINAPI HRESULT qi(d3d9 *this, const uint8_t *iid, void **out) {
    if (!memcmp(iid, IID_IUnknown_, 16) || !memcmp(iid, IID_IDirect3D9_, 16) || (this->ex && !memcmp(iid, IID_IDirect3D9Ex_, 16))) {
        __atomic_add_fetch(&this->refs, 1, __ATOMIC_RELAXED); *out = this; return D3D_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static WINAPI uint32_t add_ref(d3d9 *this) { return (uint32_t)__atomic_add_fetch(&this->refs, 1, __ATOMIC_RELAXED); }
static WINAPI uint32_t release(d3d9 *this) {
    LONG r = __atomic_sub_fetch(&this->refs, 1, __ATOMIC_ACQ_REL);
    if (!r) free(this);
    return (uint32_t)(r < 0 ? 0 : r);
}
static WINAPI HRESULT register_software(d3d9 *this, void *fn) { (void)this; (void)fn; return D3D_OK; }
static WINAPI unsigned adapter_count(d3d9 *this) { (void)this; return 1; }
// D3DADAPTER_IDENTIFIER9: driver, description, device name, driver version, vendor/device/subsystem/revision,
// device GUID, WHQL level.
static WINAPI HRESULT adapter_identifier(d3d9 *this, unsigned adapter, DWORD flags, uint8_t *ident) {
    (void)this; (void)flags;
    if (adapter) return D3DERR_INVALIDCALL;
    memset(ident, 0, 1104);
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    snprintf((char *)ident, 512, "vxrt-metal.dll");
    snprintf((char *)ident + 512, 512, "%s", dev ? dev.name.UTF8String : "Apple GPU");
    snprintf((char *)ident + 1024, 32, "\\\\.\\DISPLAY1");
    *(uint64_t *)(ident + 1056) = (31ull << 48) | (0ull << 32) | (101ull << 16) | 1;   // 31.0.101.1
    *(DWORD *)(ident + 1064) = 0x106b;                               // Apple
    *(DWORD *)(ident + 1068) = 0x1;
    memcpy(ident + 1080, "VeRantyxMetalGPU", 16);
    *(DWORD *)(ident + 1096) = 1;
    return D3D_OK;
}
static WINAPI unsigned mode_count(d3d9 *this, unsigned adapter, DWORD format) {
    (void)this;
    if (adapter || (format != D3DFMT_X8R8G8B8 && format != D3DFMT_A8R8G8B8)) return 0;
    load_modes();
    return (unsigned)g_nmodes;
}
// D3DDISPLAYMODE: width, height, refresh rate, format.
static WINAPI HRESULT enum_modes(d3d9 *this, unsigned adapter, DWORD format, unsigned i, DWORD *m) {
    if (i >= mode_count(this, adapter, format)) return D3DERR_INVALIDCALL;
    m[0] = g_modes[i].width; m[1] = g_modes[i].height; m[2] = g_modes[i].refresh; m[3] = format;
    return D3D_OK;
}
static WINAPI HRESULT display_mode(d3d9 *this, unsigned adapter, DWORD *m) {
    (void)this;
    if (adapter) return D3DERR_INVALIDCALL;
    mode c = current_mode();
    m[0] = c.width; m[1] = c.height; m[2] = c.refresh; m[3] = D3DFMT_X8R8G8B8;
    return D3D_OK;
}
static WINAPI HRESULT check_type(d3d9 *this, unsigned adapter, DWORD type, DWORD fmt, DWORD back, BOOL windowed) { (void)this; (void)type; (void)fmt; (void)back; (void)windowed; return adapter ? D3DERR_INVALIDCALL : D3D_OK; }
static WINAPI HRESULT check_format(d3d9 *this, unsigned adapter, DWORD type, DWORD fmt, DWORD usage, DWORD rtype, DWORD check) { (void)this; (void)type; (void)fmt; (void)usage; (void)rtype; (void)check; return adapter ? D3DERR_INVALIDCALL : D3D_OK; }
static WINAPI HRESULT check_multisample(d3d9 *this, unsigned adapter, DWORD type, DWORD fmt, BOOL windowed, DWORD samples, DWORD *quality) {
    (void)this; (void)type; (void)fmt; (void)windowed;
    if (adapter) return D3DERR_INVALIDCALL;
    if (quality) *quality = 1;
    return samples <= 1 || samples == 2 || samples == 4 || samples == 8 ? D3D_OK : D3DERR_NOTAVAILABLE;
}
static WINAPI HRESULT check_depth(d3d9 *this, unsigned adapter, DWORD type, DWORD fmt, DWORD rt, DWORD ds) { (void)this; (void)type; (void)fmt; (void)rt; (void)ds; return adapter ? D3DERR_INVALIDCALL : D3D_OK; }
static WINAPI HRESULT check_conversion(d3d9 *this, unsigned adapter, DWORD type, DWORD from, DWORD to) { (void)this; (void)type; (void)from; (void)to; return adapter ? D3DERR_INVALIDCALL : D3D_OK; }
// D3DCAPS9 of a shader model 3 HAL device (offsets as in d3d9caps.h).
static WINAPI HRESULT device_caps(d3d9 *this, unsigned adapter, DWORD type, uint8_t *c) {
    (void)this;
    if (adapter) return D3DERR_INVALIDCALL;
    memset(c, 0, 304);
#define SET(off, v) (*(DWORD *)(c + (off)) = (DWORD)(v))
    SET(0, type ? type : 1); SET(8, 0x20000); SET(12, 0xe0000000u); SET(20, 0x8000000fu);
    SET(28, 0x001bbef0); SET(32, 0x2fced2); SET(36, 0x07732191); SET(40, 0xff); SET(44, 0x3fff); SET(48, 0x3fff);
    SET(52, 0xff); SET(56, 0xff); SET(60, 0x1ec45); SET(64, 0x3030700); SET(68, 0x3030700); SET(72, 0x3030700); SET(76, 0x3f); SET(80, 0x3f);
    SET(88, 16384); SET(92, 16384); SET(96, 2048); SET(100, 8192); SET(104, 8192); SET(108, 16);
    *(float *)(c + 112) = 1e10f;                                    // MaxVertexW
    SET(136, 0x1ff); SET(140, 0x100008); SET(144, 0x3feffff); SET(148, 8); SET(152, 8); SET(156, 0x17b); SET(160, 8);
    SET(164, 8); SET(168, 4); SET(172, 255); *(float *)(c + 176) = 8192.0f; SET(180, 0xffffff); SET(184, 0xffffff); SET(188, 16); SET(192, 508);
    SET(196, 0xfffe0300); SET(200, 256); SET(204, 0xffff0300); *(float *)(c + 208) = 65504.0f;
    SET(212, 0x31b); SET(240, 4); SET(244, 0x3000300);
    SET(248, 1); SET(252, 24); SET(256, 32); SET(260, 4);           // VS20Caps
    SET(264, 0x1f); SET(268, 24); SET(272, 32); SET(276, 4); SET(280, 512);   // PS20Caps
    SET(284, 0x3000300); SET(288, 65535); SET(292, 65535); SET(296, 32768); SET(300, 32768);
#undef SET
    return D3D_OK;
}
static WINAPI void *adapter_monitor(d3d9 *this, unsigned adapter) {
    (void)this; (void)adapter;
    void *(WINAPI *from_point)(int64_t, DWORD) = (void *(WINAPI *)(int64_t, DWORD))vx_find_export("user32.dll", "MonitorFromPoint");
    return from_point ? from_point(0, 1) : NULL;
}
static WINAPI HRESULT create_device(d3d9 *this, unsigned adapter, DWORD type, void *wnd, DWORD flags, void *pp, void **out) {
    (void)this; (void)adapter; (void)type; (void)wnd; (void)flags; (void)pp;
    vx_log("Direct3D 9: devices are not available (Direct3D 11 is)");
    if (out) *out = NULL;
    return D3DERR_NOTAVAILABLE;
}
// IDirect3D9Ex. D3DDISPLAYMODEFILTER: size, format, scanline ordering; D3DDISPLAYMODEEX: size, width, height,
// refresh, format, scanline ordering.
static WINAPI unsigned mode_count_ex(d3d9 *this, unsigned adapter, const DWORD *filter) { return mode_count(this, adapter, filter ? filter[1] : D3DFMT_X8R8G8B8); }
static WINAPI HRESULT enum_modes_ex(d3d9 *this, unsigned adapter, const DWORD *filter, unsigned i, DWORD *m) {
    DWORD fmt = filter ? filter[1] : D3DFMT_X8R8G8B8;
    if (i >= mode_count(this, adapter, fmt)) return D3DERR_INVALIDCALL;
    m[1] = g_modes[i].width; m[2] = g_modes[i].height; m[3] = g_modes[i].refresh; m[4] = fmt; m[5] = 1;
    return D3D_OK;
}
static WINAPI HRESULT display_mode_ex(d3d9 *this, unsigned adapter, DWORD *m, DWORD *rotation) {
    (void)this;
    if (adapter) return D3DERR_INVALIDCALL;
    mode c = current_mode();
    if (m) { m[1] = c.width; m[2] = c.height; m[3] = c.refresh; m[4] = D3DFMT_X8R8G8B8; m[5] = 1; }
    if (rotation) *rotation = 1;                                    // D3DDISPLAYROTATION_IDENTITY
    return D3D_OK;
}
static WINAPI HRESULT create_device_ex(d3d9 *this, unsigned adapter, DWORD type, void *wnd, DWORD flags, void *pp, void *mode, void **out) {
    (void)mode;
    return create_device(this, adapter, type, wnd, flags, pp, out);
}
static WINAPI HRESULT adapter_luid(d3d9 *this, unsigned adapter, DWORD *luid) { (void)this; if (adapter) return D3DERR_INVALIDCALL; luid[0] = 0x1234; luid[1] = 0; return D3D_OK; }

static void *g_vtbl[] = {
    (void *)qi, (void *)add_ref, (void *)release, (void *)register_software, (void *)adapter_count, (void *)adapter_identifier,
    (void *)mode_count, (void *)enum_modes, (void *)display_mode, (void *)check_type, (void *)check_format, (void *)check_multisample,
    (void *)check_depth, (void *)check_conversion, (void *)device_caps, (void *)adapter_monitor, (void *)create_device,
    (void *)mode_count_ex, (void *)enum_modes_ex, (void *)display_mode_ex, (void *)create_device_ex, (void *)adapter_luid,
};
static d3d9 *make(int ex) {
    d3d9 *d = calloc(1, sizeof *d);
    d->vtbl = g_vtbl; d->refs = 1; d->ex = ex;
    return d;
}
static WINAPI void *Direct3DCreate9(unsigned version) { (void)version; return make(0); }
static WINAPI HRESULT Direct3DCreate9Ex(unsigned version, void **out) { (void)version; *out = make(1); return D3D_OK; }

static const vx_export d3d9_exports[] = { VX_EXPORT(Direct3DCreate9), VX_EXPORT(Direct3DCreate9Ex) };
void vx_d3d9_init(void) { vx_register_module("d3d9.dll", d3d9_exports, sizeof d3d9_exports / sizeof d3d9_exports[0]); }

#ifdef __aarch64__
#include "thunks/d3d9.m.inc"
#endif
