// oleacc (Active Accessibility), UIAutomationCore and uxtheme as a desktop with no assistive technology and no visual
// styles presents them: no accessible objects are made for windows (the documented errors), no client listens for
// UI Automation events (raising one succeeds and reaches nobody), and no theme is open (OpenThemeData returns NULL, so
// programs draw their classic look).
// Steam's Chromium delay-loads all three; it stopped with a CHECK when LoadLibrary("OLEACC.dll") failed right after
// it created its login window.
#include <stdint.h>
#include <string.h>
#include "vx.h"

typedef int32_t HRESULT;
#define S_OK 0
#define E_FAIL ((HRESULT)0x80004005)
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define E_POINTER ((HRESULT)0x80004003)
#define E_HANDLE ((HRESULT)0x80070006)

// ---- oleacc ----------------------------------------------------------------------------------------------------------------
static WINAPI intptr_t LresultFromObject(const void *iid, uintptr_t wparam, void *object) {
    (void)iid; (void)wparam;
    return object ? E_NOINTERFACE : E_INVALIDARG;                 // (an LRESULT: the error itself)
}
static WINAPI HRESULT AccessibleObjectFromWindow(HANDLE wnd, DWORD id, const void *iid, void **out) {
    (void)wnd; (void)id; (void)iid;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    return E_NOINTERFACE;
}
static WINAPI HRESULT CreateStdAccessibleObject(HANDLE wnd, LONG id, const void *iid, void **out) {
    (void)wnd; (void)id; (void)iid;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    return E_NOINTERFACE;
}
static WINAPI HRESULT AccessibleChildren(void *container, LONG start, LONG count, void *children, LONG *got) {
    (void)container; (void)start; (void)count; (void)children;
    if (got) *got = 0;
    return E_INVALIDARG;
}
static WINAPI HRESULT WindowFromAccessibleObject(void *object, HANDLE *wnd) {
    (void)object;
    if (wnd) *wnd = NULL;
    return E_FAIL;
}

// ---- UIAutomationCore ------------------------------------------------------------------------------------------------------
static WINAPI HRESULT UiaHostProviderFromHwnd(HANDLE wnd, void **provider) {
    (void)wnd;
    if (!provider) return E_INVALIDARG;
    *provider = NULL;
    return E_NOINTERFACE;
}
static WINAPI intptr_t UiaReturnRawElementProvider(HANDLE wnd, uintptr_t wparam, intptr_t lparam, void *element) {
    (void)wnd; (void)wparam; (void)lparam; (void)element;
    return 0;                                                     // (no client asked: WM_GETOBJECT not answered)
}
static WINAPI HRESULT uia_raise(void) { return S_OK; }            // (UiaRaise*Event: nobody listens)
// The reserved values (UiaGetReservedNotSupportedValue, ...MixedAttributeValue): one object each, compared by identity.
typedef struct { const void *const *vtbl; } sentinel;
static WINAPI HRESULT sentinel_qi(sentinel *s, const uint8_t *iid, void **out) {
    static const uint8_t unknown[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46 };
    if (!out) return E_POINTER;
    if (!memcmp(iid, unknown, 16)) { *out = s; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static WINAPI DWORD sentinel_ref(sentinel *s) { (void)s; return 1; }
static const void *const g_sentinel_vtbl[] = { (void *)sentinel_qi, (void *)sentinel_ref, (void *)sentinel_ref };
static sentinel g_not_supported = { g_sentinel_vtbl }, g_mixed = { g_sentinel_vtbl };
static WINAPI HRESULT UiaGetReservedNotSupportedValue(void **out) { if (!out) return E_INVALIDARG; *out = &g_not_supported; return S_OK; }
static WINAPI HRESULT UiaGetReservedMixedAttributeValue(void **out) { if (!out) return E_INVALIDARG; *out = &g_mixed; return S_OK; }
static WINAPI BOOL UiaClientsAreListening(void) { return 0; }

// ---- uxtheme ---------------------------------------------------------------------------------------------------------------
static WINAPI HANDLE OpenThemeData(HANDLE wnd, const WCHAR *classes) { (void)wnd; (void)classes; vx_set_last_error(1168); return NULL; }
static WINAPI HANDLE OpenThemeDataForDpi(HANDLE wnd, const WCHAR *classes, DWORD dpi) { (void)dpi; return OpenThemeData(wnd, classes); }
static WINAPI HRESULT CloseThemeData(HANDLE theme) { return theme ? E_HANDLE : S_OK; }
static WINAPI HRESULT theme_no_handle(void) { return E_HANDLE; } // (DrawThemeBackground, GetThemePartSize, ...: no theme is open)
static WINAPI BOOL IsThemeActive(void) { return 0; }
static WINAPI BOOL IsAppThemed(void) { return 0; }
static WINAPI HRESULT SetWindowTheme(HANDLE wnd, const WCHAR *app, const WCHAR *ids) { (void)wnd; (void)app; (void)ids; return S_OK; }
static WINAPI HANDLE GetWindowTheme(HANDLE wnd) { (void)wnd; return NULL; }

static const vx_export oleacc[] = {
    VX_EXPORT(LresultFromObject), VX_EXPORT(AccessibleObjectFromWindow), VX_EXPORT(CreateStdAccessibleObject),
    VX_EXPORT(AccessibleChildren), VX_EXPORT(WindowFromAccessibleObject),
};
static const vx_export uiautomationcore[] = {
    VX_EXPORT(UiaHostProviderFromHwnd), VX_EXPORT(UiaReturnRawElementProvider), VX_EXPORT(UiaGetReservedNotSupportedValue),
    VX_EXPORT(UiaGetReservedMixedAttributeValue), VX_EXPORT(UiaClientsAreListening),
    { "UiaRaiseAutomationEvent", (void *)uia_raise }, { "UiaRaiseAutomationPropertyChangedEvent", (void *)uia_raise },
    { "UiaRaiseNotificationEvent", (void *)uia_raise }, { "UiaRaiseStructureChangedEvent", (void *)uia_raise },
};
static const vx_export uxtheme[] = {
    VX_EXPORT(OpenThemeData), VX_EXPORT(OpenThemeDataForDpi), VX_EXPORT(CloseThemeData), VX_EXPORT(IsThemeActive), VX_EXPORT(IsAppThemed),
    VX_EXPORT(SetWindowTheme), VX_EXPORT(GetWindowTheme), { "DrawThemeBackground", (void *)theme_no_handle },
    { "GetThemeBackgroundContentRect", (void *)theme_no_handle }, { "GetThemePartSize", (void *)theme_no_handle },
};
void vx_a11y_init(void) {
    vx_register_module("oleacc.dll", oleacc, sizeof oleacc / sizeof oleacc[0]);
    vx_register_module("uiautomationcore.dll", uiautomationcore, sizeof uiautomationcore / sizeof uiautomationcore[0]);
    vx_register_module("uxtheme.dll", uxtheme, sizeof uxtheme / sizeof uxtheme[0]);
}

#ifdef __aarch64__
#include "thunks/a11y.c.inc"
#endif
