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
#include <wchar.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if_dl.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <unistd.h>
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

// ---- what Elite Dangerous (and the Epic Online Services SDK it carries) import and never needs to work for the
// game to run: string normalization as a copy, no hot keys, no routes, no crash dumps
static WINAPI int NormalizeString(int form, const WCHAR *src, int src_len, WCHAR *dst, int dst_len) {
    (void)form;
    int n = src_len < 0 ? (int)wcslen((const wchar_t *)src) + 1 : src_len;         // (ASCII input is already normalized)
    if (!dst || !dst_len) return n;
    if (n > dst_len) { vx_set_last_error(122); return -n; }                       // ERROR_INSUFFICIENT_BUFFER
    memcpy(dst, src, (size_t)n * sizeof *dst);
    return n;
}
static WINAPI int IdnToAscii(DWORD flags, const WCHAR *src, int src_len, WCHAR *dst, int dst_len) {
    (void)flags;
    int n = src_len < 0 ? (int)wcslen((const wchar_t *)src) + 1 : src_len;
    if (!dst || !dst_len) return n;
    if (n > dst_len) { vx_set_last_error(122); return 0; }
    memcpy(dst, src, (size_t)n * sizeof *dst);
    return n;
}
static WINAPI DWORD SearchPathW(const WCHAR *path, const WCHAR *file, const WCHAR *extension, DWORD size, WCHAR *buffer, WCHAR **file_part) {
    // the file as given when it exists (the launcher's own folder is the working directory); no search of PATH
    (void)path;
    DWORD (WINAPI *attributes)(const WCHAR *) = (DWORD (WINAPI *)(const WCHAR *))vx_find_export("kernel32.dll", "GetFileAttributesW");
    DWORD (WINAPI *full)(const WCHAR *, DWORD, WCHAR *, WCHAR **) = (DWORD (WINAPI *)(const WCHAR *, DWORD, WCHAR *, WCHAR **))vx_find_export("kernel32.dll", "GetFullPathNameW");
    WCHAR candidate[1024];
    size_t n = wcslen((const wchar_t *)file);
    if (n + (extension ? wcslen((const wchar_t *)extension) : 0) + 1 > 1024) { vx_set_last_error(2); return 0; }
    memcpy(candidate, file, (n + 1) * sizeof *candidate);
    if (extension && !wcschr((const wchar_t *)file, '.')) memcpy(candidate + n, extension, (wcslen((const wchar_t *)extension) + 1) * sizeof *candidate);
    if (!attributes || !full || attributes(candidate) == 0xffffffffu) { vx_set_last_error(2); return 0; }     // ERROR_FILE_NOT_FOUND
    return full(candidate, size, buffer, file_part);
}
static WINAPI BOOL VirtualUnlock(void *address, size_t size) { (void)address; (void)size; return 1; }
static WINAPI DWORD GetFinalPathNameByHandleW(HANDLE h, WCHAR *buffer, DWORD size, DWORD flags) {
    (void)h; (void)buffer; (void)size; (void)flags;
    vx_set_last_error(6);                                                           // ERROR_INVALID_HANDLE: not provided
    return 0;
}
static WINAPI int ToAsciiEx(uint32_t vk, uint32_t scan, const uint8_t *state, uint16_t *out, uint32_t flags, HANDLE layout) {
    (void)vk; (void)scan; (void)state; (void)out; (void)flags; (void)layout; return 0;
}
static WINAPI BOOL UnloadKeyboardLayout(HANDLE layout) { (void)layout; return 1; }
static WINAPI BOOL RegisterHotKey(HANDLE hwnd, int id, uint32_t modifiers, uint32_t vk) { (void)hwnd; (void)id; (void)modifiers; (void)vk; vx_set_last_error(1409); return 0; }
static WINAPI BOOL UnregisterHotKey(HANDLE hwnd, int id) { (void)hwnd; (void)id; return 0; }
static WINAPI int ExtEscape(HANDLE dc, int escape, int in_size, const char *in, int out_size, char *out) {
    (void)dc; (void)escape; (void)in_size; (void)in; (void)out_size; (void)out; return 0;                   // not supported
}
// ---- iphlpapi: the Mac's primary IPv4 interface as the one adapter (Elite Dangerous picks its outbound address with
// GetBestRoute(223.255.255.255) and reads the adapter's address with GetAdaptersAddresses before it talks to its servers)
typedef struct { char name[32]; uint32_t ip, mask, index; uint8_t mac[6]; int ok; } primary_interface;
static primary_interface primary_ipv4_interface(void) {
    primary_interface p; memset(&p, 0, sizeof p);
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list)) return p;
    // the interface of the default route: the local address a UDP socket connected to a public address gets (nothing
    // is sent), then its entry in the list; without a route, the first one up with an IPv4 address
    uint32_t preferred = 0;
    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    if (probe >= 0) {
        struct sockaddr_in far = { .sin_len = sizeof far, .sin_family = AF_INET, .sin_port = htons(53) };
        far.sin_addr.s_addr = htonl(0x08080808);
        struct sockaddr_in local; socklen_t local_length = sizeof local;
        if (!connect(probe, (struct sockaddr *)&far, sizeof far) && !getsockname(probe, (struct sockaddr *)&local, &local_length)) preferred = local.sin_addr.s_addr;
        close(probe);
    }
    for (int pass = preferred ? 0 : 1; pass < 2 && !p.ok; pass++)
    for (struct ifaddrs *a = list; a && !p.ok; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || (a->ifa_flags & IFF_LOOPBACK) || !(a->ifa_flags & IFF_UP)) continue;
        if (pass == 0 && ((struct sockaddr_in *)a->ifa_addr)->sin_addr.s_addr != preferred) continue;
        snprintf(p.name, sizeof p.name, "%s", a->ifa_name);
        p.ip = ((struct sockaddr_in *)a->ifa_addr)->sin_addr.s_addr;
        p.mask = a->ifa_netmask ? ((struct sockaddr_in *)a->ifa_netmask)->sin_addr.s_addr : 0;
        p.index = if_nametoindex(a->ifa_name);
        p.ok = 1;
    }
    for (struct ifaddrs *a = list; p.ok && a; a = a->ifa_next)
        if (a->ifa_addr && a->ifa_addr->sa_family == AF_LINK && !strcmp(a->ifa_name, p.name) && ((struct sockaddr_dl *)a->ifa_addr)->sdl_alen == 6)
            memcpy(p.mac, LLADDR((struct sockaddr_dl *)a->ifa_addr), 6);
    freeifaddrs(list);
    return p;
}
static WINAPI DWORD GetBestRoute(DWORD destination, DWORD source, DWORD *row) {   // MIB_IPFORWARDROW: 14 DWORDs
    (void)destination; (void)source;
    primary_interface p = primary_ipv4_interface();
    if (!row) return 87;
    if (!p.ok) return 1168;                                                      // ERROR_NOT_FOUND
    memset(row, 0, 56);
    row[3] = (p.ip & p.mask) | htonl(1);                                        // the default route through the network's first address
    row[4] = p.index; row[5] = 4; row[6] = 3; row[9] = 25;                       // indirect, NETMGMT, metric
    row[10] = row[11] = row[12] = row[13] = 0xffffffffu;
    return 0;
}
static WINAPI DWORD GetBestInterface(DWORD destination, DWORD *index) {
    (void)destination;
    primary_interface p = primary_ipv4_interface();
    if (!index) return 87;
    if (!p.ok) return 1168;
    *index = p.index;
    return 0;
}
static WINAPI DWORD GetIpAddrTable(uint8_t *table, DWORD *size, BOOL order) {   // MIB_IPADDRTABLE: count, then 24-byte rows
    (void)order;
    if (!size) return 87;
    primary_interface p = primary_ipv4_interface();
    if (!p.ok) return 232;                                                       // ERROR_NO_DATA
    if (!table || *size < 4 + 2 * 24) { *size = 4 + 2 * 24; return 122; }       // ERROR_INSUFFICIENT_BUFFER
    memset(table, 0, 4 + 2 * 24);
    *(uint32_t *)table = 2;
    uint32_t *row = (uint32_t *)(table + 4);
    row[0] = p.ip; row[1] = p.index; row[2] = p.mask; row[3] = p.ip | ~p.mask; row[4] = 65535; row[5] = 0x1 << 16;   // primary
    row += 6;
    row[0] = htonl(0x7f000001); row[1] = 1; row[2] = htonl(0xff000000); row[3] = htonl(0x7fffffff); row[4] = 65535; row[5] = 0x1 << 16;   // loopback
    *size = 4 + 2 * 24;
    return 0;
}
static WINAPI DWORD GetNetworkParams(uint8_t *info, DWORD *size) {       // FIXED_INFO (592 bytes): host and DNS names and servers
    enum { NEED = 592 };
    if (!size) return 87;
    if (!info || *size < NEED) { *size = NEED; return 111; }
    memset(info, 0, NEED);
    gethostname((char *)info, 128);                                              // HostName[132], DomainName[132] empty
    char *dot = strchr((char *)info, '.');
    if (dot) { snprintf((char *)info + 132, 132, "%s", dot + 1); *dot = 0; }
    *(uint64_t *)(info + 264) = (uint64_t)(uintptr_t)(info + 272);               // CurrentDnsServer -> DnsServerList
    const char *dns = "8.8.8.8";                                                 // the first nameserver of /etc/resolv.conf, else Google's
    FILE *resolv = fopen("/etc/resolv.conf", "r");
    char line[256], server[64] = "";
    while (resolv && fgets(line, sizeof line, resolv))
        if (sscanf(line, "nameserver %63s", server) == 1 && strchr(server, '.')) { dns = server; break; }
    if (resolv) fclose(resolv);
    snprintf((char *)info + 280, 16, "%s", dns);                                 // DnsServerList.IpAddress (Next NULL)
    snprintf((char *)info + 296, 16, "255.255.255.255");
    *(uint32_t *)(info + 312) = 8;                                               // NodeType: hybrid
    *(uint32_t *)(info + 584) = 1;                                               // EnableDns
    *size = NEED;
    return 0;
}
static WINAPI DWORD GetAdaptersAddresses(DWORD family, DWORD flags, void *reserved, uint8_t *buffer, DWORD *size) {
    // IP_ADAPTER_ADDRESSES_LH (448 bytes on x64) + IP_ADAPTER_UNICAST_ADDRESS_LH (64) + a Windows sockaddr_in + the strings
    // ... and an IP_ADAPTER_GATEWAY_ADDRESS_LH and IP_ADAPTER_DNS_SERVER_ADDRESS (32 bytes each: Length, Next, Address)
    enum { ADAPTER = 448, UNICAST = 64, SOCKADDR = 32, NAME = 48, WIDE = 64, ENTRY = 32,
           NEED = ADAPTER + UNICAST + SOCKADDR + NAME + WIDE + WIDE + 8 + 2 * (ENTRY + SOCKADDR) };
    (void)flags; (void)reserved;
    if (!size) return 87;
    primary_interface p = primary_ipv4_interface();
    if (!p.ok || family == 23) { *size = 0; return 232; }                        // ERROR_NO_DATA (no IPv6 here)
    if (!buffer || *size < NEED) { *size = NEED; return 111; }                  // ERROR_BUFFER_OVERFLOW
    memset(buffer, 0, NEED);
    uint8_t *adapter = buffer, *unicast = buffer + ADAPTER, *address = unicast + UNICAST, *name = address + SOCKADDR,
            *description = name + NAME, *friendly = description + WIDE, *suffix = friendly + WIDE;
    snprintf((char *)name, NAME, "{7A0B4C2D-3E1F-4A5B-9C8D-%02X%02X%02X%02X%02X%02X}", p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5]);
    for (int i = 0; i < 8; i++) { ((WCHAR *)description)[i] = (WCHAR)"Ethernet"[i]; ((WCHAR *)friendly)[i] = (WCHAR)"Ethernet"[i]; }
    address[0] = 2; address[1] = 0; memcpy(address + 4, &p.ip, 4);               // a Windows sockaddr_in: family, port, address
    *(uint32_t *)unicast = UNICAST; *(uint32_t *)(unicast + 4) = 0;             // Length, Flags
    *(uint64_t *)(unicast + 16) = (uint64_t)(uintptr_t)address; *(int32_t *)(unicast + 24) = 16;
    *(uint32_t *)(unicast + 32) = 1; *(uint32_t *)(unicast + 36) = 1; *(uint32_t *)(unicast + 40) = 4;   // manual origins, preferred
    *(uint32_t *)(unicast + 44) = *(uint32_t *)(unicast + 48) = *(uint32_t *)(unicast + 52) = 0xffffffffu;
    unicast[56] = (uint8_t)__builtin_popcount(p.mask);
    *(uint32_t *)adapter = ADAPTER; *(uint32_t *)(adapter + 4) = p.index;
    *(uint64_t *)(adapter + 16) = (uint64_t)(uintptr_t)name;
    *(uint64_t *)(adapter + 24) = (uint64_t)(uintptr_t)unicast;
    *(uint64_t *)(adapter + 56) = (uint64_t)(uintptr_t)suffix;
    *(uint64_t *)(adapter + 64) = (uint64_t)(uintptr_t)description;
    *(uint64_t *)(adapter + 72) = (uint64_t)(uintptr_t)friendly;
    memcpy(adapter + 80, p.mac, 6); *(uint32_t *)(adapter + 88) = 6;
    *(uint32_t *)(adapter + 96) = 1500; *(uint32_t *)(adapter + 100) = 6; *(uint32_t *)(adapter + 104) = 1;   // Mtu, Ethernet, up
    *(uint64_t *)(adapter + 184) = *(uint64_t *)(adapter + 192) = 1000000000ull;
    *(uint32_t *)(adapter + 216) = 25; *(uint32_t *)(adapter + 220) = 25;
    *(uint64_t *)(adapter + 224) = ((uint64_t)6 << 48) | ((uint64_t)p.index << 24);   // the LUID: type, index
    *(uint32_t *)(adapter + 248) = 1; *(uint32_t *)(adapter + 268) = 1;        // compartment, dedicated connection
    *(uint32_t *)(adapter + 92) = 0x80 | 0x4;                                    // Ipv4Enabled, Dhcpv4Enabled
    // the gateway (the network's first address, the usual home router) and the DNS server
    uint8_t *gateway = suffix + 8, *gateway_address = gateway + ENTRY, *dns = gateway_address + SOCKADDR, *dns_address = dns + ENTRY;
    uint32_t gateway_ip = (p.ip & p.mask) | htonl(1), dns_ip = gateway_ip;
    FILE *resolv = fopen("/etc/resolv.conf", "r");
    char line[256], server[64];
    while (resolv && fgets(line, sizeof line, resolv))
        if (sscanf(line, "nameserver %63s", server) == 1 && strchr(server, '.') && inet_pton(AF_INET, server, &dns_ip) == 1) break;
    if (resolv) fclose(resolv);
    *(uint32_t *)gateway = ENTRY; *(uint64_t *)(gateway + 16) = (uint64_t)(uintptr_t)gateway_address; *(int32_t *)(gateway + 24) = 16;
    gateway_address[0] = 2; memcpy(gateway_address + 4, &gateway_ip, 4);
    *(uint32_t *)dns = ENTRY; *(uint64_t *)(dns + 16) = (uint64_t)(uintptr_t)dns_address; *(int32_t *)(dns + 24) = 16;
    dns_address[0] = 2; memcpy(dns_address + 4, &dns_ip, 4);
    *(uint64_t *)(adapter + 208) = (uint64_t)(uintptr_t)gateway;                 // FirstGatewayAddress
    *(uint64_t *)(adapter + 48) = (uint64_t)(uintptr_t)dns;                      // FirstDnsServerAddress
    *size = NEED;
    return 0;
}
static WINAPI BOOL SetupDiGetDeviceInstanceIdW(HANDLE set, void *data, WCHAR *id, DWORD size, DWORD *required) {
    (void)set; (void)data; (void)id; (void)size; (void)required; vx_set_last_error(6); return 0;
}
static WINAPI BOOL MiniDumpWriteDump(HANDLE process, DWORD pid, HANDLE file, int type, void *exception, void *user, void *callback) {
    (void)process; (void)pid; (void)file; (void)type; (void)exception; (void)user; (void)callback; vx_set_last_error(50); return 0;
}
// WSAAddressToString / WSAStringToAddress: IPv4 "a.b.c.d[:port]"
static int ws_address_text(const uint8_t *address, int length, char *text, size_t capacity) {
    if (length < 16 || (address[0] | (address[1] << 8)) != 2) return 0;              // AF_INET only
    uint16_t port = (uint16_t)((address[2] << 8) | address[3]);
    if (port) snprintf(text, capacity, "%u.%u.%u.%u:%u", address[4], address[5], address[6], address[7], port);
    else snprintf(text, capacity, "%u.%u.%u.%u", address[4], address[5], address[6], address[7]);
    return 1;
}
static WINAPI int WSAAddressToStringA(const uint8_t *address, DWORD length, void *info, char *out, DWORD *out_length) {
    char text[64]; (void)info;
    if (!ws_address_text(address, (int)length, text, sizeof text)) { vx_set_last_error(10047); return -1; }     // WSAEAFNOSUPPORT
    DWORD need = (DWORD)strlen(text) + 1;
    if (!out || !out_length || *out_length < need) { if (out_length) *out_length = need; vx_set_last_error(10014); return -1; }
    memcpy(out, text, need); *out_length = need;
    return 0;
}
static WINAPI int WSAAddressToStringW(const uint8_t *address, DWORD length, void *info, WCHAR *out, DWORD *out_length) {
    char text[64]; (void)info;
    if (!ws_address_text(address, (int)length, text, sizeof text)) { vx_set_last_error(10047); return -1; }
    DWORD need = (DWORD)strlen(text) + 1;
    if (!out || !out_length || *out_length < need) { if (out_length) *out_length = need; vx_set_last_error(10014); return -1; }
    for (DWORD i = 0; i < need; i++) out[i] = (WCHAR)text[i];
    *out_length = need;
    return 0;
}
static int ws_address_parse(const char *text, int family, uint8_t *address, int *length) {
    unsigned a, b, c, d, port = 0;
    if (family != 2 || !length || *length < 16 || sscanf(text, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) < 4 || a > 255 || b > 255 || c > 255 || d > 255 || port > 65535) return 0;
    memset(address, 0, 16);
    address[0] = 2; address[2] = (uint8_t)(port >> 8); address[3] = (uint8_t)port;
    address[4] = (uint8_t)a; address[5] = (uint8_t)b; address[6] = (uint8_t)c; address[7] = (uint8_t)d;
    *length = 16;
    return 1;
}
static WINAPI int WSAStringToAddressA(const char *text, int family, void *info, uint8_t *address, int *length) {
    (void)info;
    if (!ws_address_parse(text, family, address, length)) { vx_set_last_error(10022); return -1; }           // WSAEINVAL
    return 0;
}
static WINAPI int WSAStringToAddressW(const WCHAR *text, int family, void *info, uint8_t *address, int *length) {
    char narrow[64]; size_t i = 0; (void)info;
    for (; i < 63 && text && text[i]; i++) narrow[i] = (char)text[i];
    narrow[i] = 0;
    if (!ws_address_parse(narrow, family, address, length)) { vx_set_last_error(10022); return -1; }
    return 0;
}
// A socket's overlapped operations complete through the same OVERLAPPED bookkeeping as a file's (net.c calls
// vx_io_completed), so the socket result is the file result; sockets report no completion flags.
static WINAPI BOOL WSAGetOverlappedResult(uintptr_t s, void *overlapped, DWORD *transferred, BOOL wait, DWORD *flags) {
    static BOOL (WINAPI *result)(HANDLE, const void *, DWORD *, BOOL);
    if (!result) result = (BOOL (WINAPI *)(HANDLE, const void *, DWORD *, BOOL))vx_find_export("kernel32.dll", "GetOverlappedResult");
    if (flags) *flags = 0;
    return result((HANDLE)s, overlapped, transferred, wait);
}
// the Epic Online Services SDK's credential store, certificate, LDAP and trust imports: nothing stored, nothing
// verified (its servers are not reached from here), each answered with the API's "not available" result
static WINAPI BOOL CredReadW(const WCHAR *target, DWORD type, DWORD flags, void **credential) { (void)target; (void)type; (void)flags; if (credential) *credential = NULL; vx_set_last_error(1168); return 0; }
static WINAPI BOOL CredWriteW(void *credential, DWORD flags) { (void)credential; (void)flags; vx_set_last_error(5); return 0; }
static WINAPI BOOL CredDeleteW(const WCHAR *target, DWORD type, DWORD flags) { (void)target; (void)type; (void)flags; vx_set_last_error(1168); return 0; }
static WINAPI void CredFree(void *buffer) { (void)buffer; }
static WINAPI BOOL CryptDecrypt(uintptr_t key, uintptr_t hash, BOOL final, DWORD flags, uint8_t *data, DWORD *length) { (void)key; (void)hash; (void)final; (void)flags; (void)data; (void)length; vx_set_last_error(0x80090016); return 0; }
static WINAPI BOOL CryptEnumProvidersW(DWORD index, DWORD *reserved, DWORD flags, DWORD *type, WCHAR *name, DWORD *length) { (void)index; (void)reserved; (void)flags; (void)type; (void)name; (void)length; vx_set_last_error(259); return 0; }
static WINAPI BOOL CryptExportKey(uintptr_t key, uintptr_t exp, DWORD type, DWORD flags, uint8_t *data, DWORD *length) { (void)key; (void)exp; (void)type; (void)flags; (void)data; (void)length; vx_set_last_error(0x80090016); return 0; }
static WINAPI BOOL CryptGetProvParam(uintptr_t prov, DWORD param, uint8_t *data, DWORD *length, DWORD flags) { (void)prov; (void)param; (void)data; (void)length; (void)flags; vx_set_last_error(0x80090017); return 0; }
static WINAPI BOOL CryptGetUserKey(uintptr_t prov, DWORD spec, uintptr_t *key) { (void)prov; (void)spec; if (key) *key = 0; vx_set_last_error(0x8009000D); return 0; }
static WINAPI BOOL CryptSetHashParam(uintptr_t hash, DWORD param, const uint8_t *data, DWORD flags) { (void)hash; (void)param; (void)data; (void)flags; vx_set_last_error(0x80090017); return 0; }
static WINAPI BOOL CryptSignHashW(uintptr_t hash, DWORD spec, const WCHAR *description, DWORD flags, uint8_t *signature, DWORD *length) { (void)hash; (void)spec; (void)description; (void)flags; (void)signature; (void)length; vx_set_last_error(0x8009000D); return 0; }
static WINAPI BOOL CertGetCertificateContextProperty(const void *context, DWORD property, void *data, DWORD *length) { (void)context; (void)property; (void)data; (void)length; vx_set_last_error(0x80092004); return 0; }
static WINAPI DWORD CertGetNameStringW(const void *context, DWORD type, DWORD flags, void *parameter, WCHAR *name, DWORD length) { (void)context; (void)type; (void)flags; (void)parameter; if (name && length) name[0] = 0; return 1; }
static WINAPI BOOL CryptMsgGetParam(void *message, DWORD type, DWORD index, void *data, DWORD *length) { (void)message; (void)type; (void)index; (void)data; (void)length; vx_set_last_error(0x80092004); return 0; }
static WINAPI BOOL CryptQueryObject(DWORD object_type, const void *object, DWORD expected, DWORD flags, DWORD *encoding, DWORD *content, DWORD *format, void **store, void **message, const void **context) {
    (void)object_type; (void)object; (void)expected; (void)flags; (void)encoding; (void)content; (void)format;
    if (store) *store = NULL; if (message) *message = NULL; if (context) *context = NULL;
    vx_set_last_error(0x80092009); return 0;                                    // CRYPT_E_NO_MATCH
}
static WINAPI int32_t CompleteAuthToken(void *context, void *token) { (void)context; (void)token; return (int32_t)0x80090302; }   // SEC_E_UNSUPPORTED_FUNCTION
static WINAPI int32_t WinVerifyTrust(HANDLE hwnd, void *action, void *data) { (void)hwnd; (void)action; (void)data; return (int32_t)0x800B0100; }   // TRUST_E_NOSIGNATURE
static WINAPI DWORD ldap_unavailable(void) { return 0x51; }                       // LDAP_SERVER_DOWN, for every wldap32 ordinal
static uint8_t ws_tcp_name[] = "tcp", ws_udp_name[] = "udp";
static char *ws_no_alias[] = { NULL };
static struct { char *name; char **aliases; int16_t proto; } ws_tcp = { (char *)ws_tcp_name, ws_no_alias, 6 }, ws_udp = { (char *)ws_udp_name, ws_no_alias, 17 };
static WINAPI void *ws_getprotobyname(const char *name) {
    if (name && !strcasecmp(name, "tcp")) return &ws_tcp;
    if (name && !strcasecmp(name, "udp")) return &ws_udp;
    vx_set_last_error(11004); return NULL;
}
static const vx_export advapi32_ed[] = { VX_EXPORT(CredReadW), VX_EXPORT(CredWriteW), VX_EXPORT(CredDeleteW), VX_EXPORT(CredFree), VX_EXPORT(CryptDecrypt), VX_EXPORT(CryptEnumProvidersW),
                                         VX_EXPORT(CryptExportKey), VX_EXPORT(CryptGetProvParam), VX_EXPORT(CryptGetUserKey), VX_EXPORT(CryptSetHashParam), VX_EXPORT(CryptSignHashW) };
static const vx_export crypt32_ed[] = { VX_EXPORT(CertGetCertificateContextProperty), VX_EXPORT(CertGetNameStringW), VX_EXPORT(CryptMsgGetParam), VX_EXPORT(CryptQueryObject) };
static const vx_export secur32_ed[] = { VX_EXPORT(CompleteAuthToken) };
static const vx_export wintrust_ed[] = { VX_EXPORT(WinVerifyTrust) };
static const vx_export wldap32_ed[] = { { "#27", (void *)ldap_unavailable }, { "#30", (void *)ldap_unavailable }, { "#32", (void *)ldap_unavailable }, { "#33", (void *)ldap_unavailable },
                                        { "#35", (void *)ldap_unavailable }, { "#41", (void *)ldap_unavailable }, { "#46", (void *)ldap_unavailable }, { "#50", (void *)ldap_unavailable },
                                        { "#60", (void *)ldap_unavailable }, { "#79", (void *)ldap_unavailable }, { "#143", (void *)ldap_unavailable }, { "#200", (void *)ldap_unavailable },
                                        { "#211", (void *)ldap_unavailable }, { "#301", (void *)ldap_unavailable } };
static const vx_export ws2_32_proto[] = { { "getprotobyname", (void *)ws_getprotobyname }, { "#53", (void *)ws_getprotobyname } };
// process mitigation policies (looked up with GetProcAddress and called without a check): nothing is enabled
static WINAPI BOOL GetProcessMitigationPolicy(HANDLE process, int policy, void *buffer, size_t length) {
    (void)process; (void)policy;
    if (buffer && length) memset(buffer, 0, length);
    return 1;
}
static WINAPI BOOL SetProcessMitigationPolicy(int policy, void *buffer, size_t length) { (void)policy; (void)buffer; (void)length; return 1; }
// looked up by name at start-up and called unchecked: thread information classes are accepted (ThreadHideFromDebugger
// included: there is no debugger to hide from), DLL directories are remembered as a cookie, the newer allocation and
// temp-path entry points forward to the older ones
static WINAPI int32_t NtSetInformationThread(HANDLE thread, int class_, const void *information, DWORD length) { (void)thread; (void)class_; (void)information; (void)length; return 0; }
static WINAPI void *AddDllDirectory(const WCHAR *directory) { (void)directory; static int cookie; return &cookie; }
static WINAPI BOOL RemoveDllDirectory(void *cookie) { (void)cookie; return 1; }
static WINAPI DWORD GetTempPath2W(DWORD length, WCHAR *buffer) {
    DWORD (WINAPI *temp_path)(DWORD, WCHAR *) = (DWORD (WINAPI *)(DWORD, WCHAR *))vx_find_export("kernel32.dll", "GetTempPathW");
    return temp_path ? temp_path(length, buffer) : 0;
}
static WINAPI void *VirtualAlloc2(HANDLE process, void *address, size_t size, DWORD type, DWORD protection, void *parameters, DWORD parameter_count) {
    (void)process; (void)parameters; (void)parameter_count;
    void *(WINAPI *allocate)(void *, size_t, DWORD, DWORD) = (void *(WINAPI *)(void *, size_t, DWORD, DWORD))vx_find_export("kernel32.dll", "VirtualAlloc");
    return allocate ? allocate(address, size, type, protection) : NULL;
}
// the volume behind a mount point: one fixed volume name for every drive (looked up, then called unchecked by Elite Dangerous)
static const char volume_name[] = "\\\\?\\Volume{3f4e8c2a-9b1d-4c6e-8a7f-1d2e3f4a5b6c}\\";
static WINAPI BOOL GetVolumeNameForVolumeMountPointA(const char *mount_point, char *name, DWORD length) {
    (void)mount_point;
    if (!name || length < sizeof volume_name) { vx_set_last_error(234); return 0; }   // ERROR_MORE_DATA
    memcpy(name, volume_name, sizeof volume_name);
    return 1;
}
static WINAPI BOOL GetVolumeNameForVolumeMountPointW(const WCHAR *mount_point, WCHAR *name, DWORD length) {
    (void)mount_point;
    if (!name || length < sizeof volume_name) { vx_set_last_error(234); return 0; }
    for (size_t i = 0; i < sizeof volume_name; i++) name[i] = (WCHAR)volume_name[i];
    return 1;
}
static const vx_export ntdll_ed[] = { VX_EXPORT(NtSetInformationThread) };
static const vx_export kernel32_ed[] = { VX_EXPORT(GetVolumeNameForVolumeMountPointA), VX_EXPORT(GetVolumeNameForVolumeMountPointW), VX_EXPORT(AddDllDirectory), VX_EXPORT(RemoveDllDirectory), VX_EXPORT(GetTempPath2W), VX_EXPORT(VirtualAlloc2), VX_EXPORT(GetProcessMitigationPolicy), VX_EXPORT(SetProcessMitigationPolicy), VX_EXPORT(NormalizeString), VX_EXPORT(SearchPathW), VX_EXPORT(VirtualUnlock), VX_EXPORT(GetFinalPathNameByHandleW) };
static const vx_export user32_ed[] = { VX_EXPORT(ToAsciiEx), VX_EXPORT(UnloadKeyboardLayout), VX_EXPORT(RegisterHotKey), VX_EXPORT(UnregisterHotKey) };
static const vx_export gdi32_ed[] = { VX_EXPORT(ExtEscape) };
static const vx_export iphlpapi_ed[] = { VX_EXPORT(GetBestRoute), VX_EXPORT(GetBestInterface), VX_EXPORT(GetAdaptersAddresses), VX_EXPORT(GetIpAddrTable), VX_EXPORT(GetNetworkParams) };
static const vx_export setupapi_ed[] = { VX_EXPORT(SetupDiGetDeviceInstanceIdW) };
static const vx_export dbghelp_ed[] = { VX_EXPORT(MiniDumpWriteDump) };
static const vx_export normaliz_ed[] = { VX_EXPORT(IdnToAscii) };
static const vx_export ws2_32_ed[] = { VX_EXPORT(WSAAddressToStringA), VX_EXPORT(WSAAddressToStringW), VX_EXPORT(WSAStringToAddressA), VX_EXPORT(WSAStringToAddressW), VX_EXPORT(WSAGetOverlappedResult) };

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
static const vx_export advapi32[] = { VX_EXPORT(SetFileSecurityW) };
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
    MORE("kernel32.dll", kernel32_ed); MORE("kernelbase.dll", kernel32_ed); MORE("user32.dll", user32_ed); MORE("gdi32.dll", gdi32_ed);
    MORE("iphlpapi.dll", iphlpapi_ed); MORE("setupapi.dll", setupapi_ed); MORE("dbghelp.dll", dbghelp_ed); MORE("normaliz.dll", normaliz_ed);
    MORE("ws2_32.dll", ws2_32_ed); MORE("ws2_32.dll", ws2_32_proto); MORE("advapi32.dll", advapi32_ed); MORE("crypt32.dll", crypt32_ed);
    MORE("secur32.dll", secur32_ed); MORE("wintrust.dll", wintrust_ed); MORE("wldap32.dll", wldap32_ed); MORE("ntdll.dll", ntdll_ed);
#undef MORE
}

#ifdef __aarch64__
#include "thunks/extras.c.inc"
#endif

#ifdef __aarch64__
#include "arm64.h"
__attribute__((constructor)) static void extras_guest_add(void) {   // (vCPU: answered in the guest)
    void *h = vxvm_heap();
    if (h) vx_guest_add(ms_msize, xv_g_msize, &h, sizeof h);
}
#endif
