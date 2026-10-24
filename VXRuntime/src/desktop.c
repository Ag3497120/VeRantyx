// The rest of the Win32 surface games link against (SDL, engines' platform layers, crash reporters) and mostly
// can do without: window properties, clipboard, dialogs, icons, device notifications, gamma ramps (job objects are
// jobs.c's), pipes, power status, version resources, device enumeration (setupapi), the multimedia wave API (no devices:
// programs fall back to WASAPI or XAudio2), WinINet (no connections), IME. Each does what it can honestly do
// here, or fails the way Windows does when the thing is absent.
#include "vx.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define INVALID_HANDLE ((HANDLE)(intptr_t)-1)
typedef struct { LONG left, top, right, bottom; } RECT;

// ---- kernel32 ----------------------------------------------------------------------------------------------------------------
static WINAPI int MulDiv(int a, int b, int c) {
    if (!c) return -1;
    int64_t r = (int64_t)a * b;
    r = (r < 0) == (c < 0) ? (r + c / 2) / c : (r - c / 2) / c;     // rounded, as Windows does
    return r > INT32_MAX || r < INT32_MIN ? -1 : (int)r;
}
static WINAPI BOOL IsDBCSLeadByte(uint8_t c) { (void)c; return 0; }          // the ANSI code page is UTF-8
int vx_debug_output(void);                     // kernel32.c
static WINAPI void OutputDebugStringW(const WCHAR *s) {
    if (!vx_debug_output() || !s) return;
    char *a = vx_utf8_dup(s);
    vx_log("debug: %s", a);
    free(a);
}
static WINAPI DWORD FormatMessageW(DWORD flags, const void *src, DWORD id, DWORD lang, WCHAR *buf, DWORD size, void *args) {
    (void)src; (void)lang; (void)args;
    char text[64];
    snprintf(text, sizeof text, "Windows error %u", id);
    WCHAR *w = vx_utf16_dup(text);
    DWORD n = (DWORD)vx_wcslen(w);
    if (flags & 0x100) { *(WCHAR **)buf = w; return n; }                     // FORMAT_MESSAGE_ALLOCATE_BUFFER (LocalFree frees it)
    if (!buf || size <= n) { free(w); vx_set_last_error(122); return 0; }
    memcpy(buf, w, (n + 1) * sizeof(WCHAR));
    free(w);
    return n;
}
static WINAPI DWORD GetUserDefaultLCID(void) { return 0x409; }
static WINAPI DWORD GetSystemDefaultLCID(void) { return 0x409; }
static WINAPI uint16_t GetUserDefaultLangID(void) { return 0x409; }
static WINAPI uint16_t GetSystemDefaultLangID(void) { return 0x409; }
static WINAPI uint16_t GetUserDefaultUILanguage(void) { return 0x409; }
// Locales by LCID and name: the user's is English (United States); the others are known so a program can name them
// (Elite Dangerous rejected its controller presets, made for "en-GB", when every name came back as en-US).
static const vx_locale g_locales[] = {
    { 0x0409, "en-US", "English (United States)", "ENU", "en", "US" }, { 0x0809, "en-GB", "English (United Kingdom)", "ENG", "en", "GB" },
    { 0x0c09, "en-AU", "English (Australia)", "ENA", "en", "AU" },     { 0x1009, "en-CA", "English (Canada)", "ENC", "en", "CA" },
    { 0x0407, "de-DE", "German (Germany)", "DEU", "de", "DE" },        { 0x040c, "fr-FR", "French (France)", "FRA", "fr", "FR" },
    { 0x0c0a, "es-ES", "Spanish (Spain)", "ESN", "es", "ES" },         { 0x0410, "it-IT", "Italian (Italy)", "ITA", "it", "IT" },
    { 0x0416, "pt-BR", "Portuguese (Brazil)", "PTB", "pt", "BR" },     { 0x0816, "pt-PT", "Portuguese (Portugal)", "PTG", "pt", "PT" },
    { 0x0419, "ru-RU", "Russian (Russia)", "RUS", "ru", "RU" },        { 0x0415, "pl-PL", "Polish (Poland)", "PLK", "pl", "PL" },
    { 0x0413, "nl-NL", "Dutch (Netherlands)", "NLD", "nl", "NL" },     { 0x041d, "sv-SE", "Swedish (Sweden)", "SVE", "sv", "SE" },
    { 0x0411, "ja-JP", "Japanese (Japan)", "JPN", "ja", "JP" },        { 0x0412, "ko-KR", "Korean (Korea)", "KOR", "ko", "KR" },
    { 0x0804, "zh-CN", "Chinese (Simplified, China)", "CHS", "zh", "CN" }, { 0x0404, "zh-TW", "Chinese (Traditional, Taiwan)", "CHT", "zh", "TW" },
};
const vx_locale *vx_locale_by_lcid(DWORD lcid) {
    lcid &= 0xffff;
    if (!lcid || lcid == 0x0400 || lcid == 0x0800 || lcid == 0x0c00 || lcid == 0x1000) lcid = 0x0409;   // the user's, the system's, ...
    for (size_t i = 0; i < sizeof g_locales / sizeof g_locales[0]; i++) if (g_locales[i].lcid == lcid) return &g_locales[i];
    return NULL;
}
const vx_locale *vx_locale_by_name(const char *name) {
    if (!name || !*name) return &g_locales[0];                            // LOCALE_NAME_USER_DEFAULT (NULL) and the invariant ("")
    for (size_t i = 0; i < sizeof g_locales / sizeof g_locales[0]; i++) if (!strcasecmp(g_locales[i].name, name)) return &g_locales[i];
    if (!strcasecmp(name, "!x-sys-default-locale")) return &g_locales[0];
    return NULL;
}
const vx_locale *vx_locale_at(int i) { return i >= 0 && i < (int)(sizeof g_locales / sizeof g_locales[0]) ? &g_locales[i] : NULL; }
// LOCALE_* values programs ask for: names from the table above, the rest (number and date formats) as English (United States).
static const char *locale_value(const vx_locale *l, DWORD type, char *scratch) {
    if (!l) l = &g_locales[0];
    switch (type & 0xffff) {                                   // (without LOCALE_NOUSEROVERRIDE and the like, above it)
    case 0x01: snprintf(scratch, 8, "%04x", (unsigned)l->lcid); return scratch;
    case 0x02: case 0x1001: case 0x72: case 0x73: return l->english;
    case 0x03: return l->abbrev;          case 0x59: return l->iso639; case 0x5a: return l->iso3166;
    case 0x5c: return l->name;
    case 0x0e: return ".";                case 0x0f: return ",";       case 0x14: return "$";
    case 0x1004: return "1252";           case 0x0b: return "437";     case 0x1d: return "/";
    case 0x1e: return ":";                case 0x1f: return "M/d/yyyy"; case 0x20: return "dddd, MMMM d, yyyy";
    case 0x1003: return "h:mm:ss tt";     case 0x28: return "AM";      case 0x29: return "PM";
    default: return "";
    }
}
// LOCALE_RETURN_NUMBER: the value as a DWORD in the buffer (two WCHARs, four chars) instead of text; LOCALE_ILANGUAGE
// is the LCID (its text is hex), the other numeric values are decimal text.
static int locale_number(const vx_locale *l, DWORD type, void *out, int cap, int units) {
    if (!out && !cap) return units;
    if (!out || cap < units) { vx_set_last_error(122); return 0; }
    char scratch[8];
    const char *text = locale_value(l, type, scratch);
    DWORD value = (type & 0xffff) == 0x01 ? l->lcid : (DWORD)strtoul(text, NULL, 10);
    memcpy(out, &value, sizeof value);
    return units;
}
static WINAPI int GetLocaleInfoW(DWORD lcid, DWORD type, WCHAR *out, int cap) {
    const vx_locale *l = vx_locale_by_lcid(lcid);
    if (!l) { vx_set_last_error(87); return 0; }
    if (type & 0x20000000) return locale_number(l, type, out, cap, 2);
    char scratch[8];
    return (int)vx_return_string_w(locale_value(l, type, scratch), out, (DWORD)cap) + (out && cap ? 1 : 0);
}
static WINAPI int GetLocaleInfoA(DWORD lcid, DWORD type, char *out, int cap) {
    const vx_locale *l = vx_locale_by_lcid(lcid);
    if (!l) { vx_set_last_error(87); return 0; }
    if (type & 0x20000000) return locale_number(l, type, out, cap, 4);
    char scratch[8];
    return (int)vx_return_string_a(locale_value(l, type, scratch), out, (DWORD)cap) + (out && cap ? 1 : 0);
}
// SYSTEMTIME: year, month, weekday, day, hour, minute, second, ms.
static int format_time(const uint16_t *st, int date, char *out, size_t cap) {
    if (st) return snprintf(out, cap, date ? "%u/%u/%u" : "%u:%02u:%02u", date ? st[1] : st[4], date ? st[3] : st[5], date ? st[0] : st[6]);
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    return (int)strftime(out, cap, date ? "%m/%d/%Y" : "%H:%M:%S", &tm);
}
static WINAPI int GetDateFormatW(DWORD lcid, DWORD flags, const uint16_t *st, const WCHAR *fmt, WCHAR *out, int cap) {
    (void)lcid; (void)flags; (void)fmt;
    char s[64]; format_time(st, 1, s, sizeof s);
    return (int)vx_return_string_w(s, out, (DWORD)cap) + (out && cap ? 1 : 0);
}
static WINAPI int GetTimeFormatW(DWORD lcid, DWORD flags, const uint16_t *st, const WCHAR *fmt, WCHAR *out, int cap) {
    (void)lcid; (void)flags; (void)fmt;
    char s[64]; format_time(st, 0, s, sizeof s);
    return (int)vx_return_string_w(s, out, (DWORD)cap) + (out && cap ? 1 : 0);
}
static WINAPI int GetDateFormatA(DWORD lcid, DWORD flags, const uint16_t *st, const char *fmt, char *out, int cap) {
    (void)lcid; (void)flags; (void)fmt;
    char s[64]; format_time(st, 1, s, sizeof s);
    return (int)vx_return_string_a(s, out, (DWORD)cap) + (out && cap ? 1 : 0);
}
static WINAPI int GetTimeFormatA(DWORD lcid, DWORD flags, const uint16_t *st, const char *fmt, char *out, int cap) {
    (void)lcid; (void)flags; (void)fmt;
    char s[64]; format_time(st, 0, s, sizeof s);
    return (int)vx_return_string_a(s, out, (DWORD)cap) + (out && cap ? 1 : 0);
}
static WINAPI WCHAR *lstrcatW(WCHAR *a, const WCHAR *b) { size_t n = vx_wcslen(a); memcpy(a + n, b, (vx_wcslen(b) + 1) * sizeof(WCHAR)); return a; }
static WINAPI WCHAR *lstrcpyW(WCHAR *a, const WCHAR *b) { memmove(a, b, (vx_wcslen(b) + 1) * sizeof(WCHAR)); return a; }
static WINAPI int lstrcmpiW(const WCHAR *a, const WCHAR *b) {
    for (;; a++, b++) {
        WCHAR x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y || !x) return x < y ? -1 : x > y;
    }
}
static WINAPI int lstrcmpW(const WCHAR *a, const WCHAR *b) { for (; *a == *b && *a; a++, b++) {} return *a < *b ? -1 : *a > *b; }
// GlobalAlloc: fixed memory (GMEM_MOVEABLE handles are the memory too; locking returns them).
static WINAPI void *GlobalAlloc(DWORD flags, size_t n) { return flags & 0x40 ? vx_heap_zalloc(n) : vx_heap_alloc(n); }
static WINAPI void *GlobalFree(void *p) { free(p); return NULL; }
static WINAPI void *GlobalLock(void *p) { return p; }
static WINAPI BOOL GlobalUnlock(void *p) { (void)p; return 1; }
static WINAPI size_t GlobalSize(void *p) { (void)p; return 0; }
static WINAPI DWORD SetThreadExecutionState(DWORD flags) { static DWORD previous = 0x80000000u; DWORD r = previous; previous = flags; return r; }
// SYSTEM_POWER_STATUS: AC line, battery flag, percent, system status, lifetime, full lifetime.
static WINAPI BOOL GetSystemPowerStatus(uint8_t *s) { memset(s, 0, 12); s[0] = 1; s[1] = 128; s[2] = 255; *(DWORD *)(s + 4) = *(DWORD *)(s + 8) = 0xffffffffu; return 1; }
static WINAPI BOOL GetProcessPriorityBoost(HANDLE p, BOOL *disabled) { (void)p; *disabled = 0; return 1; }
static WINAPI BOOL SetProcessPriorityBoost(HANDLE p, BOOL disable) { (void)p; (void)disable; return 1; }
static WINAPI BOOL DosDateTimeToFileTime(uint16_t date, uint16_t tm, DWORD *ft) {
    struct tm t = { .tm_year = (date >> 9) + 80, .tm_mon = ((date >> 5) & 15) - 1, .tm_mday = date & 31,
                    .tm_hour = tm >> 11, .tm_min = (tm >> 5) & 63, .tm_sec = (tm & 31) * 2 };
    uint64_t v = ((uint64_t)timegm(&t) + 11644473600ull) * 10000000ull;
    ft[0] = (DWORD)v; ft[1] = (DWORD)(v >> 32);
    return 1;
}
static WINAPI BOOL SetFileTime(HANDLE h, const void *c, const void *a, const void *w) { (void)c; (void)a; (void)w; return vx_handle_fd(h) >= 0; }
// Another process's memory cannot be reached (crash reporters read it, and cope with a refusal); this process's is
// copied with the kernel's checked copy, so an unmapped address fails instead of faulting.
static int is_this_process(HANDLE p) {
    if (p == (HANDLE)(intptr_t)-1) return 1;
    DWORD (WINAPI *pid_of)(HANDLE) = (DWORD (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "GetProcessId");
    return pid_of && pid_of(p) == (DWORD)getpid();
}
static WINAPI BOOL ReadProcessMemory(HANDLE p, const void *from, void *to, size_t n, size_t *got) {
    if (got) *got = 0;
    if (!is_this_process(p)) { vx_set_last_error(299 /* ERROR_PARTIAL_COPY */); return 0; }
    mach_vm_size_t copied = 0;
    if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)from, n, (mach_vm_address_t)(uintptr_t)to, &copied) != KERN_SUCCESS) {
        vx_set_last_error(299);
        return 0;
    }
    if (got) *got = (size_t)copied;
    return 1;
}
static WINAPI size_t VirtualQueryEx(HANDLE p, const void *addr, void *mbi, size_t len) {
    if (!is_this_process(p)) { vx_set_last_error(5); return 0; }
    size_t (WINAPI *query)(const void *, void *, size_t) = (size_t (WINAPI *)(const void *, void *, size_t))vx_find_export("kernel32.dll", "VirtualQuery");
    return query ? query(addr, mbi, len) : 0;
}
static WINAPI void FreeLibraryAndExitThread(void *module, DWORD code) {
    BOOL (WINAPI *free_lib)(void *) = (BOOL (WINAPI *)(void *))vx_find_export("kernel32.dll", "FreeLibrary");
    void (WINAPI *exit_thread)(DWORD) = (void (WINAPI *)(DWORD))vx_find_export("kernel32.dll", "ExitThread");
    if (free_lib) free_lib(module);
    if (exit_thread) exit_thread(code);
    for (;;) pause();
}
static WINAPI void *DelayLoadFailureHook(const char *dll, const char *proc) { (void)dll; (void)proc; return NULL; }
// Anonymous pipes: a Unix pipe; the handles are file handles (0x10000 + descriptor, as files.c makes them).
static WINAPI BOOL CreatePipe(HANDLE *rd, HANDLE *wr, void *sa, DWORD size) {
    (void)sa; (void)size;
    int fds[2];
    if (pipe(fds)) { vx_set_last_error(8); return 0; }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC); fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    *rd = (HANDLE)(intptr_t)(0x10000 + fds[0]); *wr = (HANDLE)(intptr_t)(0x10000 + fds[1]);
    return 1;
}
// Debugging other processes: not possible here.
static WINAPI BOOL WaitForDebugEvent(void *ev, DWORD ms) { (void)ev; (void)ms; vx_set_last_error(121); return 0; }
static WINAPI BOOL ContinueDebugEvent(DWORD pid, DWORD tid, DWORD status) { (void)pid; (void)tid; (void)status; vx_set_last_error(87); return 0; }
// Console extras: there is no console window, output goes to stderr.
static WINAPI BOOL AttachConsole(DWORD pid) { (void)pid; vx_set_last_error(6); return 0; }
static WINAPI BOOL FillConsoleOutputAttribute(HANDLE h, uint16_t a, DWORD n, DWORD xy, DWORD *done) { (void)h; (void)a; (void)xy; if (done) *done = n; return 1; }
static WINAPI BOOL FillConsoleOutputCharacterA(HANDLE h, char c, DWORD n, DWORD xy, DWORD *done) { (void)h; (void)c; (void)xy; if (done) *done = n; return 1; }
static WINAPI BOOL WriteConsoleOutputCharacterA(HANDLE h, const char *s, DWORD n, DWORD xy, DWORD *done) { (void)h; (void)s; (void)xy; if (done) *done = n; return 1; }
static WINAPI BOOL GetConsoleScreenBufferInfoEx(HANDLE h, void *info) { (void)h; (void)info; vx_set_last_error(6); return 0; }
static WINAPI BOOL SetConsoleScreenBufferInfoEx(HANDLE h, void *info) { (void)h; (void)info; vx_set_last_error(6); return 0; }
static WINAPI BOOL GetNumberOfConsoleInputEvents(HANDLE h, DWORD *n) { (void)h; *n = 0; return 1; }
static WINAPI BOOL ReadConsoleInputA(HANDLE h, void *rec, DWORD n, DWORD *got) { (void)h; (void)rec; (void)n; if (got) *got = 0; vx_set_last_error(6); return 0; }

// ---- version.dll: version resources are not read yet ----------------------------------------------------------------------
static WINAPI DWORD GetFileVersionInfoSizeA(const char *f, DWORD *h) { (void)f; if (h) *h = 0; vx_set_last_error(1813); return 0; }
static WINAPI DWORD GetFileVersionInfoSizeW(const WCHAR *f, DWORD *h) { (void)f; if (h) *h = 0; vx_set_last_error(1813); return 0; }
static WINAPI BOOL GetFileVersionInfoA(const char *f, DWORD h, DWORD n, void *d) { (void)f; (void)h; (void)n; (void)d; vx_set_last_error(1813); return 0; }
static WINAPI BOOL GetFileVersionInfoW(const WCHAR *f, DWORD h, DWORD n, void *d) { (void)f; (void)h; (void)n; (void)d; vx_set_last_error(1813); return 0; }
static WINAPI BOOL VerQueryValueA(const void *b, const char *s, void **out, DWORD *n) { (void)b; (void)s; *out = NULL; *n = 0; return 0; }
static WINAPI BOOL VerQueryValueW(const void *b, const WCHAR *s, void **out, DWORD *n) { (void)b; (void)s; *out = NULL; *n = 0; return 0; }

// ---- setupapi: device lists that are empty --------------------------------------------------------------------------------
// A list is made (an empty one): a program that looks through the HID devices for XInput controllers (Elite Dangerous,
// before it opens its DirectInput joysticks) took the failure for a broken system and set up no joystick at all.
static WINAPI HANDLE SetupDiGetClassDevsA(const void *guid, const char *e, void *w, DWORD f) { (void)guid; (void)e; (void)w; (void)f; return (HANDLE)(uintptr_t)0x5e7d0001; }
static WINAPI HANDLE SetupDiGetClassDevsW(const void *guid, const WCHAR *e, void *w, DWORD f) { (void)guid; (void)e; (void)w; (void)f; return (HANDLE)(uintptr_t)0x5e7d0001; }
static WINAPI BOOL setupdi_none(void) { vx_set_last_error(259); return 0; }                     // ERROR_NO_MORE_ITEMS
static WINAPI BOOL SetupDiDestroyDeviceInfoList(HANDLE h) { (void)h; return 1; }

// ---- user32 -------------------------------------------------------------------------------------------------------------------
// Window properties: a small table of (window, name atom or string) -> value.
typedef struct prop { void *wnd; WCHAR name[64]; HANDLE value; struct prop *next; } prop;
static prop *g_props;
static pthread_mutex_t g_props_lock = PTHREAD_MUTEX_INITIALIZER;
static void prop_name(const WCHAR *name, WCHAR *out) {
    if ((uintptr_t)name < 0x10000) { out[0] = 1; out[1] = (WCHAR)(uintptr_t)name; out[2] = 0; return; }   // an atom
    size_t n = vx_wcslen(name); if (n > 63) n = 63;
    memcpy(out, name, n * sizeof(WCHAR)); out[n] = 0;
}
static prop **find_prop(void *wnd, const WCHAR *name) {
    WCHAR key[64];
    prop_name(name, key);
    prop **p = &g_props;
    for (; *p; p = &(*p)->next) if ((*p)->wnd == wnd && !memcmp((*p)->name, key, (vx_wcslen(key) + 1) * sizeof(WCHAR))) break;
    return p;
}
static WINAPI BOOL SetPropW(void *wnd, const WCHAR *name, HANDLE value) {
    pthread_mutex_lock(&g_props_lock);
    prop **p = find_prop(wnd, name);
    if (!*p) { *p = calloc(1, sizeof **p); (*p)->wnd = wnd; prop_name(name, (*p)->name); }
    (*p)->value = value;
    pthread_mutex_unlock(&g_props_lock);
    return 1;
}
static WINAPI HANDLE GetPropW(void *wnd, const WCHAR *name) {
    pthread_mutex_lock(&g_props_lock);
    prop **p = find_prop(wnd, name);
    HANDLE v = *p ? (*p)->value : NULL;
    pthread_mutex_unlock(&g_props_lock);
    return v;
}
static WINAPI HANDLE RemovePropW(void *wnd, const WCHAR *name) {
    pthread_mutex_lock(&g_props_lock);
    prop **p = find_prop(wnd, name);
    HANDLE v = NULL;
    if (*p) { prop *d = *p; v = d->value; *p = d->next; free(d); }
    pthread_mutex_unlock(&g_props_lock);
    return v;
}
static WINAPI BOOL SetPropA(void *wnd, const char *name, HANDLE v) { WCHAR *w = (uintptr_t)name < 0x10000 ? (WCHAR *)name : vx_utf16_dup(name); BOOL r = SetPropW(wnd, w, v); if ((uintptr_t)name >= 0x10000) free(w); return r; }
static WINAPI HANDLE GetPropA(void *wnd, const char *name) { WCHAR *w = (uintptr_t)name < 0x10000 ? (WCHAR *)name : vx_utf16_dup(name); HANDLE r = GetPropW(wnd, w); if ((uintptr_t)name >= 0x10000) free(w); return r; }
static WINAPI HANDLE RemovePropA(void *wnd, const char *name) { WCHAR *w = (uintptr_t)name < 0x10000 ? (WCHAR *)name : vx_utf16_dup(name); HANDLE r = RemovePropW(wnd, w); if ((uintptr_t)name >= 0x10000) free(w); return r; }
static WINAPI BOOL IsRectEmpty(const RECT *r) { return !r || r->right <= r->left || r->bottom <= r->top; }
static WINAPI intptr_t GetMessageExtraInfo(void) { return 0; }
static WINAPI void *GetTopWindow(void *wnd) { (void)wnd; return NULL; }
static WINAPI BOOL FlashWindow(void *wnd, BOOL invert) { (void)wnd; (void)invert; return 0; }
static WINAPI BOOL FlashWindowEx(void *info) { (void)info; return 0; }
static WINAPI int SetWindowRgn(void *wnd, void *rgn, BOOL redraw) { (void)wnd; (void)rgn; (void)redraw; return 1; }
static WINAPI DWORD WaitForInputIdle(HANDLE p, DWORD ms) { (void)p; (void)ms; return 0; }
static WINAPI int ToUnicode(DWORD vk, DWORD scan, const uint8_t *state, WCHAR *out, int cap, DWORD flags) {
    (void)scan; (void)flags;
    if (cap < 1) return 0;
    WCHAR c = 0;
    int shift = state && (state[0x10] & 0x80);
    if (vk >= 'A' && vk <= 'Z') c = (WCHAR)(shift ? vk : vk + 32);
    else if (vk >= '0' && vk <= '9' && !shift) c = (WCHAR)vk;
    else if (vk == 0x20) c = ' ';
    else if (vk == 0x0d) c = '\r';
    if (!c) return 0;
    out[0] = c;
    return 1;
}
static int g_device_notification;
static WINAPI HANDLE RegisterDeviceNotificationW(HANDLE recipient, void *filter, DWORD flags) { (void)recipient; (void)filter; (void)flags; return &g_device_notification; }
static WINAPI HANDLE RegisterDeviceNotificationA(HANDLE recipient, void *filter, DWORD flags) { (void)recipient; (void)filter; (void)flags; return &g_device_notification; }
static WINAPI BOOL UnregisterDeviceNotification(HANDLE h) { (void)h; return 1; }
// The clipboard: empty, and nothing can be put on it yet.
static WINAPI BOOL EmptyClipboard(void) { return 1; }
static WINAPI HANDLE GetClipboardData(DWORD format) { (void)format; vx_set_last_error(1418); return NULL; }
static WINAPI HANDLE SetClipboardData(DWORD format, HANDLE data) { (void)format; (void)data; vx_set_last_error(1418); return NULL; }
static WINAPI DWORD GetClipboardSequenceNumber(void) { return 0; }
static WINAPI BOOL IsClipboardFormatAvailable(DWORD format) { (void)format; return 0; }
// Icons from memory: an opaque object (the window keeps the default icon).
static int g_icon;
static WINAPI void *CreateIconIndirect(void *info) { (void)info; return &g_icon; }
static WINAPI void *CreateIconFromResource(void *bits, DWORD size, BOOL icon, DWORD ver) { (void)bits; (void)size; (void)icon; (void)ver; return &g_icon; }
static WINAPI void *CreateIconFromResourceEx(void *bits, DWORD size, BOOL icon, DWORD ver, int w, int h, DWORD flags) { (void)bits; (void)size; (void)icon; (void)ver; (void)w; (void)h; (void)flags; return &g_icon; }
static WINAPI BOOL DestroyIcon(void *icon) { (void)icon; return 1; }
// Dialogs from resources: not available (the caller sees a failed dialog).
static WINAPI intptr_t DialogBoxParamA(void *inst, const char *tmpl, void *parent, void *proc, intptr_t param) { (void)inst; (void)tmpl; (void)parent; (void)proc; (void)param; vx_set_last_error(1813); return -1; }
static WINAPI intptr_t DialogBoxParamW(void *inst, const WCHAR *tmpl, void *parent, void *proc, intptr_t param) { (void)inst; (void)tmpl; (void)parent; (void)proc; (void)param; vx_set_last_error(1813); return -1; }
static WINAPI intptr_t DialogBoxIndirectParamW(void *inst, const void *tmpl, void *parent, void *proc, intptr_t param) { (void)inst; (void)tmpl; (void)parent; (void)proc; (void)param; vx_set_last_error(1813); return -1; }
static WINAPI BOOL EndDialog(void *dlg, intptr_t r) { (void)dlg; (void)r; return 1; }
static WINAPI void *GetDlgItem(void *dlg, int id) { (void)dlg; (void)id; return NULL; }
static WINAPI unsigned GetDlgItemInt(void *dlg, int id, BOOL *ok, BOOL sign) { (void)dlg; (void)id; (void)sign; if (ok) *ok = 0; return 0; }
static WINAPI BOOL SetDlgItemInt(void *dlg, int id, unsigned v, BOOL sign) { (void)dlg; (void)id; (void)v; (void)sign; return 0; }
static WINAPI unsigned GetDlgItemTextA(void *dlg, int id, char *s, int cap) { (void)dlg; (void)id; if (s && cap) s[0] = 0; return 0; }
static WINAPI BOOL SetDlgItemTextA(void *dlg, int id, const char *s) { (void)dlg; (void)id; (void)s; return 0; }

// ---- gdi32 -------------------------------------------------------------------------------------------------------------------
static WINAPI BOOL GetDeviceGammaRamp(void *dc, uint16_t *ramp) {
    (void)dc;
    for (int c = 0; c < 3; c++) for (int i = 0; i < 256; i++) ramp[c * 256 + i] = (uint16_t)(i * 257);
    return 1;
}
static WINAPI BOOL SetDeviceGammaRamp(void *dc, const uint16_t *ramp) { (void)dc; (void)ramp; return 0; }
static WINAPI int GetPixelFormat(void *dc) { (void)dc; return 0; }
static WINAPI int CombineRgn(void *dst, void *a, void *b, int mode) { (void)dst; (void)a; (void)b; (void)mode; return 1; }   // NULLREGION

// ---- shell32 -----------------------------------------------------------------------------------------------------------------
static WINAPI unsigned ExtractIconExW(const WCHAR *file, int i, void **large, void **small, unsigned n) { (void)file; (void)i; (void)n; if (large) *large = NULL; if (small) *small = NULL; return 0; }
static WINAPI unsigned ExtractIconExA(const char *file, int i, void **large, void **small, unsigned n) { (void)file; (void)i; (void)n; if (large) *large = NULL; if (small) *small = NULL; return 0; }
static WINAPI void DragAcceptFiles(void *wnd, BOOL accept) { (void)wnd; (void)accept; }
static WINAPI void DragFinish(void *drop) { (void)drop; }
static WINAPI unsigned DragQueryFileW(void *drop, unsigned i, WCHAR *out, unsigned cap) { (void)drop; (void)i; if (out && cap) out[0] = 0; return 0; }

// ---- winmm: no wave devices --------------------------------------------------------------------------------------------------
#define MMSYSERR_NODRIVER 6
#define MMSYSERR_BADDEVICEID 2
static WINAPI unsigned wave_none(void) { return MMSYSERR_NODRIVER; }
static WINAPI unsigned wave_caps(uintptr_t id, void *caps, unsigned n) { (void)id; (void)caps; (void)n; return MMSYSERR_BADDEVICEID; }
static WINAPI unsigned waveOutGetErrorTextW(unsigned err, WCHAR *out, unsigned cap) { (void)err; vx_return_string_w("No sound device", out, cap); return 0; }

// ---- imm32 extras ------------------------------------------------------------------------------------------------------------
static WINAPI LONG ImmGetCompositionStringW(void *imc, DWORD index, void *buf, DWORD n) { (void)imc; (void)index; (void)buf; (void)n; return 0; }
static WINAPI BOOL ImmSetCompositionStringW(void *imc, DWORD index, const void *a, DWORD an, const void *b, DWORD bn) { (void)imc; (void)index; (void)a; (void)an; (void)b; (void)bn; return 0; }
static WINAPI BOOL ImmSetCompositionWindow(void *imc, void *form) { (void)imc; (void)form; return 1; }
static WINAPI BOOL ImmNotifyIME(void *imc, DWORD action, DWORD index, DWORD value) { (void)imc; (void)action; (void)index; (void)value; return 1; }
static WINAPI DWORD ImmGetCandidateListW(void *imc, DWORD index, void *list, DWORD n) { (void)imc; (void)index; (void)list; (void)n; return 0; }
static WINAPI unsigned ImmGetIMEFileNameA(void *hkl, char *out, unsigned cap) { (void)hkl; if (out && cap) out[0] = 0; return 0; }
static WINAPI unsigned ImmGetIMEFileNameW(void *hkl, WCHAR *out, unsigned cap) { (void)hkl; if (out && cap) out[0] = 0; return 0; }

// ---- wininet: no internet connections (nothing is downloaded behind the user's back) ------------------------------------------
#define ERROR_INTERNET_CANNOT_CONNECT 12029
static WINAPI HANDLE InternetOpenA(const char *agent, DWORD type, const char *proxy, const char *bypass, DWORD flags) { (void)agent; (void)type; (void)proxy; (void)bypass; (void)flags; vx_set_last_error(ERROR_INTERNET_CANNOT_CONNECT); return NULL; }
static WINAPI HANDLE InternetOpenW(const WCHAR *agent, DWORD type, const WCHAR *proxy, const WCHAR *bypass, DWORD flags) { (void)agent; (void)type; (void)proxy; (void)bypass; (void)flags; vx_set_last_error(ERROR_INTERNET_CANNOT_CONNECT); return NULL; }
static WINAPI HANDLE InternetOpenUrlA(HANDLE h, const char *url, const char *hdr, DWORD n, DWORD flags, uintptr_t ctx) { (void)h; (void)url; (void)hdr; (void)n; (void)flags; (void)ctx; vx_set_last_error(ERROR_INTERNET_CANNOT_CONNECT); return NULL; }
static WINAPI BOOL InternetReadFile(HANDLE h, void *buf, DWORD n, DWORD *got) { (void)h; (void)buf; (void)n; if (got) *got = 0; vx_set_last_error(6); return 0; }
static WINAPI BOOL InternetCloseHandle(HANDLE h) { (void)h; return 1; }
static WINAPI void *InternetSetStatusCallback(HANDLE h, void *cb) { (void)h; (void)cb; return NULL; }
static WINAPI BOOL InternetCrackUrlA(const char *url, DWORD n, DWORD flags, void *parts) { (void)url; (void)n; (void)flags; (void)parts; vx_set_last_error(12006); return 0; }
static WINAPI BOOL HttpQueryInfoA(HANDLE h, DWORD level, void *buf, DWORD *n, DWORD *index) { (void)h; (void)level; (void)buf; (void)n; (void)index; vx_set_last_error(6); return 0; }

// __chkstk: the stack probe compilers call before large frames. Program threads' stacks are mapped whole, so
// there is nothing to touch (context.S: a bare return, keeping every register).
void vx_chkstk(void);
// ---- multimedia timers (winmm): a thread per timer, calling the program's callback (uID, msg, user, dw1, dw2) --------------
typedef struct mm_timer { unsigned id, delay, periodic; void *cb; uintptr_t user; volatile int stop; struct mm_timer *next; } mm_timer;
static pthread_mutex_t g_mm_lock = PTHREAD_MUTEX_INITIALIZER;
static mm_timer *g_mm_timers;
static unsigned g_mm_next = 1;
static void *mm_thread(void *arg) {
    mm_timer *t = arg;
    vx_thread_attach();
    do {
        usleep((useconds_t)t->delay * 1000);
        if (t->stop) break;
        if (t->periodic & 0x100) {                               // TIME_CALLBACK_EVENT_SET / PULSE: an event handle
            void (WINAPI *set)(HANDLE) = (void (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "SetEvent");
            set(t->cb);
        } else ((void (WINAPI *)(unsigned, unsigned, uintptr_t, uintptr_t, uintptr_t))t->cb)(t->id, 0, t->user, 0, 0);
    } while ((t->periodic & 1) && !t->stop);
    vx_thread_detach();
    return NULL;
}
static WINAPI unsigned timeSetEvent(unsigned delay, unsigned resolution, void *cb, uintptr_t user, unsigned flags) {
    (void)resolution;
    mm_timer *t = calloc(1, sizeof *t);
    t->delay = delay ? delay : 1; t->periodic = (flags & 1) | (flags & 0x30 ? 0x100 : 0); t->cb = cb; t->user = user;
    pthread_mutex_lock(&g_mm_lock);
    t->id = g_mm_next++; t->next = g_mm_timers; g_mm_timers = t;
    pthread_mutex_unlock(&g_mm_lock);
    pthread_t th; pthread_attr_t a;
    pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_attr_setstacksize(&a, 1 << 20);
    pthread_create(&th, &a, mm_thread, t);
    pthread_attr_destroy(&a);
    return t->id;
}
static WINAPI unsigned timeKillEvent(unsigned id) {
    pthread_mutex_lock(&g_mm_lock);
    mm_timer *t = g_mm_timers;
    while (t && t->id != id) t = t->next;
    if (t) t->stop = 1;                                         // the thread ends at its next wake-up (the record stays)
    pthread_mutex_unlock(&g_mm_lock);
    return t ? 0 : 97;                                          // MMSYSERR_INVALPARAM
}
// ---- Remote Desktop sessions (wtsapi32): there are none ---------------------------------------------------------------------
static WINAPI BOOL WTSQuerySessionInformationA(HANDLE server, DWORD session, int cls, char **out, DWORD *n) { (void)server; (void)session; (void)cls; *out = NULL; *n = 0; vx_set_last_error(50); return 0; }
static WINAPI BOOL WTSQuerySessionInformationW(HANDLE server, DWORD session, int cls, WCHAR **out, DWORD *n) { (void)server; (void)session; (void)cls; *out = NULL; *n = 0; vx_set_last_error(50); return 0; }
static WINAPI void WTSFreeMemory(void *p) { free(p); }
static WINAPI BOOL WTSRegisterSessionNotification(void *wnd, DWORD flags) { (void)wnd; (void)flags; return 1; }
static WINAPI BOOL WTSUnRegisterSessionNotification(void *wnd) { (void)wnd; return 1; }
static WINAPI BOOL AllowSetForegroundWindow(DWORD pid) { (void)pid; return 1; }
// ---- OpenGL (opengl32): no contexts; programs that probe for GL interop find none ---------------------------------------
static WINAPI void *wglGetCurrentContext(void) { return NULL; }
static WINAPI void *wglGetCurrentDC(void) { return NULL; }
static WINAPI void *wglCreateContext(void *dc) { (void)dc; vx_set_last_error(2000); return NULL; }
static WINAPI BOOL wglMakeCurrent(void *dc, void *rc) { (void)dc; (void)rc; return rc == NULL; }
static WINAPI BOOL wglDeleteContext(void *rc) { (void)rc; return 1; }
static WINAPI BOOL wglShareLists(void *a, void *b) { (void)a; (void)b; return 0; }
static WINAPI void *wglGetProcAddress(const char *name) { (void)name; return NULL; }
static WINAPI unsigned glGetError(void) { return 0x502; }      // GL_INVALID_OPERATION: there is no context
static WINAPI void gl_nothing(void) {}

// ---- menus (user32): kept as objects with their items; windows here show no menu bar -----------------------------------------
typedef struct { int magic, n; } vx_menu;
static WINAPI void *CreateMenu(void) { vx_menu *m = calloc(1, sizeof *m); m->magic = 0x4d454e55; return m; }
static WINAPI void *CreatePopupMenu(void) { return CreateMenu(); }
static WINAPI BOOL DestroyMenu(void *m) { free(m); return 1; }
static WINAPI BOOL InsertMenuItemA(void *m, unsigned item, BOOL by_pos, const void *info) { (void)item; (void)by_pos; (void)info; if (m) ((vx_menu *)m)->n++; return m != NULL; }
static WINAPI BOOL InsertMenuItemW(void *m, unsigned item, BOOL by_pos, const void *info) { return InsertMenuItemA(m, item, by_pos, info); }
static WINAPI BOOL AppendMenuA(void *m, unsigned flags, uintptr_t id, const char *s) { (void)flags; (void)id; (void)s; if (m) ((vx_menu *)m)->n++; return m != NULL; }
static WINAPI BOOL AppendMenuW(void *m, unsigned flags, uintptr_t id, const WCHAR *s) { (void)flags; (void)id; (void)s; if (m) ((vx_menu *)m)->n++; return m != NULL; }
static WINAPI DWORD CheckMenuItem(void *m, unsigned item, unsigned flags) { (void)m; (void)item; (void)flags; return 0; }   // MF_UNCHECKED before
static WINAPI BOOL EnableMenuItem(void *m, unsigned item, unsigned flags) { (void)m; (void)item; (void)flags; return 0; }
static WINAPI BOOL SetMenuItemInfoA(void *m, unsigned item, BOOL by_pos, const void *info) { (void)m; (void)item; (void)by_pos; (void)info; return 1; }
static WINAPI BOOL SetMenuItemInfoW(void *m, unsigned item, BOOL by_pos, const void *info) { (void)m; (void)item; (void)by_pos; (void)info; return 1; }
static WINAPI int GetMenuItemCount(void *m) { return m ? ((vx_menu *)m)->n : -1; }
static WINAPI BOOL TrackPopupMenu(void *m, unsigned flags, int x, int y, int r, void *wnd, const void *rect) { (void)m; (void)flags; (void)x; (void)y; (void)r; (void)wnd; (void)rect; return 0; }
// ---- power requests (kernel32): the Mac stays awake while VR runs anyway; requests are accepted ------------------------------
static int g_power_request;
static WINAPI HANDLE PowerCreateRequest(void *ctx) { (void)ctx; return &g_power_request; }
static WINAPI BOOL PowerSetRequest(HANDLE h, int type) { (void)h; (void)type; return 1; }
static WINAPI BOOL PowerClearRequest(HANDLE h, int type) { (void)h; (void)type; return 1; }
// ---- event tracing sessions (advapi32): none can be started ------------------------------------------------------------------
static WINAPI DWORD StartTraceA(uint64_t *h, const char *name, void *props) { (void)name; (void)props; if (h) *h = 0; return 5; }   // ERROR_ACCESS_DENIED
static WINAPI DWORD ControlTraceA(uint64_t h, const char *name, void *props, DWORD code) { (void)h; (void)name; (void)props; (void)code; return 4201; }   // ERROR_WMI_INSTANCE_NOT_FOUND
static WINAPI DWORD EnableTraceEx2(uint64_t h, const void *guid, DWORD code, uint8_t level, uint64_t any, uint64_t all, DWORD timeout, void *params) {
    (void)h; (void)guid; (void)code; (void)level; (void)any; (void)all; (void)timeout; (void)params; return 5;
}
static WINAPI DWORD FlushTraceA(uint64_t h, const char *name, void *props) { (void)h; (void)name; (void)props; return 4201; }

static const vx_export k32[] = {
    { "__chkstk", (void *)vx_chkstk }, VX_EXPORT(PowerCreateRequest), VX_EXPORT(PowerSetRequest), VX_EXPORT(PowerClearRequest),
    VX_EXPORT(MulDiv), VX_EXPORT(IsDBCSLeadByte), VX_EXPORT(OutputDebugStringW), VX_EXPORT(FormatMessageW),
    VX_EXPORT(GetUserDefaultLCID), VX_EXPORT(GetSystemDefaultLCID), VX_EXPORT(GetUserDefaultLangID), VX_EXPORT(GetSystemDefaultLangID),
    VX_EXPORT(GetUserDefaultUILanguage), VX_EXPORT(GetLocaleInfoW), VX_EXPORT(GetLocaleInfoA), VX_EXPORT(GetDateFormatW),
    VX_EXPORT(GetTimeFormatW), VX_EXPORT(GetDateFormatA), VX_EXPORT(GetTimeFormatA), VX_EXPORT(lstrcatW), VX_EXPORT(lstrcpyW),
    VX_EXPORT(lstrcmpiW), VX_EXPORT(lstrcmpW), VX_EXPORT(GlobalAlloc), VX_EXPORT(GlobalFree), VX_EXPORT(GlobalLock),
    VX_EXPORT(GlobalUnlock), VX_EXPORT(GlobalSize), VX_EXPORT(SetThreadExecutionState), VX_EXPORT(GetSystemPowerStatus),
    VX_EXPORT(GetProcessPriorityBoost), VX_EXPORT(SetProcessPriorityBoost), VX_EXPORT(DosDateTimeToFileTime), VX_EXPORT(SetFileTime),
    VX_EXPORT(ReadProcessMemory), VX_EXPORT(VirtualQueryEx), VX_EXPORT(FreeLibraryAndExitThread), VX_EXPORT(DelayLoadFailureHook),
    VX_EXPORT(CreatePipe), VX_EXPORT(WaitForDebugEvent), VX_EXPORT(ContinueDebugEvent),
    VX_EXPORT(AttachConsole), VX_EXPORT(FillConsoleOutputAttribute), VX_EXPORT(FillConsoleOutputCharacterA),
    VX_EXPORT(WriteConsoleOutputCharacterA), VX_EXPORT(GetConsoleScreenBufferInfoEx), VX_EXPORT(SetConsoleScreenBufferInfoEx),
    VX_EXPORT(GetNumberOfConsoleInputEvents), VX_EXPORT(ReadConsoleInputA),
};
static const vx_export version[] = {
    VX_EXPORT(GetFileVersionInfoSizeA), VX_EXPORT(GetFileVersionInfoSizeW), VX_EXPORT(GetFileVersionInfoA), VX_EXPORT(GetFileVersionInfoW),
    VX_EXPORT(VerQueryValueA), VX_EXPORT(VerQueryValueW),
};
static const vx_export setupapi[] = {
    VX_EXPORT(SetupDiGetClassDevsA), VX_EXPORT(SetupDiGetClassDevsW), VX_EXPORT(SetupDiDestroyDeviceInfoList),
    { "SetupDiEnumDeviceInfo", (void *)setupdi_none }, { "SetupDiEnumDeviceInterfaces", (void *)setupdi_none },
    { "SetupDiGetDeviceInterfaceDetailA", (void *)setupdi_none }, { "SetupDiGetDeviceInterfaceDetailW", (void *)setupdi_none },
    { "SetupDiGetDeviceRegistryPropertyA", (void *)setupdi_none }, { "SetupDiGetDeviceRegistryPropertyW", (void *)setupdi_none },
    { "SetupDiBuildDriverInfoList", (void *)setupdi_none }, { "SetupDiClassGuidsFromNameW", (void *)setupdi_none },
    { "SetupDiCreateDeviceInfoListExA", (void *)SetupDiGetClassDevsA }, { "SetupDiEnumDriverInfoA", (void *)setupdi_none },
    { "SetupDiGetDeviceInstallParamsA", (void *)setupdi_none }, { "SetupDiGetDeviceInstanceIdA", (void *)setupdi_none },
    { "SetupDiGetDriverInstallParamsA", (void *)setupdi_none }, { "SetupDiOpenDeviceInfoA", (void *)setupdi_none },
    { "SetupDiSetDeviceInstallParamsA", (void *)setupdi_none },
};
static const vx_export user32[] = {
    VX_EXPORT(AllowSetForegroundWindow), VX_EXPORT(CreateMenu), VX_EXPORT(CreatePopupMenu), VX_EXPORT(DestroyMenu),
    VX_EXPORT(InsertMenuItemA), VX_EXPORT(InsertMenuItemW), VX_EXPORT(AppendMenuA), VX_EXPORT(AppendMenuW), VX_EXPORT(CheckMenuItem),
    VX_EXPORT(EnableMenuItem), VX_EXPORT(SetMenuItemInfoA), VX_EXPORT(SetMenuItemInfoW), VX_EXPORT(GetMenuItemCount),
    VX_EXPORT(TrackPopupMenu), VX_EXPORT(SetPropW), VX_EXPORT(GetPropW), VX_EXPORT(RemovePropW), VX_EXPORT(SetPropA), VX_EXPORT(GetPropA), VX_EXPORT(RemovePropA),
    VX_EXPORT(IsRectEmpty), VX_EXPORT(GetMessageExtraInfo), VX_EXPORT(GetTopWindow), VX_EXPORT(FlashWindow),
    VX_EXPORT(FlashWindowEx), VX_EXPORT(SetWindowRgn), VX_EXPORT(WaitForInputIdle),
    VX_EXPORT(ToUnicode), VX_EXPORT(RegisterDeviceNotificationW), VX_EXPORT(RegisterDeviceNotificationA),
    VX_EXPORT(UnregisterDeviceNotification), VX_EXPORT(EmptyClipboard), VX_EXPORT(GetClipboardData), VX_EXPORT(SetClipboardData),
    VX_EXPORT(GetClipboardSequenceNumber), VX_EXPORT(IsClipboardFormatAvailable), VX_EXPORT(CreateIconIndirect),
    VX_EXPORT(CreateIconFromResource), VX_EXPORT(CreateIconFromResourceEx), VX_EXPORT(DestroyIcon), VX_EXPORT(DialogBoxParamA),
    VX_EXPORT(DialogBoxParamW), VX_EXPORT(DialogBoxIndirectParamW), VX_EXPORT(EndDialog), VX_EXPORT(GetDlgItem),
    VX_EXPORT(GetDlgItemInt), VX_EXPORT(SetDlgItemInt), VX_EXPORT(GetDlgItemTextA), VX_EXPORT(SetDlgItemTextA),
};
static const vx_export gdi32[] = {
    VX_EXPORT(GetDeviceGammaRamp), VX_EXPORT(SetDeviceGammaRamp), VX_EXPORT(GetPixelFormat), VX_EXPORT(CombineRgn),
};
static const vx_export shell32[] = {
    VX_EXPORT(ExtractIconExW), VX_EXPORT(ExtractIconExA), VX_EXPORT(DragAcceptFiles), VX_EXPORT(DragFinish), VX_EXPORT(DragQueryFileW),
};
static const vx_export winmm[] = {
    VX_EXPORT(waveOutGetErrorTextW), VX_EXPORT(timeSetEvent), VX_EXPORT(timeKillEvent),
    { "waveOutGetDevCapsW", (void *)wave_caps }, { "waveOutGetDevCapsA", (void *)wave_caps }, { "waveInGetDevCapsW", (void *)wave_caps },
    { "waveInGetDevCapsA", (void *)wave_caps }, { "waveOutClose", (void *)wave_none },
    { "waveOutPrepareHeader", (void *)wave_none }, { "waveOutUnprepareHeader", (void *)wave_none }, { "waveOutWrite", (void *)wave_none },
    { "waveOutReset", (void *)wave_none }, { "waveInOpen", (void *)wave_none }, { "waveInClose", (void *)wave_none },
    { "waveInPrepareHeader", (void *)wave_none }, { "waveInUnprepareHeader", (void *)wave_none }, { "waveInAddBuffer", (void *)wave_none },
    { "waveInStart", (void *)wave_none }, { "waveInReset", (void *)wave_none },
};
static const vx_export imm32[] = {
    VX_EXPORT(ImmGetCompositionStringW), VX_EXPORT(ImmSetCompositionStringW), VX_EXPORT(ImmSetCompositionWindow), VX_EXPORT(ImmNotifyIME),
    VX_EXPORT(ImmGetCandidateListW), VX_EXPORT(ImmGetIMEFileNameA), VX_EXPORT(ImmGetIMEFileNameW),
};
static const vx_export wininet[] = {
    VX_EXPORT(InternetOpenA), VX_EXPORT(InternetOpenW), VX_EXPORT(InternetOpenUrlA), VX_EXPORT(InternetReadFile),
    VX_EXPORT(InternetCloseHandle), VX_EXPORT(InternetSetStatusCallback), VX_EXPORT(InternetCrackUrlA), VX_EXPORT(HttpQueryInfoA),
};
static const vx_export wtsapi32[] = {
    VX_EXPORT(WTSQuerySessionInformationA), VX_EXPORT(WTSQuerySessionInformationW), VX_EXPORT(WTSFreeMemory),
    VX_EXPORT(WTSRegisterSessionNotification), VX_EXPORT(WTSUnRegisterSessionNotification),
};
static const vx_export opengl32[] = {
    VX_EXPORT(wglGetCurrentContext), VX_EXPORT(wglGetCurrentDC), VX_EXPORT(wglCreateContext), VX_EXPORT(wglMakeCurrent),
    VX_EXPORT(wglDeleteContext), VX_EXPORT(wglShareLists), VX_EXPORT(wglGetProcAddress), VX_EXPORT(glGetError),
    { "glBindTexture", (void *)gl_nothing }, { "glDeleteTextures", (void *)gl_nothing }, { "glDisable", (void *)gl_nothing },
    { "glEnable", (void *)gl_nothing }, { "glFlush", (void *)gl_nothing }, { "glFinish", (void *)gl_nothing },
    { "glGenTextures", (void *)gl_nothing }, { "glGetTexLevelParameteriv", (void *)gl_nothing }, { "glTexImage2D", (void *)gl_nothing },
    { "glTexParameteri", (void *)gl_nothing }, { "glGetIntegerv", (void *)gl_nothing }, { "glGetTexImage", (void *)gl_nothing },
    { "glPixelStorei", (void *)gl_nothing }, { "glGetString", (void *)wglGetCurrentContext }, { "glViewport", (void *)gl_nothing },
    { "glClear", (void *)gl_nothing }, { "glClearColor", (void *)gl_nothing },
};
static const vx_export advapi32[] = { VX_EXPORT(StartTraceA), VX_EXPORT(ControlTraceA), VX_EXPORT(EnableTraceEx2), VX_EXPORT(FlushTraceA) };
void vx_desktop_init(void) {
    vx_register_module("advapi32.dll", advapi32, sizeof advapi32 / sizeof advapi32[0]);
    vx_register_module("wtsapi32.dll", wtsapi32, sizeof wtsapi32 / sizeof wtsapi32[0]);
    if (getenv("VXRT_OPENGL") && !strcmp(getenv("VXRT_OPENGL"), "0")) vx_register_module("opengl32.dll", opengl32, sizeof opengl32 / sizeof opengl32[0]);   // (no OpenGL: opengl.c otherwise)
    vx_register_module("kernel32.dll", k32, sizeof k32 / sizeof k32[0]);
    vx_register_module("version.dll", version, sizeof version / sizeof version[0]);
    vx_register_module("setupapi.dll", setupapi, sizeof setupapi / sizeof setupapi[0]);
    vx_register_module("user32.dll", user32, sizeof user32 / sizeof user32[0]);
    vx_register_module("gdi32.dll", gdi32, sizeof gdi32 / sizeof gdi32[0]);
    vx_register_module("shell32.dll", shell32, sizeof shell32 / sizeof shell32[0]);
    vx_register_module("winmm.dll", winmm, sizeof winmm / sizeof winmm[0]);
    vx_register_module("imm32.dll", imm32, sizeof imm32 / sizeof imm32[0]);
    vx_register_module("wininet.dll", wininet, sizeof wininet / sizeof wininet[0]);
    static const vx_export ntdll[] = { { "__chkstk", (void *)vx_chkstk } };
    vx_register_module("ntdll.dll", ntdll, 1);
}

#ifdef __aarch64__
#include "thunks/desktop.c.inc"
#endif
