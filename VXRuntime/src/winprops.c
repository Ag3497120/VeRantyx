// shell32's SHGetPropertyStoreForWindow: a window's property store (IPropertyStore), where programs put the window's
// AppUserModelID, relaunch command and icon for the taskbar. macOS has no taskbar grouping by these, so each window's
// store keeps the values it is given (GetValue returns them) and Commit has nothing further to do.
// Steam's Chromium (steamwebhelper) delay-loads it when it creates its windows; it stopped with 0xc06d007f
// ("procedure not found") while it was missing.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "vx.h"

typedef int32_t HRESULT;
typedef struct { DWORD d1; uint16_t d2, d3; uint8_t d4[8]; } GUID;
typedef struct { GUID fmtid; DWORD pid; } PROPERTYKEY;
typedef struct { uint16_t vt, r1, r2, r3; uint64_t data[2]; } PROPVARIANT;    // 24 bytes on x64
#define S_OK 0
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define E_POINTER ((HRESULT)0x80004003)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define E_OUTOFMEMORY ((HRESULT)0x8007000E)
#define VT_LPWSTR 31
#define VT_BSTR 8

static const GUID IID_IUnknown = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IPropertyStore = { 0x886d8eeb, 0x8cf2, 0x4446, { 0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99 } };

typedef struct { PROPERTYKEY key; PROPVARIANT value; } prop;
typedef struct store {
    const void *const *vtbl;
    LONG refs;
    HANDLE wnd;
    int count;
    prop props[32];
} store;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// Values are kept as copies: strings duplicated (VT_LPWSTR, the form these properties take), the rest as they are.
static HRESULT copy_value(PROPVARIANT *to, const PROPVARIANT *from) {
    *to = *from;
    if (from->vt == VT_LPWSTR && from->data[0]) {
        const WCHAR *s = (const WCHAR *)(uintptr_t)from->data[0];
        size_t n = 0;
        while (s[n]) n++;
        void *(WINAPI *alloc)(size_t) = (void *(WINAPI *)(size_t))vx_find_export("ole32.dll", "CoTaskMemAlloc");
        WCHAR *d = alloc ? alloc((n + 1) * 2) : NULL;
        if (!d) return E_OUTOFMEMORY;
        memcpy(d, s, (n + 1) * 2);
        to->data[0] = (uint64_t)(uintptr_t)d;
    } else if (from->vt != 0 && from->vt != 1 && (from->vt < 2 || from->vt > 23 || from->vt == VT_BSTR || from->vt == 9 || from->vt == 13)) {
        memset(to, 0, sizeof *to);                                // (pointers we cannot copy: not kept)
    }
    return S_OK;
}
static void clear_value(PROPVARIANT *v) {
    if (v->vt == VT_LPWSTR && v->data[0]) {
        void (WINAPI *free_)(void *) = (void (WINAPI *)(void *))vx_find_export("ole32.dll", "CoTaskMemFree");
        if (free_) free_((void *)(uintptr_t)v->data[0]);
    }
    memset(v, 0, sizeof *v);
}

static WINAPI HRESULT ps_qi(store *s, const GUID *iid, void **out) {
    if (!out) return E_POINTER;
    if (!memcmp(iid, &IID_IUnknown, sizeof *iid) || !memcmp(iid, &IID_IPropertyStore, sizeof *iid)) {
        __sync_add_and_fetch(&s->refs, 1);
        *out = s;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static WINAPI DWORD ps_addref(store *s) { return (DWORD)__sync_add_and_fetch(&s->refs, 1); }
static WINAPI DWORD ps_release(store *s) {
    LONG r = __sync_sub_and_fetch(&s->refs, 1);
    if (!r) {
        for (int i = 0; i < s->count; i++) clear_value(&s->props[i].value);
        free(s);
    }
    return (DWORD)r;
}
static WINAPI HRESULT ps_count(store *s, DWORD *n) { if (!n) return E_POINTER; *n = (DWORD)s->count; return S_OK; }
static WINAPI HRESULT ps_at(store *s, DWORD i, PROPERTYKEY *key) {
    if (!key) return E_POINTER;
    if (i >= (DWORD)s->count) return E_INVALIDARG;
    *key = s->props[i].key;
    return S_OK;
}
static int find(store *s, const PROPERTYKEY *k) {
    for (int i = 0; i < s->count; i++) if (!memcmp(&s->props[i].key, k, sizeof *k)) return i;
    return -1;
}
static WINAPI HRESULT ps_get(store *s, const PROPERTYKEY *key, PROPVARIANT *v) {
    if (!key || !v) return E_POINTER;
    memset(v, 0, sizeof *v);                                      // VT_EMPTY: not set
    pthread_mutex_lock(&g_lock);
    int i = find(s, key);
    HRESULT hr = i >= 0 ? copy_value(v, &s->props[i].value) : S_OK;
    pthread_mutex_unlock(&g_lock);
    return hr;
}
static WINAPI HRESULT ps_set(store *s, const PROPERTYKEY *key, const PROPVARIANT *v) {
    if (!key || !v) return E_POINTER;
    pthread_mutex_lock(&g_lock);
    int i = find(s, key);
    HRESULT hr = S_OK;
    if (i < 0 && s->count < 32) { i = s->count++; s->props[i].key = *key; memset(&s->props[i].value, 0, sizeof s->props[i].value); }
    if (i >= 0) {
        PROPVARIANT copy;
        hr = copy_value(&copy, v);
        if (hr == S_OK) { clear_value(&s->props[i].value); s->props[i].value = copy; }
    } else hr = E_OUTOFMEMORY;
    pthread_mutex_unlock(&g_lock);
    return hr;
}
static WINAPI HRESULT ps_commit(store *s) { (void)s; return S_OK; }
static const void *const g_vtbl[] = { (void *)ps_qi, (void *)ps_addref, (void *)ps_release, (void *)ps_count, (void *)ps_at,
                                      (void *)ps_get, (void *)ps_set, (void *)ps_commit };

// One store per window, kept while the program holds it: a later call for the same window sees the values set.
static store *g_stores[256];
static int g_store_count;
static WINAPI HRESULT SHGetPropertyStoreForWindow(HANDLE wnd, const GUID *iid, void **out) {
    if (!out) return E_POINTER;
    *out = NULL;
    BOOL (WINAPI *is_window)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("user32.dll", "IsWindow");
    if (!wnd || (is_window && !is_window(wnd))) return E_INVALIDARG;
    pthread_mutex_lock(&g_lock);
    store *s = NULL;
    for (int i = 0; i < g_store_count; i++) {
        if (g_stores[i]->wnd == wnd) { s = g_stores[i]; break; }
    }
    if (!s) {
        s = calloc(1, sizeof *s);
        s->vtbl = g_vtbl; s->refs = 1; s->wnd = wnd;              // (the list's reference)
        if (g_store_count < 256) g_stores[g_store_count++] = s;
        else { s->refs = 0; }
    }
    pthread_mutex_unlock(&g_lock);
    HRESULT hr = ps_qi(s, iid, out);
    if (!s->refs) free(s);
    return hr;
}

// propsys's PSCreateMemoryPropertyStore: the same store, belonging to no window (freed with its last reference).
static WINAPI HRESULT PSCreateMemoryPropertyStore(const GUID *iid, void **out) {
    if (!out) return E_POINTER;
    *out = NULL;
    store *s = calloc(1, sizeof *s);
    if (!s) return E_OUTOFMEMORY;
    s->vtbl = g_vtbl;
    HRESULT hr = ps_qi(s, iid, out);
    if (hr != S_OK) free(s);
    return hr;
}
// PSGetPropertyKeyFromName: the property system's schema of names is not offered (TYPE_E_ELEMENTNOTFOUND).
static WINAPI HRESULT PSGetPropertyKeyFromName(const WCHAR *name, PROPERTYKEY *key) {
    (void)name;
    if (key) memset(key, 0, sizeof *key);
    return (HRESULT)0x8002802B;
}
// InitPropVariantFromCLSID: VT_CLSID with a copy of the GUID (CoTaskMemAlloc'd, as PropVariantClear frees it).
static WINAPI HRESULT InitPropVariantFromCLSID(const GUID *clsid, PROPVARIANT *v) {
    if (!clsid || !v) return E_INVALIDARG;
    void *(WINAPI *alloc)(size_t) = (void *(WINAPI *)(size_t))vx_find_export("ole32.dll", "CoTaskMemAlloc");
    GUID *g = alloc ? alloc(sizeof *g) : NULL;
    memset(v, 0, sizeof *v);
    if (!g) return E_OUTOFMEMORY;
    *g = *clsid;
    v->vt = 72;                                                   // VT_CLSID
    v->data[0] = (uint64_t)(uintptr_t)g;
    return S_OK;
}

// SHCreateItemFromParsingName: shell items (IShellItem) are not offered; the documented failure for a name that cannot
// be parsed. Chromium delay-loads it (file dialogs, jump lists) and handles the failure.
static WINAPI HRESULT SHCreateItemFromParsingName(const WCHAR *name, void *bind, const GUID *iid, void **out) {
    (void)name; (void)bind; (void)iid;
    if (!out) return E_POINTER;
    *out = NULL;
    return (HRESULT)0x80070002;                                   // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
}

// ---- shell32's VirtualDesktopManager (IVirtualDesktopManager) ---------------------------------------------------------------
// macOS's Spaces are not visible to Windows programs: there is one virtual desktop, every window is on it, and moving a
// window "to" it succeeds. Steam's Chromium asks for this class when it shows a window, and stopped on a CHECK while
// CoCreateInstance refused it.
static const GUID CLSID_VirtualDesktopManager = { 0xaa509086, 0x5ca9, 0x4c25, { 0x8f, 0x95, 0x58, 0x9d, 0x3c, 0x07, 0xb4, 0x8a } };
static const GUID IID_IVirtualDesktopManager = { 0xa5cd92ff, 0x29be, 0x454c, { 0x8d, 0x04, 0xd8, 0x28, 0x79, 0xfb, 0x3f, 0x1b } };
static const GUID g_the_desktop = { 0x7658c0b5, 0x1d2e, 0x4b3a, { 0x9e, 0x51, 0x56, 0x58, 0x52, 0x54, 0x00, 0x01 } };   // (made up, fixed)
static BOOL top_level_window(HANDLE wnd) {
    BOOL (WINAPI *is_window)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("user32.dll", "IsWindow");
    return wnd && (!is_window || is_window(wnd));
}
static WINAPI HRESULT vdm_qi(void *self, const GUID *iid, void **out) {
    if (!out) return E_POINTER;
    if (!memcmp(iid, &IID_IUnknown, sizeof *iid) || !memcmp(iid, &IID_IVirtualDesktopManager, sizeof *iid)) { *out = self; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static WINAPI DWORD vdm_addref(void *self) { (void)self; return 2; }      // (one object for the process, never freed)
static WINAPI DWORD vdm_release(void *self) { (void)self; return 1; }
static WINAPI HRESULT vdm_on_current(void *self, HANDLE wnd, BOOL *on) {
    (void)self;
    if (!on) return E_POINTER;
    if (!top_level_window(wnd)) return E_INVALIDARG;
    *on = 1;
    return S_OK;
}
static WINAPI HRESULT vdm_desktop_id(void *self, HANDLE wnd, GUID *id) {
    (void)self;
    if (!id) return E_POINTER;
    if (!top_level_window(wnd)) return E_INVALIDARG;
    *id = g_the_desktop;
    return S_OK;
}
static WINAPI HRESULT vdm_move(void *self, HANDLE wnd, const GUID *id) {
    (void)self;
    if (!top_level_window(wnd) || !id) return E_INVALIDARG;
    return memcmp(id, &g_the_desktop, sizeof *id) ? E_INVALIDARG : S_OK;    // (no other desktop exists)
}
static const void *const g_vdm_vtbl[] = { (void *)vdm_qi, (void *)vdm_addref, (void *)vdm_release, (void *)vdm_on_current,
                                          (void *)vdm_desktop_id, (void *)vdm_move };
static struct { const void *const *vtbl; } g_vdm = { g_vdm_vtbl };
static HRESULT create_vdm(const void *iid, void **out) { return vdm_qi(&g_vdm, iid, out); }
void vx_com_register_class(const void *clsid, HRESULT (*create)(const void *iid, void **out));

// ---- user32: window placement, GUI object counts ----------------------------------------------------------------------------
// SetWindowPlacement: the window moved to rcNormalPosition and shown as showCmd says (WINDOWPLACEMENT: length, flags,
// showCmd at 8, ptMinPosition, ptMaxPosition, rcNormalPosition at 28).
static WINAPI BOOL SetWindowPlacement(HANDLE wnd, const uint8_t *wp) {
    BOOL (WINAPI *set_pos)(HANDLE, HANDLE, int, int, int, int, DWORD) =
        (BOOL (WINAPI *)(HANDLE, HANDLE, int, int, int, int, DWORD))vx_find_export("user32.dll", "SetWindowPos");
    BOOL (WINAPI *show)(HANDLE, int) = (BOOL (WINAPI *)(HANDLE, int))vx_find_export("user32.dll", "ShowWindow");
    if (!wp || *(const DWORD *)wp != 44) { vx_set_last_error(87); return 0; }   // (length must be sizeof(WINDOWPLACEMENT))
    int32_t r[4];
    memcpy(r, wp + 28, sizeof r);
    if (!set_pos || !set_pos(wnd, NULL, r[0], r[1], r[2] - r[0], r[3] - r[1], 0x0004 | 0x0010 /* SWP_NOZORDER | SWP_NOACTIVATE */)) return 0;
    if (show) show(wnd, (int)*(const DWORD *)(wp + 8));
    return 1;
}
// GetGuiResources: the counts of GDI and USER objects a process holds; not kept per process (0, as for a failure).
static WINAPI DWORD GetGuiResources(HANDLE process, DWORD flags) { (void)process; (void)flags; return 0; }

// SHOpenWithDialog: Windows' "Open with" dialog, where the user picks a program for a file (OPENASINFO: the file, a
// class, OAIF_* flags). There is no such dialog for Windows programs here, and a Mac application cannot be offered as
// one: the call fails as for a shell without it (HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)), after the documented
// argument checks. The Windows Steam client's vgui2_s.dll imports it.
typedef struct { const WCHAR *file, *cls; DWORD flags; } OPENASINFO;
static WINAPI HRESULT SHOpenWithDialog(HANDLE parent, const OPENASINFO *info) {
    (void)parent;
    if (!info || !info->file) return E_INVALIDARG;
    char *f = vx_utf8_dup(info->file);
    vx_log("SHOpenWithDialog(%s, flags %#x): there is no \"Open with\" dialog here", f, (unsigned)info->flags);
    free(f);
    return (HRESULT)0x80070032;
}
static const vx_export shell32[] = { VX_EXPORT(SHGetPropertyStoreForWindow), VX_EXPORT(SHCreateItemFromParsingName), VX_EXPORT(SHOpenWithDialog) };
static const vx_export user32[] = { VX_EXPORT(SetWindowPlacement), VX_EXPORT(GetGuiResources) };
static const vx_export propsys[] = { VX_EXPORT(PSCreateMemoryPropertyStore), VX_EXPORT(PSGetPropertyKeyFromName), VX_EXPORT(InitPropVariantFromCLSID) };
void vx_winprops_init(void) {
    vx_register_module("shell32.dll", shell32, sizeof shell32 / sizeof shell32[0]);
    vx_register_module("user32.dll", user32, sizeof user32 / sizeof user32[0]);
    vx_register_module("propsys.dll", propsys, sizeof propsys / sizeof propsys[0]);
    vx_com_register_class(&CLSID_VirtualDesktopManager, create_vdm);
}

#ifdef __aarch64__
#include "thunks/winprops.c.inc"
#endif
