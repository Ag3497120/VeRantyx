// An in-process COM server for milestone 5a: one class whose objects add numbers (interface IVxAdder).
#include <windows.h>
#include <objbase.h>
static const GUID CLSID_VxAdder = { 0x6b1f4c2a, 0x3d5e, 0x4f60, { 0x9a, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 } };
static const GUID IID_IVxAdder  = { 0x6b1f4c2b, 0x3d5e, 0x4f60, { 0x9a, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 } };
static LONG objects;

typedef struct adder adder;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(adder *, REFIID, void **);
    ULONG (WINAPI *AddRef)(adder *);
    ULONG (WINAPI *Release)(adder *);
    HRESULT (WINAPI *Add)(adder *, int, int, int *);
} adder_vtbl;
struct adder { const adder_vtbl *vtbl; LONG refs; };
static HRESULT WINAPI adder_qi(adder *a, REFIID iid, void **out) {
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IVxAdder)) { *out = a; a->vtbl->AddRef(a); return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI adder_addref(adder *a) { return (ULONG)InterlockedIncrement(&a->refs); }
static ULONG WINAPI adder_release(adder *a) {
    LONG r = InterlockedDecrement(&a->refs);
    if (!r) { HeapFree(GetProcessHeap(), 0, a); InterlockedDecrement(&objects); }
    return (ULONG)r;
}
static HRESULT WINAPI adder_add(adder *a, int x, int y, int *out) { (void)a; *out = x + y; return S_OK; }
static const adder_vtbl g_adder_vtbl = { adder_qi, adder_addref, adder_release, adder_add };

static HRESULT WINAPI factory_qi(IClassFactory *f, REFIID iid, void **out) {
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IClassFactory)) { *out = f; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI factory_ref(IClassFactory *f) { (void)f; return 1; }
static HRESULT WINAPI factory_create(IClassFactory *f, IUnknown *outer, REFIID iid, void **out) {
    (void)f;
    if (outer) return CLASS_E_NOAGGREGATION;
    adder *a = HeapAlloc(GetProcessHeap(), 0, sizeof *a);
    a->vtbl = &g_adder_vtbl; a->refs = 1;
    InterlockedIncrement(&objects);
    HRESULT r = adder_qi(a, iid, out);
    adder_release(a);
    return r;
}
static HRESULT WINAPI factory_lock(IClassFactory *f, BOOL lock) { (void)f; (void)lock; return S_OK; }
static IClassFactoryVtbl g_factory_vtbl = { factory_qi, factory_ref, factory_ref, factory_create, factory_lock };
static IClassFactory g_factory = { &g_factory_vtbl };

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out) {
    if (!IsEqualGUID(clsid, &CLSID_VxAdder)) return CLASS_E_CLASSNOTAVAILABLE;
    return factory_qi(&g_factory, iid, out);
}
__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return objects ? S_FALSE : S_OK; }
