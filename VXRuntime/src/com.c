// COM (ole32) and OLE automation (oleaut32).
//
// CoCreateInstance finds a class among this runtime's built-in classes (Direct3D, DXGI and later others register
// themselves with vx_com_register_class), among those the program registered (CoRegisterClassObject), or as an
// in-process server: HKCR\CLSID\{...}\InprocServer32 names a DLL whose DllGetClassObject provides a class
// factory. Apartments are not modelled: every object is called directly from any thread.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc/malloc.h>
#include "vx.h"

typedef int32_t HRESULT;
#define S_OK 0
#define S_FALSE 1
#define E_NOTIMPL ((HRESULT)0x80004001)
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define E_POINTER ((HRESULT)0x80004003)
#define E_OUTOFMEMORY ((HRESULT)0x8007000E)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define REGDB_E_CLASSNOTREG ((HRESULT)0x80040154)
#define CLASS_E_NOAGGREGATION ((HRESULT)0x80040110)
#define CO_E_CLASSSTRING ((HRESULT)0x800401F3)
#define DISP_E_TYPEMISMATCH ((HRESULT)0x80020005)
#define DISP_E_BADINDEX ((HRESULT)0x8002000B)

typedef struct { DWORD d1; uint16_t d2, d3; uint8_t d4[8]; } GUID;
static const GUID IID_IUnknown = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IClassFactory = { 0x00000001, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IMalloc = { 0x00000002, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static int same_guid(const GUID *a, const GUID *b) { return !memcmp(a, b, sizeof *a); }

// The start of every COM object: a pointer to its method table. IUnknown's three methods come first.
typedef struct unknown_vtbl {
    HRESULT (WINAPI *QueryInterface)(void *, const GUID *, void **);
    uint32_t (WINAPI *AddRef)(void *);
    uint32_t (WINAPI *Release)(void *);
} unknown_vtbl;
typedef struct { const unknown_vtbl *vtbl; } unknown;
typedef struct {
    unknown_vtbl base;
    HRESULT (WINAPI *CreateInstance)(void *, void *, const GUID *, void **);
    HRESULT (WINAPI *LockServer)(void *, int);
} class_factory_vtbl;
#define RELEASE(p) (((unknown *)(p))->vtbl->Release(p))

// ---- GUID text ------------------------------------------------------------------------------------------------------
static void guid_text(const GUID *g, char *out) {
    sprintf(out, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g->d1, g->d2, g->d3, g->d4[0], g->d4[1],
            g->d4[2], g->d4[3], g->d4[4], g->d4[5], g->d4[6], g->d4[7]);
}
static int parse_guid(const char *s, GUID *g) {
    unsigned d1, d2, d3, b[8];
    if (strlen(s) != 38 || s[0] != '{' || s[37] != '}') return 0;
    if (sscanf(s, "{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11) return 0;
    g->d1 = d1; g->d2 = (uint16_t)d2; g->d3 = (uint16_t)d3;
    for (int i = 0; i < 8; i++) g->d4[i] = (uint8_t)b[i];
    return 1;
}
static WINAPI int StringFromGUID2(const GUID *g, WCHAR *out, int cch) {
    char text[40];
    guid_text(g, text);
    if (cch < 39) return 0;
    for (int i = 0; i < 39; i++) out[i] = (WCHAR)(uint8_t)text[i];
    return 39;
}
static WINAPI void *CoTaskMemAlloc(size_t n) { return malloc(n ? n : 1); }
static WINAPI void *CoTaskMemRealloc(void *p, size_t n) { return realloc(p, n ? n : 1); }
static WINAPI void CoTaskMemFree(void *p) { free(p); }
static WINAPI HRESULT StringFromCLSID(const GUID *g, WCHAR **out) {
    *out = CoTaskMemAlloc(39 * sizeof(WCHAR));
    if (!*out) return E_OUTOFMEMORY;
    StringFromGUID2(g, *out, 39);
    return S_OK;
}
static WINAPI HRESULT CLSIDFromString(const WCHAR *s, GUID *g) {
    char *a = vx_utf8_dup(s);
    int ok = a && parse_guid(a, g);
    free(a);
    if (!ok) { memset(g, 0, sizeof *g); return CO_E_CLASSSTRING; }
    return S_OK;
}
static WINAPI HRESULT IIDFromString(const WCHAR *s, GUID *g) { return CLSIDFromString(s, g) ? E_INVALIDARG : S_OK; }
static WINAPI HRESULT CoCreateGuid(GUID *g) {
    arc4random_buf(g, sizeof *g);
    g->d3 = (uint16_t)((g->d3 & 0x0fff) | 0x4000);       // version 4 (random)
    g->d4[0] = (uint8_t)((g->d4[0] & 0x3f) | 0x80);      // variant 1
    return S_OK;
}

// ---- IMalloc ---------------------------------------------------------------------------------------------------------------
typedef struct {
    unknown_vtbl base;
    void *(WINAPI *Alloc)(void *, size_t);
    void *(WINAPI *Realloc)(void *, void *, size_t);
    void (WINAPI *Free)(void *, void *);
    size_t (WINAPI *GetSize)(void *, void *);
    int (WINAPI *DidAlloc)(void *, void *);
    void (WINAPI *HeapMinimize)(void *);
} malloc_vtbl;
static WINAPI HRESULT malloc_qi(void *self, const GUID *iid, void **out) {
    if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IMalloc)) { *out = self; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static WINAPI uint32_t malloc_ref(void *self) { (void)self; return 1; }
static WINAPI void *malloc_alloc(void *self, size_t n) { (void)self; return CoTaskMemAlloc(n); }
static WINAPI void *malloc_realloc(void *self, void *p, size_t n) { (void)self; return CoTaskMemRealloc(p, n); }
static WINAPI void malloc_free(void *self, void *p) { (void)self; free(p); }
static WINAPI size_t malloc_getsize(void *self, void *p) { (void)self; return p ? malloc_size(p) : (size_t)-1; }
static WINAPI int malloc_didalloc(void *self, void *p) { (void)self; return p ? malloc_size(p) != 0 : -1; }
static WINAPI void malloc_minimize(void *self) { (void)self; }
static const malloc_vtbl g_malloc_vtbl = { { malloc_qi, malloc_ref, malloc_ref }, malloc_alloc, malloc_realloc, malloc_free,
                                           malloc_getsize, malloc_didalloc, malloc_minimize };
static const malloc_vtbl *g_malloc = &g_malloc_vtbl;
static WINAPI HRESULT CoGetMalloc(DWORD ctx, void **out) { (void)ctx; *out = (void *)&g_malloc; return S_OK; }

// ---- initialization -------------------------------------------------------------------------------------------------------
static __thread int t_com_inits;
static WINAPI HRESULT CoInitializeEx(void *reserved, DWORD model) { (void)reserved; (void)model; return t_com_inits++ ? S_FALSE : S_OK; }
static WINAPI HRESULT CoInitialize(void *reserved) { return CoInitializeEx(reserved, 2); }
static WINAPI void CoUninitialize(void) { if (t_com_inits) t_com_inits--; }
static WINAPI HRESULT OleInitialize(void *reserved) { return CoInitializeEx(reserved, 2); }
static WINAPI void OleUninitialize(void) { CoUninitialize(); }
static WINAPI HRESULT CoInitializeSecurity(void *sd, LONG n, void *svc, void *r1, DWORD a, DWORD b, void *c, DWORD d, void *r2) {
    (void)sd; (void)n; (void)svc; (void)r1; (void)a; (void)b; (void)c; (void)d; (void)r2; return S_OK;
}
static WINAPI HRESULT CoSetProxyBlanket(void *p, DWORD a, DWORD b, WCHAR *c, DWORD d, DWORD e, void *f, DWORD g) {
    (void)p; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; return S_OK;
}
static WINAPI DWORD CoGetCurrentProcess(void) { return (DWORD)getpid(); }
static WINAPI void CoFreeUnusedLibraries(void) {}
static WINAPI void CoFreeUnusedLibrariesEx(DWORD delay, DWORD r) { (void)delay; (void)r; }
static WINAPI HRESULT CoLockObjectExternal(void *u, int lock, int last) { (void)u; (void)lock; (void)last; return S_OK; }
static WINAPI HRESULT RegisterDragDrop(HANDLE w, void *t) { (void)w; (void)t; return S_OK; }
static WINAPI HRESULT RevokeDragDrop(HANDLE w) { (void)w; return S_OK; }
static WINAPI HRESULT CoCreateFreeThreadedMarshaler(void *outer, void **out) { (void)outer; *out = NULL; return E_NOTIMPL; }
typedef DWORD (WINAPI *wait_fn)(DWORD, const HANDLE *, BOOL, DWORD);
static WINAPI HRESULT CoWaitForMultipleHandles(DWORD flags, DWORD ms, uint32_t n, const HANDLE *hs, DWORD *index) {
    static wait_fn wait;
    if (!wait) wait = (wait_fn)vx_find_export("kernel32.dll", "WaitForMultipleObjects");
    DWORD r = wait(n, hs, flags & 1 /* COWAIT_WAITALL */, ms);
    if (r == 0x102) return (HRESULT)0x80010115;            // RPC_S_CALLPENDING
    *index = r;
    return S_OK;
}

// ---- classes ---------------------------------------------------------------------------------------------------------------
typedef struct { GUID clsid; HRESULT (*create)(const GUID *iid, void **out); void *factory; DWORD cookie; } com_class;
static com_class g_classes[256];
static int g_class_count;
static pthread_mutex_t g_class_lock = PTHREAD_MUTEX_INITIALIZER;

void vx_com_register_class(const void *clsid, HRESULT (*create)(const void *iid, void **out)) {
    pthread_mutex_lock(&g_class_lock);
    if (g_class_count < 256) g_classes[g_class_count++] = (com_class){ *(const GUID *)clsid, (HRESULT (*)(const GUID *, void **))create, NULL, 0 };
    pthread_mutex_unlock(&g_class_lock);
}
static WINAPI HRESULT CoRegisterClassObject(const GUID *clsid, void *factory, DWORD ctx, DWORD flags, DWORD *cookie) {
    (void)ctx; (void)flags;
    static DWORD next = 1;
    pthread_mutex_lock(&g_class_lock);
    HRESULT r = E_OUTOFMEMORY;
    if (g_class_count < 256) {
        ((unknown *)factory)->vtbl->AddRef(factory);
        g_classes[g_class_count++] = (com_class){ *clsid, NULL, factory, next };
        *cookie = next++;
        r = S_OK;
    }
    pthread_mutex_unlock(&g_class_lock);
    return r;
}
static WINAPI HRESULT CoRevokeClassObject(DWORD cookie) {
    pthread_mutex_lock(&g_class_lock);
    for (int i = 0; i < g_class_count; i++)
        if (g_classes[i].cookie == cookie) { RELEASE(g_classes[i].factory); g_classes[i] = g_classes[--g_class_count]; break; }
    pthread_mutex_unlock(&g_class_lock);
    return S_OK;
}

// An in-process server's class factory, from the registry.
typedef LONG (WINAPI *reg_open_fn)(void *, const char *, DWORD, DWORD, void **);
typedef LONG (WINAPI *reg_query_fn)(void *, const char *, DWORD *, DWORD *, uint8_t *, DWORD *);
typedef LONG (WINAPI *reg_close_fn)(void *);
typedef void *(WINAPI *load_fn)(const char *);
typedef void *(WINAPI *proc_fn)(void *, const char *);
static HRESULT inproc_factory(const GUID *clsid, const GUID *iid, void **out) {
    reg_open_fn open = (reg_open_fn)vx_find_export("advapi32.dll", "RegOpenKeyExA");
    reg_query_fn query = (reg_query_fn)vx_find_export("advapi32.dll", "RegQueryValueExA");
    reg_close_fn close_key = (reg_close_fn)vx_find_export("advapi32.dll", "RegCloseKey");
    char key[80], path[1024];
    char text[40];
    guid_text(clsid, text);
    snprintf(key, sizeof key, "CLSID\\%s\\InprocServer32", text);
    void *hk;
    if (open((void *)(intptr_t)(int32_t)0x80000000, key, 0, 0x20019, &hk)) return REGDB_E_CLASSNOTREG;
    DWORD type = 0, size = sizeof path - 1;
    LONG r = query(hk, NULL, NULL, &type, (uint8_t *)path, &size);
    close_key(hk);
    if (r || (type != 1 && type != 2)) return REGDB_E_CLASSNOTREG;
    path[size] = 0;
    void *dll = ((load_fn)vx_find_export("kernel32.dll", "LoadLibraryA"))(path);
    if (!dll) { vx_log("COM server %s for %s did not load", path, text); return REGDB_E_CLASSNOTREG; }
    HRESULT (WINAPI *get)(const GUID *, const GUID *, void **) = ((proc_fn)vx_find_export("kernel32.dll", "GetProcAddress"))(dll, "DllGetClassObject");
    if (!get) return REGDB_E_CLASSNOTREG;
    return get(clsid, iid, out);
}

static HRESULT class_object(const GUID *clsid, const GUID *iid, void **out, HRESULT (**create)(const GUID *, void **)) {
    *create = NULL;
    pthread_mutex_lock(&g_class_lock);
    for (int i = g_class_count - 1; i >= 0; i--) {
        if (!same_guid(&g_classes[i].clsid, clsid)) continue;
        if (g_classes[i].create) { *create = g_classes[i].create; pthread_mutex_unlock(&g_class_lock); return S_OK; }
        void *f = g_classes[i].factory;
        pthread_mutex_unlock(&g_class_lock);
        return ((unknown *)f)->vtbl->QueryInterface(f, iid, out);
    }
    pthread_mutex_unlock(&g_class_lock);
    return inproc_factory(clsid, iid, out);
}
static WINAPI HRESULT CoGetClassObject(const GUID *clsid, DWORD ctx, void *server, const GUID *iid, void **out) {
    (void)ctx; (void)server;
    HRESULT (*create)(const GUID *, void **);
    HRESULT r = class_object(clsid, iid, out, &create);
    if (r == S_OK && create) r = E_NOTIMPL;            // built-in classes are created directly, without factories
    return r;
}
static WINAPI HRESULT CoCreateInstance(const GUID *clsid, void *outer, DWORD ctx, const GUID *iid, void **out) {
    (void)ctx;
    if (!out) return E_POINTER;
    *out = NULL;
    HRESULT (*create)(const GUID *, void **);
    void *factory = NULL;
    HRESULT r = class_object(clsid, &IID_IClassFactory, &factory, &create);
    if (r != S_OK) {
        if (vx_trace) { char t[40]; guid_text(clsid, t); vx_log("CoCreateInstance: class %s is not available", t); }
        return r;
    }
    if (create) return outer ? CLASS_E_NOAGGREGATION : create(iid, out);
    const class_factory_vtbl *v = *(const class_factory_vtbl **)factory;
    r = v->CreateInstance(factory, outer, iid, out);
    RELEASE(factory);
    return r;
}
// MULTI_QI: an interface ID in, the interface and a result out.
typedef struct { const GUID *iid; void *itf; HRESULT hr; } MULTI_QI;
static WINAPI HRESULT CoCreateInstanceEx(const GUID *clsid, void *outer, DWORD ctx, void *server, DWORD n, MULTI_QI *results) {
    (void)server;
    if (!n) return E_INVALIDARG;
    HRESULT r = CoCreateInstance(clsid, outer, ctx, results[0].iid, &results[0].itf);
    results[0].hr = r;
    if (r != S_OK) { for (DWORD i = 1; i < n; i++) results[i].hr = r; return r; }
    for (DWORD i = 1; i < n; i++) {
        unknown *u = results[0].itf;
        results[i].hr = u->vtbl->QueryInterface(u, results[i].iid, &results[i].itf);
    }
    return S_OK;
}

// ---- BSTR: a length prefix (bytes), the characters, a terminator ----------------------------------------------------------------
static WCHAR *bstr_alloc(const void *data, uint32_t bytes) {
    uint8_t *p = malloc(4 + (size_t)bytes + 2);
    if (!p) return NULL;
    *(uint32_t *)p = bytes;
    if (data) memcpy(p + 4, data, bytes); else memset(p + 4, 0, bytes);
    p[4 + bytes] = p[5 + bytes] = 0;
    return (WCHAR *)(p + 4);
}
static WINAPI WCHAR *SysAllocString(const WCHAR *s) { return s ? bstr_alloc(s, (uint32_t)(vx_wcslen(s) * 2)) : NULL; }
static WINAPI WCHAR *SysAllocStringLen(const WCHAR *s, uint32_t n) { return bstr_alloc(s, n * 2); }
static WINAPI WCHAR *SysAllocStringByteLen(const char *s, uint32_t n) { return bstr_alloc(s, n); }
static WINAPI void SysFreeString(WCHAR *b) { if (b) free((uint8_t *)b - 4); }
static WINAPI uint32_t SysStringByteLen(const WCHAR *b) { return b ? *(const uint32_t *)((const uint8_t *)b - 4) : 0; }
static WINAPI uint32_t SysStringLen(const WCHAR *b) { return SysStringByteLen(b) / 2; }
static WINAPI int SysReAllocStringLen(WCHAR **b, const WCHAR *s, uint32_t n) {
    WCHAR *nb = bstr_alloc(s, n * 2);
    if (!nb) return 0;
    SysFreeString(*b);
    *b = nb;
    return 1;
}
static WINAPI int SysReAllocString(WCHAR **b, const WCHAR *s) { return SysReAllocStringLen(b, s, s ? (uint32_t)vx_wcslen(s) : 0); }

// ---- VARIANT (x64: the type, three reserved words, then the value) ------------------------------------------------------------
enum { VT_EMPTY = 0, VT_NULL = 1, VT_I2 = 2, VT_I4 = 3, VT_R4 = 4, VT_R8 = 5, VT_BSTR = 8, VT_DISPATCH = 9, VT_ERROR = 10,
       VT_BOOL = 11, VT_VARIANT = 12, VT_UNKNOWN = 13, VT_I1 = 16, VT_UI1 = 17, VT_UI2 = 18, VT_UI4 = 19, VT_I8 = 20,
       VT_UI8 = 21, VT_INT = 22, VT_UINT = 23, VT_LPSTR = 30, VT_LPWSTR = 31, VT_CLSID = 72,
       VT_ARRAY = 0x2000, VT_BYREF = 0x4000 };
typedef struct { uint16_t vt, r1, r2, r3; union { int64_t i8; uint64_t u8; int32_t i4; double r8; float r4; int16_t boolean; WCHAR *bstr; void *ptr; } v; uint64_t extra; } VARIANT;
_Static_assert(sizeof(VARIANT) == 24, "VARIANT");
typedef struct sa_bound { uint32_t count; LONG lbound; } sa_bound;
typedef struct { uint16_t dims, features; uint32_t elem_size, locks; void *data; sa_bound bounds[1]; } SAFEARRAY;
static WINAPI HRESULT SafeArrayDestroy(SAFEARRAY *sa);
static WINAPI HRESULT SafeArrayCopy(SAFEARRAY *sa, SAFEARRAY **out);

static WINAPI void VariantInit(VARIANT *v) { v->vt = VT_EMPTY; v->r1 = v->r2 = v->r3 = 0; }
static WINAPI HRESULT VariantClear(VARIANT *v) {
    if (!(v->vt & VT_BYREF)) {
        if (v->vt & VT_ARRAY) SafeArrayDestroy(v->v.ptr);
        else if (v->vt == VT_BSTR) SysFreeString(v->v.bstr);
        else if ((v->vt == VT_UNKNOWN || v->vt == VT_DISPATCH) && v->v.ptr) RELEASE(v->v.ptr);
    }
    v->vt = VT_EMPTY;
    v->v.u8 = 0;
    return S_OK;
}
static WINAPI HRESULT VariantCopy(VARIANT *dst, const VARIANT *src) {
    if (dst == src) return S_OK;
    VARIANT tmp = *src;
    if (!(src->vt & VT_BYREF)) {
        if (src->vt & VT_ARRAY) { HRESULT r = SafeArrayCopy(src->v.ptr, (SAFEARRAY **)&tmp.v.ptr); if (r) return r; }
        else if (src->vt == VT_BSTR) tmp.v.bstr = src->v.bstr ? SysAllocStringLen(src->v.bstr, SysStringLen(src->v.bstr)) : NULL;
        else if ((src->vt == VT_UNKNOWN || src->vt == VT_DISPATCH) && src->v.ptr) ((unknown *)src->v.ptr)->vtbl->AddRef(src->v.ptr);
    }
    VariantClear(dst);
    *dst = tmp;
    return S_OK;
}
// Conversions between numbers, booleans and strings (the common cases).
static int as_number(const VARIANT *v, double *d) {
    switch (v->vt) {
    case VT_EMPTY: *d = 0; return 1;
    case VT_I1: *d = (int8_t)v->v.i4; return 1;   case VT_UI1: *d = (uint8_t)v->v.i4; return 1;
    case VT_I2: *d = (int16_t)v->v.i4; return 1;  case VT_UI2: *d = (uint16_t)v->v.i4; return 1;
    case VT_I4: case VT_INT: *d = v->v.i4; return 1;
    case VT_UI4: case VT_UINT: *d = (uint32_t)v->v.i4; return 1;
    case VT_I8: *d = (double)v->v.i8; return 1;   case VT_UI8: *d = (double)v->v.u8; return 1;
    case VT_R4: *d = v->v.r4; return 1;           case VT_R8: *d = v->v.r8; return 1;
    case VT_BOOL: *d = v->v.boolean ? -1 : 0; return 1;
    case VT_BSTR: { char *a = vx_utf8_dup(v->v.bstr); char *end; *d = a ? strtod(a, &end) : 0; int ok = a && end != a && !*end; free(a); return ok; }
    default: return 0;
    }
}
static WINAPI HRESULT VariantChangeType(VARIANT *dst, const VARIANT *src, uint16_t flags, uint16_t vt) {
    (void)flags;
    if (src->vt == vt) return VariantCopy(dst, src);
    VARIANT out; VariantInit(&out); out.vt = vt;
    double d;
    if (vt == VT_BSTR) {
        char text[64];
        if (src->vt == VT_BOOL) snprintf(text, sizeof text, "%s", src->v.boolean ? "True" : "False");
        else if (!as_number(src, &d)) return DISP_E_TYPEMISMATCH;
        else if (src->vt == VT_R4 || src->vt == VT_R8) snprintf(text, sizeof text, "%.15g", d);
        else snprintf(text, sizeof text, "%.0f", d);
        WCHAR *w = vx_utf16_dup(text);
        out.v.bstr = SysAllocString(w);
        free(w);
    } else {
        if (!as_number(src, &d)) return DISP_E_TYPEMISMATCH;
        switch (vt) {
        case VT_I1: case VT_UI1: case VT_I2: case VT_UI2: case VT_I4: case VT_INT: case VT_UI4: case VT_UINT: out.v.i8 = (int64_t)d; break;
        case VT_I8: out.v.i8 = (int64_t)d; break;
        case VT_UI8: out.v.u8 = (uint64_t)d; break;
        case VT_R4: out.v.r4 = (float)d; break;
        case VT_R8: out.v.r8 = d; break;
        case VT_BOOL: out.v.boolean = d != 0 ? -1 : 0; break;
        default: return DISP_E_TYPEMISMATCH;
        }
    }
    VariantClear(dst);
    *dst = out;
    return S_OK;
}
static WINAPI HRESULT VariantChangeTypeEx(VARIANT *dst, const VARIANT *src, DWORD lcid, uint16_t flags, uint16_t vt) { (void)lcid; return VariantChangeType(dst, src, flags, vt); }
static WINAPI HRESULT PropVariantClear(VARIANT *v) {
    if (v->vt == VT_LPSTR || v->vt == VT_LPWSTR || v->vt == VT_CLSID) { CoTaskMemFree(v->v.ptr); v->vt = VT_EMPTY; v->v.u8 = 0; return S_OK; }
    return VariantClear(v);
}
// PropVariantCopy: strings and GUIDs the PROPVARIANT points to are copied with CoTaskMemAlloc.
static WINAPI HRESULT PropVariantCopy(VARIANT *dst, const VARIANT *src) {
    if (src->vt == VT_LPSTR || src->vt == VT_LPWSTR || src->vt == VT_CLSID) {
        size_t n = src->vt == VT_CLSID ? 16 : src->vt == VT_LPSTR ? strlen(src->v.ptr) + 1 : (vx_wcslen(src->v.ptr) + 1) * 2;
        void *copy = CoTaskMemAlloc(n);
        if (!copy) return E_OUTOFMEMORY;
        memcpy(copy, src->v.ptr, n);
        *dst = *src;
        dst->v.ptr = copy;
        return S_OK;
    }
    VariantInit(dst);
    return VariantCopy(dst, src);
}

// ---- SAFEARRAY -------------------------------------------------------------------------------------------------------------------
#define FADF_HAVEVARTYPE 0x80
#define FADF_BSTR 0x100
#define FADF_UNKNOWN 0x200
#define FADF_DISPATCH 0x400
#define FADF_VARIANT 0x800
static uint32_t elem_size(uint16_t vt) {
    switch (vt) {
    case VT_I1: case VT_UI1: return 1;
    case VT_I2: case VT_UI2: case VT_BOOL: return 2;
    case VT_I4: case VT_UI4: case VT_INT: case VT_UINT: case VT_R4: case VT_ERROR: return 4;
    case VT_I8: case VT_UI8: case VT_R8: case VT_BSTR: case VT_UNKNOWN: case VT_DISPATCH: return 8;
    case VT_VARIANT: return sizeof(VARIANT);
    default: return 0;
    }
}
static size_t element_count(const SAFEARRAY *sa) { size_t n = 1; for (int i = 0; i < sa->dims; i++) n *= sa->bounds[i].count; return n; }
// The array header is preceded by 16 bytes; its last 4 hold the element type (FADF_HAVEVARTYPE).
static WINAPI SAFEARRAY *SafeArrayCreate(uint16_t vt, uint32_t dims, const sa_bound *bounds) {
    uint32_t size = elem_size(vt);
    if (!size || !dims) return NULL;
    uint8_t *mem = calloc(1, 16 + sizeof(SAFEARRAY) + sizeof(sa_bound) * (dims - 1));
    SAFEARRAY *sa = (SAFEARRAY *)(mem + 16);
    *(DWORD *)(mem + 12) = vt;
    sa->dims = (uint16_t)dims; sa->elem_size = size;
    sa->features = FADF_HAVEVARTYPE | (vt == VT_BSTR ? FADF_BSTR : vt == VT_UNKNOWN ? FADF_UNKNOWN : vt == VT_DISPATCH ? FADF_DISPATCH : vt == VT_VARIANT ? FADF_VARIANT : 0);
    for (uint32_t i = 0; i < dims; i++) sa->bounds[i] = bounds[dims - 1 - i];   // stored last dimension first
    sa->data = calloc(element_count(sa) ? element_count(sa) : 1, size);
    return sa;
}
static WINAPI SAFEARRAY *SafeArrayCreateVector(uint16_t vt, LONG lbound, uint32_t count) { sa_bound b = { count, lbound }; return SafeArrayCreate(vt, 1, &b); }
static void clear_elements(SAFEARRAY *sa) {
    size_t n = element_count(sa);
    for (size_t i = 0; i < n; i++) {
        uint8_t *e = (uint8_t *)sa->data + i * sa->elem_size;
        if (sa->features & FADF_BSTR) { SysFreeString(*(WCHAR **)e); *(WCHAR **)e = NULL; }
        else if (sa->features & (FADF_UNKNOWN | FADF_DISPATCH)) { if (*(void **)e) RELEASE(*(void **)e); *(void **)e = NULL; }
        else if (sa->features & FADF_VARIANT) VariantClear((VARIANT *)e);
    }
}
static WINAPI HRESULT SafeArrayDestroy(SAFEARRAY *sa) {
    if (!sa) return S_OK;
    if (sa->locks) return (HRESULT)0x8002000D;                // DISP_E_ARRAYISLOCKED
    clear_elements(sa);
    free(sa->data);
    free((uint8_t *)sa - 16);
    return S_OK;
}
static WINAPI uint32_t SafeArrayGetDim(SAFEARRAY *sa) { return sa ? sa->dims : 0; }
static WINAPI uint32_t SafeArrayGetElemsize(SAFEARRAY *sa) { return sa ? sa->elem_size : 0; }
static WINAPI HRESULT SafeArrayGetVartype(SAFEARRAY *sa, uint16_t *vt) {
    if (!sa || !(sa->features & FADF_HAVEVARTYPE)) return E_INVALIDARG;
    *vt = (uint16_t)*(DWORD *)((uint8_t *)sa - 4);
    return S_OK;
}
static WINAPI HRESULT SafeArrayGetLBound(SAFEARRAY *sa, uint32_t dim, LONG *out) {
    if (!sa || dim < 1 || dim > sa->dims) return DISP_E_BADINDEX;
    *out = sa->bounds[sa->dims - dim].lbound;
    return S_OK;
}
static WINAPI HRESULT SafeArrayGetUBound(SAFEARRAY *sa, uint32_t dim, LONG *out) {
    if (!sa || dim < 1 || dim > sa->dims) return DISP_E_BADINDEX;
    const sa_bound *b = &sa->bounds[sa->dims - dim];
    *out = b->lbound + (LONG)b->count - 1;
    return S_OK;
}
static WINAPI HRESULT SafeArrayLock(SAFEARRAY *sa) { if (!sa) return E_INVALIDARG; sa->locks++; return S_OK; }
static WINAPI HRESULT SafeArrayUnlock(SAFEARRAY *sa) { if (!sa || !sa->locks) return E_INVALIDARG; sa->locks--; return S_OK; }
static WINAPI HRESULT SafeArrayAccessData(SAFEARRAY *sa, void **data) { if (!sa) return E_INVALIDARG; sa->locks++; *data = sa->data; return S_OK; }
static WINAPI HRESULT SafeArrayUnaccessData(SAFEARRAY *sa) { return SafeArrayUnlock(sa); }
// indices: one per dimension, first dimension first.
static void *element(SAFEARRAY *sa, const LONG *idx) {
    size_t offset = 0, scale = 1;
    for (int d = 0; d < sa->dims; d++) {
        const sa_bound *b = &sa->bounds[sa->dims - 1 - d];
        LONG i = idx[d] - b->lbound;
        if (i < 0 || (uint32_t)i >= b->count) return NULL;
        offset += (size_t)i * scale;
        scale *= b->count;
    }
    return (uint8_t *)sa->data + offset * sa->elem_size;
}
static WINAPI HRESULT SafeArrayPtrOfIndex(SAFEARRAY *sa, const LONG *idx, void **out) { *out = element(sa, idx); return *out ? S_OK : DISP_E_BADINDEX; }
static WINAPI HRESULT SafeArrayGetElement(SAFEARRAY *sa, const LONG *idx, void *out) {
    void *e = sa ? element(sa, idx) : NULL;
    if (!e) return DISP_E_BADINDEX;
    if (sa->features & FADF_BSTR) { WCHAR *b = *(WCHAR **)e; *(WCHAR **)out = b ? SysAllocStringLen(b, SysStringLen(b)) : NULL; }
    else if (sa->features & FADF_VARIANT) { VariantInit(out); return VariantCopy(out, e); }
    else { memcpy(out, e, sa->elem_size); if ((sa->features & (FADF_UNKNOWN | FADF_DISPATCH)) && *(void **)out) ((unknown *)*(void **)out)->vtbl->AddRef(*(void **)out); }
    return S_OK;
}
static WINAPI HRESULT SafeArrayPutElement(SAFEARRAY *sa, const LONG *idx, void *value) {
    void *e = sa ? element(sa, idx) : NULL;
    if (!e) return DISP_E_BADINDEX;
    if (sa->features & FADF_BSTR) { SysFreeString(*(WCHAR **)e); *(WCHAR **)e = value ? SysAllocStringLen(value, SysStringLen(value)) : NULL; }
    else if (sa->features & FADF_VARIANT) return VariantCopy(e, value);
    else if (sa->features & (FADF_UNKNOWN | FADF_DISPATCH)) {
        if (value) ((unknown *)value)->vtbl->AddRef(value);
        if (*(void **)e) RELEASE(*(void **)e);
        *(void **)e = value;
    } else memcpy(e, value, sa->elem_size);
    return S_OK;
}
static WINAPI HRESULT SafeArrayCopy(SAFEARRAY *sa, SAFEARRAY **out) {
    *out = NULL;
    if (!sa) return S_OK;
    uint16_t vt = VT_UI1;
    SafeArrayGetVartype(sa, &vt);
    sa_bound b[8];
    for (int i = 0; i < sa->dims && i < 8; i++) b[i] = sa->bounds[sa->dims - 1 - i];
    SAFEARRAY *c = SafeArrayCreate(vt, sa->dims, b);
    if (!c) return E_OUTOFMEMORY;
    size_t n = element_count(sa);
    for (size_t i = 0; i < n; i++) {
        uint8_t *s = (uint8_t *)sa->data + i * sa->elem_size, *d = (uint8_t *)c->data + i * sa->elem_size;
        if (sa->features & FADF_BSTR) *(WCHAR **)d = *(WCHAR **)s ? SysAllocStringLen(*(WCHAR **)s, SysStringLen(*(WCHAR **)s)) : NULL;
        else if (sa->features & FADF_VARIANT) { VariantInit((VARIANT *)d); VariantCopy((VARIANT *)d, (VARIANT *)s); }
        else { memcpy(d, s, sa->elem_size); if ((sa->features & (FADF_UNKNOWN | FADF_DISPATCH)) && *(void **)d) ((unknown *)*(void **)d)->vtbl->AddRef(*(void **)d); }
    }
    *out = c;
    return S_OK;
}

// ---- WinRT basics: strings (HSTRING), no activatable classes -------------------------------------------------------------
// An HSTRING: flags (1: a reference to the caller's buffer, in its HSTRING_HEADER), length, characters. NULL is "".
typedef struct { uint32_t flags, len; const WCHAR *buf; } hstring;
#define REGDB_E_CLASSNOTREG ((HRESULT)0x80040154)
static WINAPI HRESULT WindowsCreateString(const WCHAR *src, uint32_t len, hstring **out) {
    if (!len) { *out = NULL; return S_OK; }
    hstring *h = malloc(sizeof *h + ((size_t)len + 1) * sizeof(WCHAR));
    WCHAR *data = (WCHAR *)(h + 1);
    memcpy(data, src, (size_t)len * sizeof(WCHAR)); data[len] = 0;
    h->flags = 0; h->len = len; h->buf = data;
    *out = h;
    return S_OK;
}
static WINAPI HRESULT WindowsCreateStringReference(const WCHAR *src, uint32_t len, void *header, hstring **out) {
    if (!len) { *out = NULL; return S_OK; }
    hstring *h = header;                                     // HSTRING_HEADER is 24 bytes: room for this
    h->flags = 1; h->len = len; h->buf = src;
    *out = h;
    return S_OK;
}
static WINAPI HRESULT WindowsDeleteString(hstring *h) { if (h && !(h->flags & 1)) free(h); return S_OK; }
static WINAPI HRESULT WindowsDuplicateString(hstring *h, hstring **out) { return h ? WindowsCreateString(h->buf, h->len, out) : (*out = NULL, S_OK); }
static WINAPI const WCHAR *WindowsGetStringRawBuffer(hstring *h, uint32_t *len) {
    static const WCHAR empty[1] = { 0 };
    if (len) *len = h ? h->len : 0;
    return h ? h->buf : empty;
}
static WINAPI uint32_t WindowsGetStringLen(hstring *h) { return h ? h->len : 0; }
static WINAPI BOOL WindowsIsStringEmpty(hstring *h) { return !h || !h->len; }
// A buffer to fill, then turned into a string.
static WINAPI HRESULT WindowsPreallocateStringBuffer(uint32_t len, WCHAR **chars, hstring **buffer) {
    hstring *h = calloc(1, sizeof *h + ((size_t)len + 1) * sizeof(WCHAR));
    h->len = len; h->buf = (WCHAR *)(h + 1);
    *chars = (WCHAR *)(h + 1);
    *buffer = h;
    return S_OK;
}
static WINAPI HRESULT WindowsPromoteStringBuffer(hstring *buffer, hstring **out) { *out = buffer; return S_OK; }
static WINAPI HRESULT WindowsDeleteStringBuffer(hstring *buffer) { free(buffer); return S_OK; }
static WINAPI HRESULT RoInitialize(int type) { (void)type; return CoInitializeEx(NULL, 0); }
static WINAPI void RoUninitialize(void) {}
static WINAPI HRESULT RoGetActivationFactory(hstring *cls, const GUID *iid, void **out) {
    (void)iid;
    if (vx_trace && cls) { char name[256] = ""; for (uint32_t i = 0; i < cls->len && i < 255; i++) name[i] = (char)cls->buf[i]; vx_log("WinRT class %s is not available", name); }
    *out = NULL;
    return REGDB_E_CLASSNOTREG;
}
static WINAPI HRESULT RoActivateInstance(hstring *cls, void **out) { return RoGetActivationFactory(cls, NULL, out); }
static WINAPI HRESULT GetRestrictedErrorInfo(void **info) { *info = NULL; return S_FALSE; }
static WINAPI HRESULT SetRestrictedErrorInfo(void *info) { (void)info; return S_OK; }
static WINAPI BOOL RoOriginateLanguageException(HRESULT hr, hstring *msg, void *unk) { (void)hr; (void)msg; (void)unk; return 1; }
static WINAPI BOOL RoOriginateError(HRESULT hr, hstring *msg) { (void)hr; (void)msg; return 1; }
static WINAPI HRESULT CoIncrementMTAUsage(void **cookie) { *cookie = (void *)1; return S_OK; }
static WINAPI HRESULT CoDecrementMTAUsage(void *cookie) { (void)cookie; return S_OK; }
static const vx_export winrt[] = {
    VX_EXPORT(WindowsCreateString), VX_EXPORT(WindowsCreateStringReference), VX_EXPORT(WindowsDeleteString), VX_EXPORT(WindowsDuplicateString),
    VX_EXPORT(WindowsGetStringRawBuffer), VX_EXPORT(WindowsGetStringLen), VX_EXPORT(WindowsIsStringEmpty),
    VX_EXPORT(WindowsPreallocateStringBuffer), VX_EXPORT(WindowsPromoteStringBuffer), VX_EXPORT(WindowsDeleteStringBuffer),
    VX_EXPORT(RoInitialize), VX_EXPORT(RoUninitialize), VX_EXPORT(RoGetActivationFactory), VX_EXPORT(RoActivateInstance),
    VX_EXPORT(GetRestrictedErrorInfo), VX_EXPORT(SetRestrictedErrorInfo), VX_EXPORT(RoOriginateLanguageException), VX_EXPORT(RoOriginateError),
    VX_EXPORT(CoIncrementMTAUsage), VX_EXPORT(CoDecrementMTAUsage),
};

#define E(name) VX_EXPORT(name)
static const vx_export ole32[] = {
    E(CoInitialize), E(CoInitializeEx), E(CoUninitialize), E(OleInitialize), E(OleUninitialize), E(CoInitializeSecurity),
    E(CoSetProxyBlanket), E(CoGetCurrentProcess), E(CoFreeUnusedLibraries), E(CoFreeUnusedLibrariesEx), E(CoLockObjectExternal),
    E(RegisterDragDrop), E(RevokeDragDrop), E(CoCreateFreeThreadedMarshaler), E(CoWaitForMultipleHandles),
    E(CoCreateInstance), E(CoCreateInstanceEx), E(CoGetClassObject), E(CoRegisterClassObject), E(CoRevokeClassObject),
    E(CoTaskMemAlloc), E(CoTaskMemRealloc), E(CoTaskMemFree), E(CoGetMalloc), E(CoCreateGuid),
    E(StringFromGUID2), E(StringFromCLSID), { "StringFromIID", (void *)StringFromCLSID }, E(CLSIDFromString), E(IIDFromString),
    E(PropVariantClear), E(PropVariantCopy),
    // Some programs look these up in ole32 too.
    E(SysAllocString), E(SysAllocStringLen), E(SysAllocStringByteLen), E(SysFreeString), E(SysStringLen), E(SysStringByteLen),
    E(SysReAllocString), E(SysReAllocStringLen), E(VariantInit), E(VariantClear), E(VariantCopy),
};
static const vx_export oleaut32[] = {
    E(SysAllocString), E(SysAllocStringLen), E(SysAllocStringByteLen), E(SysFreeString), E(SysStringLen), E(SysStringByteLen),
    E(SysReAllocString), E(SysReAllocStringLen), E(VariantInit), E(VariantClear), E(VariantCopy), E(VariantChangeType),
    E(VariantChangeTypeEx), E(SafeArrayCreate), E(SafeArrayCreateVector), E(SafeArrayDestroy), E(SafeArrayGetDim),
    E(SafeArrayGetElemsize), E(SafeArrayGetVartype), E(SafeArrayGetLBound), E(SafeArrayGetUBound), E(SafeArrayLock),
    E(SafeArrayUnlock), E(SafeArrayAccessData), E(SafeArrayUnaccessData), E(SafeArrayPtrOfIndex), E(SafeArrayGetElement),
    E(SafeArrayPutElement), E(SafeArrayCopy),
    // oleaut32 is often imported by ordinal.
    { "#2", (void *)SysAllocString }, { "#3", (void *)SysReAllocString }, { "#4", (void *)SysAllocStringLen },
    { "#5", (void *)SysReAllocStringLen }, { "#6", (void *)SysFreeString }, { "#7", (void *)SysStringLen },
    { "#8", (void *)VariantInit }, { "#9", (void *)VariantClear }, { "#10", (void *)VariantCopy }, { "#12", (void *)VariantChangeType },
    { "#15", (void *)SafeArrayCreate }, { "#16", (void *)SafeArrayDestroy }, { "#17", (void *)SafeArrayGetDim },
    { "#18", (void *)SafeArrayGetElemsize }, { "#19", (void *)SafeArrayGetUBound }, { "#20", (void *)SafeArrayGetLBound },
    { "#21", (void *)SafeArrayLock }, { "#22", (void *)SafeArrayUnlock }, { "#23", (void *)SafeArrayAccessData },
    { "#24", (void *)SafeArrayUnaccessData }, { "#25", (void *)SafeArrayGetElement }, { "#26", (void *)SafeArrayPutElement },
    { "#147", (void *)VariantChangeTypeEx }, { "#148", (void *)SafeArrayPtrOfIndex }, { "#149", (void *)SysStringByteLen },
    { "#150", (void *)SysAllocStringByteLen }, { "#411", (void *)SafeArrayCreateVector },
};

void vx_com_init(void) {
    vx_register_module("ole32.dll", ole32, sizeof ole32 / sizeof ole32[0]);
    vx_register_module("kernel32.dll", winrt, sizeof winrt / sizeof winrt[0]);   // api-ms-win-core-winrt-* resolve to kernel32
    vx_register_module("combase.dll", winrt, sizeof winrt / sizeof winrt[0]);
    vx_register_module("ole32.dll", winrt, sizeof winrt / sizeof winrt[0]);
    vx_register_module("combase.dll", ole32, sizeof ole32 / sizeof ole32[0]);
    vx_register_module("oleaut32.dll", oleaut32, sizeof oleaut32 / sizeof oleaut32[0]);
}

#ifdef __aarch64__
#include "thunks/com.c.inc"
#endif
