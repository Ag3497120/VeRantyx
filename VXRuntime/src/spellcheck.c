// The Windows Spell Checking API's factory (CLSID_SpellCheckerFactory, ISpellCheckerFactory), from Microsoft's
// documentation of spellcheck.h: there are no spell checking dictionaries here, so the factory reports no supported
// languages (an empty IEnumString), IsSupported is FALSE for every tag, and CreateSpellChecker refuses every tag
// (E_INVALIDARG, "there is no spell checker available for languageTag").
// Chromium (the Windows Steam client's steamwebhelper) creates the factory when text is first typed into a field; it
// crashed in its spell checking while CoCreateInstance refused the class.
#include <stdlib.h>
#include <string.h>
#include "vx.h"

typedef int32_t HRESULT;
typedef struct { DWORD d1; uint16_t d2, d3; uint8_t d4[8]; } GUID;
#define S_OK 0
#define S_FALSE 1
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define E_POINTER ((HRESULT)0x80004003)
#define E_INVALIDARG ((HRESULT)0x80070057)

static const GUID IID_IUnknown = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IEnumString = { 0x00000101, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID CLSID_SpellCheckerFactory = { 0x7ab36653, 0x1796, 0x484b, { 0xbd, 0xfa, 0xe7, 0x4f, 0x1d, 0xb7, 0xc1, 0xdc } };
static const GUID IID_ISpellCheckerFactory = { 0x8e018a9d, 0x2415, 0x4677, { 0xbf, 0x08, 0x79, 0x4e, 0xa6, 0x1f, 0x94, 0xbb } };
static int same(const GUID *a, const GUID *b) { return a && !memcmp(a, b, sizeof *a); }

// ---- an empty IEnumString (Next, Skip, Reset, Clone) --------------------------------------------------------------------------
typedef struct { const void *const *vtbl; volatile LONG refs; } enum_string;
static const void *const g_enum_vtbl[];
static WINAPI HRESULT es_qi(enum_string *self, const GUID *iid, void **out) {
    if (!out) return E_POINTER;
    if (same(iid, &IID_IUnknown) || same(iid, &IID_IEnumString)) { __sync_add_and_fetch(&self->refs, 1); *out = self; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static WINAPI DWORD es_addref(enum_string *self) { return (DWORD)__sync_add_and_fetch(&self->refs, 1); }
static WINAPI DWORD es_release(enum_string *self) {
    LONG n = __sync_sub_and_fetch(&self->refs, 1);
    if (!n) free(self);
    return (DWORD)n;
}
static WINAPI HRESULT es_next(enum_string *self, DWORD n, WCHAR **out, DWORD *fetched) {
    (void)self;
    if (!out || (n > 1 && !fetched)) return E_POINTER;
    if (fetched) *fetched = 0;
    return n ? S_FALSE : S_OK;
}
static WINAPI HRESULT es_skip(enum_string *self, DWORD n) { (void)self; return n ? S_FALSE : S_OK; }
static WINAPI HRESULT es_reset(enum_string *self) { (void)self; return S_OK; }
static enum_string *new_enum(void) {
    enum_string *e = malloc(sizeof *e);
    if (e) { e->vtbl = g_enum_vtbl; e->refs = 1; }
    return e;
}
static WINAPI HRESULT es_clone(enum_string *self, enum_string **out) {
    (void)self;
    if (!out) return E_POINTER;
    *out = new_enum();
    return *out ? S_OK : (HRESULT)0x8007000E;
}
static const void *const g_enum_vtbl[] = { (void *)es_qi, (void *)es_addref, (void *)es_release, (void *)es_next, (void *)es_skip,
                                           (void *)es_reset, (void *)es_clone };

// ---- ISpellCheckerFactory: get_SupportedLanguages, IsSupported, CreateSpellChecker ---------------------------------------------
typedef struct { const void *const *vtbl; } factory;
static WINAPI HRESULT f_qi(factory *self, const GUID *iid, void **out) {
    if (!out) return E_POINTER;
    if (same(iid, &IID_IUnknown) || same(iid, &IID_ISpellCheckerFactory)) { *out = self; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static WINAPI DWORD f_addref(factory *self) { (void)self; return 2; }            // (one object for the process, never freed)
static WINAPI DWORD f_release(factory *self) { (void)self; return 1; }
static WINAPI HRESULT f_languages(factory *self, enum_string **out) {
    (void)self;
    if (!out) return E_POINTER;
    *out = new_enum();
    return *out ? S_OK : (HRESULT)0x8007000E;
}
static WINAPI HRESULT f_is_supported(factory *self, const WCHAR *tag, BOOL *out) {
    (void)self;
    if (!tag || !out) return E_POINTER;
    if (!*tag) return E_INVALIDARG;
    *out = 0;
    return S_OK;
}
static WINAPI HRESULT f_create(factory *self, const WCHAR *tag, void **out) {
    (void)self;
    if (!tag || !out) return E_POINTER;
    *out = NULL;
    return E_INVALIDARG;
}
static const void *const g_factory_vtbl[] = { (void *)f_qi, (void *)f_addref, (void *)f_release, (void *)f_languages,
                                              (void *)f_is_supported, (void *)f_create };
static factory g_factory = { g_factory_vtbl };
static HRESULT create_factory(const void *iid, void **out) { return f_qi(&g_factory, iid, out); }

void vx_com_register_class(const void *clsid, HRESULT (*create)(const void *iid, void **out));
void vx_spellcheck_init(void) {
    vx_com_register_class(&CLSID_SpellCheckerFactory, create_factory);
}

#ifdef __aarch64__
#include "thunks/spellcheck.c.inc"
#endif
