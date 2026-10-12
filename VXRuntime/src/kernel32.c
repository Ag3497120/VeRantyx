// kernel32: the Windows core API, implemented directly on macOS.
#include <mach/mach_time.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <malloc/malloc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/sysctl.h>
#include <time.h>
#include <ctype.h>
#include "vx.h"

#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE  ((DWORD)-12)

// ---- handles ----------------------------------------------------------------------------------------------------
int vx_handle_fd(HANDLE h) {
    intptr_t v = (intptr_t)h;
    if (v == -10) return 0;
    if (v == -11) return 1;
    if (v == -12) return 2;
    if (v >= 0x10000 && v < 0x10000 + 1024) return (int)(v - 0x10000);   // file handles: 0x10000 + fd
    return -1;
}

static HANDLE g_std_override[3];       // set by SetStdHandle
static WINAPI HANDLE GetStdHandle(DWORD which) {
    if (which == STD_INPUT_HANDLE || which == STD_OUTPUT_HANDLE || which == STD_ERROR_HANDLE) {
        HANDLE o = g_std_override[(DWORD)-10 - which];
        return o ? o : (HANDLE)(intptr_t)(int32_t)which;
    }
    return (HANDLE)(intptr_t)-1;
}

static WINAPI BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *overlapped) {
    int fd = vx_handle_fd(h);
    if (fd < 0) { vx_set_last_error(6 /* ERROR_INVALID_HANDLE */); return 0; }
    // OVERLAPPED: the transfer happens at its offset, at once, and completes like asynchronous I/O would.
    const DWORD *o = overlapped;
    ssize_t r = o && fd > 2 ? pwrite(fd, buf, n, (off_t)o[4] | (off_t)o[5] << 32) : write(fd, buf, n);
    if (written) *written = r > 0 ? (DWORD)r : 0;
    if (r >= 0 && overlapped) vx_io_completed(h, overlapped, (DWORD)r, 0);
    return r >= 0;
}

static WINAPI BOOL WriteConsoleA(HANDLE h, const void *buf, DWORD n, DWORD *written, void *reserved) {
    return WriteFile(h, buf, n, written, reserved);
}

static WINAPI BOOL WriteConsoleW(HANDLE h, const WCHAR *buf, DWORD n, DWORD *written, void *reserved) {
    (void)reserved;
    char *utf8 = malloc((size_t)n * 3 + 1); size_t k = 0;
    for (DWORD i = 0; i < n; i++) {
        uint32_t c = buf[i];
        if (c < 0x80) utf8[k++] = (char)c;
        else if (c < 0x800) { utf8[k++] = 0xc0 | (c >> 6); utf8[k++] = 0x80 | (c & 0x3f); }
        else { utf8[k++] = 0xe0 | (c >> 12); utf8[k++] = 0x80 | ((c >> 6) & 0x3f); utf8[k++] = 0x80 | (c & 0x3f); }
    }
    DWORD w = 0; BOOL ok = WriteFile(h, utf8, (DWORD)k, &w, NULL);
    if (written) *written = ok ? n : 0;
    free(utf8);
    return ok;
}

static WINAPI void ExitProcess(DWORD code) { fflush(stdout); fflush(stderr); _exit((int)code); }
static WINAPI BOOL TerminateProcess(HANDLE h, DWORD code) { (void)h; ExitProcess(code); return 1; }

// ---- errors, debugging, process info -------------------------------------------------------------------------------
static WINAPI DWORD GetLastError(void) { return vx_get_last_error(); }
static WINAPI void SetLastError(DWORD e) { vx_set_last_error(e); }
static WINAPI BOOL IsDebuggerPresent(void) { return 0; }
static WINAPI DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
static WINAPI DWORD GetCurrentThreadId(void) { return vx_current_tid(); }
static WINAPI HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
static WINAPI HANDLE GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
static WINAPI char *GetCommandLineA(void) { return vx_proc.command_line; }
static WINAPI WCHAR *GetCommandLineW(void) { return vx_proc.command_line_w; }

typedef struct { DWORD cb; void *reserved, *desktop, *title; DWORD x, y, xs, ys, xc, yc, fill, flags; uint16_t show, cb2; void *r2; HANDLE in, out, err; } STARTUPINFO;
static WINAPI void GetStartupInfoA(STARTUPINFO *si) {
    memset(si, 0, sizeof *si); si->cb = sizeof *si;
    si->in = (HANDLE)(intptr_t)-10; si->out = (HANDLE)(intptr_t)-11; si->err = (HANDLE)(intptr_t)-12;
}
static WINAPI void GetStartupInfoW(STARTUPINFO *si) { GetStartupInfoA(si); }

// ---- time -----------------------------------------------------------------------------------------------------------
static WINAPI BOOL SetStdHandle(DWORD which, HANDLE h) {
    if (which != STD_INPUT_HANDLE && which != STD_OUTPUT_HANDLE && which != STD_ERROR_HANDLE) { vx_set_last_error(6); return 0; }
    g_std_override[(DWORD)-10 - which] = h;
    return 1;
}
static WINAPI BOOL GetConsoleMode(HANDLE h, DWORD *mode) {
    int fd = vx_handle_fd(h);
    if (fd < 0 || !isatty(fd)) { vx_set_last_error(6); return 0; }
    *mode = fd == 0 ? 0x1f7 : 0x7;           // the defaults of a Windows console
    return 1;
}
static WINAPI BOOL SetConsoleMode(HANDLE h, DWORD mode) { (void)mode; int fd = vx_handle_fd(h); return fd >= 0 && isatty(fd); }
static WINAPI BOOL ReadConsoleW(HANDLE h, WCHAR *buf, DWORD n, DWORD *got, void *ctl) {
    (void)ctl;
    int fd = vx_handle_fd(h);
    char tmp[1024];
    ssize_t r = fd < 0 ? -1 : read(fd, tmp, n < sizeof tmp ? n : sizeof tmp);
    if (r < 0) { vx_set_last_error(6); return 0; }
    *got = (DWORD)vx_utf8_to_utf16(tmp, (size_t)r, buf, n);
    return 1;
}
// The console is the terminal vxrt runs in: 80x25 as far as programs ask, colors ignored for now.
typedef struct { int16_t w, h, cx, cy; uint16_t attr; int16_t left, top, right, bottom, maxw, maxh; } CONSOLE_SCREEN_BUFFER_INFO;
static WINAPI BOOL GetConsoleScreenBufferInfo(HANDLE h, CONSOLE_SCREEN_BUFFER_INFO *i) {
    int fd = vx_handle_fd(h);
    if (fd < 0 || !isatty(fd)) { vx_set_last_error(6); return 0; }
    memset(i, 0, sizeof *i);
    i->w = 80; i->h = 25; i->attr = 7; i->right = 79; i->bottom = 24; i->maxw = 80; i->maxh = 25;
    return 1;
}
static WINAPI BOOL SetConsoleTextAttribute(HANDLE h, uint16_t attr) { (void)h; (void)attr; return 1; }
static WINAPI BOOL SetConsoleTitleA(const char *t) { (void)t; return 1; }
static WINAPI BOOL SetConsoleTitleW(const WCHAR *t) { (void)t; return 1; }
static WINAPI void *GetConsoleWindow(void) { return NULL; }
static WINAPI BOOL AllocConsole(void) { vx_set_last_error(5); return 0; }   // the terminal is already attached
static WINAPI BOOL FreeConsole(void) { return 1; }
static WINAPI BOOL SetConsoleCtrlHandler(void *handler, BOOL add) { (void)handler; (void)add; return 1; }
static WINAPI DWORD GetConsoleCP(void) { return 65001; }

static WINAPI BOOL GetExitCodeProcess(HANDLE h, DWORD *code) {
    if ((intptr_t)h != -1) { vx_set_last_error(6); return 0; }
    *code = 259;                              // STILL_ACTIVE: only this process is known so far
    return 1;
}

// Pointer encoding is an exploit mitigation on Windows; the identity keeps every encode/decode pair correct.
static WINAPI void *EncodePointer(void *p) { return p; }
static WINAPI void *DecodePointer(void *p) { return p; }

// What the CPU offers as Rosetta 2 presents it.
static WINAPI BOOL IsProcessorFeaturePresent(DWORD f) {
    int v = 0; size_t len = sizeof v;
    switch (f) {
    case 2: case 3: case 6: case 7 + 1: case 10: case 12: case 13: case 14: case 17: case 23: case 36: case 37: case 38:
        return 1;     // cmpxchg8b/16b, SSE..SSE4.2, RDTSC, NX, fastfail
    case 39: return sysctlbyname("hw.optional.avx1_0", &v, &len, NULL, 0) == 0 && v;   // PF_AVX_INSTRUCTIONS_AVAILABLE
    case 40: return sysctlbyname("hw.optional.avx2_0", &v, &len, NULL, 0) == 0 && v;   // PF_AVX2_INSTRUCTIONS_AVAILABLE
    default: return 0;
    }
}

// SYSTEMTIME: year, month, day of week, day, hour, minute, second, milliseconds.
static void system_time(uint16_t *st, int local) {
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm tm; time_t t = tv.tv_sec;
    if (local) localtime_r(&t, &tm); else gmtime_r(&t, &tm);
    st[0] = (uint16_t)(tm.tm_year + 1900); st[1] = (uint16_t)(tm.tm_mon + 1); st[2] = (uint16_t)tm.tm_wday;
    st[3] = (uint16_t)tm.tm_mday; st[4] = (uint16_t)tm.tm_hour; st[5] = (uint16_t)tm.tm_min; st[6] = (uint16_t)tm.tm_sec;
    st[7] = (uint16_t)(tv.tv_usec / 1000);
}
static WINAPI void GetLocalTime(uint16_t *st) { system_time(st, 1); }
static WINAPI void GetSystemTime(uint16_t *st) { system_time(st, 0); }

// The Windows version programs see: Windows 10 22H2 (10.0.19045). GetVersion packs major, minor and build.
typedef struct { DWORD size, major, minor, build, platform; char csd[128]; } OSVERSIONINFOA;
typedef struct { DWORD size, major, minor, build, platform; WCHAR csd[128]; } OSVERSIONINFOW;
static WINAPI DWORD GetVersion(void) { return 10u | (0u << 8) | (19045u << 16); }
static WINAPI BOOL GetVersionExA(OSVERSIONINFOA *v) {
    v->major = 10; v->minor = 0; v->build = 19045; v->platform = 2;   // VER_PLATFORM_WIN32_NT
    v->csd[0] = 0;
    if (v->size >= sizeof *v + 8) memset((char *)v + sizeof *v, 0, 8);       // OSVERSIONINFOEX: service pack, suite, type
    return 1;
}
static WINAPI BOOL GetVersionExW(OSVERSIONINFOW *v) {
    v->major = 10; v->minor = 0; v->build = 19045; v->platform = 2;
    v->csd[0] = 0;
    if (v->size >= sizeof *v + 8) memset((char *)v + sizeof *v, 0, 8);
    return 1;
}

static WINAPI BOOL HeapSetInformation(HANDLE h, int cls, void *info, size_t len) { (void)h; (void)cls; (void)info; (void)len; return 1; }
static WINAPI DWORD SetHandleCount(DWORD n) { return n; }
// Thread times: CPU time of the calling thread (other threads report zero for now), as 100 ns FILETIMEs.
static WINAPI BOOL GetThreadTimes(HANDLE h, uint64_t *created, uint64_t *exited, uint64_t *kernel, uint64_t *user) {
    (void)h;
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    *created = *exited = *kernel = 0;
    *user = (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100;
    return 1;
}
static WINAPI BOOL GetProcessTimes(HANDLE h, uint64_t *created, uint64_t *exited, uint64_t *kernel, uint64_t *user) {
    (void)h;
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    *created = *exited = *kernel = 0;
    *user = (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100;
    return 1;
}

// VerifyVersionInfo: the condition mask holds 3 bits per field (VER_MINORVERSION=1 ... VER_PRODUCT_TYPE=0x80).
static WINAPI uint64_t VerSetConditionMask(uint64_t mask, DWORD type, uint8_t cond) {
    for (int i = 0; i < 8; i++) if (type & (1u << i)) mask |= (uint64_t)(cond & 7) << (i * 3);
    return mask;
}
static int ver_compare(uint64_t have, uint64_t want, int cond) {
    switch (cond) {
    case 1: return have == want; case 2: return have > want; case 3: return have >= want;
    case 4: return have < want;  case 5: return have <= want;
    default: return 1;
    }
}
// Major, minor and service pack compare as one version (as Windows does); build, platform and product type alone.
static BOOL verify_version(DWORD major, DWORD minor, DWORD build, DWORD platform, uint16_t sp_major, uint16_t sp_minor, uint8_t product,
                           DWORD type, uint64_t mask) {
    uint64_t have = 10ull << 48, want = (uint64_t)major << 48 | (uint64_t)minor << 32 | (uint64_t)sp_major << 16 | sp_minor;
    uint64_t want_mask = 0;
    if (type & 2) want_mask |= 0xffffull << 48;
    if (type & 1) want_mask |= 0xffffull << 32;
    if (type & 0x20) want_mask |= 0xffffull << 16;
    if (type & 0x10) want_mask |= 0xffff;
    int cond = type & 2 ? (mask >> 3) & 7 : type & 1 ? mask & 7 : type & 0x20 ? (mask >> 15) & 7 : (mask >> 12) & 7;
    BOOL ok = !want_mask || ver_compare(have & want_mask, want & want_mask, cond);
    if (type & 4) ok = ok && ver_compare(19045, build, (mask >> 6) & 7);
    if (type & 8) ok = ok && ver_compare(2, platform, (mask >> 9) & 7);
    if (type & 0x80) ok = ok && ver_compare(1, product, (mask >> 21) & 7);   // VER_NT_WORKSTATION
    if (!ok) vx_set_last_error(1150 /* ERROR_OLD_WIN_VERSION */);
    return ok;
}
static WINAPI BOOL VerifyVersionInfoW(const uint8_t *v, DWORD type, uint64_t mask) {
    const DWORD *d = (const DWORD *)v;
    return verify_version(d[1], d[2], d[3], d[4], *(const uint16_t *)(v + 276), *(const uint16_t *)(v + 278), v[282], type, mask);
}
static WINAPI BOOL VerifyVersionInfoA(const uint8_t *v, DWORD type, uint64_t mask) {
    const DWORD *d = (const DWORD *)v;
    return verify_version(d[1], d[2], d[3], d[4], *(const uint16_t *)(v + 148), *(const uint16_t *)(v + 150), v[154], type, mask);
}

static DWORD g_error_mode;
static WINAPI DWORD SetErrorMode(DWORD m) { DWORD o = g_error_mode; g_error_mode = m; return o; }
static WINAPI DWORD GetErrorMode(void) { return g_error_mode; }
static WINAPI DWORD GetCurrentProcessorNumber(void) { return 0; }
static WINAPI DWORD GetPriorityClass(HANDLE h) { (void)h; return 0x20; }    // NORMAL_PRIORITY_CLASS
static WINAPI BOOL SetPriorityClass(HANDLE h, DWORD c) { (void)h; (void)c; return 1; }
static WINAPI BOOL HeapValidate(HANDLE h, DWORD f, const void *p) { (void)h; (void)f; (void)p; return 1; }
static WINAPI BOOL DeviceIoControl(HANDLE h, DWORD code, void *in, DWORD nin, void *out, DWORD nout, DWORD *got, void *ov) {
    (void)h; (void)code; (void)in; (void)nin; (void)out; (void)nout; (void)ov;
    if (got) *got = 0;
    vx_set_last_error(50 /* ERROR_NOT_SUPPORTED */);
    return 0;
}
static WINAPI DWORD K32GetMappedFileNameW(HANDLE p, void *addr, WCHAR *buf, DWORD n) { (void)p; (void)addr; (void)buf; (void)n; vx_set_last_error(50); return 0; }
// Stack traces from Windows code: not collected yet.
static WINAPI uint16_t RtlCaptureStackBackTrace(DWORD skip, DWORD n, void **frames, DWORD *hash) { (void)skip; (void)n; (void)frames; if (hash) *hash = 0; return 0; }

// Starting other processes comes with a later milestone; programs see the failure Windows reports for a
// missing executable.
static WINAPI BOOL CreateProcessA(const char *app, char *cmd, void *pa, void *ta, BOOL inherit, DWORD flags, void *env, const char *dir, void *si, void *pi) {
    (void)pa; (void)ta; (void)inherit; (void)flags; (void)env; (void)dir; (void)si; (void)pi;
    vx_log("CreateProcess(%s) is not supported yet", app ? app : cmd ? cmd : "");
    vx_set_last_error(50);
    return 0;
}
static WINAPI BOOL CreateProcessW(const WCHAR *app, WCHAR *cmd, void *pa, void *ta, BOOL inherit, DWORD flags, void *env, const WCHAR *dir, void *si, void *pi) {
    char *a = vx_utf8_dup(app), *c = vx_utf8_dup(cmd);
    BOOL r = CreateProcessA(a, c, pa, ta, inherit, flags, env, NULL, si, pi);
    free(a); free(c);
    (void)dir;
    return r;
}
static WINAPI DWORD WinExec(const char *cmd, DWORD show) { (void)show; vx_log("WinExec(%s) is not supported yet", cmd); return 2; }

// MEMORYSTATUSEX: length, load, total/available physical, page file, virtual, extended.
typedef struct { DWORD length, load; uint64_t total_phys, avail_phys, total_page, avail_page, total_virtual, avail_virtual, avail_ext; } MEMORYSTATUSEX;
static WINAPI BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX *m) {
    uint64_t total = 0; size_t len = sizeof total;
    sysctlbyname("hw.memsize", &total, &len, NULL, 0);
    vm_statistics64_data_t vs; mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    uint64_t avail = total / 2;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &count) == KERN_SUCCESS)
        avail = ((uint64_t)vs.free_count + vs.inactive_count + vs.purgeable_count) * vm_page_size;
    m->load = total ? (DWORD)(100 - avail * 100 / total) : 0;
    m->total_phys = total; m->avail_phys = avail;
    m->total_page = total * 2; m->avail_page = avail + total;
    m->total_virtual = 0x7ffffffe0000ull; m->avail_virtual = 0x7ff000000000ull; m->avail_ext = 0;
    return 1;
}

// Computer and user names come from the Mac.
static int name_out_a(const char *name, char *buf, DWORD *size, int count_nul_on_success) {
    DWORD n = (DWORD)strlen(name);
    if (!buf || *size <= n) { *size = n + 1; vx_set_last_error(234 /* ERROR_MORE_DATA */); return 0; }
    memcpy(buf, name, n + 1);
    *size = count_nul_on_success ? n + 1 : n;
    return 1;
}
static int name_out_w(const char *name, WCHAR *buf, DWORD *size, int count_nul_on_success) {
    DWORD n = (DWORD)vx_utf8_to_utf16(name, strlen(name), NULL, 0);
    if (!buf || *size <= n) { *size = n + 1; vx_set_last_error(234); return 0; }
    vx_utf8_to_utf16(name, strlen(name), buf, n); buf[n] = 0;
    *size = count_nul_on_success ? n + 1 : n;
    return 1;
}
static const char *computer_name(void) {
    static char host[256];
    if (!host[0]) { gethostname(host, sizeof host); char *dot = strchr(host, '.'); if (dot) *dot = 0; for (char *c = host; *c; c++) *c = (char)toupper((unsigned char)*c); }
    return host;
}
static WINAPI BOOL GetComputerNameA(char *buf, DWORD *size) { return name_out_a(computer_name(), buf, size, 0); }
static WINAPI BOOL GetComputerNameW(WCHAR *buf, DWORD *size) { return name_out_w(computer_name(), buf, size, 0); }
static WINAPI BOOL GetComputerNameExA(int fmt, char *buf, DWORD *size) { (void)fmt; return name_out_a(computer_name(), buf, size, 0); }
static WINAPI BOOL GetComputerNameExW(int fmt, WCHAR *buf, DWORD *size) { (void)fmt; return name_out_w(computer_name(), buf, size, 0); }
static const char *user_name(void) { const char *u = getenv("USER"); return u && *u ? u : "vxrt"; }
static WINAPI BOOL GetUserNameA(char *buf, DWORD *size) { return name_out_a(user_name(), buf, size, 1); }
static WINAPI BOOL GetUserNameW(WCHAR *buf, DWORD *size) { return name_out_w(user_name(), buf, size, 1); }

// ---- time conversions -------------------------------------------------------------------------------------------------
static uint64_t filetime_of(const DWORD *ft) { return (uint64_t)ft[1] << 32 | ft[0]; }
static BOOL systemtime_from_unix(int64_t secs, uint32_t ms, uint16_t *st, int local) {
    time_t t = (time_t)secs; struct tm tm;
    if (!(local ? localtime_r(&t, &tm) : gmtime_r(&t, &tm))) return 0;
    st[0] = (uint16_t)(tm.tm_year + 1900); st[1] = (uint16_t)(tm.tm_mon + 1); st[2] = (uint16_t)tm.tm_wday;
    st[3] = (uint16_t)tm.tm_mday; st[4] = (uint16_t)tm.tm_hour; st[5] = (uint16_t)tm.tm_min; st[6] = (uint16_t)tm.tm_sec; st[7] = (uint16_t)ms;
    return 1;
}
static WINAPI BOOL FileTimeToSystemTime(const DWORD *ft, uint16_t *st) {
    uint64_t t = filetime_of(ft);
    return systemtime_from_unix((int64_t)(t / 10000000ull) - 11644473600ll, (uint32_t)(t % 10000000ull / 10000), st, 0);
}
static WINAPI BOOL SystemTimeToFileTime(const uint16_t *st, DWORD *ft) {
    struct tm tm = { 0 };
    tm.tm_year = st[0] - 1900; tm.tm_mon = st[1] - 1; tm.tm_mday = st[3]; tm.tm_hour = st[4]; tm.tm_min = st[5]; tm.tm_sec = st[6];
    time_t t = timegm(&tm);
    uint64_t v = ((uint64_t)t + 11644473600ull) * 10000000ull + (uint64_t)st[7] * 10000;
    ft[0] = (DWORD)v; ft[1] = (DWORD)(v >> 32);
    return 1;
}
static WINAPI BOOL FileTimeToLocalFileTime(const DWORD *in, DWORD *out) {
    uint64_t t = filetime_of(in);
    time_t secs = (time_t)(t / 10000000ull - 11644473600ull); struct tm tm; localtime_r(&secs, &tm);
    t += (uint64_t)((int64_t)tm.tm_gmtoff * 10000000ll);
    out[0] = (DWORD)t; out[1] = (DWORD)(t >> 32);
    return 1;
}
static WINAPI BOOL SystemTimeToTzSpecificLocalTime(const void *tz, const uint16_t *utc, uint16_t *local) {
    (void)tz;
    DWORD ft[2];
    SystemTimeToFileTime(utc, ft);
    uint64_t t = filetime_of(ft);
    return systemtime_from_unix((int64_t)(t / 10000000ull) - 11644473600ll, utc[7], local, 1);
}
// TIME_ZONE_INFORMATION: bias, standard name/date/bias, daylight name/date/bias. No transition rules yet.
typedef struct { LONG bias; WCHAR std_name[32]; uint16_t std_date[8]; LONG std_bias; WCHAR dst_name[32]; uint16_t dst_date[8]; LONG dst_bias; } TIME_ZONE_INFORMATION;
static WINAPI DWORD GetTimeZoneInformation(TIME_ZONE_INFORMATION *tz) {
    memset(tz, 0, sizeof *tz);
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    tz->bias = (LONG)(-(tm.tm_gmtoff - (tm.tm_isdst > 0 ? 3600 : 0)) / 60);
    tz->dst_bias = -60;
    vx_utf8_to_utf16(tzname[0], strlen(tzname[0]), tz->std_name, 31);
    vx_utf8_to_utf16(tzname[1], strlen(tzname[1]), tz->dst_name, 31);
    return 0;                                       // TIME_ZONE_ID_UNKNOWN
}

static WINAPI void Sleep(DWORD ms) { if (ms) usleep((useconds_t)ms * 1000); else sched_yield(); }
static WINAPI BOOL QueryPerformanceFrequency(int64_t *f) { *f = 1000000000; return 1; }
static WINAPI BOOL QueryPerformanceCounter(int64_t *c) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    *c = (int64_t)(mach_absolute_time() * tb.numer / tb.denom);
    return 1;
}
static WINAPI uint64_t GetTickCount64(void) { int64_t c; QueryPerformanceCounter(&c); return (uint64_t)c / 1000000; }
static WINAPI DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }
static WINAPI void GetSystemTimeAsFileTime(uint64_t *ft) {
    struct timeval tv; gettimeofday(&tv, NULL);
    *ft = ((uint64_t)tv.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)tv.tv_usec * 10;   // 100 ns since 1601
}
static WINAPI void GetSystemTimePreciseAsFileTime(uint64_t *ft) { GetSystemTimeAsFileTime(ft); }

// ---- TLS slots (TEB + 0x1480, 64 of them) --------------------------------------------------------------------------------
static uint64_t g_tls_used;
static WINAPI DWORD TlsAlloc(void) {
    for (DWORD i = 0; i < 64; i++)
        if (!(__sync_fetch_and_or(&g_tls_used, 1ull << i) & (1ull << i))) return i;
    return 0xffffffffu;   // TLS_OUT_OF_INDEXES
}
static WINAPI BOOL TlsFree(DWORD i) { if (i >= 64) return 0; __sync_fetch_and_and(&g_tls_used, ~(1ull << i)); return 1; }
static WINAPI void *TlsGetValue(DWORD i) {
    if (i >= 64) { vx_set_last_error(87); return NULL; }
    vx_set_last_error(0);
    return ((void **)(vx_teb() + TEB_TLS_SLOTS))[i];
}
static WINAPI BOOL TlsSetValue(DWORD i, void *v) {
    if (i >= 64) { vx_set_last_error(87); return 0; }
    ((void **)(vx_teb() + TEB_TLS_SLOTS))[i] = v;
    return 1;
}

// ---- memory -----------------------------------------------------------------------------------------------------------
// Virtual memory with Windows semantics on Mach VM: a reservation at a requested address either lands exactly
// there or fails (programs probe for free address ranges this way); reservations are 64 KB aligned (Windows'
// allocation granularity); MEM_RELEASE frees the whole reservation, MEM_DECOMMIT gives the pages back (they
// read as zero when committed again). Reserved memory is accessible before it is committed, and everything is
// read/write/execute: protection changes are recorded but not enforced yet.
#define PAGE_EXECUTE_READWRITE 0x40
#define MEM_COMMIT   0x1000
#define MEM_RESERVE  0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE  0x8000
#define MEM_FREE     0x10000
#define MEM_PRIVATE  0x20000
#define MEM_IMAGE    0x1000000
#define GRANULARITY  0x10000

typedef struct { uintptr_t base; size_t size; DWORD protect; } reservation;
static reservation *g_res; static int g_res_count, g_res_cap;
static pthread_mutex_t g_vm_lock = PTHREAD_MUTEX_INITIALIZER;

static reservation *find_reservation(uintptr_t addr) {      // with g_vm_lock held
    for (int i = 0; i < g_res_count; i++)
        if (addr >= g_res[i].base && addr < g_res[i].base + g_res[i].size) return &g_res[i];
    return NULL;
}

static WINAPI void *VirtualAlloc(void *addr, size_t size, DWORD type, DWORD protect) {
    if (!size) { vx_set_last_error(87); return NULL; }
    pthread_mutex_lock(&g_vm_lock);
    reservation *r = addr ? find_reservation((uintptr_t)addr) : NULL;
    if (r && !(type & MEM_RESERVE)) {                       // committing inside an existing reservation
        pthread_mutex_unlock(&g_vm_lock);
        return (void *)((uintptr_t)addr & ~(uintptr_t)0xfff);
    }
    if (addr && !(type & MEM_RESERVE)) { pthread_mutex_unlock(&g_vm_lock); vx_set_last_error(487 /* ERROR_INVALID_ADDRESS */); return NULL; }
    mach_vm_address_t a = (mach_vm_address_t)addr & ~(mach_vm_address_t)(GRANULARITY - 1);
    mach_vm_size_t len = ((mach_vm_address_t)addr + size - a + 0xfff) & ~(mach_vm_size_t)0xfff;
    kern_return_t kr = addr
        ? mach_vm_allocate(mach_task_self(), &a, len, VM_FLAGS_FIXED)
        : mach_vm_map(mach_task_self(), &a, len, GRANULARITY - 1, VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, 0,
                      VM_PROT_ALL, VM_PROT_ALL, VM_INHERIT_COPY);
    if (kr != KERN_SUCCESS) {
        pthread_mutex_unlock(&g_vm_lock);
        vx_set_last_error(addr ? 487 : 8 /* ERROR_NOT_ENOUGH_MEMORY */);
        return NULL;
    }
    mach_vm_protect(mach_task_self(), a, len, 0, VM_PROT_ALL);
    if (g_res_count == g_res_cap) { g_res_cap = g_res_cap ? g_res_cap * 2 : 256; g_res = realloc(g_res, g_res_cap * sizeof *g_res); }
    g_res[g_res_count++] = (reservation){ (uintptr_t)a, (size_t)len, protect };
    pthread_mutex_unlock(&g_vm_lock);
    return (void *)a;
}
static WINAPI BOOL VirtualFree(void *addr, size_t size, DWORD type) {
    pthread_mutex_lock(&g_vm_lock);
    reservation *r = find_reservation((uintptr_t)addr);
    BOOL ok = 0;
    if (r && (type & MEM_RELEASE) && !size && r->base == (uintptr_t)addr) {
        mach_vm_deallocate(mach_task_self(), r->base, r->size);
        *r = g_res[--g_res_count];
        ok = 1;
    } else if (r && (type & MEM_DECOMMIT)) {
        uintptr_t start = (uintptr_t)addr & ~(uintptr_t)0xfff;
        size_t len = size ? (((uintptr_t)addr + size + 0xfff) & ~(uintptr_t)0xfff) - start : r->base + r->size - start;
        mach_vm_address_t a = start;                       // fresh zero pages over the range
        ok = mach_vm_allocate(mach_task_self(), &a, len, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) == KERN_SUCCESS;
        if (ok) mach_vm_protect(mach_task_self(), a, len, 0, VM_PROT_ALL);
    }
    pthread_mutex_unlock(&g_vm_lock);
    if (!ok) vx_set_last_error(487);
    return ok;
}
static WINAPI BOOL VirtualProtect(void *addr, size_t size, DWORD prot, DWORD *old) {
    (void)size;
    pthread_mutex_lock(&g_vm_lock);
    reservation *r = find_reservation((uintptr_t)addr);
    if (old) *old = r ? r->protect : PAGE_EXECUTE_READWRITE;
    if (r) r->protect = prot;
    pthread_mutex_unlock(&g_vm_lock);
    return 1;
}
typedef struct { void *base, *alloc_base; DWORD alloc_protect; uint16_t partition, pad; size_t region; DWORD state, protect, type, pad2; } MEMORY_BASIC_INFORMATION;
static WINAPI size_t VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, size_t len) {
    if (len < sizeof *mbi) { vx_set_last_error(24 /* ERROR_BAD_LENGTH */); return 0; }
    uintptr_t page = (uintptr_t)addr & ~(uintptr_t)0xfff;
    memset(mbi, 0, sizeof *mbi);
    mbi->base = (void *)page;
    const vx_image *img = vx_image_at(page);
    pthread_mutex_lock(&g_vm_lock);
    reservation *r = find_reservation(page);
    if (r) {
        mbi->alloc_base = (void *)r->base; mbi->alloc_protect = mbi->protect = r->protect;
        mbi->region = r->base + r->size - page; mbi->state = MEM_COMMIT; mbi->type = MEM_PRIVATE;
    }
    pthread_mutex_unlock(&g_vm_lock);
    if (r) return sizeof *mbi;
    if (img) {
        mbi->alloc_base = img->base; mbi->alloc_protect = mbi->protect = PAGE_EXECUTE_READWRITE;
        mbi->region = (uintptr_t)img->base + img->size - page; mbi->state = MEM_COMMIT; mbi->type = MEM_IMAGE;
        return sizeof *mbi;
    }
    // Anything else: ask Mach whether the page is mapped, and how far the free gap extends if not.
    mach_vm_address_t a = page; mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info; mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
    kern_return_t kr = mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &obj);
    if (kr == KERN_SUCCESS && a <= page) {
        mbi->alloc_base = (void *)a; mbi->alloc_protect = mbi->protect = PAGE_EXECUTE_READWRITE;
        mbi->region = a + sz - page; mbi->state = MEM_COMMIT; mbi->type = MEM_PRIVATE;
    } else {
        mbi->state = MEM_FREE; mbi->protect = 0x01;      // PAGE_NOACCESS
        mbi->region = (kr == KERN_SUCCESS ? a : 0x7fffffff0000ull) - page;
    }
    return sizeof *mbi;
}

// Heaps: one process heap, backed by malloc.
#define HEAP_ZERO_MEMORY 0x8
static WINAPI HANDLE GetProcessHeap(void) { return (HANDLE)(intptr_t)0x48454150; }
static WINAPI HANDLE HeapCreate(DWORD f, size_t a, size_t b) { (void)f; (void)a; (void)b; return GetProcessHeap(); }
static WINAPI BOOL HeapDestroy(HANDLE h) { (void)h; return 1; }
static WINAPI void *HeapAlloc(HANDLE h, DWORD flags, size_t n) { (void)h; return flags & HEAP_ZERO_MEMORY ? calloc(1, n ? n : 1) : malloc(n ? n : 1); }
static WINAPI void *HeapReAlloc(HANDLE h, DWORD flags, void *p, size_t n) {
    (void)h;
    size_t old = p ? malloc_size(p) : 0;
    void *q = realloc(p, n ? n : 1);
    if (q && (flags & HEAP_ZERO_MEMORY) && n > old) memset((char *)q + old, 0, n - old);
    return q;
}
static WINAPI BOOL HeapFree(HANDLE h, DWORD f, void *p) { (void)h; (void)f; free(p); return 1; }
static WINAPI size_t HeapSize(HANDLE h, DWORD f, const void *p) { (void)h; (void)f; return malloc_size(p); }

// ---- messages and debugging output ----------------------------------------------------------------------------------------
#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x100
static WINAPI DWORD FormatMessageA(DWORD flags, const void *src, DWORD id, DWORD lang, char *buf, DWORD size, void *args) {
    (void)src; (void)lang; (void)args;
    char text[64];
    int n = snprintf(text, sizeof text, "Windows error %u", id);
    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) { *(char **)buf = strdup(text); return (DWORD)n; }
    if (!buf || size <= (DWORD)n) { vx_set_last_error(122 /* ERROR_INSUFFICIENT_BUFFER */); return 0; }
    memcpy(buf, text, (size_t)n + 1);
    return (DWORD)n;
}
static WINAPI void *LocalAlloc(DWORD flags, size_t n) { return flags & 0x40 /* LMEM_ZEROINIT */ ? calloc(1, n ? n : 1) : malloc(n ? n : 1); }
static WINAPI void *LocalFree(void *p) { free(p); return NULL; }
static WINAPI void OutputDebugStringA(const char *s) { if (vx_trace) vx_log("debug: %s", s); }

// ---- system info -----------------------------------------------------------------------------------------------------------
typedef struct { uint16_t arch, reserved; DWORD page; void *min_addr, *max_addr; uint64_t mask; DWORD ncpu, type, gran; uint16_t level, rev; } SYSTEM_INFO;
static WINAPI void GetSystemInfo(SYSTEM_INFO *si) {
    memset(si, 0, sizeof *si);
    si->arch = 9;   // PROCESSOR_ARCHITECTURE_AMD64 (the program runs as x86-64 under Rosetta)
    si->page = 4096; si->gran = 65536;
    si->min_addr = (void *)0x10000; si->max_addr = (void *)0x7ffffffeffff;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    si->ncpu = (DWORD)n; si->mask = n >= 64 ? ~0ull : (1ull << n) - 1;
    si->type = 8664; si->level = 6;
}
static WINAPI void GetNativeSystemInfo(SYSTEM_INFO *si) { GetSystemInfo(si); }

static const vx_export k32[] = {
    VX_EXPORT(GetStdHandle), VX_EXPORT(WriteFile), VX_EXPORT(WriteConsoleA), VX_EXPORT(WriteConsoleW),
    VX_EXPORT(ExitProcess), VX_EXPORT(TerminateProcess),
    VX_EXPORT(GetLastError), VX_EXPORT(SetLastError), VX_EXPORT(IsDebuggerPresent),
    VX_EXPORT(GetCurrentProcessId), VX_EXPORT(GetCurrentThreadId), VX_EXPORT(GetCurrentProcess), VX_EXPORT(GetCurrentThread),
    VX_EXPORT(GetCommandLineA), VX_EXPORT(GetCommandLineW), VX_EXPORT(GetStartupInfoA), VX_EXPORT(GetStartupInfoW),
    VX_EXPORT(Sleep), VX_EXPORT(QueryPerformanceFrequency), VX_EXPORT(QueryPerformanceCounter),
    VX_EXPORT(GetTickCount), VX_EXPORT(GetTickCount64), VX_EXPORT(GetSystemTimeAsFileTime), VX_EXPORT(GetSystemTimePreciseAsFileTime),
    VX_EXPORT(TlsAlloc), VX_EXPORT(TlsFree), VX_EXPORT(TlsGetValue), VX_EXPORT(TlsSetValue),
    VX_EXPORT(VirtualAlloc), VX_EXPORT(VirtualFree), VX_EXPORT(VirtualProtect), VX_EXPORT(VirtualQuery),
    VX_EXPORT(GetProcessHeap), VX_EXPORT(HeapCreate), VX_EXPORT(HeapDestroy), VX_EXPORT(HeapAlloc), VX_EXPORT(HeapReAlloc),
    VX_EXPORT(HeapFree), VX_EXPORT(HeapSize),
    VX_EXPORT(GetSystemInfo), VX_EXPORT(GetNativeSystemInfo),
    VX_EXPORT(FormatMessageA), VX_EXPORT(LocalAlloc), VX_EXPORT(LocalFree), VX_EXPORT(OutputDebugStringA),
    VX_EXPORT(SetStdHandle), VX_EXPORT(GetConsoleMode), VX_EXPORT(SetConsoleMode), VX_EXPORT(ReadConsoleW),
    VX_EXPORT(GetExitCodeProcess), VX_EXPORT(EncodePointer), VX_EXPORT(DecodePointer),
    { "EncodeSystemPointer", (void *)EncodePointer }, { "DecodeSystemPointer", (void *)DecodePointer },
    VX_EXPORT(IsProcessorFeaturePresent), VX_EXPORT(GetLocalTime), VX_EXPORT(GetSystemTime),
    VX_EXPORT(GetVersion), VX_EXPORT(GetVersionExA), VX_EXPORT(GetVersionExW),
    VX_EXPORT(SetErrorMode), VX_EXPORT(GetErrorMode), VX_EXPORT(GetCurrentProcessorNumber), VX_EXPORT(GetPriorityClass),
    VX_EXPORT(SetPriorityClass), VX_EXPORT(HeapValidate), VX_EXPORT(DeviceIoControl), VX_EXPORT(K32GetMappedFileNameW),
    VX_EXPORT(RtlCaptureStackBackTrace), VX_EXPORT(CreateProcessA), VX_EXPORT(CreateProcessW), VX_EXPORT(WinExec),
    VX_EXPORT(GlobalMemoryStatusEx), VX_EXPORT(GetComputerNameA), VX_EXPORT(GetComputerNameW), VX_EXPORT(GetComputerNameExA),
    VX_EXPORT(GetComputerNameExW), VX_EXPORT(FileTimeToSystemTime), VX_EXPORT(SystemTimeToFileTime), VX_EXPORT(FileTimeToLocalFileTime),
    VX_EXPORT(SystemTimeToTzSpecificLocalTime), VX_EXPORT(GetTimeZoneInformation),
    VX_EXPORT(VerSetConditionMask), VX_EXPORT(VerifyVersionInfoA), VX_EXPORT(VerifyVersionInfoW),
    VX_EXPORT(GetConsoleScreenBufferInfo), VX_EXPORT(SetConsoleTextAttribute), VX_EXPORT(SetConsoleTitleA), VX_EXPORT(SetConsoleTitleW),
    VX_EXPORT(GetConsoleWindow), VX_EXPORT(AllocConsole), VX_EXPORT(FreeConsole), VX_EXPORT(SetConsoleCtrlHandler), VX_EXPORT(GetConsoleCP),
    VX_EXPORT(HeapSetInformation), VX_EXPORT(SetHandleCount), VX_EXPORT(GetThreadTimes), VX_EXPORT(GetProcessTimes),
};

// shell32: opening documents and URLs is not available yet; programs see the "no association" failure.
static WINAPI void *ShellExecuteA(void *w, const char *op, const char *file, const char *params, const char *dir, int show) {
    (void)w; (void)op; (void)params; (void)dir; (void)show;
    vx_log("ShellExecute(%s) is not supported yet", file ? file : "");
    return (void *)31;                        // SE_ERR_NOASSOC
}
static WINAPI void *ShellExecuteW(void *w, const WCHAR *op, const WCHAR *file, const WCHAR *params, const WCHAR *dir, int show) {
    (void)op; (void)params; (void)dir;
    char *f = vx_utf8_dup(file);
    void *r = ShellExecuteA(w, NULL, f, NULL, NULL, show);
    free(f);
    return r;
}
// advapi32: security objects exist but grant nothing special (the user is not an administrator); event tracing
// accepts and drops events.
static WINAPI BOOL InitializeSecurityDescriptor(void *sd, DWORD rev) { (void)rev; memset(sd, 0, 40); return 1; }
static WINAPI BOOL SetSecurityDescriptorDacl(void *sd, BOOL present, void *acl, BOOL defaulted) { (void)sd; (void)present; (void)acl; (void)defaulted; return 1; }
static WINAPI BOOL AllocateAndInitializeSid(void *auth, uint8_t n, DWORD a0, DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6, DWORD a7, void **sid) {
    (void)auth; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    *sid = calloc(1, 8 + 4 * (size_t)n);
    return *sid != NULL;
}
static WINAPI void *FreeSid(void *sid) { free(sid); return NULL; }
static WINAPI BOOL CheckTokenMembership(HANDLE token, void *sid, BOOL *member) { (void)token; (void)sid; *member = 0; return 1; }
static WINAPI DWORD EventRegister(const void *guid, void *cb, void *ctx, uint64_t *h) { (void)guid; (void)cb; (void)ctx; *h = 1; return 0; }
static WINAPI DWORD EventUnregister(uint64_t h) { (void)h; return 0; }
static WINAPI DWORD EventWrite(uint64_t h, const void *d, DWORD n, void *data) { (void)h; (void)d; (void)n; (void)data; return 0; }
static WINAPI DWORD EventWriteTransfer(uint64_t h, const void *d, const void *a, const void *r, DWORD n, void *data) {
    (void)h; (void)d; (void)a; (void)r; (void)n; (void)data; return 0;
}
static WINAPI BOOL EventEnabled(uint64_t h, const void *d) { (void)h; (void)d; return 0; }
static const vx_export advapi_misc[] = {
    VX_EXPORT(InitializeSecurityDescriptor), VX_EXPORT(SetSecurityDescriptorDacl), VX_EXPORT(AllocateAndInitializeSid),
    VX_EXPORT(FreeSid), VX_EXPORT(CheckTokenMembership), VX_EXPORT(EventRegister), VX_EXPORT(EventUnregister),
    VX_EXPORT(EventWrite), VX_EXPORT(EventWriteTransfer), VX_EXPORT(EventEnabled), VX_EXPORT(GetUserNameA), VX_EXPORT(GetUserNameW),
};

static const vx_export shell32[] = { VX_EXPORT(ShellExecuteA), VX_EXPORT(ShellExecuteW) };

void vx_kernel32_init(void) {
    vx_register_module("advapi32.dll", advapi_misc, sizeof advapi_misc / sizeof advapi_misc[0]);
    vx_register_module("kernelbase.dll", advapi_misc, sizeof advapi_misc / sizeof advapi_misc[0]);
    vx_register_module("shell32.dll", shell32, sizeof shell32 / sizeof shell32[0]);
    vx_register_module("kernel32.dll", k32, sizeof k32 / sizeof k32[0]);
    vx_register_module("kernelbase.dll", k32, sizeof k32 / sizeof k32[0]);
}
