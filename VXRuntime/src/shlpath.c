// shlwapi's path string functions that Chromium (the Windows Steam client's steamwebhelper) imports and the runtime
// did not have: PathFindFileName, PathFindExtension, PathRemoveExtension, PathMatchSpec(Ex). Pure string work on
// Windows paths, from Microsoft's documentation of each function; nothing touches the file system.
#include "vx.h"

typedef int32_t HRESULT;
#define S_OK 0
#define S_FALSE 1
#define E_INVALIDARG ((HRESULT)0x80070057)
enum { PMSF_NORMAL = 0, PMSF_MULTIPLE = 1, PMSF_DONT_STRIP_SPACES = 0x10000 };

// The same code for the A (bytes) and W (UTF-16) forms: CH is the character type.
#define DEFINE_PATH_FUNCTIONS(SUFFIX, CH)                                                                                \
/* The last component: after the last \, / or drive colon that is followed by more than separators ("c:\a\b\" ->  */ \
/* "b\"); the whole string when there is none. */                                                                     \
static WINAPI CH *PathFindFileName##SUFFIX(const CH *path) {                                                          \
    if (!path) return NULL;                                                                                            \
    const CH *last = path;                                                                                             \
    for (const CH *p = path; *p; p++)                                                                                  \
        if ((*p == '\\' || *p == '/' || *p == ':') && p[1] && p[1] != '\\' && p[1] != '/') last = p + 1;            \
    return (CH *)last;                                                                                                 \
}                                                                                                                      \
/* The "." that starts the last component's extension, else the terminating NUL. */                                   \
static WINAPI CH *PathFindExtension##SUFFIX(const CH *path) {                                                         \
    if (!path) return NULL;                                                                                            \
    const CH *dot = NULL, *p = path;                                                                                   \
    for (; *p; p++) {                                                                                                  \
        if (*p == '\\' || *p == '/' || *p == ':') dot = NULL;                                                          \
        else if (*p == '.') dot = p;                                                                                   \
    }                                                                                                                  \
    return (CH *)(dot ? dot : p);                                                                                      \
}                                                                                                                      \
static WINAPI void PathRemoveExtension##SUFFIX(CH *path) {                                                            \
    if (path) *PathFindExtension##SUFFIX(path) = 0;                                                                    \
}                                                                                                                      \
/* MS-DOS wildcards, without case: * any run of characters, ? one character; "*.*" matches every name, even one  */  \
/* without a dot. */                                                                                                  \
static CH fold##SUFFIX(CH c) { return c >= 'a' && c <= 'z' ? (CH)(c - 32) : c; }                                    \
static int wild##SUFFIX(const CH *s, const CH *p, const CH *end) {                                                    \
    while (p < end) {                                                                                                  \
        if (*p == '*') {                                                                                               \
            while (p < end && *p == '*') p++;                                                                          \
            if (p == end) return 1;                                                                                    \
            for (; *s; s++) if (wild##SUFFIX(s, p, end)) return 1;                                                     \
            return 0;                                                                                                  \
        }                                                                                                              \
        if (!*s || (*p != '?' && fold##SUFFIX(*p) != fold##SUFFIX(*s))) return 0;                                     \
        s++; p++;                                                                                                      \
    }                                                                                                                  \
    return !*s;                                                                                                        \
}                                                                                                                      \
static int match_one##SUFFIX(const CH *file, const CH *p, const CH *end) {                                            \
    if (end - p == 3 && p[0] == '*' && p[1] == '.' && p[2] == '*') return 1;                                          \
    return wild##SUFFIX(file, p, end);                                                                                 \
}                                                                                                                      \
/* One pattern, or (multiple) a list separated by ";" whose patterns lose leading spaces unless told not to. */       \
static int match_spec##SUFFIX(const CH *file, const CH *spec, DWORD flags) {                                          \
    if (!(flags & PMSF_MULTIPLE)) {                                                                                    \
        const CH *end = spec; while (*end) end++;                                                                      \
        if (!(flags & PMSF_DONT_STRIP_SPACES)) while (*spec == ' ') spec++;                                            \
        return match_one##SUFFIX(file, spec, end);                                                                     \
    }                                                                                                                  \
    for (;;) {                                                                                                         \
        if (!(flags & PMSF_DONT_STRIP_SPACES)) while (*spec == ' ') spec++;                                            \
        const CH *end = spec; while (*end && *end != ';') end++;                                                       \
        if (end > spec && match_one##SUFFIX(file, spec, end)) return 1;                                                \
        if (!*end) return 0;                                                                                           \
        spec = end + 1;                                                                                                \
    }                                                                                                                  \
}                                                                                                                      \
static WINAPI BOOL PathMatchSpec##SUFFIX(const CH *file, const CH *spec) {                                            \
    if (!file || !spec) return 0;                                                                                      \
    return match_spec##SUFFIX(file, spec, PMSF_MULTIPLE);                                                              \
}                                                                                                                      \
static WINAPI HRESULT PathMatchSpecEx##SUFFIX(const CH *file, const CH *spec, DWORD flags) {                          \
    if (!file || !spec || (flags & ~(DWORD)(PMSF_MULTIPLE | PMSF_DONT_STRIP_SPACES))) return E_INVALIDARG;            \
    return match_spec##SUFFIX(file, spec, flags) ? S_OK : S_FALSE;                                                     \
}

DEFINE_PATH_FUNCTIONS(A, char)
DEFINE_PATH_FUNCTIONS(W, WCHAR)

static const vx_export shlwapi[] = {
    VX_EXPORT(PathFindFileNameA), VX_EXPORT(PathFindFileNameW), VX_EXPORT(PathFindExtensionA), VX_EXPORT(PathFindExtensionW),
    VX_EXPORT(PathRemoveExtensionA), VX_EXPORT(PathRemoveExtensionW), VX_EXPORT(PathMatchSpecA), VX_EXPORT(PathMatchSpecW),
    VX_EXPORT(PathMatchSpecExA), VX_EXPORT(PathMatchSpecExW),
};

void vx_shlpath_init(void) {
    vx_register_module("shlwapi.dll", shlwapi, sizeof shlwapi / sizeof shlwapi[0]);
    vx_register_module("kernelbase.dll", shlwapi, sizeof shlwapi / sizeof shlwapi[0]);   // (api-ms-win-core-path... and
}                                                                                         //  Windows 8's kernelbase have them)

#ifdef __aarch64__
#include "thunks/shlpath.c.inc"
#endif
