// Text: UTF-8 <-> UTF-16 conversion, code pages, environment variables.
//
// The "ANSI" code page is UTF-8 here (as with Windows' "use Unicode UTF-8 for worldwide language support"
// setting): every A function takes and returns UTF-8, which is also what macOS uses, so paths and names pass
// through unchanged.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <strings.h>
#include "vx.h"

// Decodes one code point from UTF-8; invalid bytes decode as U+FFFD and advance one byte.
static uint32_t utf8_next(const uint8_t **p, const uint8_t *end) {
    const uint8_t *s = *p;
    uint32_t c = *s++;
    int extra = c >= 0xf0 && c < 0xf8 ? 3 : c >= 0xe0 ? (c < 0xf0 ? 2 : 0) : c >= 0xc0 ? 1 : 0;
    if (c >= 0x80 && !extra) { *p = s; return 0xfffd; }
    c &= extra == 3 ? 0x07 : extra == 2 ? 0x0f : extra == 1 ? 0x1f : 0x7f;
    for (int i = 0; i < extra; i++) {
        if (s >= end || (*s & 0xc0) != 0x80) { *p = *p + 1; return 0xfffd; }
        c = (c << 6) | (*s++ & 0x3f);
    }
    *p = s;
    return c;
}

size_t vx_utf8_to_utf16(const char *src, size_t n, WCHAR *out, size_t cap) {
    const uint8_t *s = (const uint8_t *)src, *end = s + n;
    size_t k = 0;
    while (s < end) {
        uint32_t c = utf8_next(&s, end);
        if (c >= 0x10000) {
            c -= 0x10000;
            if (k < cap) out[k] = (WCHAR)(0xd800 | (c >> 10));
            k++;
            c = 0xdc00 | (c & 0x3ff);
        }
        if (k < cap) out[k] = (WCHAR)c;
        k++;
    }
    return k;
}

size_t vx_utf16_to_utf8(const WCHAR *w, size_t n, char *out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = w[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && w[i + 1] >= 0xdc00 && w[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (w[++i] - 0xdc00);
        else if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
        uint8_t b[4]; size_t len;
        if (c < 0x80) { b[0] = (uint8_t)c; len = 1; }
        else if (c < 0x800) { b[0] = 0xc0 | (c >> 6); b[1] = 0x80 | (c & 0x3f); len = 2; }
        else if (c < 0x10000) { b[0] = 0xe0 | (c >> 12); b[1] = 0x80 | ((c >> 6) & 0x3f); b[2] = 0x80 | (c & 0x3f); len = 3; }
        else { b[0] = 0xf0 | (c >> 18); b[1] = 0x80 | ((c >> 12) & 0x3f); b[2] = 0x80 | ((c >> 6) & 0x3f); b[3] = 0x80 | (c & 0x3f); len = 4; }
        for (size_t j = 0; j < len; j++, k++) if (k < cap) out[k] = (char)b[j];
    }
    return k;
}

size_t vx_wcslen(const WCHAR *w) { size_t n = 0; while (w[n]) n++; return n; }

char *vx_utf8_dup(const WCHAR *w) {
    if (!w) return NULL;
    size_t n = vx_wcslen(w), len = vx_utf16_to_utf8(w, n, NULL, 0);
    char *s = malloc(len + 1);
    vx_utf16_to_utf8(w, n, s, len);
    s[len] = 0;
    return s;
}

WCHAR *vx_utf16_dup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s), len = vx_utf8_to_utf16(s, n, NULL, 0);
    WCHAR *w = malloc((len + 1) * sizeof(WCHAR));
    vx_utf8_to_utf16(s, n, w, len);
    w[len] = 0;
    return w;
}

// Copies UTF-8 text into a caller's W buffer of `cap` characters the Windows way: the length without the
// terminator on success; the size needed including the terminator when it does not fit.
DWORD vx_return_string_w(const char *s, WCHAR *buf, DWORD cap) {
    size_t n = strlen(s), len = vx_utf8_to_utf16(s, n, NULL, 0);
    if (!buf || len + 1 > cap) return (DWORD)len + 1;
    vx_utf8_to_utf16(s, n, buf, len);
    buf[len] = 0;
    return (DWORD)len;
}
DWORD vx_return_string_a(const char *s, char *buf, DWORD cap) {
    size_t len = strlen(s);
    if (!buf || len + 1 > cap) return (DWORD)len + 1;
    memcpy(buf, s, len + 1);
    return (DWORD)len;
}

// ---- code pages ---------------------------------------------------------------------------------------------------
#define CP_UTF8 65001
static WINAPI DWORD GetACP(void) { return CP_UTF8; }
static WINAPI DWORD GetOEMCP(void) { return CP_UTF8; }
static WINAPI DWORD GetConsoleOutputCP(void) { return CP_UTF8; }
static WINAPI BOOL SetConsoleOutputCP(DWORD cp) { (void)cp; return 1; }
static WINAPI BOOL IsValidCodePage(DWORD cp) { return cp == 0 || cp == 1 || cp == CP_UTF8 || cp == 1252 || cp == 437; }

// Every code page is treated as UTF-8 (1252 and 437 differ only above 0x7f).
static WINAPI int MultiByteToWideChar(DWORD cp, DWORD flags, const char *s, int n, WCHAR *out, int cap) {
    (void)cp; (void)flags;
    size_t len = n < 0 ? strlen(s) + 1 : (size_t)n;
    size_t need = vx_utf8_to_utf16(s, len, NULL, 0);
    if (!cap) return (int)need;
    if (need > (size_t)cap) { vx_set_last_error(122 /* ERROR_INSUFFICIENT_BUFFER */); return 0; }
    return (int)vx_utf8_to_utf16(s, len, out, (size_t)cap);
}
static WINAPI int WideCharToMultiByte(DWORD cp, DWORD flags, const WCHAR *w, int n, char *out, int cap, const char *def, BOOL *used_def) {
    (void)cp; (void)flags; (void)def;
    if (used_def) *used_def = 0;
    size_t len = n < 0 ? vx_wcslen(w) + 1 : (size_t)n;
    size_t need = vx_utf16_to_utf8(w, len, NULL, 0);
    if (!cap) return (int)need;
    if (need > (size_t)cap) { vx_set_last_error(122); return 0; }
    return (int)vx_utf16_to_utf8(w, len, out, (size_t)cap);
}

// ---- environment ------------------------------------------------------------------------------------------------------
#define ERROR_ENVVAR_NOT_FOUND 203
// Names that start with '=' ("=C:", the C runtime's current directory of each drive) cannot go in the Unix
// environment: they are kept here.
static struct { char *name, *value; } g_hidden[32];
static pthread_mutex_t g_hidden_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *env_get(const char *name) {
    if (name[0] != '=') return getenv(name);
    const char *v = NULL;
    pthread_mutex_lock(&g_hidden_lock);
    for (int i = 0; i < 32 && !v; i++) if (g_hidden[i].name && !strcasecmp(g_hidden[i].name, name)) v = g_hidden[i].value;
    pthread_mutex_unlock(&g_hidden_lock);
    return v;                                                      // (values are replaced, never freed)
}
static int env_set(const char *name, const char *value) {
    if (name[0] != '=') return (value ? setenv(name, value, 1) : unsetenv(name)) == 0;
    pthread_mutex_lock(&g_hidden_lock);
    int slot = -1;
    for (int i = 0; i < 32; i++) {
        if (g_hidden[i].name && !strcasecmp(g_hidden[i].name, name)) { slot = i; break; }
        if (!g_hidden[i].name && slot < 0) slot = i;
    }
    if (slot >= 0) {
        if (!g_hidden[slot].name) g_hidden[slot].name = strdup(name);
        g_hidden[slot].value = value ? strdup(value) : NULL;
    }
    pthread_mutex_unlock(&g_hidden_lock);
    return slot >= 0;
}
static WINAPI DWORD GetEnvironmentVariableA(const char *name, char *buf, DWORD cap) {
    const char *v = env_get(name);
    if (!v) { vx_set_last_error(ERROR_ENVVAR_NOT_FOUND); return 0; }
    return vx_return_string_a(v, buf, cap);
}
static WINAPI DWORD GetEnvironmentVariableW(const WCHAR *name, WCHAR *buf, DWORD cap) {
    char *n = vx_utf8_dup(name);
    const char *v = env_get(n);
    free(n);
    if (!v) { vx_set_last_error(ERROR_ENVVAR_NOT_FOUND); return 0; }
    return vx_return_string_w(v, buf, cap);
}
static WINAPI BOOL SetEnvironmentVariableA(const char *name, const char *value) {
    if (!name || !*name) { vx_set_last_error(87); return 0; }
    return env_set(name, value);
}
static WINAPI BOOL SetEnvironmentVariableW(const WCHAR *name, const WCHAR *value) {
    char *n = vx_utf8_dup(name), *v = vx_utf8_dup(value);
    BOOL ok = SetEnvironmentVariableA(n, v);
    free(n); free(v);
    return ok;
}

// ---- character classes and case mapping (Unicode, through macOS's wctype) --------------------------------------------------
#define LCMAP_LOWERCASE 0x100
#define LCMAP_UPPERCASE 0x200
static int lcmap(DWORD flags, const WCHAR *src, int n, WCHAR *dst, int cap) {
    size_t len = n < 0 ? vx_wcslen(src) + 1 : (size_t)n;
    if (!cap) return (int)len;
    if ((size_t)cap < len) { vx_set_last_error(122); return 0; }
    for (size_t i = 0; i < len; i++)
        dst[i] = flags & LCMAP_UPPERCASE ? (WCHAR)towupper(src[i]) : flags & LCMAP_LOWERCASE ? (WCHAR)towlower(src[i]) : src[i];
    return (int)len;
}
static WINAPI int LCMapStringW(DWORD lcid, DWORD flags, const WCHAR *src, int n, WCHAR *dst, int cap) { (void)lcid; return lcmap(flags, src, n, dst, cap); }
static WINAPI int LCMapStringEx(const WCHAR *locale, DWORD flags, const WCHAR *src, int n, WCHAR *dst, int cap, void *v, void *r, intptr_t h) {
    (void)locale; (void)v; (void)r; (void)h; return lcmap(flags, src, n, dst, cap);
}
static WINAPI BOOL GetStringTypeW(DWORD type, const WCHAR *src, int n, uint16_t *out) {
    size_t len = n < 0 ? vx_wcslen(src) + 1 : (size_t)n;
    for (size_t i = 0; i < len; i++) {
        wint_t c = src[i];
        uint16_t t = 0;
        if (type == 1) {                                  // CT_CTYPE1
            if (iswupper(c)) t |= 0x001; if (iswlower(c)) t |= 0x002; if (iswdigit(c)) t |= 0x004;
            if (iswspace(c)) t |= 0x008; if (iswpunct(c)) t |= 0x010; if (iswcntrl(c)) t |= 0x020;
            if (c == ' ' || c == '\t') t |= 0x040; if (iswxdigit(c)) t |= 0x080; if (iswalpha(c)) t |= 0x100;
            if (c) t |= 0x200;                            // C1_DEFINED
        }
        out[i] = t;
    }
    return 1;
}
// CPINFO: MaxCharSize, DefaultChar[2], LeadByte[12].
static WINAPI BOOL GetCPInfo(DWORD cp, uint8_t *info) {
    (void)cp;
    memset(info, 0, 20);
    *(DWORD *)info = 4;                                   // UTF-8: up to 4 bytes per character
    info[4] = '?';
    return 1;
}

// CompareString: 1 less, 2 equal, 3 greater (CSTR_*). NORM_IGNORECASE folds case; otherwise code point order.
static int compare(const WCHAR *a, int na, const WCHAR *b, int nb, int fold) {
    size_t la = na < 0 ? vx_wcslen(a) : (size_t)na, lb = nb < 0 ? vx_wcslen(b) : (size_t)nb;
    for (size_t i = 0; i < la && i < lb; i++) {
        wint_t x = a[i], y = b[i];
        if (fold) { x = towlower(x); y = towlower(y); }
        if (x != y) return x < y ? 1 : 3;
    }
    return la == lb ? 2 : la < lb ? 1 : 3;
}
static WINAPI int CompareStringW(DWORD lcid, DWORD flags, const WCHAR *a, int na, const WCHAR *b, int nb) { (void)lcid; return compare(a, na, b, nb, flags & 1); }
static WINAPI int CompareStringEx(const WCHAR *loc, DWORD flags, const WCHAR *a, int na, const WCHAR *b, int nb, void *v, void *r, intptr_t p) {
    (void)loc; (void)v; (void)r; (void)p; return compare(a, na, b, nb, flags & 1);
}
static WINAPI int CompareStringOrdinal(const WCHAR *a, int na, const WCHAR *b, int nb, BOOL ignore_case) { return compare(a, na, b, nb, ignore_case); }
static WINAPI int CompareStringA(DWORD lcid, DWORD flags, const char *a, int na, const char *b, int nb) {
    (void)lcid;
    size_t la = na < 0 ? strlen(a) : (size_t)na, lb = nb < 0 ? strlen(b) : (size_t)nb;
    int c = (flags & 1) ? strncasecmp(a, b, la < lb ? la : lb) : strncmp(a, b, la < lb ? la : lb);
    if (c) return c < 0 ? 1 : 3;
    return la == lb ? 2 : la < lb ? 1 : 3;
}

// %NAME% references replaced by environment values (unknown names stay as they are). Returns the size needed,
// terminator included.
static char *expand(const char *src) {
    size_t cap = strlen(src) + 1, len = 0;
    char *out = malloc(cap);
    for (const char *p = src; *p;) {
        const char *end = *p == '%' ? strchr(p + 1, '%') : NULL;
        const char *val = NULL; size_t take = 1;
        if (end && end > p + 1) {
            char name[256]; size_t n = (size_t)(end - p - 1);
            if (n < sizeof name) { memcpy(name, p + 1, n); name[n] = 0; val = getenv(name); }
            if (val) take = n + 2;
        }
        const char *piece = val ? val : p; size_t plen = val ? strlen(val) : 1;
        if (len + plen + 1 > cap) { cap = (len + plen + 1) * 2; out = realloc(out, cap); }
        memcpy(out + len, piece, plen); len += plen;
        p += take;
    }
    out[len] = 0;
    return out;
}
static WINAPI DWORD ExpandEnvironmentStringsA(const char *src, char *dst, DWORD n) {
    char *e = expand(src);
    DWORD need = (DWORD)strlen(e) + 1;
    if (dst && n >= need) memcpy(dst, e, need);
    free(e);
    return need;
}
static WINAPI DWORD ExpandEnvironmentStringsW(const WCHAR *src, WCHAR *dst, DWORD n) {
    char *a = vx_utf8_dup(src), *e = expand(a);
    size_t len = vx_utf8_to_utf16(e, strlen(e), NULL, 0);
    if (dst && n >= len + 1) { vx_utf8_to_utf16(e, strlen(e), dst, len); dst[len] = 0; }
    free(a); free(e);
    return (DWORD)len + 1;
}

// ---- the environment block -----------------------------------------------------------------------------------------------
extern char **environ;
static WINAPI char *GetEnvironmentStringsA(void) {
    size_t len = 1;
    for (char **e = environ; *e; e++) len += strlen(*e) + 1;
    char *block = malloc(len), *p = block;
    for (char **e = environ; *e; e++) { size_t n = strlen(*e) + 1; memcpy(p, *e, n); p += n; }
    *p = 0;
    return block;
}
static WINAPI WCHAR *GetEnvironmentStringsW(void) {
    char *a = GetEnvironmentStringsA();
    size_t len = 0;
    while (a[len] || a[len + 1]) len++;
    len += 2;
    size_t n = vx_utf8_to_utf16(a, len, NULL, 0);
    WCHAR *w = malloc(n * sizeof(WCHAR));
    vx_utf8_to_utf16(a, len, w, n);
    free(a);
    return w;
}
static WINAPI BOOL FreeEnvironmentStringsA(char *b) { free(b); return 1; }
static WINAPI BOOL FreeEnvironmentStringsW(WCHAR *b) { free(b); return 1; }

static WINAPI int lstrcmpiA(const char *a, const char *b) { return strcasecmp(a, b); }
static WINAPI int lstrcmpA(const char *a, const char *b) { return strcmp(a, b); }
static WINAPI int lstrlenA(const char *s) { return s ? (int)strlen(s) : 0; }
static WINAPI int lstrlenW(const WCHAR *s) { return s ? (int)vx_wcslen(s) : 0; }

static const vx_export text_k32[] = {
    VX_EXPORT(lstrcmpiA), VX_EXPORT(lstrcmpA), VX_EXPORT(lstrlenA), VX_EXPORT(lstrlenW),
    VX_EXPORT(CompareStringW), VX_EXPORT(CompareStringEx), VX_EXPORT(CompareStringOrdinal), VX_EXPORT(CompareStringA),
    VX_EXPORT(ExpandEnvironmentStringsA), VX_EXPORT(ExpandEnvironmentStringsW),
    VX_EXPORT(LCMapStringW), VX_EXPORT(LCMapStringEx), VX_EXPORT(GetStringTypeW), VX_EXPORT(GetCPInfo),
    VX_EXPORT(GetEnvironmentStringsA), VX_EXPORT(GetEnvironmentStringsW), VX_EXPORT(FreeEnvironmentStringsA), VX_EXPORT(FreeEnvironmentStringsW),
    { "GetEnvironmentStrings", (void *)GetEnvironmentStringsA },
    VX_EXPORT(GetACP), VX_EXPORT(GetOEMCP), VX_EXPORT(GetConsoleOutputCP), VX_EXPORT(SetConsoleOutputCP),
    VX_EXPORT(IsValidCodePage), VX_EXPORT(MultiByteToWideChar), VX_EXPORT(WideCharToMultiByte),
    VX_EXPORT(GetEnvironmentVariableA), VX_EXPORT(GetEnvironmentVariableW),
    VX_EXPORT(SetEnvironmentVariableA), VX_EXPORT(SetEnvironmentVariableW),
};

void vx_text_init(void) {
    vx_register_module("kernel32.dll", text_k32, sizeof text_k32 / sizeof text_k32[0]);
    vx_register_module("kernelbase.dll", text_k32, sizeof text_k32 / sizeof text_k32[0]);
}
