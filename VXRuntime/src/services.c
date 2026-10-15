// More of what game DLLs import: PE resources (FindResource walks the module's .rsrc), command lines split into
// arguments, random numbers, process memory figures, UUIDs, path helpers, C runtime collation and time
// formatting, keyboard layouts and IME state, Direct3D 9 profiling markers, and assorted kernel32 functions.
#include "vx.h"
#include <ctype.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#define INVALID_HANDLE ((HANDLE)(intptr_t)-1)

// ---- resources ------------------------------------------------------------------------------------------------------------
// A resource directory: 16-byte header (named entries, then id entries), 8-byte entries (name or id; offset of a
// subdirectory, high bit set, or of a data entry: RVA, size, code page).
static const uint8_t *image_base_of(void *module) { return module ? module : vx_proc.image_base; }
static const uint8_t *resource_root(const uint8_t *base, uint32_t *size) {
    uint32_t pe = *(const uint32_t *)(base + 0x3c);
    const uint8_t *opt = base + pe + 24;
    if (*(const uint16_t *)opt != 0x20b) return NULL;             // PE32+ only
    uint32_t rva = *(const uint32_t *)(opt + 112 + 2 * 8), n = *(const uint32_t *)(opt + 112 + 2 * 8 + 4);
    if (!rva || !n) return NULL;
    *size = n;
    return base + rva;
}
// A name or id as Windows takes them: an integer below 0x10000, "#123", or a string (compared without case).
static int entry_matches(const uint8_t *root, uint32_t name_field, const WCHAR *want) {
    if ((uintptr_t)want < 0x10000) return !(name_field & 0x80000000u) && name_field == (uintptr_t)want;
    if (want[0] == '#') {
        uint32_t id = 0;
        for (const WCHAR *c = want + 1; *c >= '0' && *c <= '9'; c++) id = id * 10 + (uint32_t)(*c - '0');
        return !(name_field & 0x80000000u) && name_field == id;
    }
    if (!(name_field & 0x80000000u)) return 0;
    const uint16_t *s = (const uint16_t *)(root + (name_field & 0x7fffffffu));
    size_t len = s[0], wl = vx_wcslen(want);
    if (len != wl) return 0;
    for (size_t i = 0; i < len; i++) if (towupper(s[1 + i]) != towupper(want[i])) return 0;
    return 1;
}
static const uint8_t *find_entry(const uint8_t *root, const uint8_t *dir, const WCHAR *want) {
    uint16_t named = *(const uint16_t *)(dir + 12), ids = *(const uint16_t *)(dir + 14);
    const uint32_t *e = (const uint32_t *)(dir + 16);
    for (int i = 0; i < named + ids; i++, e += 2)
        if (!want || entry_matches(root, e[0], want)) return (const uint8_t *)e;
    return NULL;
}
static void *find_resource(void *module, const WCHAR *name, const WCHAR *type, uint16_t lang) {
    (void)lang;                                                   // the first language there is
    uint32_t size;
    const uint8_t *root = resource_root(image_base_of(module), &size);
    if (!root) { vx_set_last_error(1812); return NULL; }          // ERROR_RESOURCE_DATA_NOT_FOUND
    const uint8_t *t = find_entry(root, root, type);
    if (!t || !(*(const uint32_t *)(t + 4) & 0x80000000u)) { vx_set_last_error(1813); return NULL; }
    const uint8_t *n = find_entry(root, root + (*(const uint32_t *)(t + 4) & 0x7fffffffu), name);
    if (!n || !(*(const uint32_t *)(n + 4) & 0x80000000u)) { vx_set_last_error(1814); return NULL; }
    const uint8_t *l = find_entry(root, root + (*(const uint32_t *)(n + 4) & 0x7fffffffu), NULL);
    if (!l || (*(const uint32_t *)(l + 4) & 0x80000000u)) { vx_set_last_error(1815); return NULL; }
    return (void *)(root + *(const uint32_t *)(l + 4));            // the data entry: the HRSRC
}
static WCHAR *wide_name(const char *s) { return (uintptr_t)s < 0x10000 ? (WCHAR *)(uintptr_t)s : vx_utf16_dup(s); }
static void free_name(const char *s, WCHAR *w) { if ((uintptr_t)s >= 0x10000) free(w); }
static WINAPI void *FindResourceW(void *module, const WCHAR *name, const WCHAR *type) { return find_resource(module, name, type, 0); }
static WINAPI void *FindResourceExW(void *module, const WCHAR *type, const WCHAR *name, uint16_t lang) { return find_resource(module, name, type, lang); }
static WINAPI void *FindResourceA(void *module, const char *name, const char *type) {
    WCHAR *n = wide_name(name), *t = wide_name(type);
    void *r = find_resource(module, n, t, 0);
    free_name(name, n); free_name(type, t);
    return r;
}
static WINAPI void *FindResourceExA(void *module, const char *type, const char *name, uint16_t lang) { (void)lang; return FindResourceA(module, name, type); }
static WINAPI void *LoadResource(void *module, const uint32_t *entry) { return entry ? (void *)(image_base_of(module) + entry[0]) : NULL; }
static WINAPI void *LockResource(void *data) { return data; }
static WINAPI DWORD SizeofResource(void *module, const uint32_t *entry) { (void)module; return entry ? entry[1] : 0; }
static WINAPI BOOL FreeResource(void *data) { (void)data; return 0; }

// ---- the command line (shell32): Windows' quoting rules ----------------------------------------------------------------------
static WINAPI WCHAR **CommandLineToArgvW(const WCHAR *cmd, int *argc) {
    size_t len = vx_wcslen(cmd);
    // Pointers, then the strings (never longer than the input, plus terminators).
    WCHAR **argv = calloc(1, (len + 2) * sizeof(WCHAR *) + (len + 2) * sizeof(WCHAR) * 2);
    WCHAR *out = (WCHAR *)(argv + len + 2);
    int n = 0;
    const WCHAR *p = cmd;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[n++] = out;
        int quoted = 0;
        if (n == 1) {                                             // the program name: quotes only, no escapes
            if (*p == '"') { p++; while (*p && *p != '"') *out++ = *p++; if (*p) p++; }
            else while (*p && *p != ' ' && *p != '\t') *out++ = *p++;
            *out++ = 0;
            continue;
        }
        for (; *p && (quoted || (*p != ' ' && *p != '\t')); ) {
            if (*p == '\\') {
                int slashes = 0;
                while (*p == '\\') { slashes++; p++; }
                if (*p == '"') {
                    for (int i = 0; i < slashes / 2; i++) *out++ = '\\';
                    if (slashes % 2) *out++ = *p++;
                } else for (int i = 0; i < slashes; i++) *out++ = '\\';
            } else if (*p == '"') {
                p++;
                if (quoted && *p == '"') { *out++ = *p++; continue; }   // "" inside quotes: a quote
                quoted = !quoted;
            } else *out++ = *p++;
        }
        *out++ = 0;
    }
    argv[n] = NULL;
    *argc = n;
    return argv;
}

// ---- process memory (psapi), module information ----------------------------------------------------------------------------
// PROCESS_MEMORY_COUNTERS: size, page faults, peak and current working set, pool figures, pagefile usage.
static WINAPI BOOL GetProcessMemoryInfo(HANDLE p, uint8_t *c, DWORD size) {
    (void)p;
    if (size < 72) { vx_set_last_error(24); return 0; }
    memset(c, 0, size);
    *(DWORD *)c = size;
    mach_task_basic_info_data_t info; mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &n) == KERN_SUCCESS) {
        *(uint64_t *)(c + 8) = info.resident_size_max;            // PeakWorkingSetSize
        *(uint64_t *)(c + 16) = info.resident_size;               // WorkingSetSize
        *(uint64_t *)(c + 56) = info.resident_size;               // PagefileUsage
        *(uint64_t *)(c + 64) = info.resident_size_max;           // PeakPagefileUsage
        if (size >= 80) *(uint64_t *)(c + 72) = info.resident_size;   // PrivateUsage (EX)
    }
    return 1;
}
// MODULEINFO: base, size, entry point.
static WINAPI BOOL GetModuleInformation(HANDLE p, void *module, void **info, DWORD size) {
    (void)p;
    const vx_image *m = vx_image_at((uint64_t)(uintptr_t)(module ? module : vx_proc.image_base));
    if (!m || size < 24) { vx_set_last_error(87); return 0; }
    info[0] = m->base; *(DWORD *)&info[1] = (DWORD)m->size; info[2] = m->base + m->entry_rva;
    return 1;
}

// ---- random numbers, cryptographic providers, event logs (advapi32) ----------------------------------------------------------
static WINAPI BOOL SystemFunction036(void *buf, DWORD n) { arc4random_buf(buf, n); return 1; }   // RtlGenRandom
static WINAPI BOOL CryptAcquireContextA(uintptr_t *prov, const char *c, const char *p, DWORD type, DWORD flags) { (void)c; (void)p; (void)type; (void)flags; *prov = 1; return 1; }
static WINAPI BOOL CryptAcquireContextW(uintptr_t *prov, const WCHAR *c, const WCHAR *p, DWORD type, DWORD flags) { (void)c; (void)p; (void)type; (void)flags; *prov = 1; return 1; }
static WINAPI BOOL CryptGenRandom(uintptr_t prov, DWORD n, void *buf) { (void)prov; arc4random_buf(buf, n); return 1; }
static WINAPI BOOL CryptReleaseContext(uintptr_t prov, DWORD flags) { (void)prov; (void)flags; return 1; }
static int g_event_source;
static WINAPI HANDLE RegisterEventSourceW(const WCHAR *server, const WCHAR *name) { (void)server; (void)name; return &g_event_source; }
static WINAPI HANDLE RegisterEventSourceA(const char *server, const char *name) { (void)server; (void)name; return &g_event_source; }
static WINAPI BOOL DeregisterEventSource(HANDLE h) { (void)h; return 1; }
static WINAPI BOOL ReportEventW(HANDLE h, uint16_t type, uint16_t cat, DWORD id, void *sid, uint16_t n, DWORD size, const WCHAR **strings, void *data) {
    (void)h; (void)type; (void)cat; (void)id; (void)sid; (void)size; (void)data;
    for (uint16_t i = 0; i < n && vx_trace; i++) { char *a = vx_utf8_dup(strings[i]); vx_log("event log: %s", a); free(a); }
    return 1;
}
static WINAPI DWORD EventSetInformation(uint64_t reg, int cls, void *info, DWORD n) { (void)reg; (void)cls; (void)info; (void)n; return 0; }
static WINAPI BOOL AdjustTokenPrivileges(HANDLE t, BOOL all, void *state, DWORD n, void *prev, DWORD *ret) { (void)t; (void)all; (void)state; (void)n; (void)prev; if (ret) *ret = 0; vx_set_last_error(0); return 1; }
static WINAPI BOOL LookupPrivilegeValueW(const WCHAR *sys, const WCHAR *name, DWORD *luid) { (void)sys; (void)name; luid[0] = 20; luid[1] = 0; return 1; }
static WINAPI BOOL LookupPrivilegeValueA(const char *sys, const char *name, DWORD *luid) { (void)sys; (void)name; luid[0] = 20; luid[1] = 0; return 1; }
static WINAPI BOOL EqualSid(const uint8_t *a, const uint8_t *b) { return a && b && a[1] == b[1] && !memcmp(a, b, 8 + 4 * (size_t)a[1]); }
static WINAPI BOOL LookupAccountSidA(const char *sys, void *sid, char *name, DWORD *nn, char *dom, DWORD *dn, int *use) { (void)sys; (void)sid; (void)name; (void)nn; (void)dom; (void)dn; (void)use; vx_set_last_error(1332); return 0; }
static WINAPI DWORD GetNamedSecurityInfoA(const char *obj, int type, DWORD info, void **owner, void **group, void **dacl, void **sacl, void **sd) {
    (void)obj; (void)type; (void)info; (void)owner; (void)group; (void)dacl; (void)sacl; if (sd) *sd = NULL; return 50;   // ERROR_NOT_SUPPORTED
}
static WINAPI LONG RegConnectRegistryA(const char *machine, HANDLE key, HANDLE *out) { (void)machine; (void)key; *out = NULL; return 53; }   // ERROR_BAD_NETPATH

// ---- UUIDs (rpcrt4) ------------------------------------------------------------------------------------------------------------
typedef struct { DWORD d1; uint16_t d2, d3; uint8_t d4[8]; } UUID;
static WINAPI LONG UuidCreate(UUID *u) {
    arc4random_buf(u, sizeof *u);
    u->d3 = (uint16_t)((u->d3 & 0x0fff) | 0x4000);                 // version 4
    u->d4[0] = (uint8_t)((u->d4[0] & 0x3f) | 0x80);
    return 0;
}
static WINAPI LONG UuidToStringA(const UUID *u, char **out) {
    char *s = malloc(37);
    snprintf(s, 37, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", u->d1, u->d2, u->d3, u->d4[0], u->d4[1],
             u->d4[2], u->d4[3], u->d4[4], u->d4[5], u->d4[6], u->d4[7]);
    *out = s;
    return 0;
}
static WINAPI LONG UuidFromStringA(const char *s, UUID *u) {
    unsigned v[11];
    if (!s || sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10]) != 11) return 1705;   // RPC_S_INVALID_STRING_UUID
    u->d1 = v[0]; u->d2 = (uint16_t)v[1]; u->d3 = (uint16_t)v[2];
    for (int i = 0; i < 8; i++) u->d4[i] = (uint8_t)v[3 + i];
    return 0;
}
static WINAPI LONG RpcStringFreeA(char **s) { free(*s); *s = NULL; return 0; }

// ---- shell32 and shlwapi: folders as ID lists, path helpers ----------------------------------------------------------------
// An item ID list here is the folder's Windows path (UTF-16), which SHGetPathFromIDList copies out.
static WINAPI LONG SHGetSpecialFolderLocation(void *wnd, int csidl, void **pidl) {
    LONG (WINAPI *path)(void *, int, HANDLE, DWORD, WCHAR *) = (LONG (WINAPI *)(void *, int, HANDLE, DWORD, WCHAR *))vx_find_export("shell32.dll", "SHGetFolderPathW");
    WCHAR *buf = calloc(260, sizeof(WCHAR));
    LONG r = path ? path(wnd, csidl, NULL, 0, buf) : (LONG)0x80004005;
    if (r) { free(buf); *pidl = NULL; return r; }
    *pidl = buf;
    return 0;
}
static WINAPI BOOL SHGetPathFromIDListW(const WCHAR *pidl, WCHAR *out) { if (!pidl) return 0; memcpy(out, pidl, (vx_wcslen(pidl) + 1) * sizeof(WCHAR)); return 1; }
static WINAPI BOOL SHGetPathFromIDListA(const WCHAR *pidl, char *out) { if (!pidl) return 0; vx_utf16_to_utf8(pidl, vx_wcslen(pidl) + 1, out, 260); return 1; }
static WINAPI uintptr_t SHGetFileInfoA(const char *path, DWORD attrs, void *info, unsigned n, unsigned flags) { (void)path; (void)attrs; (void)flags; if (info) memset(info, 0, n); return 0; }
static WINAPI unsigned DragQueryFileA(void *drop, unsigned i, char *out, unsigned cap) { (void)drop; (void)i; if (out && cap) out[0] = 0; return 0; }
static WINAPI BOOL Shell_NotifyIconA(DWORD msg, void *data) { (void)msg; (void)data; return 1; }
static WINAPI BOOL Shell_NotifyIconW(DWORD msg, void *data) { (void)msg; (void)data; return 1; }
static WINAPI BOOL PathIsRelativeW(const WCHAR *p) { return !p || !((p[0] == '\\') || (p[0] && p[1] == ':')); }
static WINAPI BOOL PathIsRelativeA(const char *p) { return !p || !((p[0] == '\\') || (p[0] && p[1] == ':')); }
static WINAPI BOOL PathIsUNCW(const WCHAR *p) { return p && p[0] == '\\' && p[1] == '\\'; }
static WINAPI WCHAR *PathCombineW(WCHAR *out, const WCHAR *dir, const WCHAR *file) {
    WCHAR tmp[520]; size_t n = 0;
    if (file && !PathIsRelativeW(file)) dir = NULL;
    if (dir) { size_t dl = vx_wcslen(dir); memcpy(tmp, dir, dl * sizeof(WCHAR)); n = dl; }
    if (file && *file) {
        if (n && tmp[n - 1] != '\\') tmp[n++] = '\\';
        size_t fl = vx_wcslen(file); memcpy(tmp + n, file, fl * sizeof(WCHAR)); n += fl;
    }
    tmp[n] = 0;
    char *a = vx_utf8_dup(tmp), full[1024];
    if (vx_full_path(a, full, sizeof full)) vx_utf8_to_utf16(full, strlen(full) + 1, out, 260);
    else memcpy(out, tmp, (n + 1) * sizeof(WCHAR));
    free(a);
    return out;
}
static WINAPI LONG SHDeleteKeyA(HANDLE key, const char *sub) { (void)key; (void)sub; return 2; }   // ERROR_FILE_NOT_FOUND

// ---- the C runtime: wide character classes, collation, time formatting ----------------------------------------------------
static WINAPI int vx_iswctype(WCHAR c, uint16_t mask) {
    int r = 0;
    if ((mask & 0x01) && iswupper(c)) r |= 0x01;  if ((mask & 0x02) && iswlower(c)) r |= 0x02;
    if ((mask & 0x04) && iswdigit(c)) r |= 0x04;  if ((mask & 0x08) && iswspace(c)) r |= 0x08;
    if ((mask & 0x10) && iswpunct(c)) r |= 0x10;  if ((mask & 0x20) && iswcntrl(c)) r |= 0x20;
    if ((mask & 0x40) && c == ' ') r |= 0x40;     if ((mask & 0x80) && iswxdigit(c)) r |= 0x80;
    if ((mask & 0x100) && iswalpha(c)) r |= 0x100;
    return r;
}
static WINAPI uint16_t vx_wctype(const char *name) {
    static const struct { const char *n; uint16_t m; } t[] = { { "upper", 1 }, { "lower", 2 }, { "digit", 4 }, { "space", 8 },
        { "punct", 0x10 }, { "cntrl", 0x20 }, { "blank", 0x40 }, { "xdigit", 0x80 }, { "alpha", 0x100 }, { "alnum", 0x104 },
        { "print", 0x157 }, { "graph", 0x117 } };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(t[i].n, name)) return t[i].m;
    return 0;
}
static WINAPI WCHAR vx_towlower(WCHAR c) { return (WCHAR)towlower(c); }
static WINAPI int vx_iswspace(WCHAR c) { return iswspace(c); }
static WINAPI int vx_iswdigit(WCHAR c) { return iswdigit(c); }
static WINAPI int vx_iswalpha(WCHAR c) { return iswalpha(c); }
static WINAPI int vx_iswalnum(WCHAR c) { return iswalnum(c); }
static WINAPI int vx_iswupper(WCHAR c) { return iswupper(c); }
static WINAPI int vx_iswlower(WCHAR c) { return iswlower(c); }
static WINAPI int vx_iswxdigit(WCHAR c) { return iswxdigit(c); }
static WINAPI int vx_iswpunct(WCHAR c) { return iswpunct(c); }
static WINAPI int vx_iswprint(WCHAR c) { return iswprint(c); }
static WINAPI WCHAR vx_towupper(WCHAR c) { return (WCHAR)towupper(c); }
static WINAPI int vx_strcoll(const char *a, const char *b) { return strcmp(a, b); }
static WINAPI size_t vx_strxfrm(char *d, const char *s, size_t n) { size_t l = strlen(s); if (n) { strncpy(d, s, n); if (l >= n) d[n - 1] = 0; } return l; }
static WINAPI int vx_wcscoll(const WCHAR *a, const WCHAR *b) { for (; *a == *b && *a; a++, b++) {} return *a < *b ? -1 : *a > *b; }
static WINAPI size_t vx_wcsxfrm(WCHAR *d, const WCHAR *s, size_t n) {
    size_t l = vx_wcslen(s);
    if (n) { size_t c = l < n ? l + 1 : n; memcpy(d, s, c * sizeof(WCHAR)); if (l >= n) d[n - 1] = 0; }
    return l;
}
// struct tm is nine ints, as on the Mac.
static WINAPI size_t vx_strftime(char *out, size_t cap, const char *fmt, const struct tm *t) { return strftime(out, cap, fmt, t); }
static WINAPI size_t vx_wcsftime(WCHAR *out, size_t cap, const WCHAR *fmt, const struct tm *t) {
    char *f = vx_utf8_dup(fmt), buf[1024];
    size_t n = strftime(buf, sizeof buf, f, t);
    free(f);
    if (!n || n >= cap) return 0;
    return vx_utf8_to_utf16(buf, n + 1, out, cap) - 1;
}
static WINAPI int vx_mb_cur_max_func(void) { return 4; }          // UTF-8
static int g_crt_errno_dummy;
static WINAPI int vx_set_errno(int e) { int *(WINAPI *loc)(void) = (int *(WINAPI *)(void))vx_find_export("ucrtbase.dll", "_errno"); *(loc ? loc() : &g_crt_errno_dummy) = e; return 0; }
static WINAPI void vx_invalid_parameter_noinfo_noreturn(void) { vx_log("the program reported an invalid parameter to the C runtime"); abort(); }
static WINAPI int vx_seh_filter_dll(DWORD code, void *ptrs) { (void)code; (void)ptrs; return 0; }   // EXCEPTION_CONTINUE_SEARCH
static WINAPI int vx_callnewh(size_t n) { (void)n; return 0; }

// ---- keyboard layouts and the IME: US English, no composition ---------------------------------------------------------------
#define US_LAYOUT ((void *)(uintptr_t)0x04090409)
static WINAPI int GetKeyboardLayoutList(int n, void **out) { if (n >= 1 && out) out[0] = US_LAYOUT; return 1; }
static WINAPI void *LoadKeyboardLayoutA(const char *id, unsigned flags) { (void)id; (void)flags; return US_LAYOUT; }
static WINAPI void *LoadKeyboardLayoutW(const WCHAR *id, unsigned flags) { (void)id; (void)flags; return US_LAYOUT; }
static WINAPI void *ActivateKeyboardLayout(void *hkl, unsigned flags) { (void)hkl; (void)flags; return US_LAYOUT; }
static WINAPI int ToUnicodeEx(DWORD vk, DWORD scan, const uint8_t *state, WCHAR *out, int cap, DWORD flags, void *hkl) {
    (void)hkl;
    int (WINAPI *to)(DWORD, DWORD, const uint8_t *, WCHAR *, int, DWORD) = (int (WINAPI *)(DWORD, DWORD, const uint8_t *, WCHAR *, int, DWORD))vx_find_export("user32.dll", "ToUnicode");
    return to ? to(vk, scan, state, out, cap, flags) : 0;
}
static WINAPI BOOL ImmGetConversionStatus(void *imc, DWORD *conv, DWORD *sent) { (void)imc; if (conv) *conv = 0; if (sent) *sent = 0; return 1; }
static WINAPI BOOL ImmSetConversionStatus(void *imc, DWORD conv, DWORD sent) { (void)imc; (void)conv; (void)sent; return 1; }
static WINAPI DWORD ImmGetProperty(void *hkl, DWORD index) { (void)hkl; (void)index; return 0; }
static WINAPI unsigned ImmGetVirtualKey(void *wnd) { (void)wnd; return 0; }
static WINAPI BOOL ImmIsUIMessageA(void *wnd, unsigned msg, uintptr_t w, intptr_t l) { (void)wnd; (void)msg; (void)w; (void)l; return 0; }
static WINAPI BOOL ImmSetCandidateWindow(void *imc, void *form) { (void)imc; (void)form; return 1; }
static WINAPI DWORD ImmGetCandidateListCountW(void *imc, DWORD *count) { (void)imc; if (count) *count = 0; return 0; }

// ---- user32 odds and ends ------------------------------------------------------------------------------------------------------
typedef struct { LONG left, top, right, bottom; } RECT;
static WINAPI BOOL SetRect(RECT *r, int l, int t, int rt, int b) { if (!r) return 0; r->left = l; r->top = t; r->right = rt; r->bottom = b; return 1; }
// WINDOWINFO: size, window and client rectangles, style, extended style, status, borders, class atom, version.
static WINAPI BOOL GetWindowInfo(void *wnd, uint8_t *info) {
    BOOL (WINAPI *wrect)(void *, RECT *) = (BOOL (WINAPI *)(void *, RECT *))vx_find_export("user32.dll", "GetWindowRect");
    BOOL (WINAPI *crect)(void *, RECT *) = (BOOL (WINAPI *)(void *, RECT *))vx_find_export("user32.dll", "GetClientRect");
    BOOL (WINAPI *to_screen)(void *, LONG *) = (BOOL (WINAPI *)(void *, LONG *))vx_find_export("user32.dll", "ClientToScreen");
    memset(info + 4, 0, 56);
    RECT w, c;
    if (!wrect || !crect || !wrect(wnd, &w) || !crect(wnd, &c)) return 0;
    LONG origin[2] = { 0, 0 };
    if (to_screen) to_screen(wnd, origin);
    c.left += origin[0]; c.right += origin[0]; c.top += origin[1]; c.bottom += origin[1];
    memcpy(info + 4, &w, 16); memcpy(info + 20, &c, 16);
    *(DWORD *)(info + 36) = 0x10cf0000u;                          // WS_VISIBLE | WS_OVERLAPPEDWINDOW
    *(DWORD *)(info + 44) = 1;                                    // WS_ACTIVECAPTION
    *(uint16_t *)(info + 58) = 0x0500;
    return 1;
}
static WINAPI BOOL EnumChildWindows(void *parent, void *fn, intptr_t param) { (void)parent; (void)fn; (void)param; return 1; }
static WINAPI BOOL GetGUIThreadInfo(DWORD tid, uint8_t *info) { (void)tid; DWORD size = *(DWORD *)info; memset(info + 4, 0, size > 4 ? size - 4 : 0); return 1; }
static int g_station;
static WINAPI HANDLE GetProcessWindowStation(void) { return &g_station; }
static int g_desktop;
static WINAPI HANDLE GetThreadDesktop(DWORD tid) { (void)tid; return &g_desktop; }   // (the Default desktop: see below)
// The window station is WinSta0 and every desktop handle is its Default desktop, the input desktop of an unlocked
// session (SteamVR's compositor checks the input desktop's name, and shows a "display locked" colour otherwise).
static BOOL user_object_info(HANDLE obj, int index, void *out, DWORD n, DWORD *need, int wide) {
    if (index == 1) {                                             // UOI_FLAGS: WSF_VISIBLE
        if (need) *need = 12;
        if (n < 12 || !out) { vx_set_last_error(122); return 0; }
        memset(out, 0, 12); ((DWORD *)out)[2] = 1;
        return 1;
    }
    const char *text = NULL;
    if (index == 2) text = obj == &g_station ? "WinSta0" : "Default";              // UOI_NAME
    if (index == 3) text = obj == &g_station ? "WindowStation" : "Desktop";        // UOI_TYPE
    if (!text) { if (need) *need = 0; vx_set_last_error(87); return 0; }
    DWORD size = (DWORD)(strlen(text) + 1) * (wide ? 2 : 1);
    if (need) *need = size;
    if (!out || n < size) { vx_set_last_error(122); return 0; }
    if (wide) vx_return_string_w(text, out, n / 2); else vx_return_string_a(text, out, n);
    return 1;
}
static WINAPI BOOL GetUserObjectInformationW(HANDLE obj, int index, void *out, DWORD n, DWORD *need) { return user_object_info(obj, index, out, n, need, 1); }
static WINAPI BOOL GetUserObjectInformationA(HANDLE obj, int index, void *out, DWORD n, DWORD *need) { return user_object_info(obj, index, out, n, need, 0); }
static int g_icon_copy;
static WINAPI void *CopyIcon(void *icon) { (void)icon; return &g_icon_copy; }
static WINAPI BOOL DrawIconEx(void *dc, int x, int y, void *icon, int w, int h, unsigned step, void *brush, unsigned flags) { (void)dc; (void)x; (void)y; (void)icon; (void)w; (void)h; (void)step; (void)brush; (void)flags; return 1; }
static WINAPI BOOL GetIconInfo(void *icon, void *info) { (void)icon; (void)info; vx_set_last_error(1402); return 0; }

// ---- Direct3D 9: the profiling markers (PIX); the IDirect3D9 object is in d3d9.m ------------------------------------------------------------------------------
static WINAPI int D3DPERF_BeginEvent(DWORD color, const WCHAR *name) { (void)color; (void)name; return 0; }
static WINAPI int D3DPERF_EndEvent(void) { return 0; }
static WINAPI void D3DPERF_SetMarker(DWORD color, const WCHAR *name) { (void)color; (void)name; }
static WINAPI DWORD D3DPERF_GetStatus(void) { return 0; }

// ---- kernel32 ---------------------------------------------------------------------------------------------------------------
static WINAPI BOOL IsBadReadPtr(const void *p, uintptr_t n) { (void)n; return !p; }
static WINAPI BOOL IsBadWritePtr(void *p, uintptr_t n) { (void)n; return !p; }
static WINAPI BOOL IsBadCodePtr(void *p) { return !p; }
static WINAPI BOOL IsDBCSLeadByteEx(unsigned cp, uint8_t c) { (void)cp; (void)c; return 0; }
static WINAPI DWORD GetShortPathNameW(const WCHAR *in, WCHAR *out, DWORD cap) {
    size_t n = vx_wcslen(in);
    if (!out || cap <= n) return (DWORD)n + 1;
    memmove(out, in, (n + 1) * sizeof(WCHAR));
    return (DWORD)n;
}
static WINAPI DWORD GetShortPathNameA(const char *in, char *out, DWORD cap) {
    size_t n = strlen(in);
    if (!out || cap <= n) return (DWORD)n + 1;
    memmove(out, in, n + 1);
    return (DWORD)n;
}
static unsigned g_temp_counter;
static WINAPI unsigned GetTempFileNameA(const char *dir, const char *prefix, unsigned unique, char *out) {
    unsigned u = unique ? unique : (unsigned)(getpid() + __atomic_add_fetch(&g_temp_counter, 1, __ATOMIC_RELAXED)) & 0xffff;
    snprintf(out, 260, "%s%s%.3s%04X.tmp", dir, dir[0] && dir[strlen(dir) - 1] != '\\' ? "\\" : "", prefix ? prefix : "", u);
    if (!unique) {                                                // the file is created, empty
        HANDLE (WINAPI *create)(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE) = (HANDLE (WINAPI *)(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE))vx_find_export("kernel32.dll", "CreateFileA");
        BOOL (WINAPI *close_handle)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "CloseHandle");
        HANDLE h = create(out, 0x40000000u, 0, NULL, 2, 0, NULL);
        if (h != INVALID_HANDLE) close_handle(h);
    }
    return u;
}
static WINAPI unsigned GetTempFileNameW(const WCHAR *dir, const WCHAR *prefix, unsigned unique, WCHAR *out) {
    char *d = vx_utf8_dup(dir), *p = prefix ? vx_utf8_dup(prefix) : NULL, a[300];
    unsigned u = GetTempFileNameA(d, p, unique, a);
    vx_utf8_to_utf16(a, strlen(a) + 1, out, 260);
    free(d); free(p);
    return u;
}
static WINAPI BOOL GetDiskFreeSpaceExW(const WCHAR *root, uint64_t *avail, uint64_t *total, uint64_t *free_bytes) {
    char path[1024] = "/";
    if (root) { char *a = vx_utf8_dup(root); if (!vx_unix_path(a, path, sizeof path)) snprintf(path, sizeof path, "/"); free(a); }
    struct statfs st;
    if (statfs(path, &st)) { vx_set_last_error(3); return 0; }
    if (avail) *avail = (uint64_t)st.f_bavail * st.f_bsize;
    if (total) *total = (uint64_t)st.f_blocks * st.f_bsize;
    if (free_bytes) *free_bytes = (uint64_t)st.f_bfree * st.f_bsize;
    return 1;
}
static WINAPI BOOL GetDiskFreeSpaceExA(const char *root, uint64_t *avail, uint64_t *total, uint64_t *free_bytes) {
    WCHAR *w = root ? vx_utf16_dup(root) : NULL;
    BOOL r = GetDiskFreeSpaceExW(w, avail, total, free_bytes);
    free(w);
    return r;
}
static WINAPI BOOL GetDiskFreeSpaceA(const char *root, DWORD *spc, DWORD *bps, DWORD *nfree, DWORD *ntotal) {
    uint64_t avail = 0, total = 0;
    if (!GetDiskFreeSpaceExA(root, &avail, &total, NULL)) return 0;
    if (spc) *spc = 8; if (bps) *bps = 512;
    if (nfree) *nfree = (DWORD)MIN(avail / 4096, 0xffffffffull);
    if (ntotal) *ntotal = (DWORD)MIN(total / 4096, 0xffffffffull);
    return 1;
}
static WINAPI DWORD GetLogicalDriveStringsA(DWORD cap, char *out) { static const char d[] = "C:\\\0Z:\\\0"; if (cap < sizeof d) return sizeof d; memcpy(out, d, sizeof d); return sizeof d - 1; }
static WINAPI DWORD GetLogicalDriveStringsW(DWORD cap, WCHAR *out) {
    static const WCHAR d[] = { 'C', ':', '\\', 0, 'Z', ':', '\\', 0, 0 };
    if (cap < 9) return 9;
    memcpy(out, d, sizeof d);
    return 8;
}
static WINAPI BOOL GetFileTime(HANDLE h, DWORD *c, DWORD *a, DWORD *w) {
    int fd = vx_handle_fd(h);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) { vx_set_last_error(6); return 0; }
    DWORD *out[3] = { c, a, w };
    struct timespec ts[3] = { st.st_birthtimespec, st.st_atimespec, st.st_mtimespec };
    for (int i = 0; i < 3; i++) {
        if (!out[i]) continue;
        uint64_t t = ((uint64_t)ts[i].tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts[i].tv_nsec / 100;
        out[i][0] = (DWORD)t; out[i][1] = (DWORD)(t >> 32);
    }
    return 1;
}
static WINAPI BOOL FileTimeToDosDateTime(const DWORD *ft, uint16_t *date, uint16_t *tm) {
    uint64_t v = ((uint64_t)ft[1] << 32 | ft[0]) / 10000000ull;
    if (v < 11644473600ull) return 0;
    time_t t = (time_t)(v - 11644473600ull);
    struct tm g; gmtime_r(&t, &g);
    *date = (uint16_t)(((g.tm_year - 80) << 9) | ((g.tm_mon + 1) << 5) | g.tm_mday);
    *tm = (uint16_t)((g.tm_hour << 11) | (g.tm_min << 5) | (g.tm_sec / 2));
    return 1;
}
static WINAPI BOOL LocalFileTimeToFileTime(const DWORD *local, DWORD *ft) {
    time_t now = time(NULL); struct tm lt; localtime_r(&now, &lt);
    int64_t v = (int64_t)((uint64_t)local[1] << 32 | local[0]) - (int64_t)lt.tm_gmtoff * 10000000ll;
    ft[0] = (DWORD)v; ft[1] = (DWORD)((uint64_t)v >> 32);
    return 1;
}
static WINAPI DWORD GetDynamicTimeZoneInformation(uint8_t *tz) {
    DWORD (WINAPI *get)(void *) = (DWORD (WINAPI *)(void *))vx_find_export("kernel32.dll", "GetTimeZoneInformation");
    memset(tz, 0, 432);
    return get ? get(tz) : 0;
}
static WINAPI BOOL GetSystemTimeAdjustment(DWORD *adj, DWORD *inc, BOOL *disabled) { *adj = 156250; *inc = 156250; *disabled = 1; return 1; }
static WINAPI DWORD GetThreadLocale(void) { return 0x409; }
static WINAPI BOOL IsValidLocale(DWORD lcid, DWORD flags) { (void)flags; return lcid == 0x409 || lcid == 0x400 || lcid == 0x800; }
static WINAPI BOOL EnumSystemLocalesA(void *fn, DWORD flags) { (void)flags; return ((BOOL (WINAPI *)(char *))fn)("00000409"), 1; }
static WINAPI BOOL EnumSystemLocalesW(void *fn, DWORD flags) { (void)flags; static WCHAR id[] = { '0', '0', '0', '0', '0', '4', '0', '9', 0 }; ((BOOL (WINAPI *)(WCHAR *))fn)(id); return 1; }
static WINAPI int ResolveLocaleName(const WCHAR *name, WCHAR *out, int cap) { (void)name; return (int)vx_return_string_w("en-US", out, (DWORD)cap) + 1; }
static WINAPI LONG GetUserGeoID(DWORD cls) { (void)cls; return 244; }    // the United States
static WINAPI int GetGeoInfoW(LONG geo, DWORD type, WCHAR *out, int cap, uint16_t lang) { (void)geo; (void)lang; return (int)vx_return_string_w(type == 4 ? "US" : type == 5 ? "USA" : "United States", out, (DWORD)cap) + 1; }
// Number and currency formatting: the value as given (programs pass already-formatted digits).
static WINAPI int copy_number_w(const WCHAR *value, WCHAR *out, int cap) {
    int n = (int)vx_wcslen(value) + 1;
    if (!out || !cap) return n;
    if (cap < n) { vx_set_last_error(122); return 0; }
    memcpy(out, value, (size_t)n * sizeof(WCHAR));
    return n;
}
static WINAPI int GetNumberFormatW(DWORD lcid, DWORD flags, const WCHAR *v, const void *fmt, WCHAR *out, int cap) { (void)lcid; (void)flags; (void)fmt; return copy_number_w(v, out, cap); }
static WINAPI int GetNumberFormatEx(const WCHAR *loc, DWORD flags, const WCHAR *v, const void *fmt, WCHAR *out, int cap) { (void)loc; (void)flags; (void)fmt; return copy_number_w(v, out, cap); }
static WINAPI int GetCurrencyFormatW(DWORD lcid, DWORD flags, const WCHAR *v, const void *fmt, WCHAR *out, int cap) { (void)lcid; (void)flags; (void)fmt; return copy_number_w(v, out, cap); }
static WINAPI int GetCurrencyFormatEx(const WCHAR *loc, DWORD flags, const WCHAR *v, const void *fmt, WCHAR *out, int cap) { (void)loc; (void)flags; (void)fmt; return copy_number_w(v, out, cap); }
static WINAPI int GetDateFormatEx(const WCHAR *loc, DWORD flags, const void *st, const WCHAR *fmt, WCHAR *out, int cap, const WCHAR *cal) {
    (void)loc; (void)cal;
    int (WINAPI *f)(DWORD, DWORD, const void *, const WCHAR *, WCHAR *, int) = (int (WINAPI *)(DWORD, DWORD, const void *, const WCHAR *, WCHAR *, int))vx_find_export("kernel32.dll", "GetDateFormatW");
    return f ? f(0x409, flags, st, fmt, out, cap) : 0;
}
static WINAPI int GetTimeFormatEx(const WCHAR *loc, DWORD flags, const void *st, const WCHAR *fmt, WCHAR *out, int cap) {
    (void)loc;
    int (WINAPI *f)(DWORD, DWORD, const void *, const WCHAR *, WCHAR *, int) = (int (WINAPI *)(DWORD, DWORD, const void *, const WCHAR *, WCHAR *, int))vx_find_export("kernel32.dll", "GetTimeFormatW");
    return f ? f(0x409, flags, st, fmt, out, cap) : 0;
}
// CPINFOEX: max char size, default char, lead bytes, Unicode default, code page, name.
static WINAPI BOOL GetCPInfoExA(unsigned cp, DWORD flags, uint8_t *info) {
    (void)flags;
    memset(info, 0, 288);
    *(DWORD *)info = cp == 65001 || cp == 0 ? 4 : 1; info[4] = '?';
    *(uint16_t *)(info + 18) = '?'; *(DWORD *)(info + 20) = cp ? cp : 65001;
    snprintf((char *)info + 24, 260, "%u (UTF-8)", cp ? cp : 65001);
    return 1;
}
// MEMORYSTATUS (the 32-bit one): length, load, physical, page file, virtual figures.
static WINAPI void GlobalMemoryStatus(DWORD *m) {
    uint8_t ex[64] = { 0 };
    *(DWORD *)ex = 64;
    BOOL (WINAPI *get)(void *) = (BOOL (WINAPI *)(void *))vx_find_export("kernel32.dll", "GlobalMemoryStatusEx");
    if (get) get(ex);
    m[0] = 32; m[1] = *(DWORD *)(ex + 4);
    for (int i = 0; i < 6; i++) { uint64_t v = *(uint64_t *)(ex + 8 + 8 * i); m[2 + i] = v > 0xffffffffull ? 0xffffffffu : (DWORD)v; }
}
static WINAPI BOOL HeapLock(HANDLE h) { (void)h; return 1; }
static WINAPI BOOL HeapUnlock(HANDLE h) { (void)h; return 1; }
static WINAPI BOOL HeapWalk(HANDLE h, void *entry) { (void)h; (void)entry; vx_set_last_error(259); return 0; }   // nothing to walk
static WINAPI BOOL HeapQueryInformation(HANDLE h, int cls, void *info, size_t n, size_t *ret) {
    (void)h;
    if (cls == 0 && n >= 4) { *(DWORD *)info = 2; if (ret) *ret = 4; return 1; }   // HeapCompatibilityInformation: LFH
    vx_set_last_error(87);
    return 0;
}
static WINAPI DWORD GetProcessHeaps(DWORD n, HANDLE *out) {
    HANDLE (WINAPI *heap)(void) = (HANDLE (WINAPI *)(void))vx_find_export("kernel32.dll", "GetProcessHeap");
    if (n >= 1 && out && heap) out[0] = heap();
    return 1;
}
static WINAPI void *LocalReAlloc(void *p, size_t n, unsigned flags) { (void)flags; return realloc(p, n ? n : 1); }
static WINAPI BOOL QueryThreadCycleTime(HANDLE t, uint64_t *cycles) {
    (void)t;
    struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    *cycles = ((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec) * 3;   // nanoseconds at a nominal 3 GHz
    return 1;
}
static WINAPI void DebugBreak(void) { __builtin_debugtrap(); }
static WINAPI void FatalAppExitA(unsigned action, const char *msg) { (void)action; vx_log("fatal application exit: %s", msg ? msg : ""); _exit(255); }
static WINAPI BOOL FlushInstructionCache(HANDLE p, const void *addr, size_t n) { (void)p; (void)addr; (void)n; return 1; }
// Function tables registered at run time (code generators): accepted; such code has no unwind data here.
static WINAPI BOOL RtlAddFunctionTable(void *table, DWORD count, uint64_t base) { (void)table; (void)count; (void)base; return 1; }
static WINAPI BOOL RtlDeleteFunctionTable(void *table) { (void)table; return 1; }
static WINAPI BOOL RtlInstallFunctionTableCallback(uint64_t id, uint64_t base, DWORD len, void *cb, void *ctx, const WCHAR *dll) { (void)id; (void)base; (void)len; (void)cb; (void)ctx; (void)dll; return 1; }
static WINAPI BOOL SetHandleInformation(HANDLE h, DWORD mask, DWORD flags) { (void)h; (void)mask; (void)flags; return 1; }
static WINAPI BOOL GetHandleInformation(HANDLE h, DWORD *flags) { (void)h; *flags = 0; return 1; }
static WINAPI BOOL SetSystemTime(const void *st) { (void)st; vx_set_last_error(1314); return 0; }
static WINAPI DWORD SignalObjectAndWait(HANDLE signal, HANDLE wait, DWORD ms, BOOL alertable) {
    (void)alertable;
    BOOL (WINAPI *set)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "SetEvent");
    BOOL (WINAPI *release)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "ReleaseMutex");
    DWORD (WINAPI *wait_one)(HANDLE, DWORD) = (DWORD (WINAPI *)(HANDLE, DWORD))vx_find_export("kernel32.dll", "WaitForSingleObject");
    if (!set(signal) && release) release(signal);
    return wait_one(wait, ms);
}
static WINAPI BOOL ReadDirectoryChangesW(HANDLE dir, void *buf, DWORD n, BOOL sub, DWORD filter, DWORD *got, void *ov, void *done) {
    (void)dir; (void)buf; (void)n; (void)sub; (void)filter; (void)ov; (void)done;
    if (got) *got = 0;
    vx_set_last_error(50);                                        // not supported: programs poll instead
    return 0;
}
static WINAPI BOOL CopyFileExW(const WCHAR *from, const WCHAR *to, void *progress, void *data, BOOL *cancel, DWORD flags) {
    (void)progress; (void)data; (void)cancel;
    BOOL (WINAPI *copy)(const WCHAR *, const WCHAR *, BOOL) = (BOOL (WINAPI *)(const WCHAR *, const WCHAR *, BOOL))vx_find_export("kernel32.dll", "CopyFileW");
    return copy ? copy(from, to, flags & 1) : 0;
}
// Profile (.ini) files: the defaults (nothing is read yet).
static WINAPI unsigned GetPrivateProfileIntW(const WCHAR *app, const WCHAR *key, int def, const WCHAR *file) { (void)app; (void)key; (void)file; return (unsigned)def; }
static WINAPI unsigned GetPrivateProfileIntA(const char *app, const char *key, int def, const char *file) { (void)app; (void)key; (void)file; return (unsigned)def; }
static WINAPI DWORD GetPrivateProfileSectionW(const WCHAR *app, WCHAR *out, DWORD cap, const WCHAR *file) { (void)app; (void)file; if (out && cap >= 2) out[0] = out[1] = 0; return 0; }
static WINAPI BOOL PeekConsoleInputA(HANDLE h, void *rec, DWORD n, DWORD *got) { (void)h; (void)rec; (void)n; *got = 0; return 1; }
static WINAPI BOOL ReadConsoleInputW(HANDLE h, void *rec, DWORD n, DWORD *got) { (void)h; (void)rec; (void)n; if (got) *got = 0; vx_set_last_error(6); return 0; }
static WINAPI BOOL FlushConsoleInputBuffer(HANDLE h) { (void)h; return 1; }
static WINAPI BOOL GetNumaHighestNodeNumber(DWORD *n) { *n = 0; return 1; }
static WINAPI size_t GetLargePageMinimum(void) { return 0; }
// InitOnceBeginInitialize/InitOnceComplete: the synchronous form (the first caller initializes, others wait).
static pthread_mutex_t g_once_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_once_cond = PTHREAD_COND_INITIALIZER;
static WINAPI BOOL InitOnceBeginInitialize(uint64_t *once, DWORD flags, BOOL *pending, void **ctx) {
    pthread_mutex_lock(&g_once_lock);
    if (flags & 2) {                                              // INIT_ONCE_CHECK_ONLY
        *pending = *once != 2;
        pthread_mutex_unlock(&g_once_lock);
        if (*pending) { vx_set_last_error(87); return 0; }
        if (ctx) *ctx = NULL;
        return 1;
    }
    while (*once == 1) pthread_cond_wait(&g_once_cond, &g_once_lock);
    if (*once == 2) { *pending = 0; if (ctx) *ctx = NULL; }
    else { *once = 1; *pending = 1; }
    pthread_mutex_unlock(&g_once_lock);
    return 1;
}
static WINAPI BOOL InitOnceComplete(uint64_t *once, DWORD flags, void *ctx) {
    (void)ctx;
    pthread_mutex_lock(&g_once_lock);
    *once = flags & 4 ? 0 : 2;                                    // INIT_ONCE_INIT_FAILED: back to the start
    pthread_cond_broadcast(&g_once_cond);
    pthread_mutex_unlock(&g_once_lock);
    return 1;
}
// RtlRestoreContext: resumes at a CONTEXT (context.S).
void vx_restore_context(const void *ctx) __attribute__((noreturn));
static WINAPI void RtlRestoreContext(const void *ctx, void *rec) { (void)rec; vx_restore_context(ctx); }

// ---- files (the Windows 8 forms) ------------------------------------------------------------------------------------------
// CREATEFILE2_EXTENDED_PARAMETERS: size, attributes, flags, security QoS, security attributes, template.
static WINAPI HANDLE CreateFile2(const WCHAR *name, DWORD access, DWORD share, DWORD disposition, const DWORD *params) {
    HANDLE (WINAPI *create)(const WCHAR *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE) = (HANDLE (WINAPI *)(const WCHAR *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE))vx_find_export("kernel32.dll", "CreateFileW");
    return create(name, access, share, NULL, disposition, params ? params[1] | params[2] : 0, NULL);
}
static WINAPI LONG CopyFile2(const WCHAR *from, const WCHAR *to, const DWORD *params) {
    BOOL (WINAPI *copy)(const WCHAR *, const WCHAR *, BOOL) = (BOOL (WINAPI *)(const WCHAR *, const WCHAR *, BOOL))vx_find_export("kernel32.dll", "CopyFileW");
    BOOL fail_if_exists = params && (params[1] & 1);                  // COPY_FILE_FAIL_IF_EXISTS
    return copy(from, to, fail_if_exists) ? 0 : (LONG)(0x80070000u | vx_get_last_error());
}
static WINAPI BOOL CreateHardLinkW(const WCHAR *link_name, const WCHAR *existing, void *sa) {
    (void)sa;
    char *a = vx_utf8_dup(existing), *b = vx_utf8_dup(link_name), pa[1024], pb[1024];
    BOOL ok = vx_unix_path(a, pa, sizeof pa) && vx_unix_path_write(b, pb, sizeof pb, 0) && link(pa, pb) == 0;
    if (!ok) vx_set_last_error(5);
    free(a); free(b);
    return ok;
}
// wsprintfW (user32): the C runtime's wide formatting.
int vx_format_w(WCHAR *out, size_t cap, const WCHAR *fmt, const char *ap);
static WINAPI int wsprintfW(WCHAR *buf, const WCHAR *fmt, ...) {
    vx_va_list ap; vx_va_start(ap, fmt);
    int n = vx_format_w(buf, 1025, fmt, ap);
    vx_va_end(ap);
    return n;
}
static WINAPI int wvsprintfW(WCHAR *buf, const WCHAR *fmt, const char *ap) { return vx_format_w(buf, 1025, fmt, ap); }
// ---- services and security (advapi32): no service manager; SIDs and ACLs as plain memory -----------------------------------
static WINAPI HANDLE OpenSCManagerA(const char *machine, const char *db, DWORD access) { (void)machine; (void)db; (void)access; vx_set_last_error(5); return NULL; }
static WINAPI HANDLE OpenSCManagerW(const WCHAR *machine, const WCHAR *db, DWORD access) { (void)machine; (void)db; (void)access; vx_set_last_error(5); return NULL; }
static WINAPI HANDLE OpenServiceA(HANDLE scm, const char *name, DWORD access) { (void)scm; (void)name; (void)access; vx_set_last_error(1060); return NULL; }
static WINAPI BOOL service_none(void) { vx_set_last_error(6); return 0; }
static WINAPI BOOL CloseServiceHandle(HANDLE h) { (void)h; return 1; }
// A SID: revision, sub-authority count, 6-byte identifier authority, sub-authorities.
static WINAPI DWORD GetLengthSid(const uint8_t *sid) { return 8 + 4 * (DWORD)sid[1]; }
static WINAPI BOOL IsValidSid(const uint8_t *sid) { return sid && sid[0] == 1 && sid[1] <= 15; }
static WINAPI BOOL CopySid(DWORD n, uint8_t *dst, const uint8_t *src) { DWORD len = GetLengthSid(src); if (n < len) { vx_set_last_error(122); return 0; } memcpy(dst, src, len); return 1; }
static WINAPI BOOL CreateWellKnownSid(int type, const void *domain, uint8_t *sid, DWORD *size) {
    (void)domain;
    if (!size || *size < 16) { if (size) *size = 16; vx_set_last_error(122); return 0; }
    memset(sid, 0, 16);
    sid[0] = 1; sid[1] = 1; sid[7] = type == 1 ? 1 : 5;               // World (S-1-1-0) or NT authority
    *(DWORD *)(sid + 8) = type == 1 ? 0 : type == 26 ? 18 : 11;       // ...: LocalSystem 18, else Authenticated Users 11
    *size = 12;
    return 1;
}
static WINAPI BOOL ConvertStringSidToSidA(const char *s, void **sid) {
    (void)s;
    uint8_t *p = calloc(1, 16);
    p[0] = 1; p[1] = 1; p[7] = 1;                                      // S-1-1-0
    *sid = p;
    return 1;
}
static WINAPI BOOL InitializeAcl(uint8_t *acl, DWORD n, DWORD rev) { if (n < 8) return 0; memset(acl, 0, 8); acl[0] = (uint8_t)rev; *(uint16_t *)(acl + 2) = (uint16_t)n; return 1; }
static WINAPI BOOL AddAccessAllowedAce(uint8_t *acl, DWORD rev, DWORD mask, void *sid) { (void)acl; (void)rev; (void)mask; (void)sid; return 1; }
static WINAPI BOOL ConvertStringSecurityDescriptorToSecurityDescriptorW(const WCHAR *s, DWORD rev, void **sd, DWORD *size) {
    (void)s; (void)rev;
    *sd = calloc(1, 40);
    ((uint8_t *)*sd)[0] = 1;                                           // an empty self-relative descriptor
    if (size) *size = 40;
    return 1;
}
// EventWriteString: shown with VXRT_TRACE_EVENTS=1 (programs such as SteamVR write thousands per second).
static WINAPI DWORD EventWriteString(uint64_t reg, uint8_t level, uint64_t keyword, const WCHAR *s) {
    (void)reg; (void)level; (void)keyword;
    static int on = -1;
    if (on < 0) on = getenv("VXRT_TRACE_EVENTS") && atoi(getenv("VXRT_TRACE_EVENTS"));
    if (on && s) vx_log("event: %s", (const char *)s);           // SteamVR passes narrow text here
    return 0;
}

// ---- this process, as the only one -------------------------------------------------------------------------------------
static WINAPI BOOL K32EnumProcesses(DWORD *ids, DWORD size, DWORD *got) { if (size >= 4) ids[0] = (DWORD)getpid(); *got = size >= 4 ? 4 : 0; return 1; }
static DWORD image_name_a(char *out, DWORD cap) {
    const vx_image *m = vx_image_at((uint64_t)(uintptr_t)vx_proc.image_base);
    const char *w = m && m->windows_path ? m->windows_path : "C:\\program.exe";
    DWORD n = (DWORD)strlen(w);
    if (!out || cap <= n) { vx_set_last_error(122); return 0; }
    memcpy(out, w, n + 1);
    return n;
}
static WINAPI DWORD K32GetProcessImageFileNameA(HANDLE p, char *out, DWORD cap) { (void)p; return image_name_a(out, cap); }
static WINAPI BOOL QueryFullProcessImageNameA(HANDLE p, DWORD flags, char *out, DWORD *size) { (void)p; (void)flags; DWORD n = image_name_a(out, *size); if (!n) return 0; *size = n; return 1; }
static WINAPI BOOL QueryFullProcessImageNameW(HANDLE p, DWORD flags, WCHAR *out, DWORD *size) {
    (void)p; (void)flags;
    char a[1024];
    if (!image_name_a(a, sizeof a)) return 0;
    size_t n = vx_utf8_to_utf16(a, strlen(a) + 1, out, *size);
    if (!n || n > *size) { vx_set_last_error(122); return 0; }
    *size = (DWORD)n - 1;
    return 1;
}
static WINAPI BOOL ReplaceFileW(const WCHAR *replaced, const WCHAR *replacement, const WCHAR *backup, DWORD flags, void *a, void *b) {
    (void)backup; (void)flags; (void)a; (void)b;
    BOOL (WINAPI *move)(const WCHAR *, const WCHAR *, DWORD) = (BOOL (WINAPI *)(const WCHAR *, const WCHAR *, DWORD))vx_find_export("kernel32.dll", "MoveFileExW");
    return move(replacement, replaced, 1);
}

static const vx_export k32[] = {
    VX_EXPORT(FindResourceW), VX_EXPORT(FindResourceA), VX_EXPORT(FindResourceExW), VX_EXPORT(FindResourceExA), VX_EXPORT(LoadResource),
    VX_EXPORT(LockResource), VX_EXPORT(SizeofResource), VX_EXPORT(FreeResource), VX_EXPORT(IsBadReadPtr), VX_EXPORT(IsBadWritePtr),
    VX_EXPORT(IsBadCodePtr), VX_EXPORT(IsDBCSLeadByteEx), VX_EXPORT(GetShortPathNameW), VX_EXPORT(GetShortPathNameA),
    VX_EXPORT(GetTempFileNameA), VX_EXPORT(GetTempFileNameW), VX_EXPORT(GetDiskFreeSpaceExW), VX_EXPORT(GetDiskFreeSpaceExA),
    VX_EXPORT(GetDiskFreeSpaceA), VX_EXPORT(GetLogicalDriveStringsA), VX_EXPORT(GetLogicalDriveStringsW), VX_EXPORT(GetFileTime),
    VX_EXPORT(FileTimeToDosDateTime), VX_EXPORT(LocalFileTimeToFileTime), VX_EXPORT(GetDynamicTimeZoneInformation),
    VX_EXPORT(GetSystemTimeAdjustment), VX_EXPORT(GetThreadLocale), VX_EXPORT(IsValidLocale), VX_EXPORT(EnumSystemLocalesA),
    VX_EXPORT(EnumSystemLocalesW), VX_EXPORT(ResolveLocaleName), VX_EXPORT(GetUserGeoID), VX_EXPORT(GetGeoInfoW),
    VX_EXPORT(GetNumberFormatW), VX_EXPORT(GetNumberFormatEx), VX_EXPORT(GetCurrencyFormatW), VX_EXPORT(GetCurrencyFormatEx),
    VX_EXPORT(GetDateFormatEx), VX_EXPORT(GetTimeFormatEx), VX_EXPORT(GetCPInfoExA), VX_EXPORT(GlobalMemoryStatus),
    VX_EXPORT(HeapLock), VX_EXPORT(HeapUnlock), VX_EXPORT(HeapWalk), VX_EXPORT(HeapQueryInformation), VX_EXPORT(GetProcessHeaps),
    VX_EXPORT(LocalReAlloc), VX_EXPORT(QueryThreadCycleTime), VX_EXPORT(DebugBreak), VX_EXPORT(FatalAppExitA),
    VX_EXPORT(FlushInstructionCache), VX_EXPORT(RtlAddFunctionTable), VX_EXPORT(RtlDeleteFunctionTable),
    VX_EXPORT(RtlInstallFunctionTableCallback), VX_EXPORT(SetHandleInformation), VX_EXPORT(GetHandleInformation),
    VX_EXPORT(SetSystemTime), VX_EXPORT(SignalObjectAndWait), VX_EXPORT(ReadDirectoryChangesW),
    VX_EXPORT(CopyFileExW), VX_EXPORT(GetPrivateProfileIntW),
    VX_EXPORT(GetPrivateProfileIntA), VX_EXPORT(GetPrivateProfileSectionW), VX_EXPORT(PeekConsoleInputA), VX_EXPORT(ReadConsoleInputW),
    VX_EXPORT(FlushConsoleInputBuffer), VX_EXPORT(GetNumaHighestNodeNumber), VX_EXPORT(GetLargePageMinimum),
    VX_EXPORT(InitOnceBeginInitialize), VX_EXPORT(InitOnceComplete), VX_EXPORT(RtlRestoreContext),
    VX_EXPORT(CreateFile2), VX_EXPORT(CopyFile2), VX_EXPORT(CreateHardLinkW), VX_EXPORT(K32EnumProcesses),
    VX_EXPORT(K32GetProcessImageFileNameA), VX_EXPORT(QueryFullProcessImageNameA), VX_EXPORT(QueryFullProcessImageNameW),
    VX_EXPORT(ReplaceFileW),
    { "K32GetProcessMemoryInfo", (void *)GetProcessMemoryInfo }, { "K32GetModuleInformation", (void *)GetModuleInformation },
};
static const vx_export psapi[] = { VX_EXPORT(GetProcessMemoryInfo), VX_EXPORT(GetModuleInformation), { "EnumProcesses", (void *)K32EnumProcesses },
                                   { "GetProcessImageFileNameA", (void *)K32GetProcessImageFileNameA } };
static const vx_export advapi[] = {
    VX_EXPORT(SystemFunction036), VX_EXPORT(CryptAcquireContextA), VX_EXPORT(CryptAcquireContextW), VX_EXPORT(CryptGenRandom),
    VX_EXPORT(CryptReleaseContext), VX_EXPORT(RegisterEventSourceW), VX_EXPORT(RegisterEventSourceA), VX_EXPORT(DeregisterEventSource),
    VX_EXPORT(ReportEventW), VX_EXPORT(EventSetInformation), VX_EXPORT(AdjustTokenPrivileges), VX_EXPORT(LookupPrivilegeValueW),
    VX_EXPORT(LookupPrivilegeValueA), VX_EXPORT(EqualSid), VX_EXPORT(LookupAccountSidA), VX_EXPORT(GetNamedSecurityInfoA),
    VX_EXPORT(RegConnectRegistryA), VX_EXPORT(OpenSCManagerA), VX_EXPORT(OpenSCManagerW), VX_EXPORT(OpenServiceA),
    VX_EXPORT(CloseServiceHandle), { "OpenServiceW", (void *)OpenServiceA }, { "StartServiceA", (void *)service_none },
    { "StartServiceW", (void *)service_none }, { "ControlService", (void *)service_none }, { "QueryServiceStatusEx", (void *)service_none },
    { "QueryServiceConfigA", (void *)service_none }, { "QueryServiceConfigW", (void *)service_none },
    VX_EXPORT(GetLengthSid), VX_EXPORT(IsValidSid), VX_EXPORT(CopySid), VX_EXPORT(CreateWellKnownSid), VX_EXPORT(ConvertStringSidToSidA),
    VX_EXPORT(InitializeAcl), VX_EXPORT(AddAccessAllowedAce), VX_EXPORT(ConvertStringSecurityDescriptorToSecurityDescriptorW),
    VX_EXPORT(EventWriteString),
};
static const vx_export rpcrt4[] = { VX_EXPORT(UuidCreate), VX_EXPORT(UuidToStringA), VX_EXPORT(UuidFromStringA), VX_EXPORT(RpcStringFreeA) };
static const vx_export shell32[] = {
    VX_EXPORT(CommandLineToArgvW), VX_EXPORT(SHGetSpecialFolderLocation), VX_EXPORT(SHGetPathFromIDListW), VX_EXPORT(SHGetPathFromIDListA),
    VX_EXPORT(SHGetFileInfoA), VX_EXPORT(DragQueryFileA), VX_EXPORT(Shell_NotifyIconA), VX_EXPORT(Shell_NotifyIconW),
};
static const vx_export shlwapi[] = { VX_EXPORT(PathIsRelativeW), VX_EXPORT(PathIsRelativeA), VX_EXPORT(PathIsUNCW), VX_EXPORT(PathCombineW), VX_EXPORT(SHDeleteKeyA) };
static const vx_export crt[] = {
    { "iswctype", (void *)vx_iswctype }, { "iswspace", (void *)vx_iswspace }, { "iswdigit", (void *)vx_iswdigit },
    { "iswalpha", (void *)vx_iswalpha }, { "iswalnum", (void *)vx_iswalnum }, { "iswupper", (void *)vx_iswupper },
    { "iswlower", (void *)vx_iswlower }, { "iswxdigit", (void *)vx_iswxdigit }, { "iswpunct", (void *)vx_iswpunct },
    { "iswprint", (void *)vx_iswprint }, { "wctype", (void *)vx_wctype }, { "towlower", (void *)vx_towlower }, { "towupper", (void *)vx_towupper },
    { "strcoll", (void *)vx_strcoll }, { "strxfrm", (void *)vx_strxfrm }, { "wcscoll", (void *)vx_wcscoll }, { "wcsxfrm", (void *)vx_wcsxfrm },
    { "strftime", (void *)vx_strftime }, { "wcsftime", (void *)vx_wcsftime }, { "___mb_cur_max_func", (void *)vx_mb_cur_max_func },
    { "_set_errno", (void *)vx_set_errno }, { "_invalid_parameter_noinfo_noreturn", (void *)vx_invalid_parameter_noinfo_noreturn },
    { "_seh_filter_dll", (void *)vx_seh_filter_dll }, { "_seh_filter_exe", (void *)vx_seh_filter_dll }, { "_callnewh", (void *)vx_callnewh },
};
static const vx_export user32[] = {
    VX_EXPORT(GetKeyboardLayoutList), VX_EXPORT(LoadKeyboardLayoutA), VX_EXPORT(LoadKeyboardLayoutW), VX_EXPORT(ActivateKeyboardLayout),
    VX_EXPORT(ToUnicodeEx), VX_EXPORT(SetRect), VX_EXPORT(wsprintfW), VX_EXPORT(wvsprintfW), VX_EXPORT(GetWindowInfo), VX_EXPORT(EnumChildWindows), VX_EXPORT(GetGUIThreadInfo),
    VX_EXPORT(GetProcessWindowStation), VX_EXPORT(GetThreadDesktop), VX_EXPORT(GetUserObjectInformationW), VX_EXPORT(GetUserObjectInformationA), VX_EXPORT(CopyIcon), VX_EXPORT(DrawIconEx), VX_EXPORT(GetIconInfo),
};
static const vx_export imm32[] = {
    VX_EXPORT(ImmGetConversionStatus), VX_EXPORT(ImmSetConversionStatus), VX_EXPORT(ImmGetProperty), VX_EXPORT(ImmGetVirtualKey),
    VX_EXPORT(ImmIsUIMessageA), { "ImmIsUIMessageW", (void *)ImmIsUIMessageA }, VX_EXPORT(ImmSetCandidateWindow), VX_EXPORT(ImmGetCandidateListCountW),
};
static const vx_export d3d9[] = {
    VX_EXPORT(D3DPERF_BeginEvent), VX_EXPORT(D3DPERF_EndEvent), VX_EXPORT(D3DPERF_SetMarker), VX_EXPORT(D3DPERF_GetStatus),
};
void vx_services_init(void) {
    vx_register_module("kernel32.dll", k32, sizeof k32 / sizeof k32[0]);
    vx_register_module("psapi.dll", psapi, sizeof psapi / sizeof psapi[0]);
    vx_register_module("advapi32.dll", advapi, sizeof advapi / sizeof advapi[0]);
    vx_register_module("rpcrt4.dll", rpcrt4, sizeof rpcrt4 / sizeof rpcrt4[0]);
    vx_register_module("shell32.dll", shell32, sizeof shell32 / sizeof shell32[0]);
    vx_register_module("shlwapi.dll", shlwapi, sizeof shlwapi / sizeof shlwapi[0]);
    vx_register_module("ucrtbase.dll", crt, sizeof crt / sizeof crt[0]);
    vx_register_module("msvcrt.dll", crt, sizeof crt / sizeof crt[0]);
    vx_register_module("user32.dll", user32, sizeof user32 / sizeof user32[0]);
    vx_register_module("imm32.dll", imm32, sizeof imm32 / sizeof imm32[0]);
    vx_register_module("d3d9.dll", d3d9, sizeof d3d9 / sizeof d3d9[0]);
}

#ifdef __aarch64__
#include "thunks/services.c.inc"
#endif
