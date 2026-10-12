// Milestone 5a: COM and OLE automation. GUID text, BSTR, VARIANT conversions, SAFEARRAY, the task allocator, an
// unknown class, and an in-process server (vxcom.dll) found through HKCR\CLSID\{...}\InprocServer32.
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)
static const GUID CLSID_VxAdder = { 0x6b1f4c2a, 0x3d5e, 0x4f60, { 0x9a, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 } };
static const GUID IID_IVxAdder  = { 0x6b1f4c2b, 0x3d5e, 0x4f60, { 0x9a, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 } };
typedef struct adder adder;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(adder *, REFIID, void **);
    ULONG (WINAPI *AddRef)(adder *);
    ULONG (WINAPI *Release)(adder *);
    HRESULT (WINAPI *Add)(adder *, int, int, int *);
} adder_vtbl;
struct adder { const adder_vtbl *vtbl; };

int main(void) {
    CHECK(CoInitializeEx(NULL, COINIT_MULTITHREADED) == S_OK);
    CHECK(CoInitializeEx(NULL, COINIT_MULTITHREADED) == S_FALSE);
    CoUninitialize();

    // GUIDs as text, both ways.
    GUID g, back;
    CHECK(CoCreateGuid(&g) == S_OK);
    WCHAR text[40];
    CHECK(StringFromGUID2(&g, text, 40) == 39 && text[0] == L'{' && text[37] == L'}');
    CHECK(CLSIDFromString(text, &back) == S_OK && IsEqualGUID(&g, &back));
    CHECK(CLSIDFromString(L"{not-a-guid}", &back) == CO_E_CLASSSTRING);

    // BSTR.
    BSTR b = SysAllocString(L"automation");
    CHECK(SysStringLen(b) == 10 && SysStringByteLen(b) == 20 && !wcscmp(b, L"automation"));
    CHECK(SysReAllocString(&b, L"ole") && SysStringLen(b) == 3);
    SysFreeString(b);
    SysFreeString(NULL);

    // VARIANT: copies and conversions.
    VARIANT v, w;
    VariantInit(&v); VariantInit(&w);
    v.vt = VT_I4; v.lVal = 1234;
    CHECK(VariantChangeType(&w, &v, 0, VT_BSTR) == S_OK && w.vt == VT_BSTR && !wcscmp(w.bstrVal, L"1234"));
    VARIANT x; VariantInit(&x);
    CHECK(VariantCopy(&x, &w) == S_OK && x.bstrVal != w.bstrVal && !wcscmp(x.bstrVal, L"1234"));
    CHECK(VariantChangeType(&v, &x, 0, VT_R8) == S_OK && v.vt == VT_R8 && v.dblVal == 1234.0);
    VariantClear(&w); VariantClear(&x);
    CHECK(w.vt == VT_EMPTY);

    // SAFEARRAY of BSTRs.
    SAFEARRAY *sa = SafeArrayCreateVector(VT_BSTR, 1, 3);
    CHECK(sa != NULL);
    LONG lo = 0, hi = 0;
    SafeArrayGetLBound(sa, 1, &lo); SafeArrayGetUBound(sa, 1, &hi);
    CHECK(lo == 1 && hi == 3);
    LONG i = 2;
    BSTR in = SysAllocString(L"two"), out = NULL;
    CHECK(SafeArrayPutElement(sa, &i, in) == S_OK && SafeArrayGetElement(sa, &i, &out) == S_OK && !wcscmp(out, L"two"));
    LONG bad = 9;
    CHECK(SafeArrayGetElement(sa, &bad, &out) == DISP_E_BADINDEX);
    SysFreeString(in); SysFreeString(out);
    VARTYPE vt = 0; SafeArrayGetVartype(sa, &vt);
    CHECK(vt == VT_BSTR);
    CHECK(SafeArrayDestroy(sa) == S_OK);

    // The task allocator.
    void *p = CoTaskMemAlloc(64);
    CHECK(p != NULL);
    p = CoTaskMemRealloc(p, 128);
    CoTaskMemFree(p);

    // Classes: one nobody registered, then the in-process server from the registry.
    void *unknown_obj = NULL;
    GUID none = { 0x12345678, 0, 0, { 0 } };
    CHECK(CoCreateInstance(&none, NULL, CLSCTX_INPROC_SERVER, &IID_IUnknown, &unknown_obj) == REGDB_E_CLASSNOTREG && !unknown_obj);
    char dll[MAX_PATH], key[128];
    GetModuleFileNameA(NULL, dll, sizeof dll);
    strcpy(strrchr(dll, '\\') + 1, "vxcom.dll");
    WCHAR clsid_text[40];
    StringFromGUID2(&CLSID_VxAdder, clsid_text, 40);
    sprintf(key, "CLSID\\%ls\\InprocServer32", clsid_text);
    HKEY k;
    CHECK(RegCreateKeyExA(HKEY_CLASSES_ROOT, key, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL) == ERROR_SUCCESS);
    RegSetValueExA(k, NULL, 0, REG_SZ, (const BYTE *)dll, (DWORD)strlen(dll) + 1);
    RegCloseKey(k);
    adder *a = NULL;
    CHECK(CoCreateInstance(&CLSID_VxAdder, NULL, CLSCTX_INPROC_SERVER, &IID_IVxAdder, (void **)&a) == S_OK && a);
    int sum = 0;
    if (a) { CHECK(a->vtbl->Add(a, 2, 40, &sum) == S_OK && sum == 42); a->vtbl->Release(a); }
    HMODULE m = GetModuleHandleA("vxcom.dll");
    HRESULT (WINAPI *can_unload)(void) = m ? (void *)GetProcAddress(m, "DllCanUnloadNow") : NULL;
    CHECK(can_unload && can_unload() == S_OK);                  // the object is gone again
    sprintf(key, "CLSID\\%ls\\InprocServer32", clsid_text);
    RegDeleteKeyA(HKEY_CLASSES_ROOT, key);
    sprintf(key, "CLSID\\%ls", clsid_text);
    RegDeleteKeyA(HKEY_CLASSES_ROOT, key);
    CoUninitialize();
    printf("com: guids, bstr, variant, safearray, in-process server (2+40=%d): %d failed checks -> %s\n", sum, failures, failures ? "FAILED" : "ok");
    return failures;
}
