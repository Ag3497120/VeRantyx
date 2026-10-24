// What desktop clients (the Steam client) use beyond games: product and session queries, base64 and the rest of crypt32
// refused (certificates, stores and chains are crypt32.c's), random numbers through bcrypt, common controls, in-memory
// fonts, and other processes' module names.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "vx.h"

// ---- kernel32 -------------------------------------------------------------------------------------------------------------------
static WINAPI BOOL GetProductInfo(DWORD major, DWORD minor, DWORD sp_major, DWORD sp_minor, DWORD *type) {
    (void)major; (void)minor; (void)sp_major; (void)sp_minor;
    if (type) *type = 0x30;                                        // PRODUCT_PROFESSIONAL
    return 1;
}
static WINAPI BOOL ProcessIdToSessionId(DWORD pid, DWORD *session) { (void)pid; if (session) *session = 1; return 1; }
static WINAPI DWORD WTSGetActiveConsoleSessionId(void) { return 1; }
static WINAPI BOOL ReadConsoleA(HANDLE h, void *buf, DWORD n, DWORD *got, void *ctl) {
    (void)h; (void)buf; (void)n; (void)ctl;
    if (got) *got = 0;
    vx_set_last_error(6);                                          // there is no console to read
    return 0;
}

// ---- user32, gdi32 --------------------------------------------------------------------------------------------------------------
static WINAPI uint64_t SetClassLongPtrW(HANDLE wnd, int index, uint64_t value) { (void)wnd; (void)index; (void)value; return 0; }
static WINAPI HANDLE AddFontMemResourceEx(void *data, DWORD size, void *reserved, DWORD *count) {
    (void)data; (void)size; (void)reserved;
    static int fonts;
    if (count) *count = 1;
    return (HANDLE)(uintptr_t)(0x7f000 + __sync_add_and_fetch(&fonts, 1));   // text is drawn with the system's fonts
}
static WINAPI BOOL RemoveFontMemResourceEx(HANDLE h) { (void)h; return 1; }
static WINAPI HANDLE CreateICW(const WCHAR *drv, const WCHAR *dev, const WCHAR *out, const void *dm) {
    HANDLE (WINAPI *create)(const WCHAR *, const WCHAR *, const WCHAR *, const void *) =
        (HANDLE (WINAPI *)(const WCHAR *, const WCHAR *, const WCHAR *, const void *))vx_find_export("gdi32.dll", "CreateDCW");
    return create ? create(drv, dev, out, dm) : NULL;
}
static WINAPI HANDLE CreateICA(const char *drv, const char *dev, const char *out, const void *dm) {
    (void)drv; (void)dev; (void)out;
    return CreateICW(NULL, NULL, NULL, dm);
}

// ---- shell32, comctl32 ----------------------------------------------------------------------------------------------------------
static WINAPI BOOL IsUserAnAdmin(void) { return 0; }
static WINAPI uint64_t SHGetFileInfoW(const WCHAR *path, DWORD attrs, void *info, DWORD size, DWORD flags) {
    (void)path; (void)attrs; (void)flags;
    if (info && size) memset(info, 0, size);
    return 0;
}
static WINAPI BOOL InitCommonControlsEx(const void *icc) { (void)icc; return 1; }
static WINAPI void InitCommonControls(void) {}

// ---- psapi: module names of a process -------------------------------------------------------------------------------------------
static WINAPI DWORD GetModuleFileNameExW(HANDLE process, void *module, WCHAR *buf, DWORD size) {
    DWORD (WINAPI *pid_of)(HANDLE) = (DWORD (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "GetProcessId");
    if (process == (HANDLE)(intptr_t)-1 || (pid_of && pid_of(process) == (DWORD)getpid())) {
        DWORD (WINAPI *name)(void *, WCHAR *, DWORD) = (DWORD (WINAPI *)(void *, WCHAR *, DWORD))vx_find_export("kernel32.dll", "GetModuleFileNameW");
        return name(module, buf, size);
    }
    vx_set_last_error(5);                                          // another process's modules are not visible
    return 0;
}
static WINAPI DWORD GetModuleFileNameExA(HANDLE process, void *module, char *buf, DWORD size) {
    WCHAR w[1024];
    DWORD n = GetModuleFileNameExW(process, module, w, 1024);
    if (!n) return 0;
    char *a = vx_utf8_dup(w);
    DWORD r = vx_return_string_a(a, buf, size);
    free(a);
    return r;
}

// ---- bcrypt: random numbers --------------------------------------------------------------------------------------------------
static WINAPI LONG BCryptGenRandom(void *alg, uint8_t *buf, DWORD n, DWORD flags) {
    (void)alg; (void)flags;
    arc4random_buf(buf, n);
    return 0;
}

// ---- more kernel32 ----------------------------------------------------------------------------------------------------------
static WINAPI BOOL SetProcessShutdownParameters(DWORD level, DWORD flags) { (void)level; (void)flags; return 1; }
static WINAPI BOOL SetThreadErrorMode(DWORD mode, DWORD *old) { (void)mode; if (old) *old = 0; return 1; }
static WINAPI BOOL IsWow64Process(HANDLE h, BOOL *wow) { (void)h; if (wow) *wow = 0; return 1; }
static WINAPI BOOL GetBinaryTypeW(const WCHAR *path, DWORD *type) {
    DWORD (WINAPI *attrs)(const WCHAR *) = (DWORD (WINAPI *)(const WCHAR *))vx_find_export("kernel32.dll", "GetFileAttributesW");
    if (attrs(path) == 0xffffffffu) return 0;
    if (type) *type = 6;                                           // SCS_64BIT_BINARY
    return 1;
}
static WINAPI BOOL GetVolumeInformationA(const char *root, char *name, DWORD name_cap, DWORD *serial, DWORD *max_len, DWORD *flags, char *fs, DWORD fs_cap) {
    (void)root;
    if (name && name_cap) vx_return_string_a("", name, name_cap);
    if (serial) *serial = 0x56585254;
    if (max_len) *max_len = 255;
    if (flags) *flags = 0x3;                                       // case-sensitive search, case-preserved names
    if (fs && fs_cap) vx_return_string_a("NTFS", fs, fs_cap);
    return 1;
}
static WINAPI BOOL GetVolumePathNameW(const WCHAR *path, WCHAR *out, DWORD cap) {
    char drive[4] = "C:\\";
    if (path && path[0] && path[1] == ':') drive[0] = (char)path[0];
    return vx_return_string_w(drive, out, cap) < cap;
}
static WINAPI BOOL GetVolumeNameForVolumeMountPointW(const WCHAR *mount, WCHAR *out, DWORD cap) {
    (void)mount; (void)out; (void)cap;
    vx_set_last_error(1);                                          // ERROR_INVALID_FUNCTION: no volume GUIDs here
    return 0;
}
static WINAPI DWORD GetPrivateProfileStringA(const char *app, const char *key, const char *def, char *out, DWORD cap, const char *file) {
    (void)app; (void)key; (void)file;                              // .ini files are not read: the default comes back
    if (!out || !cap) return 0;
    DWORD n = (DWORD)strlen(def ? def : "");
    if (n >= cap) n = cap - 1;
    memcpy(out, def ? def : "", n); out[n] = 0;
    return n;
}
static WINAPI int FoldStringW(DWORD flags, const WCHAR *src, int n, WCHAR *out, int cap) {
    (void)flags;
    size_t len = n < 0 ? vx_wcslen(src) + 1 : (size_t)n;
    if (!cap) return (int)len;
    if (len > (size_t)cap) { vx_set_last_error(122); return 0; }
    memcpy(out, src, len * sizeof(WCHAR));
    return (int)len;
}
static WINAPI BOOL EnumResourceNamesW(HANDLE module, const WCHAR *type, void *fn, intptr_t param) {
    (void)module; (void)type; (void)fn; (void)param;
    vx_set_last_error(1813);                                       // ERROR_RESOURCE_TYPE_NOT_FOUND
    return 0;
}
static WINAPI BOOL no_debugging(DWORD pid) { (void)pid; vx_set_last_error(5); return 0; }
static WINAPI BOOL GenerateConsoleCtrlEvent(DWORD ev, DWORD group) { (void)ev; (void)group; vx_set_last_error(6); return 0; }

// ---- advapi32 -----------------------------------------------------------------------------------------------------------------
static WINAPI LONG RegSetKeyValueW(HANDLE key, const WCHAR *sub, const WCHAR *value, DWORD type, const void *data, DWORD size) {
    LONG (WINAPI *create)(HANDLE, const WCHAR *, DWORD, void *, DWORD, DWORD, void *, HANDLE *, DWORD *) =
        (LONG (WINAPI *)(HANDLE, const WCHAR *, DWORD, void *, DWORD, DWORD, void *, HANDLE *, DWORD *))vx_find_export("advapi32.dll", "RegCreateKeyExW");
    LONG (WINAPI *set)(HANDLE, const WCHAR *, DWORD, DWORD, const void *, DWORD) =
        (LONG (WINAPI *)(HANDLE, const WCHAR *, DWORD, DWORD, const void *, DWORD))vx_find_export("advapi32.dll", "RegSetValueExW");
    LONG (WINAPI *close_key)(HANDLE) = (LONG (WINAPI *)(HANDLE))vx_find_export("advapi32.dll", "RegCloseKey");
    HANDLE k = key;
    if (sub && *sub) { LONG r = create(key, sub, 0, NULL, 0, 0xf003f, NULL, &k, NULL); if (r) return r; }
    LONG r = set(k, value, 0, type, data, size);
    if (k != key) close_key(k);
    return r;
}
static WINAPI LONG RegDeleteKeyValueW(HANDLE key, const WCHAR *sub, const WCHAR *value) {
    LONG (WINAPI *open)(HANDLE, const WCHAR *, DWORD, DWORD, HANDLE *) = (LONG (WINAPI *)(HANDLE, const WCHAR *, DWORD, DWORD, HANDLE *))vx_find_export("advapi32.dll", "RegOpenKeyExW");
    LONG (WINAPI *del)(HANDLE, const WCHAR *) = (LONG (WINAPI *)(HANDLE, const WCHAR *))vx_find_export("advapi32.dll", "RegDeleteValueW");
    LONG (WINAPI *close_key)(HANDLE) = (LONG (WINAPI *)(HANDLE))vx_find_export("advapi32.dll", "RegCloseKey");
    HANDLE k = key;
    if (sub && *sub) { LONG r = open(key, sub, 0, 0xf003f, &k); if (r) return r; }
    LONG r = del(k, value);
    if (k != key) close_key(k);
    return r;
}

// ---- more user32 ------------------------------------------------------------------------------------------------------------------
typedef struct { LONG left, top, right, bottom; } RECT;
static WINAPI BOOL SetRectEmpty(RECT *r) { if (!r) return 0; memset(r, 0, sizeof *r); return 1; }
static WINAPI BOOL PtInRect(const RECT *r, int64_t pt) {
    LONG x = (LONG)(int32_t)pt, y = (LONG)(int32_t)(pt >> 32);
    return r && x >= r->left && x < r->right && y >= r->top && y < r->bottom;
}
static WINAPI BOOL IntersectRect(RECT *out, const RECT *a, const RECT *b) {
    RECT r = { a->left > b->left ? a->left : b->left, a->top > b->top ? a->top : b->top,
               a->right < b->right ? a->right : b->right, a->bottom < b->bottom ? a->bottom : b->bottom };
    if (r.left >= r.right || r.top >= r.bottom) { memset(out, 0, sizeof *out); return 0; }
    *out = r;
    return 1;
}
static WINAPI BOOL UnionRect(RECT *out, const RECT *a, const RECT *b) {
    int ea = a->left >= a->right || a->top >= a->bottom, eb = b->left >= b->right || b->top >= b->bottom;
    if (ea && eb) { memset(out, 0, sizeof *out); return 0; }
    if (ea) { *out = *b; return 1; }
    if (eb) { *out = *a; return 1; }
    RECT r = { a->left < b->left ? a->left : b->left, a->top < b->top ? a->top : b->top,
               a->right > b->right ? a->right : b->right, a->bottom > b->bottom ? a->bottom : b->bottom };
    *out = r;
    return 1;
}
static WINAPI BOOL AttachThreadInput(DWORD a, DWORD b, BOOL on) { (void)a; (void)b; (void)on; return 1; }
static WINAPI BOOL ExitWindowsEx(DWORD flags, DWORD reason) { (void)flags; (void)reason; vx_set_last_error(5); return 0; }
static WINAPI uint64_t GetClassLongPtrA(HANDLE wnd, int index) { (void)wnd; (void)index; return 0; }
static WINAPI int GetClipboardFormatNameA(DWORD fmt, char *out, int cap) { (void)fmt; if (out && cap) out[0] = 0; return 0; }
static WINAPI DWORD EnumClipboardFormats(DWORD fmt) { (void)fmt; vx_set_last_error(0); return 0; }
static WINAPI DWORD RegisterClipboardFormatW(const WCHAR *name) {
    static DWORD next = 0xC100;                                    // a new atom for each call (names are not compared)
    (void)name;
    return __sync_fetch_and_add(&next, 1);
}
static WINAPI BOOL GetCursorInfo(DWORD *info) {
    if (!info || info[0] < 24) return 0;
    BOOL (WINAPI *pos)(LONG *) = (BOOL (WINAPI *)(LONG *))vx_find_export("user32.dll", "GetCursorPos");
    info[1] = 1;                                                   // CURSOR_SHOWING
    *(void **)(info + 2) = NULL;
    pos((LONG *)(info + 4));
    return 1;
}
static WINAPI DWORD GetMessagePos(void) {
    LONG p[2] = { 0, 0 };
    BOOL (WINAPI *pos)(LONG *) = (BOOL (WINAPI *)(LONG *))vx_find_export("user32.dll", "GetCursorPos");
    pos(p);
    return (DWORD)(uint16_t)p[0] | (DWORD)(uint16_t)p[1] << 16;
}
static WINAPI HANDLE GetShellWindow(void) { return NULL; }
static WINAPI BOOL IsWindowUnicode(HANDLE w) { (void)w; return 1; }
static WINAPI BOOL MapDialogRect(HANDLE dlg, RECT *r) {           // dialog units at an 8-point system font: 6x13 per 4x8
    (void)dlg;
    r->left = r->left * 6 / 4; r->right = r->right * 6 / 4; r->top = r->top * 13 / 8; r->bottom = r->bottom * 13 / 8;
    return 1;
}
static WINAPI HANDLE SetParent(HANDLE wnd, HANDLE parent) {
    int64_t (WINAPI *set)(HANDLE, int, int64_t) = (int64_t (WINAPI *)(HANDLE, int, int64_t))vx_find_export("user32.dll", "SetWindowLongPtrW");
    return (HANDLE)(intptr_t)set(wnd, -8 /* GWLP_HWNDPARENT */, (int64_t)(intptr_t)parent);
}
static int g_win_event_hook;
static WINAPI HANDLE SetWinEventHook(DWORD min, DWORD max, HANDLE mod, void *fn, DWORD pid, DWORD tid, DWORD flags) {
    (void)min; (void)max; (void)mod; (void)fn; (void)pid; (void)tid; (void)flags;
    return &g_win_event_hook;                                      // no accessibility events are sent
}
static WINAPI BOOL UnhookWinEvent(HANDLE h) { (void)h; return 1; }
static WINAPI BOOL InsertMenuW(HANDLE menu, unsigned pos, unsigned flags, uintptr_t id, const WCHAR *item) { (void)menu; (void)pos; (void)flags; (void)id; (void)item; return 1; }
static WINAPI BOOL DeleteMenu(HANDLE menu, unsigned pos, unsigned flags) { (void)menu; (void)pos; (void)flags; return 1; }
static WINAPI BOOL GetMenuItemInfoW(HANDLE menu, unsigned item, BOOL by_pos, void *info) { (void)menu; (void)item; (void)by_pos; (void)info; vx_set_last_error(1456); return 0; }
static WINAPI HANDLE CreateDialogIndirectParamA(HANDLE inst, const void *tmpl, HANDLE parent, void *proc, intptr_t param) {
    (void)inst; (void)tmpl; (void)parent; (void)proc; (void)param;
    vx_set_last_error(120);
    return NULL;
}
static WINAPI intptr_t DialogBoxIndirectParamA(HANDLE inst, const void *tmpl, HANDLE parent, void *proc, intptr_t param) {
    (void)inst; (void)tmpl; (void)parent; (void)proc; (void)param;
    vx_set_last_error(120);
    return -1;
}
// The display configuration API: not supported (callers fall back to EnumDisplaySettings).
static WINAPI LONG display_config_unsupported(void) { return 50; }   // ERROR_NOT_SUPPORTED

// ---- gdi32, imm32, winmm, ole32 -------------------------------------------------------------------------------------------------
static WINAPI LONG GetBitmapBits(HANDLE bmp, LONG n, void *out) { (void)bmp; (void)n; (void)out; return 0; }
static WINAPI BOOL GetICMProfileW(HANDLE dc, DWORD *n, WCHAR *out) { (void)dc; (void)n; (void)out; return 0; }
static WINAPI BOOL SetBrushOrgEx(HANDLE dc, int x, int y, LONG *old) { (void)dc; (void)x; (void)y; if (old) old[0] = old[1] = 0; return 1; }
static WINAPI BOOL ImmGetCompositionFontW(HANDLE imc, void *font) { (void)imc; (void)font; return 0; }
static WINAPI DWORD waveOutMessage(HANDLE h, unsigned msg, uintptr_t a, uintptr_t b) { (void)h; (void)msg; (void)a; (void)b; return 8; }   // MMSYSERR_NOTSUPPORTED
static WINAPI void ReleaseStgMedium(void *medium) { (void)medium; }

// ---- shell32 --------------------------------------------------------------------------------------------------------------------
static WINAPI LONG SHGetMalloc(void **out) {
    LONG (WINAPI *get)(DWORD, void **) = (LONG (WINAPI *)(DWORD, void **))vx_find_export("ole32.dll", "CoGetMalloc");
    return get(1, out);
}
static WINAPI void ILFree(void *pidl) { free(pidl); }
static WINAPI void *ILCreateFromPathW(const WCHAR *path) { (void)path; return NULL; }   // no shell namespace
static WINAPI LONG SHOpenFolderAndSelectItems(const void *folder, unsigned n, const void *items, DWORD flags) {
    (void)folder; (void)n; (void)items; (void)flags;
    return (LONG)0x80004001;                                       // E_NOTIMPL
}

// ---- psapi: this process's modules ----------------------------------------------------------------------------------------------
static WINAPI BOOL EnumProcessModules(HANDLE process, void **mods, DWORD cb, DWORD *needed) {
    (void)process;
    const vx_image *list[512];
    int n = vx_image_list(list, 512);
    if (n > 512) n = 512;
    for (int i = 0; i < n && (DWORD)(i + 1) * 8 <= cb; i++) mods[i] = list[i]->base;
    if (needed) *needed = (DWORD)n * 8;
    return 1;
}
static WINAPI BOOL EnumProcessModulesEx(HANDLE process, void **mods, DWORD cb, DWORD *needed, DWORD filter) { (void)filter; return EnumProcessModules(process, mods, cb, needed); }
static WINAPI DWORD GetModuleBaseNameW(HANDLE process, void *module, WCHAR *buf, DWORD size) {
    WCHAR full[1024];
    DWORD n = GetModuleFileNameExW(process, module, full, 1024);
    if (!n) return 0;
    const WCHAR *base = full;
    for (const WCHAR *c = full; *c; c++) if (*c == '\\' || *c == '/') base = c + 1;
    char *a = vx_utf8_dup(base);
    DWORD r = vx_return_string_w(a, buf, size);
    free(a);
    return r;
}
static WINAPI DWORD GetModuleBaseNameA(HANDLE process, void *module, char *buf, DWORD size) {
    WCHAR w[512];
    if (!GetModuleBaseNameW(process, module, w, 512)) return 0;
    char *a = vx_utf8_dup(w);
    DWORD r = vx_return_string_a(a, buf, size);
    free(a);
    return r;
}

// ---- setupapi, hid, iphlpapi: no device enumeration -----------------------------------------------------------------------------
static WINAPI DWORD CM_Get_DevNode_Status(DWORD *status, DWORD *problem, DWORD node, DWORD flags) {
    (void)node; (void)flags;
    if (status) *status = 0; if (problem) *problem = 0;
    return 0x0D;                                                   // CR_NO_SUCH_DEVNODE
}
static WINAPI BOOL SetupDiGetDevicePropertyW(HANDLE set, void *data, const void *key, DWORD *type, void *buf, DWORD size, DWORD *need, DWORD flags) {
    (void)set; (void)data; (void)key; (void)type; (void)buf; (void)size; (void)need; (void)flags;
    vx_set_last_error(0xE0000225u);                                // ERROR_NOT_FOUND (setupapi)
    return 0;
}
static WINAPI void HidD_GetHidGuid(uint8_t *guid) {
    static const uint8_t hid[16] = { 0xb2, 0x55, 0x1e, 0x4d, 0x6f, 0xf1, 0xcf, 0x11, 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 };
    memcpy(guid, hid, 16);
}
static WINAPI BOOL hid_fails(void) { vx_set_last_error(1); return 0; }
static WINAPI LONG hidp_fails(void) { return (LONG)0xC0110001; }  // HIDP_STATUS_INVALID_PREPARSED_DATA
static WINAPI DWORD iphlp_unsupported(void) { return 50; }       // ERROR_NOT_SUPPORTED

// ---- crypt32 extras: base64 and the rest refused ----------------------------------------------------------------------------------
static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
#define CRYPT_STRING_BASE64HEADER 0
#define CRYPT_STRING_BASE64 1
#define CRYPT_STRING_BINARY 2
#define CRYPT_STRING_NOCRLF 0x40000000u
static WINAPI BOOL CryptBinaryToStringA(const uint8_t *data, DWORD n, DWORD flags, char *out, DWORD *cap) {
    DWORD kind = flags & 0xffff;
    if (kind != CRYPT_STRING_BASE64 && kind != CRYPT_STRING_BASE64HEADER) { vx_set_last_error(87); return 0; }
    int crlf = !(flags & CRYPT_STRING_NOCRLF);
    DWORD chars = (n + 2) / 3 * 4, lines = (chars + 63) / 64, need = chars + (crlf ? lines * 2 : 0) + 1;
    if (!out) { *cap = need; return 1; }
    if (*cap < need) { *cap = need; vx_set_last_error(234); return 0; }
    DWORD o = 0, col = 0;
    for (DWORD i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16 | (i + 1 < n ? (uint32_t)data[i + 1] << 8 : 0) | (i + 2 < n ? data[i + 2] : 0);
        out[o++] = b64[v >> 18 & 63]; out[o++] = b64[v >> 12 & 63];
        out[o++] = i + 1 < n ? b64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? b64[v & 63] : '=';
        if ((col += 4) == 64 && crlf) { out[o++] = '\r'; out[o++] = '\n'; col = 0; }
    }
    if (crlf && col) { out[o++] = '\r'; out[o++] = '\n'; }
    out[o] = 0;
    *cap = o;
    return 1;
}
static WINAPI BOOL CryptStringToBinaryA(const char *s, DWORD len, DWORD flags, uint8_t *out, DWORD *cap, DWORD *skip, DWORD *used) {
    if (!len) len = (DWORD)strlen(s);
    if (skip) *skip = 0;
    if (used) *used = flags;
    if (flags == CRYPT_STRING_BINARY) {
        if (out && *cap < len) { *cap = len; vx_set_last_error(234); return 0; }
        if (out) memcpy(out, s, len);
        *cap = len;
        return 1;
    }
    const char *p = s, *end = s + len;
    if (flags == CRYPT_STRING_BASE64HEADER || flags == 6 /* ANY */ || flags == 7 /* HEX_ANY */) {   // skip "-----BEGIN ...-----"
        const char *h = memmem(s, len, "-----BEGIN", 10);
        if (h) { const char *e = memchr(h + 10, '\n', (size_t)(end - h - 10)); if (e) p = e + 1; const char *f = memmem(p, (size_t)(end - p), "-----END", 8); if (f) end = f; }
    }
    DWORD o = 0; uint32_t acc = 0; int bits = 0;
    for (; p < end; p++) {
        const char *k = memchr(b64, *p, 64);
        if (!k) { if (*p == '=' || *p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') continue; vx_set_last_error(13); return 0; }
        acc = acc << 6 | (uint32_t)(k - b64); bits += 6;
        if (bits >= 8) { bits -= 8; if (out) { if (o >= *cap) { vx_set_last_error(234); return 0; } out[o] = (uint8_t)(acc >> bits); } o++; }
    }
    *cap = o;
    return 1;
}
static WINAPI BOOL crypt_refuses(void) { vx_set_last_error(0x80092002u); return 0; }   // CRYPT_E_BAD_ENCODE
static WINAPI void *crypt_refuses_ptr(void) { vx_set_last_error(0x80092002u); return NULL; }

// ---- bcrypt, ncrypt, secur32: only random numbers are offered ----------------------------------------------------------------
static WINAPI BOOL ProcessPrng(uint8_t *buf, size_t n) { arc4random_buf(buf, n); return 1; }   // bcryptprimitives (Chromium)
static WINAPI LONG bcrypt_unsupported(void) { return (LONG)0xC00000BB; }   // STATUS_NOT_SUPPORTED
static WINAPI LONG BCryptCloseAlgorithmProvider(void *alg, DWORD flags) { (void)alg; (void)flags; return 0; }
static WINAPI LONG ncrypt_unsupported(void) { return (LONG)0x80090029; }   // NTE_NOT_SUPPORTED
static WINAPI LONG sspi_unsupported(void) { return (LONG)0x80090302; }     // SEC_E_UNSUPPORTED_FUNCTION
static WINAPI LONG sspi_ok(void) { return 0; }

// ---- wininet (wide): no connections, like the narrow functions in desktop.c ---------------------------------------------------
static WINAPI void *inet_none(void) { vx_set_last_error(12029); return NULL; }   // ERROR_INTERNET_CANNOT_CONNECT
static WINAPI BOOL inet_fails(void) { vx_set_last_error(12029); return 0; }
static WINAPI BOOL inet_ok(void) { return 1; }

// ---- avifil32: AVI files are not read or written (video capture features report a failure) --------------------------------
static WINAPI void AVIFileInit(void) {}
static WINAPI LONG avi_fails(void) { return (LONG)0x80044067; }            // AVIERR_FILEOPEN
static WINAPI DWORD avi_zero(void) { return 0; }
static WINAPI void *avi_null(void) { return NULL; }


// ---- what the Steam client's own UI toolkit (vgui) and its child programs reach for --------------------------------------------
// Every face asked for "exists" (text is drawn with the system's fonts): the callback sees the request back once, as a
// TrueType font. An empty face name lists one family.
static int enum_fonts(void *lf, int wide, void *fn, intptr_t param) {
    uint8_t font[92] = { 0 }, metrics[96] = { 0 };                 // LOGFONTW is the larger; TEXTMETRICW and more
    size_t lf_size = wide ? 92 : 60, face = 28;
    memcpy(font, lf, lf_size);
    if (wide ? !((WCHAR *)(font + face))[0] : !font[face]) {
        const char *def = "Arial";
        if (wide) for (int i = 0; def[i]; i++) ((WCHAR *)(font + face))[i] = (WCHAR)def[i];
        else memcpy(font + face, def, strlen(def) + 1);
    }
    int32_t *tm = (int32_t *)metrics;
    int32_t h = *(int32_t *)font; if (h < 0) h = -h; if (!h) h = 16;
    tm[0] = h; tm[1] = h * 4 / 5; tm[2] = h - tm[1]; tm[5] = h / 2; tm[6] = h;   // height, ascent, descent, avg, max width
    tm[7] = 400;                                                   // weight
    int (WINAPI *cb)(const void *, const void *, DWORD, intptr_t) = (int (WINAPI *)(const void *, const void *, DWORD, intptr_t))fn;
    return cb(font, metrics, 4 /* TRUETYPE_FONTTYPE */, param);
}
static WINAPI int EnumFontFamiliesExA(HANDLE dc, void *lf, void *fn, intptr_t param, DWORD flags) { (void)dc; (void)flags; return enum_fonts(lf, 0, fn, param); }
static WINAPI int EnumFontFamiliesExW(HANDLE dc, void *lf, void *fn, intptr_t param, DWORD flags) { (void)dc; (void)flags; return enum_fonts(lf, 1, fn, param); }
static WINAPI int AddFontResourceExA(const char *name, DWORD fl, void *res) { (void)name; (void)fl; (void)res; return 1; }
static WINAPI int AddFontResourceA(const char *name) { (void)name; return 1; }
static WINAPI BOOL RemoveFontResourceA(const char *name) { (void)name; return 1; }
static WINAPI BOOL GetCharABCWidthsW(HANDLE dc, unsigned first, unsigned last, int *abc) {   // a fixed 8-pixel advance
    (void)dc;
    for (unsigned c = first; c <= last; c++, abc += 3) { abc[0] = 0; abc[1] = 8; abc[2] = 0; }
    return 1;
}
static WINAPI BOOL Polyline(HANDLE dc, const void *pts, int n) { (void)dc; (void)pts; (void)n; return 1; }
static WINAPI BOOL SetRectRgn(HANDLE rgn, int l, int t, int r, int b) { (void)rgn; (void)l; (void)t; (void)r; (void)b; return 1; }
static int g_imc;
static WINAPI HANDLE ImmCreateContext(void) { return &g_imc; }
static WINAPI BOOL ImmDestroyContext(HANDLE imc) { (void)imc; return 1; }
static WINAPI BOOL msimg_ok(void) { return 1; }                   // AlphaBlend, GradientFill, TransparentBlt: nothing drawn
static WINAPI uintptr_t SHAppBarMessage(DWORD msg, void *data) { (void)msg; (void)data; return 0; }
static WINAPI HANDLE CopyImage(HANDLE h, unsigned type, int cx, int cy, unsigned flags) { (void)type; (void)cx; (void)cy; (void)flags; return h; }
static WINAPI BOOL caret_ok(void) { return 1; }                   // CreateCaret, DestroyCaret, SetCaretPos, ShowCaret, HideCaret
// GetClassWord: GCW_ATOM (-32), the class's atom (Steam's vgui asks it of the window under the pointer to know its own
// windows); other indexes as GetClassLongPtrW answers them (the extra class bytes: 0).
static WINAPI uint16_t GetClassWord(HANDLE w, int index) {
    uint64_t (WINAPI *get)(HANDLE, int) = (uint64_t (WINAPI *)(HANDLE, int))vx_find_export("user32.dll", "GetClassLongPtrW");
    return get ? (uint16_t)get(w, index) : 0;
}
static WINAPI BOOL OffsetRect(RECT *r, int dx, int dy) { if (!r) return 0; r->left += dx; r->right += dx; r->top += dy; r->bottom += dy; return 1; }
static WINAPI BOOL GetOpenFileNameW(void *ofn) { (void)ofn; return 0; }   // cancelled
static WINAPI DWORD CommDlgExtendedError(void) { return 0; }
static WINAPI LONG DoDragDrop(void *data, void *source, DWORD ok, DWORD *effect) { (void)data; (void)source; (void)ok; if (effect) *effect = 0; return 0x40101; }   // DRAGDROP_S_CANCEL
static WINAPI void gl_ignored(void) {}

// ---- what SteamVR Home (Source 2) and its in-process Steam game server reach for ------------------------------------------
static WINAPI BOOL SetupDiClassGuidsFromNameA(const char *name, void *guids, DWORD n, DWORD *need) { (void)name; (void)guids; (void)n; if (need) *need = 0; return 1; }
static WINAPI HANDLE SetupDiOpenDevRegKey(HANDLE set, void *data, DWORD scope, DWORD profile, DWORD type, DWORD access) {
    (void)set; (void)data; (void)scope; (void)profile; (void)type; (void)access;
    vx_set_last_error(0xE0000209u);                                // ERROR_KEY_DOES_NOT_EXIST (setupapi)
    return (HANDLE)(intptr_t)-1;
}
static WINAPI DWORD CM_Locate_DevNodeA(DWORD *node, const char *id, DWORD flags) { (void)id; (void)flags; if (node) *node = 0; return 0x0D; }
static WINAPI DWORD iphlp_not_found(void) { return 2; }          // ERROR_FILE_NOT_FOUND: no such interface
static WINAPI DWORD mixer_none(void) { return 2; }               // MMSYSERR_BADDEVICEID: no mixer devices
static WINAPI LONG usp_fails(void) { return (LONG)0x80004005; }  // E_FAIL: callers fall back to plain GDI text
static WINAPI LONG ScriptFreeCache(void **cache) { if (cache) *cache = NULL; return 0; }
static WINAPI DWORD gdi_none(void) { return 0; }                 // GetCharacterPlacement, GetFontLanguageInfo, GetOutlineTextMetrics
static WINAPI DWORD GetGlyphOutline(void) { return 0xffffffffu; } // GDI_ERROR
static WINAPI BOOL TranslateCharsetInfo(DWORD *src, DWORD *cs, DWORD flags) { (void)src; (void)flags; if (cs) memset(cs, 0, 24); return 0; }
static WINAPI BOOL GetVolumeInformationW(const WCHAR *root, WCHAR *name, DWORD name_cap, DWORD *serial, DWORD *max_len, DWORD *flags, WCHAR *fs, DWORD fs_cap) {
    (void)root;
    if (name && name_cap) vx_return_string_w("", name, name_cap);
    if (serial) *serial = 0x56585254;
    if (max_len) *max_len = 255;
    if (flags) *flags = 0x3;
    if (fs && fs_cap) vx_return_string_w("NTFS", fs, fs_cap);
    return 1;
}
static WINAPI DWORD GetPrivateProfileStringW(const WCHAR *app, const WCHAR *key, const WCHAR *def, WCHAR *out, DWORD cap, const WCHAR *file) {
    (void)app; (void)key; (void)file;
    if (!out || !cap) return 0;
    size_t n = def ? vx_wcslen(def) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(out, def, n * 2);
    out[n] = 0;
    return (DWORD)n;
}
// QueueUserWorkItem: on the thread pool's one-off callbacks (system.c), which pass (instance, context).
typedef struct { DWORD (WINAPI *fn)(void *); void *ctx; } work_item;
static WINAPI void run_work_item(void *instance, void *arg) { (void)instance; work_item w = *(work_item *)arg; free(arg); w.fn(w.ctx); }
static WINAPI BOOL QueueUserWorkItem(DWORD (WINAPI *fn)(void *), void *ctx, DWORD flags) {
    (void)flags;
    BOOL (WINAPI *submit)(void *, void *, void *) = (BOOL (WINAPI *)(void *, void *, void *))vx_find_export("kernel32.dll", "TrySubmitThreadpoolCallback");
    work_item *w = malloc(sizeof *w);
    w->fn = fn; w->ctx = ctx;
    return submit((void *)run_work_item, w, NULL);
}
static WINAPI BOOL io_ex_fails(void) { vx_set_last_error(50); return 0; }   // ReadFileEx/WriteFileEx (completion routines): not supported
static WINAPI BOOL CancelSynchronousIo(HANDLE t) { (void)t; vx_set_last_error(1168 /* ERROR_NOT_FOUND */); return 0; }
static WINAPI BOOL SetSystemPowerState(BOOL suspend, BOOL force) { (void)suspend; (void)force; vx_set_last_error(5); return 0; }
static WINAPI BOOL WriteConsoleOutputAttribute(HANDLE h, const uint16_t *a, DWORD n, DWORD pos, DWORD *written) { (void)h; (void)a; (void)pos; if (written) *written = n; return 1; }
static int this_process(HANDLE p) {
    DWORD (WINAPI *pid_of)(HANDLE) = (DWORD (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "GetProcessId");
    return p == (HANDLE)(intptr_t)-1 || (pid_of && pid_of(p) == (DWORD)getpid());
}
static WINAPI void *VirtualAllocEx(HANDLE p, void *addr, size_t size, DWORD type, DWORD protect) {
    if (!this_process(p)) { vx_set_last_error(5); return NULL; }
    void *(WINAPI *va)(void *, size_t, DWORD, DWORD) = (void *(WINAPI *)(void *, size_t, DWORD, DWORD))vx_find_export("kernel32.dll", "VirtualAlloc");
    return va(addr, size, type, protect);
}
static WINAPI BOOL VirtualProtectEx(HANDLE p, void *addr, size_t size, DWORD prot, DWORD *old) {
    if (!this_process(p)) { vx_set_last_error(5); return 0; }
    BOOL (WINAPI *vp)(void *, size_t, DWORD, DWORD *) = (BOOL (WINAPI *)(void *, size_t, DWORD, DWORD *))vx_find_export("kernel32.dll", "VirtualProtect");
    return vp(addr, size, prot, old);
}
static WINAPI BOOL WriteProcessMemory(HANDLE p, void *to, const void *from, size_t n, size_t *written) {
    if (written) *written = 0;
    if (!this_process(p)) { vx_set_last_error(5); return 0; }
    memcpy(to, from, n);
    if (written) *written = n;
    return 1;
}
static WINAPI HANDLE FindWindowExW(HANDLE parent, HANDLE after, const WCHAR *cls, const WCHAR *title) {
    if (parent || after) return NULL;                              // top-level windows only
    HANDLE (WINAPI *find)(const WCHAR *, const WCHAR *) = (HANDLE (WINAPI *)(const WCHAR *, const WCHAR *))vx_find_export("user32.dll", "FindWindowW");
    return find(cls, title);
}
static WINAPI int16_t VkKeyScanExA(char c, void *layout) {
    (void)layout;
    if (c >= 'a' && c <= 'z') return (int16_t)(c - 32);
    if (c >= 'A' && c <= 'Z') return (int16_t)(0x100 | c);       // shift + the key
    if (c >= '0' && c <= '9') return c;
    return c == ' ' ? 0x20 : -1;
}
static WINAPI void keybd_event(uint8_t vk, uint8_t scan, DWORD flags, uintptr_t extra) { (void)vk; (void)scan; (void)flags; (void)extra; }
static WINAPI BOOL PathFileExistsW(const WCHAR *path) {
    DWORD (WINAPI *attrs)(const WCHAR *) = (DWORD (WINAPI *)(const WCHAR *))vx_find_export("kernel32.dll", "GetFileAttributesW");
    return path && attrs(path) != 0xffffffffu;
}
static WINAPI BOOL PathRemoveFileSpecW(WCHAR *path) {
    if (!path) return 0;
    WCHAR *last = NULL;
    for (WCHAR *c = path; *c; c++) if (*c == '\\' || *c == '/') last = c;
    if (!last) { BOOL had = path[0] != 0; path[0] = 0; return had; }
    if (last == path || (last == path + 2 && path[1] == ':')) last[1] = 0;   // keep the root ("\\", "C:\\")
    else *last = 0;
    return 1;
}

static const vx_export k32[] = {
    VX_EXPORT(GetProductInfo), VX_EXPORT(ProcessIdToSessionId), VX_EXPORT(WTSGetActiveConsoleSessionId), VX_EXPORT(ReadConsoleA),
    { "K32GetModuleFileNameExW", (void *)GetModuleFileNameExW }, { "K32GetModuleFileNameExA", (void *)GetModuleFileNameExA },
    VX_EXPORT(SetProcessShutdownParameters), VX_EXPORT(SetThreadErrorMode), VX_EXPORT(IsWow64Process), VX_EXPORT(GetBinaryTypeW),
    VX_EXPORT(GetVolumeInformationA), VX_EXPORT(GetVolumePathNameW), VX_EXPORT(GetVolumeNameForVolumeMountPointW),
    VX_EXPORT(GetPrivateProfileStringA), VX_EXPORT(FoldStringW), VX_EXPORT(EnumResourceNamesW),
    { "DebugActiveProcess", (void *)no_debugging }, { "DebugActiveProcessStop", (void *)no_debugging }, VX_EXPORT(GenerateConsoleCtrlEvent),
    { "K32EnumProcessModules", (void *)EnumProcessModules }, { "K32EnumProcessModulesEx", (void *)EnumProcessModulesEx },
    { "K32GetModuleBaseNameW", (void *)GetModuleBaseNameW }, { "K32GetModuleBaseNameA", (void *)GetModuleBaseNameA },
    VX_EXPORT(GetVolumeInformationW), VX_EXPORT(GetPrivateProfileStringW),
    VX_EXPORT(QueueUserWorkItem), { "ReadFileEx", (void *)io_ex_fails }, { "WriteFileEx", (void *)io_ex_fails }, VX_EXPORT(CancelSynchronousIo),
    VX_EXPORT(SetSystemPowerState), VX_EXPORT(WriteConsoleOutputAttribute), VX_EXPORT(VirtualAllocEx), VX_EXPORT(VirtualProtectEx),
    VX_EXPORT(WriteProcessMemory),
};
static const vx_export usp10[] = {
    { "ScriptItemize", (void *)usp_fails }, { "ScriptShape", (void *)usp_fails }, { "ScriptPlace", (void *)usp_fails },
    { "ScriptTextOut", (void *)usp_fails }, { "ScriptBreak", (void *)usp_fails }, VX_EXPORT(ScriptFreeCache),
};
static const vx_export shlwapi[] = { VX_EXPORT(PathFileExistsW), VX_EXPORT(PathRemoveFileSpecW) };
static const vx_export msimg32[] = { { "AlphaBlend", (void *)msimg_ok }, { "GradientFill", (void *)msimg_ok }, { "TransparentBlt", (void *)msimg_ok } };
static const vx_export comdlg32[] = {
    VX_EXPORT(GetOpenFileNameW), { "GetSaveFileNameW", (void *)GetOpenFileNameW }, { "GetOpenFileNameA", (void *)GetOpenFileNameW },
    { "GetSaveFileNameA", (void *)GetOpenFileNameW }, VX_EXPORT(CommDlgExtendedError),
};
static const vx_export opengl32[] = {
    { "glBegin", (void *)gl_ignored }, { "glEnd", (void *)gl_ignored }, { "glBlendFunc", (void *)gl_ignored }, { "glColor4fv", (void *)gl_ignored },
    { "glColor4ub", (void *)gl_ignored }, { "glDisableClientState", (void *)gl_ignored }, { "glEnableClientState", (void *)gl_ignored },
    { "glIsTexture", (void *)gl_ignored }, { "glLineWidth", (void *)gl_ignored }, { "glLoadIdentity", (void *)gl_ignored },
    { "glMatrixMode", (void *)gl_ignored }, { "glOrtho", (void *)gl_ignored }, { "glRenderMode", (void *)gl_ignored },
    { "glScissor", (void *)gl_ignored }, { "glShadeModel", (void *)gl_ignored }, { "glTexCoord2f", (void *)gl_ignored },
    { "glTexEnvf", (void *)gl_ignored }, { "glTexParameterf", (void *)gl_ignored }, { "glTexSubImage2D", (void *)gl_ignored },
    { "glTranslatef", (void *)gl_ignored }, { "glVertex2f", (void *)gl_ignored }, { "glPushMatrix", (void *)gl_ignored },
    { "glPopMatrix", (void *)gl_ignored }, { "glVertexPointer", (void *)gl_ignored }, { "glTexCoordPointer", (void *)gl_ignored },
    { "glColorPointer", (void *)gl_ignored }, { "glDrawArrays", (void *)gl_ignored }, { "glColor4f", (void *)gl_ignored },
};
static const vx_export advapi32[] = { VX_EXPORT(RegSetKeyValueW), VX_EXPORT(RegDeleteKeyValueW) };
static const vx_export user32[] = {
    VX_EXPORT(SetClassLongPtrW), { "SetClassLongPtrA", (void *)SetClassLongPtrW }, VX_EXPORT(SetRectEmpty), VX_EXPORT(PtInRect),
    VX_EXPORT(IntersectRect), VX_EXPORT(UnionRect), VX_EXPORT(AttachThreadInput),
    VX_EXPORT(ExitWindowsEx), VX_EXPORT(GetClassLongPtrA),
    VX_EXPORT(GetClipboardFormatNameA), VX_EXPORT(EnumClipboardFormats), VX_EXPORT(RegisterClipboardFormatW),
    { "RegisterClipboardFormatA", (void *)RegisterClipboardFormatW }, VX_EXPORT(GetCursorInfo), VX_EXPORT(GetMessagePos),
    VX_EXPORT(GetShellWindow), VX_EXPORT(IsWindowUnicode), VX_EXPORT(MapDialogRect), VX_EXPORT(SetParent), VX_EXPORT(SetWinEventHook),
    VX_EXPORT(UnhookWinEvent), VX_EXPORT(InsertMenuW), VX_EXPORT(DeleteMenu), VX_EXPORT(GetMenuItemInfoW),
    VX_EXPORT(CreateDialogIndirectParamA), VX_EXPORT(DialogBoxIndirectParamA), VX_EXPORT(CopyImage), { "CreateCaret", (void *)caret_ok },
    { "DestroyCaret", (void *)caret_ok }, { "SetCaretPos", (void *)caret_ok }, { "ShowCaret", (void *)caret_ok }, { "HideCaret", (void *)caret_ok },
    VX_EXPORT(GetClassWord), VX_EXPORT(OffsetRect), VX_EXPORT(FindWindowExW), VX_EXPORT(VkKeyScanExA), VX_EXPORT(keybd_event),
    { "GetDisplayConfigBufferSizes", (void *)display_config_unsupported }, { "QueryDisplayConfig", (void *)display_config_unsupported },
    { "DisplayConfigGetDeviceInfo", (void *)display_config_unsupported }, { "DisplayConfigSetDeviceInfo", (void *)display_config_unsupported },
};
static const vx_export imm32[] = { VX_EXPORT(ImmGetCompositionFontW), VX_EXPORT(ImmCreateContext), VX_EXPORT(ImmDestroyContext) };
static const vx_export winmm[] = {
    VX_EXPORT(waveOutMessage), { "waveInMessage", (void *)waveOutMessage }, { "mixerOpen", (void *)mixer_none }, { "mixerClose", (void *)mixer_none },
    { "mixerGetID", (void *)mixer_none }, { "mixerGetDevCapsA", (void *)mixer_none }, { "mixerGetLineInfoA", (void *)mixer_none },
    { "mixerGetLineControlsA", (void *)mixer_none }, { "mixerGetControlDetailsA", (void *)mixer_none }, { "mixerSetControlDetails", (void *)mixer_none },
    { "mixerGetNumDevs", (void *)gdi_none },
};
static const vx_export ole32[] = { VX_EXPORT(ReleaseStgMedium), VX_EXPORT(DoDragDrop) };
static const vx_export setupapi[] = {
    VX_EXPORT(CM_Get_DevNode_Status), VX_EXPORT(SetupDiGetDevicePropertyW), VX_EXPORT(SetupDiClassGuidsFromNameA),
    VX_EXPORT(SetupDiOpenDevRegKey), VX_EXPORT(CM_Locate_DevNodeA),
};
static const vx_export hid[] = {
    VX_EXPORT(HidD_GetHidGuid), { "HidD_GetAttributes", (void *)hid_fails }, { "HidD_GetPreparsedData", (void *)hid_fails },
    { "HidD_FreePreparsedData", (void *)hid_fails }, { "HidD_GetProductString", (void *)hid_fails }, { "HidD_GetManufacturerString", (void *)hid_fails },
    { "HidD_GetSerialNumberString", (void *)hid_fails }, { "HidD_GetFeature", (void *)hid_fails }, { "HidD_SetFeature", (void *)hid_fails },
    { "HidD_SetOutputReport", (void *)hid_fails }, { "HidD_GetInputReport", (void *)hid_fails }, { "HidD_SetNumInputBuffers", (void *)hid_fails },
    { "HidP_GetCaps", (void *)hidp_fails }, { "HidP_GetButtonCaps", (void *)hidp_fails }, { "HidP_GetValueCaps", (void *)hidp_fails },
    { "HidP_GetData", (void *)hidp_fails }, { "HidP_MaxDataListLength", (void *)hidp_fails }, { "HidP_GetUsages", (void *)hidp_fails },
    { "HidP_GetUsageValue", (void *)hidp_fails }, { "HidP_GetScaledUsageValue", (void *)hidp_fails },
};
static const vx_export iphlpapi[] = {
    { "GetExtendedTcpTable", (void *)iphlp_unsupported }, { "GetExtendedUdpTable", (void *)iphlp_unsupported },
    { "GetAdaptersInfo", (void *)iphlp_unsupported },                   // (GetAdaptersAddresses, GetBestInterface, GetBestRoute: extras.c)
    { "GetIfTable", (void *)iphlp_unsupported },                         // (GetNetworkParams: extras.c; NotifyAddrChange, GetIfEntry2: iphlp.c)
    { "ConvertInterfaceGuidToLuid", (void *)iphlp_not_found },
};
static const vx_export gdi32[] = {
    VX_EXPORT(AddFontMemResourceEx), VX_EXPORT(RemoveFontMemResourceEx), VX_EXPORT(CreateICW), VX_EXPORT(CreateICA),
    VX_EXPORT(GetBitmapBits), VX_EXPORT(GetICMProfileW), VX_EXPORT(SetBrushOrgEx), VX_EXPORT(EnumFontFamiliesExA),
    VX_EXPORT(EnumFontFamiliesExW), VX_EXPORT(AddFontResourceExA), VX_EXPORT(AddFontResourceA),
    { "AddFontResourceExW", (void *)AddFontResourceExA }, { "AddFontResourceW", (void *)AddFontResourceA }, VX_EXPORT(RemoveFontResourceA),
    { "RemoveFontResourceW", (void *)RemoveFontResourceA }, { "RemoveFontResourceExA", (void *)RemoveFontResourceA },
    VX_EXPORT(GetCharABCWidthsW), VX_EXPORT(Polyline), VX_EXPORT(SetRectRgn), { "GetCharacterPlacementA", (void *)gdi_none },
    { "GetCharacterPlacementW", (void *)gdi_none }, { "GetFontLanguageInfo", (void *)gdi_none }, { "GetOutlineTextMetricsA", (void *)gdi_none },
    { "GetOutlineTextMetricsW", (void *)gdi_none }, { "GetGlyphOutlineA", (void *)GetGlyphOutline }, { "GetGlyphOutlineW", (void *)GetGlyphOutline },
    VX_EXPORT(TranslateCharsetInfo),
};
static const vx_export shell32[] = {
    VX_EXPORT(IsUserAnAdmin), { "#680", (void *)IsUserAnAdmin }, VX_EXPORT(SHGetFileInfoW), { "SHGetFileInfoA", (void *)SHGetFileInfoW },
    VX_EXPORT(SHGetMalloc), VX_EXPORT(ILFree), { "#155", (void *)ILFree }, VX_EXPORT(ILCreateFromPathW), { "#190", (void *)ILCreateFromPathW },
    VX_EXPORT(SHOpenFolderAndSelectItems), VX_EXPORT(SHAppBarMessage),
};
// TaskDialogIndirect: programs show fatal errors this way. The text goes to the log, and the first button is
// pressed. TASKDIALOGCONFIG is packed to 1 byte: title at 28, main instruction at 44, content at 52 (strings, or
// resource ids below 0x10000).
static void log_dialog_text(const char *what, const uint8_t *field) {
    const void *p;
    memcpy(&p, field, sizeof p);
    if ((uintptr_t)p < 0x10000) return;
    char *t = vx_utf8_dup(p);
    vx_log("task dialog %s: %s", what, t);
    free(t);
}
static WINAPI LONG TaskDialogIndirect(const uint8_t *config, int *button, int *radio, BOOL *checked) {
    log_dialog_text("title", config + 28);
    log_dialog_text("instruction", config + 44);
    log_dialog_text("content", config + 52);
    if (button) *button = 1;                           // IDOK
    if (radio) *radio = 0;
    if (checked) *checked = 0;
    return 0;
}
static const vx_export comctl32[] = { VX_EXPORT(InitCommonControlsEx), VX_EXPORT(InitCommonControls), { "#17", (void *)InitCommonControls },
                                      VX_EXPORT(TaskDialogIndirect) };
static const vx_export psapi[] = {
    VX_EXPORT(GetModuleFileNameExW), VX_EXPORT(GetModuleFileNameExA), VX_EXPORT(EnumProcessModules), VX_EXPORT(EnumProcessModulesEx),
    VX_EXPORT(GetModuleBaseNameW), VX_EXPORT(GetModuleBaseNameA),
};
// (certificates, stores, chains and their policies: crypt32.c)
static const vx_export crypt32[] = {
    VX_EXPORT(CryptBinaryToStringA), VX_EXPORT(CryptStringToBinaryA), { "CryptEncodeObjectEx", (void *)crypt_refuses },
    { "CryptDecodeObjectEx", (void *)crypt_refuses }, { "CertStrToNameA", (void *)crypt_refuses }, { "CertStrToNameW", (void *)crypt_refuses },
    { "CertSetCertificateContextProperty", (void *)crypt_refuses }, { "CryptHashCertificate2", (void *)crypt_refuses },
    { "CertCreateSelfSignCertificate", (void *)crypt_refuses_ptr },
};
static const vx_export bcrypt[] = {
    VX_EXPORT(BCryptGenRandom), VX_EXPORT(BCryptCloseAlgorithmProvider), { "BCryptOpenAlgorithmProvider", (void *)bcrypt_unsupported },
    { "BCryptCreateHash", (void *)bcrypt_unsupported }, { "BCryptHashData", (void *)bcrypt_unsupported }, { "BCryptFinishHash", (void *)bcrypt_unsupported },
    { "BCryptDestroyHash", (void *)bcrypt_unsupported }, { "BCryptGetProperty", (void *)bcrypt_unsupported },
};
static const vx_export bcryptprimitives[] = { VX_EXPORT(ProcessPrng) };
static const vx_export ncrypt[] = {
    { "NCryptOpenStorageProvider", (void *)ncrypt_unsupported }, { "NCryptCreatePersistedKey", (void *)ncrypt_unsupported },
    { "NCryptDeleteKey", (void *)ncrypt_unsupported }, { "NCryptExportKey", (void *)ncrypt_unsupported }, { "NCryptFinalizeKey", (void *)ncrypt_unsupported },
    { "NCryptFreeObject", (void *)ncrypt_unsupported }, { "NCryptImportKey", (void *)ncrypt_unsupported }, { "NCryptSetProperty", (void *)ncrypt_unsupported },
    { "NCryptOpenKey", (void *)ncrypt_unsupported },
};
static const vx_export secur32[] = {
    { "AcquireCredentialsHandleA", (void *)sspi_unsupported }, { "AcquireCredentialsHandleW", (void *)sspi_unsupported },
    { "InitializeSecurityContextA", (void *)sspi_unsupported }, { "InitializeSecurityContextW", (void *)sspi_unsupported },
    { "AcceptSecurityContext", (void *)sspi_unsupported }, { "ApplyControlToken", (void *)sspi_unsupported },
    { "DecryptMessage", (void *)sspi_unsupported }, { "EncryptMessage", (void *)sspi_unsupported },
    { "QueryContextAttributesA", (void *)sspi_unsupported }, { "QueryContextAttributesW", (void *)sspi_unsupported },
    { "SetContextAttributesA", (void *)sspi_unsupported }, { "DeleteSecurityContext", (void *)sspi_ok },
    { "FreeContextBuffer", (void *)sspi_ok }, { "FreeCredentialsHandle", (void *)sspi_ok },
};
static const vx_export wininet[] = {
    { "InternetConnectW", (void *)inet_none }, { "HttpOpenRequestW", (void *)inet_none }, { "HttpSendRequestW", (void *)inet_fails },
    { "HttpQueryInfoW", (void *)inet_fails }, { "HttpAddRequestHeadersW", (void *)inet_fails }, { "InternetCrackUrlW", (void *)inet_fails },
    { "InternetQueryDataAvailable", (void *)inet_fails }, { "InternetSetOptionW", (void *)inet_ok }, { "InternetSetOptionA", (void *)inet_ok },
    { "InternetOpenUrlW", (void *)inet_none },
};
static const vx_export avifil32[] = {
    VX_EXPORT(AVIFileInit), { "AVIFileExit", (void *)AVIFileInit }, { "AVIFileOpenA", (void *)avi_fails }, { "AVIFileOpenW", (void *)avi_fails },
    { "AVIFileCreateStreamA", (void *)avi_fails }, { "AVIFileGetStream", (void *)avi_fails }, { "AVIFileInfoA", (void *)avi_fails },
    { "AVIMakeCompressedStream", (void *)avi_fails }, { "AVISaveOptions", (void *)avi_zero }, { "AVIStreamSetFormat", (void *)avi_fails },
    { "AVIStreamWrite", (void *)avi_fails }, { "AVIStreamStart", (void *)avi_zero }, { "AVIStreamLength", (void *)avi_zero },
    { "AVIStreamGetFrameOpen", (void *)avi_null }, { "AVIStreamGetFrame", (void *)avi_null }, { "AVIStreamGetFrameClose", (void *)avi_zero },
    { "AVIStreamRelease", (void *)avi_zero }, { "AVIFileRelease", (void *)avi_zero },
};
void vx_client_init(void) {
    vx_register_module("kernel32.dll", k32, sizeof k32 / sizeof k32[0]);
    vx_register_module("user32.dll", user32, sizeof user32 / sizeof user32[0]);
    vx_register_module("gdi32.dll", gdi32, sizeof gdi32 / sizeof gdi32[0]);
    vx_register_module("shell32.dll", shell32, sizeof shell32 / sizeof shell32[0]);
    vx_register_module("comctl32.dll", comctl32, sizeof comctl32 / sizeof comctl32[0]);
    vx_register_module("psapi.dll", psapi, sizeof psapi / sizeof psapi[0]);
    vx_register_module("crypt32.dll", crypt32, sizeof crypt32 / sizeof crypt32[0]);
    vx_register_module("bcrypt.dll", bcrypt, sizeof bcrypt / sizeof bcrypt[0]);
    vx_register_module("advapi32.dll", advapi32, sizeof advapi32 / sizeof advapi32[0]);
    vx_register_module("imm32.dll", imm32, sizeof imm32 / sizeof imm32[0]);
    vx_register_module("winmm.dll", winmm, sizeof winmm / sizeof winmm[0]);
    vx_register_module("ole32.dll", ole32, sizeof ole32 / sizeof ole32[0]);
    vx_register_module("setupapi.dll", setupapi, sizeof setupapi / sizeof setupapi[0]);
    vx_register_module("hid.dll", hid, sizeof hid / sizeof hid[0]);
    vx_register_module("iphlpapi.dll", iphlpapi, sizeof iphlpapi / sizeof iphlpapi[0]);
    vx_register_module("bcryptprimitives.dll", bcryptprimitives, sizeof bcryptprimitives / sizeof bcryptprimitives[0]);
    vx_register_module("ncrypt.dll", ncrypt, sizeof ncrypt / sizeof ncrypt[0]);
    vx_register_module("secur32.dll", secur32, sizeof secur32 / sizeof secur32[0]);
    vx_register_module("wininet.dll", wininet, sizeof wininet / sizeof wininet[0]);
    vx_register_module("usp10.dll", usp10, sizeof usp10 / sizeof usp10[0]);
    vx_register_module("shlwapi.dll", shlwapi, sizeof shlwapi / sizeof shlwapi[0]);
    vx_register_module("avifil32.dll", avifil32, sizeof avifil32 / sizeof avifil32[0]);
    vx_register_module("msimg32.dll", msimg32, sizeof msimg32 / sizeof msimg32[0]);
    vx_register_module("comdlg32.dll", comdlg32, sizeof comdlg32 / sizeof comdlg32[0]);
    if (getenv("VXRT_OPENGL") && !strcmp(getenv("VXRT_OPENGL"), "0")) vx_register_module("opengl32.dll", opengl32, sizeof opengl32 / sizeof opengl32[0]);   // (stubs instead of opengl.c)
}

#ifdef __aarch64__
#include "thunks/client.c.inc"
#endif
