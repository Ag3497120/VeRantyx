// oleacc, UIAutomationCore and uxtheme load and answer as a desktop without assistive technology or visual styles
// (src/a11y.c): no accessible objects, UI Automation events raised to nobody, no theme data. Steam's Chromium
// delay-loads these DLLs when it creates its windows.
#include <windows.h>
#include <oleacc.h>
#include <uxtheme.h>
#include <stdio.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static const IID iid_unknown = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID iid_accessible = { 0x618736e0, 0x3c3d, 0x11cf, { 0x81, 0x0c, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

int main(void) {
    HWND w = CreateWindowExW(0, L"STATIC", L"a11y", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
    void *obj = (void *)1;
    CHECK(FAILED(AccessibleObjectFromWindow(w, (DWORD)OBJID_CLIENT, &iid_accessible, &obj)) && !obj, "AccessibleObjectFromWindow");
    obj = (void *)1;
    CHECK(FAILED(CreateStdAccessibleObject(w, OBJID_CLIENT, &iid_accessible, &obj)) && !obj, "CreateStdAccessibleObject");
    CHECK(LresultFromObject(&iid_accessible, 0, NULL) < 0, "LresultFromObject");
    HWND from = (HWND)1;
    CHECK(FAILED(WindowFromAccessibleObject(NULL, &from)) && !from, "WindowFromAccessibleObject");

    HMODULE uia = LoadLibraryA("UIAutomationCore.dll");
    CHECK(uia != NULL, "UIAutomationCore loads");
    if (uia) {
        HRESULT (WINAPI *host)(HWND, void **) = (void *)GetProcAddress(uia, "UiaHostProviderFromHwnd");
        LRESULT (WINAPI *ret)(HWND, WPARAM, LPARAM, void *) = (void *)GetProcAddress(uia, "UiaReturnRawElementProvider");
        HRESULT (WINAPI *raise)(void *, int) = (void *)GetProcAddress(uia, "UiaRaiseAutomationEvent");
        HRESULT (WINAPI *ns)(IUnknown **) = (void *)GetProcAddress(uia, "UiaGetReservedNotSupportedValue");
        HRESULT (WINAPI *mixed)(IUnknown **) = (void *)GetProcAddress(uia, "UiaGetReservedMixedAttributeValue");
        CHECK(host && ret && raise && ns && mixed, "UIA exports");
        if (host && ret && raise && ns && mixed) {
            void *p = (void *)1;
            CHECK(FAILED(host(w, &p)) && !p, "UiaHostProviderFromHwnd");
            CHECK(ret(w, 0, -25, NULL) == 0, "UiaReturnRawElementProvider");
            CHECK(raise(NULL, 20005) == S_OK, "UiaRaiseAutomationEvent");
            IUnknown *a = NULL, *b = NULL, *c = NULL;
            CHECK(ns(&a) == S_OK && a && ns(&c) == S_OK && a == c, "not-supported value is one object");
            CHECK(mixed(&b) == S_OK && b && b != a, "mixed value differs");
            IUnknown *q = NULL;
            CHECK(a && a->lpVtbl->QueryInterface(a, &iid_unknown, (void **)&q) == S_OK && q == a, "sentinel IUnknown");
        }
    }

    HTHEME t = OpenThemeData(w, L"BUTTON");
    CHECK(t == NULL, "no theme data");
    CHECK(!IsThemeActive() && !IsAppThemed(), "no visual styles");
    CHECK(CloseThemeData(NULL) == S_OK, "CloseThemeData(NULL)");
    SIZE sz;
    CHECK(FAILED(GetThemePartSize(NULL, NULL, 1, 1, NULL, TS_TRUE, &sz)), "GetThemePartSize without a theme");
    DestroyWindow(w);
    printf(fails ? "a11y: %d failures\n" : "a11y: ok\n", fails);
    return fails != 0;
}
