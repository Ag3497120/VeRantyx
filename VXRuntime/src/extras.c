// Smaller functions more programs need (first: Google Earth VR): disk space, file locks, UI languages, a few C runtime
// functions, dialog and menu calls that may fail, MMCSS (avrt.dll), security calls that succeed, a few WinSock
// lookups by ordinal, and GDI+ enough to start up (images are not decoded: loading one fails, as a missing file would).
#include "vx.h"
#include <errno.h>
#include <malloc/malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // (MD5, for programs that ask for it; nothing secure here)
#include <CommonCrypto/CommonDigest.h>

// ---- kernel32 -------------------------------------------------------------------------------------------------------
static WINAPI BOOL GetDiskFreeSpaceW(const WCHAR *root, DWORD *sectors_per_cluster, DWORD *bytes_per_sector, DWORD *free_clusters, DWORD *clusters) {
    (void)root;
    struct statfs fs;
    if (statfs(vx_root(), &fs)) { vx_set_last_error(3); return 0; }
    uint64_t cluster = 4096, free = (uint64_t)fs.f_bavail * fs.f_bsize / cluster, total = (uint64_t)fs.f_blocks * fs.f_bsize / cluster;
    if (sectors_per_cluster) *sectors_per_cluster = 8;
    if (bytes_per_sector) *bytes_per_sector = 512;
    if (free_clusters) *free_clusters = (DWORD)(free > 0xffffffffu ? 0xffffffffu : free);
    if (clusters) *clusters = (DWORD)(total > 0xffffffffu ? 0xffffffffu : total);
    return 1;
}
// File locks: one program per file here; locking always succeeds.
static WINAPI BOOL LockFile(HANDLE f, DWORD a, DWORD b, DWORD c, DWORD d) { (void)f; (void)a; (void)b; (void)c; (void)d; return 1; }
static WINAPI BOOL UnlockFile(HANDLE f, DWORD a, DWORD b, DWORD c, DWORD d) { (void)f; (void)a; (void)b; (void)c; (void)d; return 1; }
static WINAPI BOOL LockFileEx(HANDLE f, DWORD flags, DWORD r, DWORD lo, DWORD hi, void *ov) { (void)f; (void)flags; (void)r; (void)lo; (void)hi; (void)ov; return 1; }
static WINAPI BOOL UnlockFileEx(HANDLE f, DWORD r, DWORD lo, DWORD hi, void *ov) { (void)f; (void)r; (void)lo; (void)hi; (void)ov; return 1; }
static WINAPI size_t HeapCompact(HANDLE heap, DWORD flags) { (void)heap; (void)flags; return 1u << 20; }
// The preferred UI languages: "en-US" (a double-NUL-terminated list).
static WINAPI BOOL GetUserPreferredUILanguages(DWORD flags, DWORD *count, WCHAR *buf, DWORD *len) {
    (void)flags;
    static const WCHAR list[] = { 'e', 'n', '-', 'U', 'S', 0, 0 };
    if (count) *count = 1;
    if (!len) { vx_set_last_error(87); return 0; }
    if (buf && *len < 7) { *len = 7; vx_set_last_error(122); return 0; }
    if (buf) memcpy(buf, list, sizeof list);
    *len = 7;
    return 1;
}
static WINAPI BOOL GetThreadPreferredUILanguages(DWORD flags, DWORD *count, WCHAR *buf, DWORD *len) { return GetUserPreferredUILanguages(flags, count, buf, len); }
static WINAPI int GetGeoInfoA(int32_t geo, DWORD type, char *data, int size, uint16_t lang) {
    (void)geo; (void)type; (void)data; (void)size; (void)lang;
    vx_set_last_error(87);
    return 0;
}

// ---- C runtime --------------------------------------------------------------------------------------------------------
typedef struct { void (*fn)(void *); void *arg; } thread_start;
static WINAPI DWORD start_thread(void *p) {
    thread_start s = *(thread_start *)p;
    free(p);
    ((void (WINAPI *)(void *))s.fn)(s.arg);
    return 0;
}
static WINAPI uintptr_t ms_beginthread(void (*fn)(void *), unsigned stack, void *arg) {
    typedef uintptr_t (WINAPI *beginthreadex_fn)(void *, unsigned, DWORD (WINAPI *)(void *), void *, unsigned, unsigned *);
    beginthreadex_fn begin = (beginthreadex_fn)vx_find_export("ucrtbase.dll", "_beginthreadex");
    thread_start *s = malloc(sizeof *s);
    s->fn = fn; s->arg = arg;
    uintptr_t h = begin ? begin(NULL, stack, start_thread, s, 0, NULL) : 0;
    if (!h) { free(s); return (uintptr_t)-1; }
    return h;
}
static WINAPI void ms_endthread(void) {
    void (WINAPI *exit_thread)(DWORD) = (void (WINAPI *)(DWORD))vx_find_export("kernel32.dll", "ExitThread");
    if (exit_thread) exit_thread(0);
}
static WINAPI int ms_resetstkoflw(void) { return 1; }
static WINAPI float ms_scalblnf(float x, int32_t n) { return scalbnf(x, n); }   // (long is 32-bit on Windows)
static WINAPI int64_t ms_filelengthi64(int fd) { struct stat st; if (fstat(fd, &st)) { errno = EBADF; return -1; } return st.st_size; }
// Positional arguments (%1$d) are not supported: the plain formatter handles the rest the same.
static WINAPI int ms_vsprintf_p(uint64_t options, char *buf, size_t count, const char *fmt, void *locale, void *ap) {
    typedef int (WINAPI *vsprintf_fn)(uint64_t, char *, size_t, const char *, void *, void *);
    vsprintf_fn f = (vsprintf_fn)vx_find_export("ucrtbase.dll", "__stdio_common_vsprintf");
    return f ? f(options, buf, count, fmt, locale, ap) : -1;
}
static WINAPI void *ms_freopen(const char *path, const char *mode, void *stream) { (void)path; (void)mode; (void)stream; errno = EACCES; return NULL; }
static WINAPI int ms_initialize_lconv(void) { return 0; }
static WINAPI size_t ms_msize(void *p) { return p ? malloc_size(p) : (size_t)-1; }
static WINAPI void *ms_expand(void *p, size_t n) { return p && malloc_size(p) >= n ? p : NULL; }
static WINAPI int ms_itoa_s(int value, char *buf, size_t size, int radix) {
    if (!buf || !size || radix < 2 || radix > 36) return EINVAL;
    char tmp[40]; int i = 0; int neg = value < 0 && radix == 10;
    unsigned v = neg ? (unsigned)-(int64_t)value : (unsigned)value;
    do { int d = (int)(v % (unsigned)radix); tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= (unsigned)radix; } while (v);
    if (neg) tmp[i++] = '-';
    if ((size_t)i + 1 > size) { buf[0] = 0; return ERANGE; }
    for (int k = 0; k < i; k++) buf[k] = tmp[i - 1 - k];
    buf[i] = 0;
    return 0;
}

// ---- user32, winmm, ole32 ---------------------------------------------------------------------------------------------
static WINAPI HANDLE CreateDialogParamW(HANDLE inst, const WCHAR *name, HANDLE parent, void *proc, intptr_t param) {
    (void)inst; (void)name; (void)parent; (void)proc; (void)param;
    vx_set_last_error(1813);                      // ERROR_RESOURCE_TYPE_NOT_FOUND: dialogs from resources are not made yet
    return NULL;
}
static WINAPI BOOL SetDlgItemTextW(HANDLE dlg, int id, const WCHAR *text) { (void)dlg; (void)id; (void)text; return 1; }
static WINAPI BOOL RemoveMenu(HANDLE menu, uint32_t pos, uint32_t flags) { (void)menu; (void)pos; (void)flags; return 1; }
static WINAPI int LoadStringW(HANDLE inst, uint32_t id, WCHAR *buf, int max) {
    (void)inst; (void)id;
    if (buf && max > 0) buf[0] = 0;
    vx_set_last_error(1814);                      // ERROR_RESOURCE_NAME_NOT_FOUND (string tables are not read yet)
    return 0;
}
static WINAPI uint32_t waveInStop(HANDLE wave) { (void)wave; return 0; }
static WINAPI int32_t CreateStreamOnHGlobal(HANDLE global, BOOL release, void **stream) {
    (void)global; (void)release;
    if (stream) *stream = NULL;
    return (int32_t)0x80004001;                   // E_NOTIMPL
}

// ---- avrt, advapi32 -----------------------------------------------------------------------------------------------------
static WINAPI HANDLE AvSetMmThreadCharacteristicsW(const WCHAR *task, DWORD *index) { (void)task; if (index) *index = 1; return (HANDLE)1; }
static WINAPI BOOL AvRevertMmThreadCharacteristics(HANDLE h) { (void)h; return 1; }
static WINAPI DWORD SetEntriesInAclW(uint32_t n, void *entries, void *old, void **acl) { (void)n; (void)entries; (void)old; if (acl) *acl = NULL; return 0; }
static WINAPI BOOL SetFileSecurityW(const WCHAR *file, DWORD info, void *sd) { (void)file; (void)info; (void)sd; return 1; }

// ---- ws2_32 by ordinal: lookups that find nothing -----------------------------------------------------------------------
static WINAPI void *ws_gethostbyaddr(const char *addr, int len, int type) { (void)addr; (void)len; (void)type; vx_set_last_error(11004); return NULL; }
static WINAPI void *ws_getservbyname(const char *name, const char *proto) { (void)name; (void)proto; vx_set_last_error(11004); return NULL; }
static WINAPI void *ws_getservbyport(int port, const char *proto) { (void)port; (void)proto; vx_set_last_error(11004); return NULL; }

// ---- gdiplus ----------------------------------------------------------------------------------------------------------
enum { Ok = 0, GenericError = 1, InvalidParameter = 2, NotImplemented = 6 };
static WINAPI int GdiplusStartup(uintptr_t *token, const void *input, void *output) { (void)input; if (output) memset(output, 0, 16); if (token) *token = 1; return Ok; }
static WINAPI void GdiplusShutdown(uintptr_t token) { (void)token; }
static WINAPI void *GdipAlloc(size_t n) { return malloc(n); }
static WINAPI void GdipFree(void *p) { free(p); }
static WINAPI int GdipLoadImageFromStream(void *stream, void **image) { (void)stream; if (image) *image = NULL; return NotImplemented; }
static WINAPI int GdipCloneImage(void *image, void **clone) { (void)image; if (clone) *clone = NULL; return InvalidParameter; }
static WINAPI int GdipDisposeImage(void *image) { (void)image; return Ok; }
static WINAPI int GdipGetImageWidth(void *image, uint32_t *w) { (void)image; if (w) *w = 0; return InvalidParameter; }
static WINAPI int GdipGetImageHeight(void *image, uint32_t *h) { (void)image; if (h) *h = 0; return InvalidParameter; }
static WINAPI int GdipCreateFromHDC(HANDLE dc, void **graphics) { (void)dc; if (graphics) *graphics = (void *)1; return Ok; }
static WINAPI int GdipDeleteGraphics(void *graphics) { (void)graphics; return Ok; }
static WINAPI int GdipDrawImageRectI(void *g, void *image, int x, int y, int w, int h) { (void)g; (void)image; (void)x; (void)y; (void)w; (void)h; return InvalidParameter; }

// ---- more of the same (first: The Lab and Bigscreen, Unity) -----------------------------------------------------------
typedef struct { int left, top, right, bottom; } RECT_;
static WINAPI BOOL CopyRect(RECT_ *dst, const RECT_ *src) { if (!dst || !src) return 0; *dst = *src; return 1; }
static WINAPI uint32_t GetCaretBlinkTime(void) { return 530; }                     // Windows' default
static WINAPI BOOL DragDetect(HANDLE w, int64_t pt) { (void)w; (void)pt; return 0; }
static WINAPI intptr_t SendDlgItemMessageW(HANDLE dlg, int id, uint32_t msg, uintptr_t wp, intptr_t lp) { (void)dlg; (void)id; (void)msg; (void)wp; (void)lp; return 0; }
// The US keyboard layout ("00000409"), and key names as Windows gives them for it.
static WINAPI BOOL GetKeyboardLayoutNameW(WCHAR *name) { if (!name) return 0; vx_return_string_w("00000409", name, 9); return 1; }
static WINAPI int GetKeyNameTextW(int32_t lparam, WCHAR *buf, int cap) {
    static const char *const names[0x60] = {
        [0x01] = "Esc", [0x0e] = "Backspace", [0x0f] = "Tab", [0x1c] = "Enter", [0x1d] = "Ctrl", [0x2a] = "Shift", [0x36] = "Right Shift",
        [0x38] = "Alt", [0x39] = "Space", [0x3a] = "Caps Lock", [0x3b] = "F1", [0x3c] = "F2", [0x3d] = "F3", [0x3e] = "F4", [0x3f] = "F5",
        [0x40] = "F6", [0x41] = "F7", [0x42] = "F8", [0x43] = "F9", [0x44] = "F10", [0x57] = "F11", [0x58] = "F12",
    };
    static const char row[] = "\0\0" "1234567890-=\0\0" "QWERTYUIOP[]\0\0" "ASDFGHJKL;'`\0\\" "ZXCVBNM,./";
    int scan = (lparam >> 16) & 0xff;
    char one[2] = { 0, 0 };
    const char *name = scan < 0x60 ? names[scan] : NULL;
    if (!name && scan < (int)sizeof row - 1 && row[scan]) { one[0] = row[scan]; name = one; }
    if (!name || !buf || cap <= 0) return 0;
    return (int)vx_return_string_w(name, buf, (DWORD)cap);
}
// System times: idle, kernel, user (100 ns units); a Mac has no cheap equivalent for a program to compare, so all
// zero (programs use them to show CPU load).
static WINAPI BOOL GetSystemTimes(uint64_t *idle, uint64_t *kernel, uint64_t *user) { if (idle) *idle = 0; if (kernel) *kernel = 0; if (user) *user = 0; return 1; }
// Process creation attribute lists: kept as opaque blocks (CreateProcess here ignores them).
static WINAPI BOOL InitializeProcThreadAttributeList(void *list, DWORD count, DWORD flags, size_t *size) {
    (void)flags;
    size_t need = 48 + (size_t)count * 24;
    if (!list) { if (size) *size = need; vx_set_last_error(122); return 0; }
    if (size && *size < need) { *size = need; vx_set_last_error(122); return 0; }
    memset(list, 0, need);
    return 1;
}
static WINAPI BOOL UpdateProcThreadAttribute(void *list, DWORD flags, uintptr_t attr, void *value, size_t size, void *prev, size_t *ret) {
    (void)list; (void)flags; (void)attr; (void)value; (void)size; (void)prev; (void)ret; return 1;
}
static WINAPI void DeleteProcThreadAttributeList(void *list) { (void)list; }
// Hashes (CryptCreateHash on any provider): MD5, SHA-1, SHA-256 through CommonCrypto. Keys and encryption are not
// available (NTE_BAD_KEY): programs fall back or skip what needs them.
typedef struct { unsigned alg; union { CC_MD5_CTX md5; CC_SHA1_CTX sha1; CC_SHA256_CTX sha256; } c; } crypt_hash;
#define CALG_MD5 0x8003u
#define CALG_SHA1 0x8004u
#define CALG_SHA_256 0x800cu
static WINAPI BOOL CryptCreateHash(uintptr_t prov, unsigned alg, uintptr_t key, DWORD flags, crypt_hash **out) {
    (void)prov; (void)key; (void)flags;
    if (!out || (alg != CALG_MD5 && alg != CALG_SHA1 && alg != CALG_SHA_256)) { vx_set_last_error(0x80090008u); return 0; }   // NTE_BAD_ALGID
    crypt_hash *h = calloc(1, sizeof *h);
    h->alg = alg;
    if (alg == CALG_MD5) CC_MD5_Init(&h->c.md5); else if (alg == CALG_SHA1) CC_SHA1_Init(&h->c.sha1); else CC_SHA256_Init(&h->c.sha256);
    *out = h;
    return 1;
}
static WINAPI BOOL CryptHashData(crypt_hash *h, const uint8_t *data, DWORD len, DWORD flags) {
    (void)flags;
    if (!h) { vx_set_last_error(6); return 0; }
    if (h->alg == CALG_MD5) CC_MD5_Update(&h->c.md5, data, len); else if (h->alg == CALG_SHA1) CC_SHA1_Update(&h->c.sha1, data, len); else CC_SHA256_Update(&h->c.sha256, data, len);
    return 1;
}
static WINAPI BOOL CryptGetHashParam(crypt_hash *h, DWORD param, uint8_t *out, DWORD *len, DWORD flags) {
    (void)flags;
    if (!h || !len) { vx_set_last_error(87); return 0; }
    DWORD size = h->alg == CALG_MD5 ? 16 : h->alg == CALG_SHA1 ? 20 : 32;
    if (param == 1 || param == 4) {                                    // HP_ALGID, HP_HASHSIZE
        if (!out) { *len = 4; return 1; }
        if (*len < 4) { *len = 4; vx_set_last_error(234); return 0; }
        *(DWORD *)out = param == 1 ? h->alg : size; *len = 4;
        return 1;
    }
    if (param != 2) { vx_set_last_error(0x80090009u); return 0; }      // NTE_BAD_FLAGS; HP_HASHVAL below
    if (!out) { *len = size; return 1; }
    if (*len < size) { *len = size; vx_set_last_error(234); return 0; }   // ERROR_MORE_DATA
    if (h->alg == CALG_MD5) CC_MD5_Final(out, &h->c.md5); else if (h->alg == CALG_SHA1) CC_SHA1_Final(out, &h->c.sha1); else CC_SHA256_Final(out, &h->c.sha256);
    *len = size;
    return 1;
}
static WINAPI BOOL CryptDestroyHash(crypt_hash *h) { free(h); return 1; }
static WINAPI BOOL CryptImportKey(uintptr_t prov, const void *data, DWORD len, uintptr_t pub, DWORD flags, uintptr_t *key) {
    (void)prov; (void)data; (void)len; (void)pub; (void)flags; if (key) *key = 0; vx_set_last_error(0x80090003u); return 0;   // NTE_BAD_KEY
}
static WINAPI BOOL CryptEncrypt(uintptr_t key, uintptr_t hash, BOOL final, DWORD flags, void *data, DWORD *len, DWORD cap) {
    (void)key; (void)hash; (void)final; (void)flags; (void)data; (void)len; (void)cap; vx_set_last_error(0x80090003u); return 0;
}
static WINAPI BOOL CryptDestroyKey(uintptr_t key) { (void)key; return 1; }
// Paths: "." and ".." removed, as PathCanonicalize does.
static WINAPI BOOL PathCanonicalizeW(WCHAR *out, const WCHAR *in) {
    if (!out || !in) return 0;
    WCHAR tmp[1024]; size_t n = 0;
    for (const WCHAR *p = in; *p && n < 1023; ) {
        if (p[0] == '.' && (p[1] == '\\' || !p[1]) && (n == 0 || tmp[n - 1] == '\\')) { p += p[1] ? 2 : 1; continue; }
        if (p[0] == '.' && p[1] == '.' && (p[2] == '\\' || !p[2]) && n > 0 && tmp[n - 1] == '\\') {
            size_t back = n - 1;
            while (back > 0 && tmp[back - 1] != '\\') back--;
            if (back > 0 && !(back >= 2 && tmp[back - 2] == ':')) n = back; else if (back > 0) n = back;
            p += p[2] ? 3 : 2; continue;
        }
        tmp[n++] = *p++;
    }
    if (n > 1 && tmp[n - 1] == '\\' && !(n == 3 && tmp[1] == ':')) n--;
    tmp[n] = 0;
    memcpy(out, tmp, (n + 1) * sizeof(WCHAR));
    return 1;
}
static WINAPI LONG SHDeleteKeyW(HANDLE key, const WCHAR *sub) { (void)key; (void)sub; return 2; }   // ERROR_FILE_NOT_FOUND
static WINAPI int SHFileOperationW(void *op) { (void)op; return 0x7c; }                          // DE_INVALIDFILES: nothing done
static WINAPI uint32_t waveOutGetPosition(HANDLE wo, void *time, uint32_t size) { (void)wo; (void)time; (void)size; return 8; }   // MMSYSERR_NOTSUPPORTED
static WINAPI int WSASendDisconnect(uintptr_t s, void *data) { (void)s; (void)data; return 0; }
#define HIDP_STATUS_NOT_IMPLEMENTED ((LONG)0xC0110020)
static WINAPI LONG HidP_SetUsageValue(int type, uint16_t page, uint16_t coll, uint16_t usage, uint32_t value, void *pre, char *report, uint32_t len) {
    (void)type; (void)page; (void)coll; (void)usage; (void)value; (void)pre; (void)report; (void)len; return HIDP_STATUS_NOT_IMPLEMENTED;
}
static WINAPI LONG HidP_SetUsages(int type, uint16_t page, uint16_t coll, uint16_t *list, uint32_t *n, void *pre, char *report, uint32_t len) {
    (void)type; (void)page; (void)coll; (void)list; (void)n; (void)pre; (void)report; (void)len; return HIDP_STATUS_NOT_IMPLEMENTED;
}

static const vx_export kernel32_more[] = {
    VX_EXPORT(GetSystemTimes), VX_EXPORT(InitializeProcThreadAttributeList),
    VX_EXPORT(UpdateProcThreadAttribute), VX_EXPORT(DeleteProcThreadAttributeList),
};
static const vx_export user32_more[] = {
    VX_EXPORT(CopyRect), VX_EXPORT(GetCaretBlinkTime), VX_EXPORT(DragDetect), VX_EXPORT(SendDlgItemMessageW),
    VX_EXPORT(GetKeyboardLayoutNameW), VX_EXPORT(GetKeyNameTextW),
};
static const vx_export advapi32_more[] = {
    VX_EXPORT(CryptCreateHash), VX_EXPORT(CryptHashData), VX_EXPORT(CryptGetHashParam), VX_EXPORT(CryptDestroyHash),
    VX_EXPORT(CryptImportKey), VX_EXPORT(CryptEncrypt), VX_EXPORT(CryptDestroyKey),
};
static const vx_export shlwapi_more[] = { VX_EXPORT(PathCanonicalizeW), VX_EXPORT(SHDeleteKeyW) };
static const vx_export shell32_more[] = { VX_EXPORT(SHFileOperationW) };
static const vx_export winmm_more[] = { VX_EXPORT(waveOutGetPosition) };
static const vx_export ws2_32_more[] = { VX_EXPORT(WSASendDisconnect) };
static const vx_export hid_more[] = { VX_EXPORT(HidP_SetUsageValue), VX_EXPORT(HidP_SetUsages) };

static const vx_export kernel32[] = {
    VX_EXPORT(GetDiskFreeSpaceW), VX_EXPORT(LockFile), VX_EXPORT(UnlockFile), VX_EXPORT(LockFileEx), VX_EXPORT(UnlockFileEx),
    VX_EXPORT(HeapCompact), VX_EXPORT(GetUserPreferredUILanguages), VX_EXPORT(GetThreadPreferredUILanguages), VX_EXPORT(GetGeoInfoA),
};
static const vx_export ucrtbase[] = {
    { "_beginthread", (void *)ms_beginthread }, { "_endthread", (void *)ms_endthread }, { "_resetstkoflw", (void *)ms_resetstkoflw },
    { "scalblnf", (void *)ms_scalblnf }, { "_filelengthi64", (void *)ms_filelengthi64 }, { "__stdio_common_vsprintf_p", (void *)ms_vsprintf_p },
    { "freopen", (void *)ms_freopen }, { "__initialize_lconv_for_unsigned_char", (void *)ms_initialize_lconv },
    { "_msize", (void *)ms_msize }, { "_expand", (void *)ms_expand }, { "_itoa_s", (void *)ms_itoa_s },
};
static const vx_export user32[] = { VX_EXPORT(CreateDialogParamW), VX_EXPORT(SetDlgItemTextW), VX_EXPORT(RemoveMenu), VX_EXPORT(LoadStringW) };
static const vx_export winmm[] = { VX_EXPORT(waveInStop) };
static const vx_export ole32[] = { VX_EXPORT(CreateStreamOnHGlobal) };
static const vx_export avrt[] = { VX_EXPORT(AvSetMmThreadCharacteristicsW), VX_EXPORT(AvRevertMmThreadCharacteristics) };
static const vx_export advapi32[] = { VX_EXPORT(SetEntriesInAclW), VX_EXPORT(SetFileSecurityW) };
static const vx_export ws2_32[] = {
    { "gethostbyaddr", (void *)ws_gethostbyaddr }, { "#51", (void *)ws_gethostbyaddr }, { "getservbyname", (void *)ws_getservbyname },
    { "#55", (void *)ws_getservbyname }, { "getservbyport", (void *)ws_getservbyport }, { "#56", (void *)ws_getservbyport },
};
static const vx_export gdiplus[] = {
    VX_EXPORT(GdiplusStartup), VX_EXPORT(GdiplusShutdown), VX_EXPORT(GdipAlloc), VX_EXPORT(GdipFree), VX_EXPORT(GdipLoadImageFromStream),
    VX_EXPORT(GdipCloneImage), VX_EXPORT(GdipDisposeImage), VX_EXPORT(GdipGetImageWidth), VX_EXPORT(GdipGetImageHeight),
    VX_EXPORT(GdipCreateFromHDC), VX_EXPORT(GdipDeleteGraphics), VX_EXPORT(GdipDrawImageRectI),
};
void vx_extras_init(void) {
    vx_register_module("kernel32.dll", kernel32, sizeof kernel32 / sizeof kernel32[0]);
    vx_register_module("ucrtbase.dll", ucrtbase, sizeof ucrtbase / sizeof ucrtbase[0]);
    vx_register_module("user32.dll", user32, sizeof user32 / sizeof user32[0]);
    vx_register_module("winmm.dll", winmm, sizeof winmm / sizeof winmm[0]);
    vx_register_module("ole32.dll", ole32, sizeof ole32 / sizeof ole32[0]);
    vx_register_module("avrt.dll", avrt, sizeof avrt / sizeof avrt[0]);
    vx_register_module("advapi32.dll", advapi32, sizeof advapi32 / sizeof advapi32[0]);
    vx_register_module("ws2_32.dll", ws2_32, sizeof ws2_32 / sizeof ws2_32[0]);
    vx_register_module("gdiplus.dll", gdiplus, sizeof gdiplus / sizeof gdiplus[0]);
#define MORE(dll, t) vx_register_module(dll, t, sizeof t / sizeof t[0])
    MORE("kernel32.dll", kernel32_more); MORE("kernelbase.dll", kernel32_more); MORE("user32.dll", user32_more);
    MORE("advapi32.dll", advapi32_more); MORE("shlwapi.dll", shlwapi_more); MORE("shell32.dll", shell32_more);
    MORE("winmm.dll", winmm_more); MORE("ws2_32.dll", ws2_32_more); MORE("hid.dll", hid_more);
#undef MORE
}

#ifdef __aarch64__
#include "thunks/extras.c.inc"
#endif
