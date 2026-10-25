// SHGetPropertyStoreForWindow: a window's IPropertyStore keeps the AppUserModelID set in it, a second store for the
// same window sees it, an unset key reads VT_EMPTY, a non-window is refused (src/winprops.c). Steam's Chromium sets
// these on its windows.
#define COBJMACROS
#include <windows.h>
#include <shobjidl.h>
#include <propidl.h>
#include <stdio.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static const PROPERTYKEY key_id = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 };
static const PROPERTYKEY key_other = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 2 };
static const IID iid_store = { 0x886d8eeb, 0x8cf2, 0x4446, { 0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99 } };
static const IID iid_unknown = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

int main(void) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    HWND w = CreateWindowExW(0, L"STATIC", L"props", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
    CHECK(w != NULL, "window");
    IPropertyStore *ps = NULL;
    HRESULT hr = SHGetPropertyStoreForWindow(w, &iid_store, (void **)&ps);
    CHECK(hr == S_OK && ps, "store: %#lx", hr);
    if (ps) {
        PROPVARIANT v;
        memset(&v, 0, sizeof v);
        v.vt = VT_LPWSTR; v.pwszVal = L"Made.Up.AppId";
        CHECK(IPropertyStore_SetValue(ps, &key_id, &v) == S_OK, "set");
        CHECK(IPropertyStore_Commit(ps) == S_OK, "commit");
        DWORD n = 0;
        CHECK(IPropertyStore_GetCount(ps, &n) == S_OK && n == 1, "count %lu", n);
        PROPERTYKEY k;
        CHECK(IPropertyStore_GetAt(ps, 0, &k) == S_OK && !memcmp(&k, &key_id, sizeof k), "key 0");
        IPropertyStore *again = NULL;
        CHECK(SHGetPropertyStoreForWindow(w, &iid_store, (void **)&again) == S_OK && again, "second store");
        PROPVARIANT got;
        memset(&got, 0, sizeof got);
        CHECK(again && IPropertyStore_GetValue(again, &key_id, &got) == S_OK && got.vt == VT_LPWSTR && got.pwszVal != v.pwszVal &&
              !lstrcmpW(got.pwszVal, L"Made.Up.AppId"), "value kept");
        PropVariantClear(&got);
        CHECK(again && IPropertyStore_GetValue(again, &key_other, &got) == S_OK && got.vt == VT_EMPTY, "unset key");
        if (again) IPropertyStore_Release(again);
        IUnknown *unk = NULL;
        CHECK(IPropertyStore_QueryInterface(ps, &iid_unknown, (void **)&unk) == S_OK && unk, "IUnknown");
        if (unk) IUnknown_Release(unk);
        IPropertyStore_Release(ps);
    }
    IPropertyStore *none = (IPropertyStore *)1;
    CHECK(FAILED(SHGetPropertyStoreForWindow((HWND)(UINT_PTR)0x7ff0, &iid_store, (void **)&none)) && !none, "not a window");
    // SetWindowPlacement moves the window to rcNormalPosition; a wrong length is refused.
    WINDOWPLACEMENT wp = { sizeof wp };
    wp.showCmd = SW_SHOWNOACTIVATE;
    SetRect(&wp.rcNormalPosition, 40, 50, 340, 250);
    CHECK(SetWindowPlacement(w, &wp), "SetWindowPlacement");
    RECT r;
    GetWindowRect(w, &r);
    CHECK(r.left == 40 && r.top == 50 && r.right - r.left == 300 && r.bottom - r.top == 200, "placed at %ld,%ld %ldx%ld",
          r.left, r.top, r.right - r.left, r.bottom - r.top);
    CHECK(IsWindowVisible(w), "shown");
    wp.length = 4;
    CHECK(!SetWindowPlacement(w, &wp), "bad length");
    IUnknown *item = (IUnknown *)1;
    CHECK(FAILED(SHCreateItemFromParsingName(L"C:\\made\\up.txt", NULL, &iid_unknown, (void **)&item)) && !item, "shell item");
    // IVirtualDesktopManager: one desktop, every window on it.
    static const CLSID clsid_vdm = { 0xaa509086, 0x5ca9, 0x4c25, { 0x8f, 0x95, 0x58, 0x9d, 0x3c, 0x07, 0xb4, 0x8a } };
    static const IID iid_vdm = { 0xa5cd92ff, 0x29be, 0x454c, { 0x8d, 0x04, 0xd8, 0x28, 0x79, 0xfb, 0x3f, 0x1b } };
    IVirtualDesktopManager *vdm = NULL;
    hr = CoCreateInstance(&clsid_vdm, NULL, CLSCTX_ALL, &iid_vdm, (void **)&vdm);
    CHECK(hr == S_OK && vdm, "VirtualDesktopManager: %#lx", hr);
    if (vdm) {
        BOOL on = FALSE;
        GUID id, id2;
        CHECK(IVirtualDesktopManager_IsWindowOnCurrentVirtualDesktop(vdm, w, &on) == S_OK && on, "on current desktop");
        CHECK(IVirtualDesktopManager_GetWindowDesktopId(vdm, w, &id) == S_OK, "desktop id");
        CHECK(IVirtualDesktopManager_MoveWindowToDesktop(vdm, w, &id) == S_OK, "move to its desktop");
        HWND w2 = CreateWindowExW(0, L"STATIC", L"props2", WS_OVERLAPPEDWINDOW, 0, 0, 50, 50, NULL, NULL, NULL, NULL);
        CHECK(IVirtualDesktopManager_GetWindowDesktopId(vdm, w2, &id2) == S_OK && !memcmp(&id, &id2, sizeof id), "same desktop");
        DestroyWindow(w2);
        CHECK(IVirtualDesktopManager_IsWindowOnCurrentVirtualDesktop(vdm, (HWND)(UINT_PTR)0x7ff0, &on) == E_INVALIDARG, "not a window");
        IVirtualDesktopManager_Release(vdm);
    }
    DestroyWindow(w);
    printf(fails ? "winprops: %d failures\n" : "winprops: ok\n", fails);
    return fails != 0;
}
