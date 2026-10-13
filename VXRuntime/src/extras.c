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

static const vx_export kernel32[] = {
    VX_EXPORT(GetDiskFreeSpaceW), VX_EXPORT(LockFile), VX_EXPORT(UnlockFile), VX_EXPORT(LockFileEx), VX_EXPORT(UnlockFileEx),
    VX_EXPORT(HeapCompact), VX_EXPORT(GetUserPreferredUILanguages), VX_EXPORT(GetGeoInfoA),
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
}
