// kernel32: the Windows core API, implemented directly on macOS.
#include <errno.h>
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
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
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
    if (v >= 0x10000 && v < 0x20000) return (int)(v - 0x10000);   // file handles: 0x10000 + fd (descriptors below 65536)
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

int vx_pipe_write(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov, BOOL *ok);
DWORD vx_file_io_error(int fd, int e);                             // files.c
static WINAPI BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *overlapped) {
    BOOL piped;
    if (vx_pipe_write(h, buf, n, written, overlapped, &piped)) return piped;
    int fd = vx_handle_fd(h);
    if (fd < 0) { vx_set_last_error(6 /* ERROR_INVALID_HANDLE */); return 0; }
    // OVERLAPPED: the transfer happens at its offset, at once, and completes like asynchronous I/O would; the file
    // pointer then follows the bytes written, as a synchronous handle's does (ReadFile in files.c alike). An offset of
    // 0xFFFFFFFF:0xFFFFFFFF writes at the end of the file.
    const DWORD *o = overlapped;
    off_t at = o ? (off_t)o[4] | (off_t)o[5] << 32 : 0;
    if (o && fd > 2 && o[4] == 0xffffffffu && o[5] == 0xffffffffu) { struct stat st; at = fstat(fd, &st) ? 0 : st.st_size; }
    ssize_t r = o && fd > 2 ? pwrite(fd, buf, n, at) : write(fd, buf, n);
    if (written) *written = r > 0 ? (DWORD)r : 0;
    if (r < 0) { vx_set_last_error(vx_file_io_error(fd, errno)); return 0; }
    if (o && fd > 2) lseek(fd, at + r, SEEK_SET);
    if (overlapped) vx_io_completed(h, overlapped, (DWORD)r, 0);
    return 1;
}

// ReadFileEx / WriteFileEx (Microsoft's documentation): asynchronous I/O at the OVERLAPPED's offset whose result
// comes as a call of the completion routine (error, bytes, OVERLAPPED) on the issuing thread, queued as an APC that
// runs in its next alertable wait (SleepEx, WaitForSingleObjectEx, WaitForMultipleObjectsEx). The
// OVERLAPPED's hEvent belongs to the program and is not touched; Internal and InternalHigh get the status and the
// bytes transferred. Here the transfer itself happens at once. Reading at or past the end fails with ERROR_HANDLE_EOF
// and queues nothing. Files only (pipes and sockets: ERROR_NOT_SUPPORTED, as before). Steam's content system
// (steamclient64.dll) imports WriteFileEx.
typedef struct { void (WINAPI *routine)(DWORD, DWORD, void *); DWORD error, bytes; void *ov; } io_completion;
static WINAPI void io_completion_apc(uintptr_t data) {
    io_completion *c = (io_completion *)data;
    c->routine(c->error, c->bytes, c->ov);
    free(c);
}
static BOOL file_io_ex(HANDLE h, void *buf, DWORD n, void *ov, void *routine, int write) {
    int fd = vx_handle_fd(h);
    struct stat st;
    if (!ov || !routine) { vx_set_last_error(87 /* ERROR_INVALID_PARAMETER */); return 0; }
    if (fd < 0) { vx_set_last_error(6 /* ERROR_INVALID_HANDLE */); return 0; }
    if (fd <= 2 || fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        vx_log("%s on a handle that is not a file is not supported yet", write ? "WriteFileEx" : "ReadFileEx");
        vx_set_last_error(50 /* ERROR_NOT_SUPPORTED */);
        return 0;
    }
    uint64_t *o = ov;
    const DWORD *off = ov;
    off_t at = (off_t)off[4] | (off_t)off[5] << 32;
    if (write && off[4] == 0xffffffffu && off[5] == 0xffffffffu) at = st.st_size;   // (the end of the file)
    ssize_t r = write ? pwrite(fd, buf, n, at) : pread(fd, buf, n, at);
    if (r < 0) { vx_set_last_error(vx_file_io_error(fd, errno)); return 0; }
    if (!write && !r && n) { vx_set_last_error(38 /* ERROR_HANDLE_EOF */); return 0; }
    o[0] = 0;                                                        // Internal: STATUS_SUCCESS
    o[1] = (uint64_t)r;                                              // InternalHigh: bytes transferred
    static DWORD (WINAPI *queue)(void (WINAPI *)(uintptr_t), HANDLE, uintptr_t);
    if (!queue) queue = (DWORD (WINAPI *)(void (WINAPI *)(uintptr_t), HANDLE, uintptr_t))vx_find_export("kernel32.dll", "QueueUserAPC");
    io_completion *c = malloc(sizeof *c);
    *c = (io_completion){ (void (WINAPI *)(DWORD, DWORD, void *))routine, 0, (DWORD)r, ov };
    if (!queue(io_completion_apc, (HANDLE)(intptr_t)-2, (uintptr_t)c)) { free(c); return 0; }
    vx_set_last_error(0);
    return 1;
}
static WINAPI BOOL ReadFileEx(HANDLE h, void *buf, DWORD n, void *ov, void *routine) { return file_io_ex(h, buf, n, ov, routine, 0); }
static WINAPI BOOL WriteFileEx(HANDLE h, const void *buf, DWORD n, void *ov, void *routine) { return file_io_ex(h, (void *)buf, n, ov, routine, 1); }

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

static WINAPI void ExitProcess(DWORD code) {
    {                                                              // (always: why a program ended is worth a line)
        const vx_image *m = vx_image_at((uint64_t)(uintptr_t)VX_RETURN_ADDRESS());
        vx_log("ExitProcess(%u) from %s+%#llx", code, m ? m->name : "?", m ? (unsigned long long)((uint8_t *)VX_RETURN_ADDRESS() - m->base) : 0ull);
    }
    vx_profile_finish();
    vx_jobs_process_exit();
#ifdef __aarch64__
    { void xa_report(void); xa_report(); }                       // (statically recompiled code: how much ran, src/arm64.c)
#endif
    fflush(stdout); fflush(stderr); _exit((int)code);
}
int vx_process_info(HANDLE h, DWORD *code);
HANDLE vx_process_handle(int pid, int child, HANDLE *second);
static WINAPI BOOL TerminateProcess(HANDLE h, DWORD code) {
    if ((intptr_t)h == -1) {                                       // (who ends it: ExitProcess below only sees this function)
        const vx_image *m = vx_image_at((uint64_t)(uintptr_t)VX_RETURN_ADDRESS());
        vx_log("TerminateProcess(current process, %u) from %s+%#llx, thread %u", (unsigned)code, m ? m->name : "?",
               m ? (unsigned long long)((uint8_t *)VX_RETURN_ADDRESS() - m->base) : 0ull, (unsigned)vx_current_tid());
        vx_log_caller_stack(16);                                   // (the callers above: the return address is often a helper)
        vx_trace_ring_dump("TerminateProcess");                    // (VXRT_TRACE_CALLS_RING: the calls before it)
        ExitProcess(code); return 1;
    }
    int pid = vx_process_info(h, NULL);
    if (pid <= 0) { vx_set_last_error(6); return 0; }
    vx_log("TerminateProcess: ending process %d with code %u", pid, (unsigned)code);   // (always: otherwise it just vanishes)
    kill(pid, SIGKILL);
    return 1;
}

// ---- errors, debugging, process info -------------------------------------------------------------------------------
static WINAPI DWORD GetLastError(void) { return vx_get_last_error(); }
static WINAPI void SetLastError(DWORD e) { vx_set_last_error(e); }
// VXRT_DEBUGGER=1 says a debugger is attached: programs then send their logs to OutputDebugString (shown with
// VXRT_TRACE=1). Breakpoints still go to the program's handlers.
static WINAPI BOOL IsDebuggerPresent(void) { static int on = -1; if (on < 0) on = getenv("VXRT_DEBUGGER") && atoi(getenv("VXRT_DEBUGGER")); return on; }
static WINAPI BOOL CheckRemoteDebuggerPresent(HANDLE p, BOOL *present) { (void)p; *present = 0; return 1; }
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
    if ((intptr_t)h == -1) { *code = 259; return 1; }          // STILL_ACTIVE
    if (vx_process_info(h, code) < 0) { vx_set_last_error(6); return 0; }
    return 1;
}
static WINAPI DWORD GetProcessId(HANDLE h) { return (intptr_t)h == -1 ? (DWORD)getpid() : (DWORD)vx_process_info(h, NULL); }

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
// OSVERSIONINFOEX adds the service pack (none), the suite mask (VER_SUITE_SINGLEUSERTS: one interactive session, as
// on every workstation not in application-server mode) and the product type (VER_NT_WORKSTATION, as
// VerifyVersionInfo below and shlwapi's IsOS in osinfo.c answer).
typedef struct { DWORD size, major, minor, build, platform; char csd[128]; } OSVERSIONINFOA;
typedef struct { DWORD size, major, minor, build, platform; WCHAR csd[128]; } OSVERSIONINFOW;
static WINAPI DWORD GetVersion(void) { return 10u | (0u << 8) | (19045u << 16); }
static void version_ex(uint8_t *ex) {                               // wServicePackMajor/Minor, wSuiteMask, wProductType, wReserved
    memset(ex, 0, 8);
    ex[4] = 0x00; ex[5] = 0x01;                                     // wSuiteMask 0x0100 (little-endian)
    ex[6] = 1;
}
static WINAPI BOOL GetVersionExA(OSVERSIONINFOA *v) {
    v->major = 10; v->minor = 0; v->build = 19045; v->platform = 2;   // VER_PLATFORM_WIN32_NT
    v->csd[0] = 0;
    if (v->size >= sizeof *v + 8) version_ex((uint8_t *)v + sizeof *v);
    return 1;
}
static WINAPI BOOL GetVersionExW(OSVERSIONINFOW *v) {
    v->major = 10; v->minor = 0; v->build = 19045; v->platform = 2;
    v->csd[0] = 0;
    if (v->size >= sizeof *v + 8) version_ex((uint8_t *)v + sizeof *v);
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
// RtlVerifyVersionInfo: the same test on an RTL_OSVERSIONINFOEXW, as an NTSTATUS: STATUS_SUCCESS, or
// STATUS_REVISION_MISMATCH when the version does not meet it, STATUS_INVALID_PARAMETER for no structure or no test.
static WINAPI uint32_t RtlVerifyVersionInfo(const uint8_t *v, DWORD type, uint64_t mask) {
    if (!v || !type || !mask) return 0xC000000D;
    return VerifyVersionInfoW(v, type, mask) ? 0 : 0xC0000059;
}
// RtlGetVersion: GetVersionExW's answer as an NTSTATUS (callers use it to see past compatibility shims).
static WINAPI uint32_t RtlGetVersion(OSVERSIONINFOW *v) {
    if (!v) return 0xC000000D;                                       // STATUS_INVALID_PARAMETER
    GetVersionExW(v);
    return 0;
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
// (RtlCaptureStackBackTrace: src/seh.c)

// Starting other processes comes with a later milestone; programs see the failure Windows reports for a
// missing executable.
// CreateProcess: the program runs in another vxrt process (this runtime's own executable), with the Windows command
// line split into arguments, the environment block given (plus this runtime's VXRT_ settings), and the working
// directory. PROCESS_INFORMATION: process and thread handles (both wait for the process), process and thread IDs.
static char **split_command_line(const char *cmd, int *argc) {
    WCHAR *w = vx_utf16_dup(cmd);
    WCHAR **(WINAPI *split)(const WCHAR *, int *) = (WCHAR **(WINAPI *)(const WCHAR *, int *))vx_find_export("shell32.dll", "CommandLineToArgvW");
    WCHAR **wargv = split(w, argc);
    char **argv = calloc((size_t)*argc + 1, sizeof *argv);
    for (int i = 0; i < *argc; i++) argv[i] = vx_utf8_dup(wargv[i]);
    free(wargv); free(w);
    return argv;
}
static int find_program(const char *name, char *unix_path, size_t cap) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", name);
    size_t n = strlen(tmp);
    if (n < 4 || (strcasecmp(tmp + n - 4, ".exe") && strcasecmp(tmp + n - 4, ".com") && strcasecmp(tmp + n - 4, ".bat")))
        snprintf(tmp + n, sizeof tmp - n, ".exe");
    struct stat st;
    if (strchr(tmp, '\\') || strchr(tmp, '/') || strchr(tmp, ':'))
        return vx_unix_path(tmp, unix_path, cap) && !stat(unix_path, &st);
    // A bare name: the program's directory, the current directory, then system32.
    char exe[1024], dir[1024];
    const vx_image *m = vx_image_at((uint64_t)(uintptr_t)vx_proc.image_base);
    snprintf(dir, sizeof dir, "%s", m && m->windows_path ? m->windows_path : "C:\\");
    char *slash = strrchr(dir, '\\'); if (slash) slash[1] = 0;
    const char *dirs[3] = { dir, NULL, "C:\\windows\\system32\\" };
    char cwd[1024]; vx_get_cwd(cwd, sizeof cwd); strcat(cwd, "\\"); dirs[1] = cwd;
    for (int i = 0; i < 3; i++) {
        snprintf(exe, sizeof exe, "%s%s", dirs[i], tmp);
        if (vx_unix_path(exe, unix_path, cap) && !stat(unix_path, &st)) return 1;
    }
    return 0;
}
// The child's environment: the block given (else this process's), this runtime's settings, and the command line the
// child is to have word for word (VXRT_COMMAND_LINE, which the child takes out again: loader.c).
static char **environment_from_block(const void *block, int wide, const char *command_line) {
    extern char **environ;
    int n = 0, cap = 64;
    char **env = malloc((size_t)cap * sizeof *env);
    if (block) {
        if (wide) for (const WCHAR *p = block; *p; p += vx_wcslen(p) + 1) { if (n + 2 >= cap) env = realloc(env, (size_t)(cap *= 2) * sizeof *env); env[n++] = vx_utf8_dup(p); }
        else for (const char *p = block; *p; p += strlen(p) + 1) { if (n + 2 >= cap) env = realloc(env, (size_t)(cap *= 2) * sizeof *env); env[n++] = strdup(p); }
    }
    for (char **e = environ; *e; e++) {                           // the runtime's settings, and everything when no block
        if (block && strncmp(*e, "VXRT_", 5) && strncmp(*e, "HOME=", 5) && strncmp(*e, "TMPDIR=", 7)) continue;
        if (!strncmp(*e, "VXRT_COMMAND_LINE=", 18)) continue;
        if (n + 2 >= cap) env = realloc(env, (size_t)(cap *= 2) * sizeof *env);
        env[n++] = strdup(*e);
    }
    env = realloc(env, (size_t)(n + 3) * sizeof *env);
    if (!getenv("VXRT_CHILD")) env[n++] = strdup("VXRT_CHILD=1");   // its log lines say which process they are from
    if (command_line) {
        env[n] = malloc(strlen(command_line) + 19);
        strcpy(env[n], "VXRT_COMMAND_LINE="); strcat(env[n++], command_line);
    }
    env[n] = NULL;
    return env;
}
// A native 32-bit x86 program (PE32, machine i386, no CLR header): this runtime runs 64-bit code only, so CreateProcess
// fails as Windows does for an image it cannot run, ERROR_BAD_EXE_FORMAT, before any process is started (the Windows
// Steam client starts the 32-bit gldriverquery.exe and vulkandriverquery.exe, and carries on without their answers).
// Managed (.NET) programs, whatever their header says, are left to the loader.
static int native_32bit_program(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t h[0x200];
    size_t n = fread(h, 1, sizeof h, f);
    uint32_t lfanew = n >= 0x40 ? (uint32_t)h[0x3c] | (uint32_t)h[0x3d] << 8 | (uint32_t)h[0x3e] << 16 | (uint32_t)h[0x3f] << 24 : 0;
    uint8_t pe[24 + 96 + 16 * 8];
    int r = n >= 0x40 && h[0] == 'M' && h[1] == 'Z' && !fseek(f, (long)lfanew, SEEK_SET) && fread(pe, 1, sizeof pe, f) == sizeof pe &&
            !memcmp(pe, "PE\0\0", 4) && (pe[4] | pe[5] << 8) == 0x014c && (pe[24] | pe[25] << 8) == 0x10b;
    if (r) {
        uint32_t ndirs, clr_size;
        memcpy(&ndirs, pe + 24 + 92, 4);                       // NumberOfRvaAndSizes; directory 14 is the CLR header
        memcpy(&clr_size, pe + 24 + 96 + 14 * 8 + 4, 4);
        if (ndirs > 14 && clr_size) r = 0;
    }
    fclose(f);
    return r;
}
// si: the program's STARTUPINFO (A and W share the layout up to hStdError). With STARTF_USESTDHANDLES (0x100) the
// child's standard handles are the given ones: pipes and files here are descriptors, so they are dup2'd into 0, 1, 2.
static BOOL create_process(const char *app, const char *cmd, const void *env_block, int wide_env, const char *dir, const void *si, void **pi) {
    int argc = 0;
    char **args = split_command_line(cmd && *cmd ? cmd : app, &argc);
    char exe[1024];
    if (!find_program(app && *app ? app : argc ? args[0] : "", exe, sizeof exe)) {
        vx_log("CreateProcess: %s not found", app && *app ? app : cmd);
        vx_set_last_error(2);
        return 0;
    }
    if (native_32bit_program(exe)) {
        vx_log("CreateProcess: %s is a 32-bit x86 program, which this runtime does not run (ERROR_BAD_EXE_FORMAT)", exe);
        for (int i = 0; i < argc; i++) free(args[i]);
        free(args);
        vx_set_last_error(193 /* ERROR_BAD_EXE_FORMAT */);
        return 0;
    }
    char self[1024]; uint32_t len = sizeof self;
    if (_NSGetExecutablePath(self, &len)) { vx_set_last_error(8); return 0; }
    char **argv = calloc((size_t)argc + 3, sizeof *argv);
    argv[0] = self; argv[1] = exe;
    for (int i = 1; i < argc; i++) argv[i + 1] = args[i];
    char **envp = environment_from_block(env_block, wide_env, cmd && *cmd ? cmd : app);   // (no command line: the program's name is it)
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    char cwd_win[1024], cwd_unix[1024];
    if (dir && *dir) snprintf(cwd_win, sizeof cwd_win, "%s", dir); else vx_get_cwd(cwd_win, sizeof cwd_win);
#if !VX_XROS   // (visionOS: an app starts no processes; posix_spawn fails there, VISIONOS_PORT.md)
    if (vx_unix_path(cwd_win, cwd_unix, sizeof cwd_unix)) posix_spawn_file_actions_addchdir_np(&fa, cwd_unix);
#else
    (void)cwd_unix;
#endif
    if (si && (*(const DWORD *)((const char *)si + 60) & 0x100)) {
        for (int k = 0; k < 3; k++) {
            HANDLE h = *(HANDLE const *)((const char *)si + 80 + 8 * k);       // hStdInput, hStdOutput, hStdError
            int fd = h ? vx_handle_fd(h) : -1;
            if (fd >= 0 && fd != k) posix_spawn_file_actions_adddup2(&fa, fd, k);
        }
    }
    pid_t pid;
    int r = posix_spawn(&pid, self, &fa, NULL, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    for (int i = 0; envp[i]; i++) free(envp[i]);
    free(envp); free(argv);
    for (int i = 0; i < argc; i++) free(args[i]);
    free(args);
    if (r) { vx_set_last_error(vx_error_from_errno(r)); return 0; }
    if (vx_trace) vx_log("CreateProcess: %s as process %d (command line: %s)", exe, (int)pid, cmd ? cmd : "");
    HANDLE thread;
    HANDLE process = vx_process_handle(pid, 1, &thread);
    if (pi) { pi[0] = process; pi[1] = thread; ((DWORD *)pi)[4] = (DWORD)pid; ((DWORD *)pi)[5] = (DWORD)pid; }
    return 1;
}
#define CREATE_UNICODE_ENVIRONMENT 0x400
static WINAPI BOOL CreateProcessA(const char *app, char *cmd, void *pa, void *ta, BOOL inherit, DWORD flags, void *env, const char *dir, void *si, void *pi) {
    (void)pa; (void)ta; (void)inherit;
    return create_process(app, cmd, env, (flags & CREATE_UNICODE_ENVIRONMENT) != 0, dir, si, pi);
}
static WINAPI BOOL CreateProcessW(const WCHAR *app, WCHAR *cmd, void *pa, void *ta, BOOL inherit, DWORD flags, void *env, const WCHAR *dir, void *si, void *pi) {
    (void)pa; (void)ta; (void)inherit;
    char *a = vx_utf8_dup(app), *c = vx_utf8_dup(cmd), *d = vx_utf8_dup(dir);
    BOOL r = create_process(a, c, env, (flags & CREATE_UNICODE_ENVIRONMENT) != 0, d, si, pi);
    free(a); free(c); free(d);
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
        avail = ((uint64_t)vs.free_count + vs.inactive_count + vs.purgeable_count) * vm_kernel_page_size;   // (the kernel's pages: 16 KB, also under Rosetta)
    if (avail > total) avail = total;                  // (purgeable pages are also counted as active or inactive)
    m->load = total ? (DWORD)(100 - avail * 100 / total) : 0;
    m->total_phys = total; m->avail_phys = avail;
    m->total_page = total * 2; m->avail_page = avail + total;
    m->total_virtual = 0x7ffffffe0000ull; m->avail_virtual = 0x7ff000000000ull; m->avail_ext = 0;
    return 1;
}

// Computer and user names come from the Mac. `too_small` is the error for a buffer that is too small (the size
// needed, with the terminator, back in *size).
static int name_out_a(const char *name, char *buf, DWORD *size, int count_nul_on_success, DWORD too_small) {
    DWORD n = (DWORD)strlen(name);
    if (!buf || *size <= n) { *size = n + 1; vx_set_last_error(too_small); return 0; }
    memcpy(buf, name, n + 1);
    *size = count_nul_on_success ? n + 1 : n;
    return 1;
}
static int name_out_w(const char *name, WCHAR *buf, DWORD *size, int count_nul_on_success, DWORD too_small) {
    DWORD n = (DWORD)vx_utf8_to_utf16(name, strlen(name), NULL, 0);
    if (!buf || *size <= n) { *size = n + 1; vx_set_last_error(too_small); return 0; }
    vx_utf8_to_utf16(name, strlen(name), buf, n); buf[n] = 0;
    *size = count_nul_on_success ? n + 1 : n;
    return 1;
}
// The computer's names, from the Mac's host name: the DNS host name is its first label, as macOS has it; the DNS
// domain the rest, but for mDNS's "local" (a link-local name, no domain assigned to the computer: none, as on a
// Windows computer without a primary DNS suffix). The NetBIOS name follows the DNS host name the way Windows documents
// (Computer Names, DnsHostnameToComputerName): at most MAX_COMPUTERNAME_LENGTH (15) bytes of it, in upper case. A
// longer host name gives a truncated NetBIOS name (GetComputerName); the DNS forms of GetComputerNameEx keep it whole.
#define MAX_COMPUTERNAME_LENGTH 15
static char g_dns_host[256], g_dns_domain[256], g_netbios[MAX_COMPUTERNAME_LENGTH + 1];
static pthread_once_t g_names_once = PTHREAD_ONCE_INIT;
static void computer_names(void) {
    char host[256] = "";
    if (gethostname(host, sizeof host - 1)) host[0] = 0;
    char *dot = strchr(host, '.');
    if (dot) {
        *dot = 0;
        if (strcasecmp(dot + 1, "local")) snprintf(g_dns_domain, sizeof g_dns_domain, "%s", dot + 1);
    }
    snprintf(g_dns_host, sizeof g_dns_host, "%s", host);
    size_t n = strlen(host);
    if (n > MAX_COMPUTERNAME_LENGTH) {
        n = MAX_COMPUTERNAME_LENGTH;
        while (n && ((unsigned char)host[n] & 0xc0) == 0x80) n--;   // (not inside a character)
    }
    for (size_t i = 0; i < n; i++) g_netbios[i] = (char)toupper((unsigned char)host[i]);
    g_netbios[n] = 0;
}
static const char *computer_name(void) { pthread_once(&g_names_once, computer_names); return g_netbios; }
// COMPUTER_NAME_FORMAT: NetBIOS, DNS host name, DNS domain, fully qualified, then the same four "physical" ones (no
// cluster here: the same names); NULL for anything else.
static const char *computer_name_ex(int fmt, char *full, size_t cap) {
    pthread_once(&g_names_once, computer_names);
    switch (fmt) {
    case 0: case 4: return g_netbios;
    case 1: case 5: return g_dns_host;
    case 2: case 6: return g_dns_domain;
    case 3: case 7: snprintf(full, cap, "%s%s%s", g_dns_host, g_dns_domain[0] ? "." : "", g_dns_domain); return full;
    }
    return NULL;
}
static WINAPI BOOL GetComputerNameA(char *buf, DWORD *size) { return name_out_a(computer_name(), buf, size, 0, 111 /* ERROR_BUFFER_OVERFLOW */); }
static WINAPI BOOL GetComputerNameW(WCHAR *buf, DWORD *size) { return name_out_w(computer_name(), buf, size, 0, 111); }
static WINAPI BOOL GetComputerNameExA(int fmt, char *buf, DWORD *size) {
    char full[520];
    const char *name = computer_name_ex(fmt, full, sizeof full);
    if (!name || !size) { vx_set_last_error(87); return 0; }
    return name_out_a(name, buf, size, 0, 234 /* ERROR_MORE_DATA */);
}
static WINAPI BOOL GetComputerNameExW(int fmt, WCHAR *buf, DWORD *size) {
    char full[520];
    const char *name = computer_name_ex(fmt, full, sizeof full);
    if (!name || !size) { vx_set_last_error(87); return 0; }
    return name_out_w(name, buf, size, 0, 234);
}
static const char *user_name(void) { const char *u = getenv("USER"); return u && *u ? u : "vxrt"; }
static WINAPI BOOL GetUserNameA(char *buf, DWORD *size) { return name_out_a(user_name(), buf, size, 1, 234); }
static WINAPI BOOL GetUserNameW(WCHAR *buf, DWORD *size) { return name_out_w(user_name(), buf, size, 1, 234); }

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

// ---- TLS slots: 64 in the TEB (+0x1480) and, as on Windows since Windows 2000, 1024 more (TLS_EXPANSION_SLOTS) in a
// block the TEB points to (+0x1780, TlsExpansionSlots), made for a thread when it first stores a value there.
// Valve's tier0 stops with "Out of thread local storage!" when TlsAlloc fails: Steam's client takes more than 64.
#define TLS_MINIMUM_AVAILABLE 64
#define TLS_EXPANSION_SLOTS 1024
#define TLS_INDEXES (TLS_MINIMUM_AVAILABLE + TLS_EXPANSION_SLOTS)
static uint64_t g_tls_used[TLS_INDEXES / 64];
static WINAPI DWORD TlsAlloc(void) {
    for (DWORD i = 0; i < TLS_INDEXES; i++)
        if (!(__sync_fetch_and_or(&g_tls_used[i / 64], 1ull << (i % 64)) & (1ull << (i % 64)))) return i;
    vx_set_last_error(8 /* ERROR_NOT_ENOUGH_MEMORY */);
    return 0xffffffffu;   // TLS_OUT_OF_INDEXES
}
static WINAPI BOOL TlsFree(DWORD i) {
    if (i >= TLS_INDEXES || !(g_tls_used[i / 64] & (1ull << (i % 64)))) { vx_set_last_error(87); return 0; }
    __sync_fetch_and_and(&g_tls_used[i / 64], ~(1ull << (i % 64)));
    return 1;
}
static void **tls_expansion(int make) {
    void ***slot = (void ***)(vx_teb() + TEB_TLS_EXPANSION);
    if (!*slot && make) *slot = calloc(TLS_EXPANSION_SLOTS, sizeof(void *));
    return *slot;
}
static WINAPI void *TlsGetValue(DWORD i) {
    if (i >= TLS_INDEXES) { vx_set_last_error(87); return NULL; }
    vx_set_last_error(0);
    if (i >= TLS_MINIMUM_AVAILABLE) { void **x = tls_expansion(0); return x ? x[i - TLS_MINIMUM_AVAILABLE] : NULL; }
    return ((void **)(vx_teb() + TEB_TLS_SLOTS))[i];
}
// The value is also put at gs:0x1480 + 8i, where the slot is on Windows: code compiled for Windows reads it there
// directly (Mono's JIT does: Unity games read a garbage slot and recursed on NullReferenceExceptions until the stack
// ran out). On macOS that is a TSD slot pthread keys reach only after some 400 of them exist.
static WINAPI BOOL TlsSetValue(DWORD i, void *v) {
    if (i >= TLS_INDEXES) { vx_set_last_error(87); return 0; }
    if (i >= TLS_MINIMUM_AVAILABLE) {
        void **x = tls_expansion(1);
        if (!x) { vx_set_last_error(8); return 0; }
        x[i - TLS_MINIMUM_AVAILABLE] = v;
        return 1;
    }
    ((void **)(vx_teb() + TEB_TLS_SLOTS))[i] = v;
#ifndef __aarch64__
    __asm__ volatile("movq %0, %%gs:(%1)" :: "r"(v), "r"((uint64_t)TEB_TLS_SLOTS + 8ull * i) : "memory");
#endif
    return 1;
}

#ifdef __aarch64__
#define VX_PROT_RWX (VM_PROT_READ | VM_PROT_WRITE)    // (the program's code is read by the translator, never run)
void vx_code_freed(void *addr, size_t len);            // (its translations go: src/arm64.c)
#define VX_CODE_FREED(a, n) vx_code_freed(a, n)
#else
#define VX_PROT_RWX VM_PROT_ALL
#define VX_CODE_FREED(a, n) ((void)0)
#endif
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
// A reservation maps whole 64 KB units, as Windows uses them up: with only its pages mapped, every small one leaves
// a gap no aligned reservation fits in, and the kernel's search for room walks all of those (2 ms per call with
// Half-Life: Alyx's 60000 small blocks).
static size_t mapped_size(size_t n) { return (n + GRANULARITY - 1) & ~(size_t)(GRANULARITY - 1); }
static reservation *g_res; static int g_res_count, g_res_cap;
static pthread_mutex_t g_vm_lock = PTHREAD_MUTEX_INITIALIZER;

// Reservations are kept sorted by address (games make thousands; Half-Life: Alyx allocates and frees large
// blocks every frame at high resolutions, and a linear search took a core).
static reservation *find_reservation(uintptr_t addr) {      // with g_vm_lock held
    int lo = 0, hi = g_res_count;
    while (lo < hi) { int mid = (lo + hi) / 2; if (g_res[mid].base + g_res[mid].size <= addr) lo = mid + 1; else hi = mid; }
    return lo < g_res_count && g_res[lo].base <= addr ? &g_res[lo] : NULL;
}
static void res_insert(uintptr_t base, size_t size, DWORD protect) {
    if (g_res_count == g_res_cap) { g_res_cap = g_res_cap ? g_res_cap * 2 : 256; g_res = realloc(g_res, g_res_cap * sizeof *g_res); }
    int lo = 0, hi = g_res_count;
    while (lo < hi) { int mid = (lo + hi) / 2; if (g_res[mid].base < base) lo = mid + 1; else hi = mid; }
    memmove(g_res + lo + 1, g_res + lo, (size_t)(g_res_count - lo) * sizeof *g_res);
    g_res[lo] = (reservation){ base, size, protect };
    g_res_count++;
}
static void res_remove(reservation *r) {
    memmove(r, r + 1, (size_t)(g_res + g_res_count - r - 1) * sizeof *r);
    g_res_count--;
}
// Released blocks of 1 MB and more are kept a while for the next allocation of the same size: it gets fresh zero
// pages at the same place (one call), instead of the kernel looking for room in a crowded address space. For the
// program the range is free: a reservation at an address there drops the kept block first (allocators align this
// way: reserve more, release, reserve again at the aligned address inside; tier0 retries that forever otherwise).
typedef struct { uintptr_t base; size_t size; } spare_block;
static spare_block g_spare[32];
static int g_nspare;
static size_t g_spare_bytes;
static uint64_t g_va_kind[4];                     // commits in reservations, failed at an address, new, failed
// VXRT_TRACE_VALLOC: failed reservations (the first 20, with the program's return addresses on the stack) and slow or
// every 1024th new one, with the caller.
static void valloc_trace(void *caller, uint64_t *stack, const char *fmt, ...) {
    char text[200];
    va_list ap; va_start(ap, fmt); vsnprintf(text, sizeof text, fmt, ap); va_end(ap);
    const vx_image *m = vx_image_at((uint64_t)(uintptr_t)caller);
    vx_log("VirtualAlloc%s, from %s+%#llx", text, m ? m->name : "?", m ? (unsigned long long)((uint8_t *)caller - m->base) : 0ull);
    for (int i = 0; stack && i < 160; i++) {
        const vx_image *c = vx_image_at(stack[i]);
        if (c) vx_log("   [%d] %s+%#llx", i, c->name, (unsigned long long)(stack[i] - (uint64_t)(uintptr_t)c->base));
    }
}
static void valloc_stats(uint64_t ns) {               // VXRT_STATS: VirtualAlloc calls and time, every 2000
    static int on = -1;
    static uint64_t n, total;
    if (on < 0) on = getenv("VXRT_STATS") != NULL;
    if (!on) return;
    total += ns;
    if (++n % 2000 == 0) vx_log("VirtualAlloc: %llu calls, %.1f ms, %d reservations, %d spare; commits %llu, failed at an address %llu, failed %llu, new %llu",
                               (unsigned long long)n, total / 1e6, g_res_count, g_nspare, (unsigned long long)g_va_kind[0], (unsigned long long)g_va_kind[1], (unsigned long long)g_va_kind[3], (unsigned long long)g_va_kind[2]);
}

// Protection set on page ranges (VirtualAlloc commits, VirtualProtect), as Windows reports it back: macOS keeps
// every page readable, writable and executable (programs here only read the protection back, they do not rely on
// faults). Sorted, non-overlapping ranges; with g_vm_lock held.
typedef struct { uintptr_t start, end; DWORD protect; } prot_range;
static prot_range *g_prot;
static size_t g_nprot, g_cprot;
static void prot_set(uintptr_t start, uintptr_t end, DWORD protect) {   // protect 0: forget the range
    size_t i = 0;
    while (i < g_nprot && g_prot[i].end <= start) i++;
    prot_range left = { 0, 0, 0 }, right = { 0, 0, 0 };
    size_t j = i;
    while (j < g_nprot && g_prot[j].start < end) {               // overlapping ranges: keep their parts outside
        if (g_prot[j].start < start) left = (prot_range){ g_prot[j].start, start, g_prot[j].protect };
        if (g_prot[j].end > end) right = (prot_range){ end, g_prot[j].end, g_prot[j].protect };
        j++;
    }
    prot_range add[3]; size_t n = 0;
    if (left.end) add[n++] = left;
    if (protect) add[n++] = (prot_range){ start, end, protect };
    if (right.end) add[n++] = right;
    if (g_nprot - (j - i) + n > g_cprot) { g_cprot = (g_nprot + n) * 2 + 64; g_prot = realloc(g_prot, g_cprot * sizeof *g_prot); }
    memmove(g_prot + i + n, g_prot + j, (g_nprot - j) * sizeof *g_prot);
    memcpy(g_prot + i, add, n * sizeof *add);
    g_nprot = g_nprot - (j - i) + n;
}
static DWORD prot_get(uintptr_t a) {
    size_t lo = 0, hi = g_nprot;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (g_prot[mid].end <= a) lo = mid + 1; else hi = mid; }
    return lo < g_nprot && g_prot[lo].start <= a ? g_prot[lo].protect : 0;
}
// A loaded image's page as Windows maps it: code execute-read, writable data copy-on-write, the rest read-only.
static DWORD image_protect(const vx_image *m, uintptr_t a) {
    const uint8_t *b = m->base, *nt = b + *(const uint32_t *)(b + 0x3c);
    uint16_t nsec = *(const uint16_t *)(nt + 6), opt = *(const uint16_t *)(nt + 20);
    const uint8_t *sec = nt + 24 + opt;
    uintptr_t rva = a - (uintptr_t)b;
    for (uint16_t i = 0; i < nsec; i++, sec += 40) {
        uint32_t va = *(const uint32_t *)(sec + 12), size = *(const uint32_t *)(sec + 8), flags = *(const uint32_t *)(sec + 36);
        if (rva < va || rva >= va + ((size + 0xfff) & ~0xfffu)) continue;
        int x = (flags & 0x20000000u) != 0, w = (flags & 0x80000000u) != 0;
        return x ? (w ? 0x80 /* PAGE_EXECUTE_WRITECOPY */ : 0x20 /* PAGE_EXECUTE_READ */) : w ? 0x08 /* PAGE_WRITECOPY */ : 0x02 /* PAGE_READONLY */;
    }
    return 0x02;
}
static WINAPI void *VirtualAlloc(void *addr, size_t size, DWORD type, DWORD protect) {
    if (!size) { vx_set_last_error(87); return NULL; }
    VX_NO_STOP_LOCK(&g_vm_lock);
    reservation *r = addr ? find_reservation((uintptr_t)addr) : NULL;
    if (r && !(type & MEM_RESERVE)) {                       // committing inside an existing reservation
        uintptr_t start = (uintptr_t)addr & ~(uintptr_t)0xfff;
        prot_set(start, ((uintptr_t)addr + size + 0xfff) & ~(uintptr_t)0xfff, protect);
        g_va_kind[0]++; valloc_stats(0);
        VX_NO_STOP_UNLOCK(&g_vm_lock);
        return (void *)((uintptr_t)addr & ~(uintptr_t)0xfff);
    }
    if (addr && !(type & MEM_RESERVE)) { VX_NO_STOP_UNLOCK(&g_vm_lock); vx_set_last_error(487 /* ERROR_INVALID_ADDRESS */); return NULL; }
    mach_vm_address_t a = (mach_vm_address_t)addr & ~(mach_vm_address_t)(GRANULARITY - 1);
    mach_vm_size_t len = ((mach_vm_address_t)addr + size - a + 0xfff) & ~(mach_vm_size_t)0xfff;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (!addr) for (int i = g_nspare - 1; i >= 0; i--) {
        if (g_spare[i].size != mapped_size(len)) continue;
        mach_vm_address_t got = g_spare[i].base;
        g_spare_bytes -= g_spare[i].size;
        g_spare[i] = g_spare[--g_nspare];
        if (mach_vm_allocate(mach_task_self(), &got, mapped_size(len), VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) != KERN_SUCCESS) break;
        mach_vm_protect(mach_task_self(), got, mapped_size(len), 0, VX_PROT_RWX);
        res_insert((uintptr_t)got, (size_t)len, protect);
        valloc_stats(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0);
        VX_NO_STOP_UNLOCK(&g_vm_lock);
        return (void *)got;
    }
    if (addr) for (int i = g_nspare - 1; i >= 0; i--) {            // the range is free for the program: kept blocks there go
        if (g_spare[i].base >= a + mapped_size(len) || g_spare[i].base + g_spare[i].size <= a) continue;
        mach_vm_deallocate(mach_task_self(), g_spare[i].base, g_spare[i].size);
        g_spare_bytes -= g_spare[i].size;
        g_spare[i] = g_spare[--g_nspare];
    }
    kern_return_t kr = addr
        ? mach_vm_allocate(mach_task_self(), &a, mapped_size(len), VM_FLAGS_FIXED)
        : mach_vm_map(mach_task_self(), &a, mapped_size(len), GRANULARITY - 1, VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, 0,
                      VX_PROT_RWX, VX_PROT_RWX, VM_INHERIT_COPY);
    if (kr != KERN_SUCCESS) {
        g_va_kind[addr ? 1 : 3]++; valloc_stats(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0);
        static uint64_t failed;
        if (getenv("VXRT_TRACE_VALLOC") && failed++ < 20)
            valloc_trace(VX_RETURN_ADDRESS(), __builtin_frame_address(0), "(%p, %#zx, %#x) failed", addr, size, type);
        VX_NO_STOP_UNLOCK(&g_vm_lock);
        vx_set_last_error(addr ? 487 : 8 /* ERROR_NOT_ENOUGH_MEMORY */);
        return NULL;
    }
    mach_vm_protect(mach_task_self(), a, mapped_size(len), 0, VX_PROT_RWX);
    res_insert((uintptr_t)a, (size_t)len, protect);
    g_va_kind[2]++;
    static uint64_t made;
    uint64_t dt = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    if (getenv("VXRT_TRACE_VALLOC") && (dt > 2000000 || (++made & 0x3ff) == 0))
        valloc_trace(VX_RETURN_ADDRESS(), NULL, "(%p, %#zx, %#x) = %#llx in %.2f ms", addr, size, type, (unsigned long long)a, dt / 1e6);
    valloc_stats(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0);
    VX_NO_STOP_UNLOCK(&g_vm_lock);
    return (void *)a;
}
// A reservation below `limit` (NtAllocateVirtualMemory's ZeroBits: LuaJIT asks for the low 2 GB). vxrt is linked
// with a 64 KB __PAGEZERO so the low 4 GB can be mapped; free gaps are found with mach_vm_region.
#ifdef __aarch64__
// The vCPU's low memory (vx.h): the host's at vx_low_base + the address, granules marked in vx_low_map; the host
// address is returned (the thunk hands the program its own). Not 0x40000000-0x4000ffff: shared resources' handles.
static int in_low(uintptr_t a) { return vx_low_base && a - vx_low_base < (1ull << 32); }
static void *low_alloc(size_t size, uintptr_t limit, DWORD protect) {
    static uintptr_t cursor = 0x1000000;
    size_t n = (size + GRANULARITY - 1) / GRANULARITY;
    VX_NO_STOP_LOCK(&g_vm_lock);
    for (int pass = 0; pass < 2; pass++)
        for (uintptr_t g = (pass ? 0x1000000 : cursor) / GRANULARITY, run = 0; (g + 1) * GRANULARITY <= limit; g++) {
            int usable = !vx_low_map[g] && (g < 0x4000 || g > 0x4000);
            run = usable ? run + 1 : 0;
            if (run < n) continue;
            uintptr_t a = (g + 1 - n) * GRANULARITY;
            mach_vm_address_t host = vx_low_base + a;
            if (mach_vm_allocate(mach_task_self(), &host, n * GRANULARITY, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) != KERN_SUCCESS) break;
            mach_vm_protect(mach_task_self(), host, n * GRANULARITY, 0, VX_PROT_RWX);
            memset(vx_low_map + a / GRANULARITY, 1, n);
            res_insert((uintptr_t)host, n * GRANULARITY, protect);
            cursor = a + n * GRANULARITY;
            VX_NO_STOP_UNLOCK(&g_vm_lock);
            return (void *)(uintptr_t)host;
        }
    VX_NO_STOP_UNLOCK(&g_vm_lock);
    vx_set_last_error(8);
    return NULL;
}
#else
static const uint64_t vx_low_base = 0;
static int in_low(uintptr_t a) { (void)a; return 0; }
#endif
void *vx_virtual_alloc_below(size_t size, uintptr_t limit, DWORD protect) {
#ifdef __aarch64__
    if (vx_low_base) return low_alloc(size, limit, protect);
#endif
    static uintptr_t cursor = 0x1000000;
    mach_vm_size_t len = (size + GRANULARITY - 1) & ~(mach_vm_size_t)(GRANULARITY - 1);
    VX_NO_STOP_LOCK(&g_vm_lock);
    for (int pass = 0; pass < 2; pass++) {
        mach_vm_address_t a = pass ? 0x1000000 : cursor;
        while (a + len <= limit) {
            mach_vm_address_t region = a; mach_vm_size_t region_size = 0;
            vm_region_basic_info_data_64_t info; mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t object;
            kern_return_t kr = mach_vm_region(mach_task_self(), &region, &region_size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
            if (kr != KERN_SUCCESS || region >= a + len) {          // a gap big enough at a
                mach_vm_address_t got = a;
                if (mach_vm_allocate(mach_task_self(), &got, len, VM_FLAGS_FIXED) == KERN_SUCCESS) {
                    mach_vm_protect(mach_task_self(), got, len, 0, VX_PROT_RWX);
                    res_insert((uintptr_t)got, (size_t)len, protect);
                    cursor = (uintptr_t)(got + len);
                    VX_NO_STOP_UNLOCK(&g_vm_lock);
                    return (void *)got;
                }
                a += GRANULARITY;
                continue;
            }
            a = (region + region_size + GRANULARITY - 1) & ~(mach_vm_address_t)(GRANULARITY - 1);
        }
    }
    VX_NO_STOP_UNLOCK(&g_vm_lock);
    vx_set_last_error(8);
    return NULL;
}
static WINAPI BOOL VirtualFree(void *addr, size_t size, DWORD type) {
    VX_NO_STOP_LOCK(&g_vm_lock);
    reservation *r = find_reservation((uintptr_t)addr);
    BOOL ok = 0;
    if (r && (type & MEM_RELEASE) && !size && r->base == (uintptr_t)addr) {
        prot_set(r->base, r->base + r->size, 0);
        VX_CODE_FREED((void *)r->base, r->size);
        size_t n = mapped_size(r->size);
        if (in_low(r->base)) {                                            // (back to the low reservation, no access)
            mach_vm_address_t a = r->base;
            mach_vm_allocate(mach_task_self(), &a, n, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE);
            mach_vm_protect(mach_task_self(), a, n, 0, VM_PROT_NONE);
#ifdef __aarch64__
            memset(vx_low_map + (r->base - vx_low_base) / GRANULARITY, 0, n / GRANULARITY);
#endif
        } else if (n >= (1u << 20) && g_nspare < 32 && g_spare_bytes + n <= (512u << 20) && r->base >= (1ull << 32)) {
            g_spare[g_nspare++] = (spare_block){ r->base, n };              // (low-memory blocks are not reused)
            g_spare_bytes += n;
            mach_vm_behavior_set(mach_task_self(), r->base, n, VM_BEHAVIOR_REUSABLE);   // its pages can go meanwhile
        } else mach_vm_deallocate(mach_task_self(), r->base, n);
        res_remove(r);
        ok = 1;
    } else if (r && (type & MEM_DECOMMIT)) {
        uintptr_t start = (uintptr_t)addr & ~(uintptr_t)0xfff;
        size_t len = size ? (((uintptr_t)addr + size + 0xfff) & ~(uintptr_t)0xfff) - start : r->base + r->size - start;
        VX_CODE_FREED((void *)start, len);
        mach_vm_address_t a = start;                       // fresh zero pages over the range
        ok = mach_vm_allocate(mach_task_self(), &a, len, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) == KERN_SUCCESS;
        prot_set(start, start + len, 0);
        if (ok) mach_vm_protect(mach_task_self(), a, len, 0, VX_PROT_RWX);
    }
    VX_NO_STOP_UNLOCK(&g_vm_lock);
    if (!ok) vx_set_last_error(487);
    return ok;
}
// Memory that is neither the program's (a reservation, a PE image) nor writable at all: macOS code, this runtime's own
// functions (GetProcAddress of a built-in DLL gives their addresses). Hooking libraries (the Steam overlay's) make a
// function's first bytes writable to patch them; that cannot be done here, and saying so beats the fault that the
// write would be. Mach's maximum protection tells.
static int never_writable(uintptr_t start, uintptr_t end) {
    for (mach_vm_address_t a = start; a < end;) {
        mach_vm_size_t sz = 0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
        mach_vm_address_t at = a;
        if (mach_vm_region(mach_task_self(), &at, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &obj) != KERN_SUCCESS || at > a) return 0;
        if (info.max_protection & VM_PROT_WRITE) return 0;
        a = at + sz;
    }
    return 1;
}
static WINAPI BOOL VirtualProtect(void *addr, size_t size, DWORD prot, DWORD *old) {
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)0xfff, end = ((uintptr_t)addr + (size ? size : 1) + 0xfff) & ~(uintptr_t)0xfff;
    const vx_image *img = vx_image_at(start);
    if (!img && (prot & 0xCC) && never_writable(start, end)) {   // PAGE_READWRITE, WRITECOPY, EXECUTE_READWRITE, EXECUTE_WRITECOPY
        static int told;                                            // (the first few: a hooking library tries dozens)
        Dl_info info;
        int named = dladdr(addr, &info) && info.dli_sname;
        if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 8) vx_log("VirtualProtect(%p, %#zx, %#x): %s%s%s cannot be made writable", addr, size, (unsigned)prot,
               named ? "native code (" : "memory", named ? info.dli_sname : "", named ? ")" : "");
        vx_set_last_error(487);                                     // ERROR_INVALID_ADDRESS
        return 0;
    }
    VX_NO_STOP_LOCK(&g_vm_lock);
    reservation *r = find_reservation(start);
    DWORD was = prot_get(start);
    if (!was) was = r ? r->protect : img ? image_protect(img, start) : PAGE_EXECUTE_READWRITE;
    if (old) *old = was;
    prot_set(start, end, prot);
    VX_NO_STOP_UNLOCK(&g_vm_lock);
    return 1;
}
typedef struct { void *base, *alloc_base; DWORD alloc_protect; uint16_t partition, pad; size_t region; DWORD state, protect, type, pad2; } MEMORY_BASIC_INFORMATION;
static DWORD mach_protect(vm_prot_t p) {
    int r = p & VM_PROT_READ, w = p & VM_PROT_WRITE, x = p & VM_PROT_EXECUTE;
    if (x) return w ? PAGE_EXECUTE_READWRITE : r ? 0x20 /* PAGE_EXECUTE_READ */ : 0x10 /* PAGE_EXECUTE */;
    return w ? 0x04 /* PAGE_READWRITE */ : r ? 0x02 /* PAGE_READONLY */ : 0x01 /* PAGE_NOACCESS */;
}
static size_t virtual_query(const void *addr, MEMORY_BASIC_INFORMATION *mbi, size_t len);
// (the vCPU's low memory: its addresses as the program has them)
static WINAPI size_t VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, size_t len) {
    size_t r = virtual_query(addr, mbi, len);
    if (r && in_low((uintptr_t)mbi->base)) {
        mbi->base = (uint8_t *)mbi->base - vx_low_base;
        if (in_low((uintptr_t)mbi->alloc_base)) mbi->alloc_base = (uint8_t *)mbi->alloc_base - vx_low_base;
    }
    return r;
}
static size_t virtual_query(const void *addr, MEMORY_BASIC_INFORMATION *mbi, size_t len) {
    if (len < sizeof *mbi) { vx_set_last_error(24 /* ERROR_BAD_LENGTH */); return 0; }
    uintptr_t page = (uintptr_t)addr & ~(uintptr_t)0xfff;
    const uintptr_t top = 0x7ffffffeffffull + 1;               // past the highest application address
    if (page >= top) { vx_set_last_error(87); return 0; }
    memset(mbi, 0, sizeof *mbi);
    mbi->base = (void *)page;
    const vx_image *img = vx_image_at(page);
    VX_NO_STOP_LOCK(&g_vm_lock);
    reservation *r = find_reservation(page);
    DWORD tracked = prot_get(page);
    if (r) {
        mbi->alloc_base = (void *)r->base; mbi->alloc_protect = mbi->protect = r->protect;
        if (tracked) mbi->protect = tracked;
        mbi->region = r->base + r->size - page; mbi->state = MEM_COMMIT; mbi->type = MEM_PRIVATE;
    }
    VX_NO_STOP_UNLOCK(&g_vm_lock);
    if (r) return sizeof *mbi;
    if (img) {
        mbi->alloc_base = img->base; mbi->alloc_protect = 0x80;   // PAGE_EXECUTE_WRITECOPY, as for every image
        mbi->protect = tracked ? tracked : image_protect(img, page);
        mbi->region = (uintptr_t)img->base + img->size - page; mbi->state = MEM_COMMIT; mbi->type = MEM_IMAGE;
        return sizeof *mbi;
    }
    // Anything else: ask Mach whether the page is mapped, and how far the free gap extends if not.
    mach_vm_address_t a = page; mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info; mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
    kern_return_t kr = mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &obj);
    if (kr == KERN_SUCCESS && a <= page) {
        // Mapped by macOS (the system's libraries, malloc, Rosetta, thread stacks): adjacent mappings with the same
        // protection are reported as one region, as programs walking the address space expect Windows' few hundred
        // regions, not macOS's tens of thousands. The protection is the real one: the Boehm GC (Unity's Mono) finds
        // the end of a thread's stack by walking down while pages are PAGE_READWRITE, and with every page
        // PAGE_EXECUTE_READWRITE it scanned no stack at all and freed live objects.
        vm_prot_t prot = info.protection;
        mach_vm_address_t end = a + sz;
        for (int i = 0; i < 100000 && end < top && !vx_image_at(end); i++) {
            mach_vm_address_t n = end; mach_vm_size_t nsz = 0;
            count = VM_REGION_BASIC_INFO_COUNT_64;
            if (mach_vm_region(mach_task_self(), &n, &nsz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &obj) != KERN_SUCCESS || n != end) break;
            if (info.protection != prot) break;
            VX_NO_STOP_LOCK(&g_vm_lock);
            reservation *next = find_reservation(n);
            VX_NO_STOP_UNLOCK(&g_vm_lock);
            if (next) break;
            end = n + nsz;
        }
        if (end > top) end = top;
        mbi->alloc_base = (void *)a;
        mbi->alloc_protect = mbi->protect = mach_protect(prot);
        mbi->region = end - page; mbi->state = MEM_COMMIT; mbi->type = MEM_PRIVATE;
    } else {
        mbi->state = MEM_FREE; mbi->protect = 0x01;      // PAGE_NOACCESS
        mbi->region = (kr == KERN_SUCCESS && a < top ? a : top) - page;
    }
    return sizeof *mbi;
}

// Heaps: one process heap, backed by malloc.
#define HEAP_ZERO_MEMORY 0x8
static WINAPI HANDLE GetProcessHeap(void) { return (HANDLE)(intptr_t)0x48454150; }
static WINAPI HANDLE HeapCreate(DWORD f, size_t a, size_t b) { (void)f; (void)a; (void)b; return GetProcessHeap(); }
static WINAPI BOOL HeapDestroy(HANDLE h) { (void)h; return 1; }
int vx_heap_stats;
// (malloc's classes on a large-memory Mac: tiny to 1008 bytes, small to 127 KB (MALLOC_SMALL), medium to 8 MB, large)
static uint64_t heap_bytes[4], heap_blocks[4];
static int heap_class(size_t n) { return n <= 1008 ? 0 : n <= (127u << 10) ? 1 : n <= (8u << 20) ? 2 : 3; }
void vx_heap_note(const void *p, int sign) {
    size_t n = malloc_size(p);
    if (!n) return;                                  // (not malloc's: the ARM64 build's guest range)
    int k = heap_class(n);
    __atomic_add_fetch(&heap_bytes[k], (uint64_t)(int64_t)sign * n, __ATOMIC_RELAXED);
    __atomic_add_fetch(&heap_blocks[k], (uint64_t)(int64_t)sign, __ATOMIC_RELAXED);
}
int vx_heap_report(char *buf, size_t size) {
    if (!vx_heap_stats) return 0;
    static const char *const name[4] = { "tiny", "small", "medium", "large" };
    size_t o = 0;
    for (int k = 0; k < 4 && o < size; k++)
        o += (size_t)snprintf(buf + o, size - o, "%s%s %.0f MB (%lld)", k ? ", " : "", name[k],
                              (double)(int64_t)__atomic_load_n(&heap_bytes[k], __ATOMIC_RELAXED) / 1048576.0,
                              (long long)__atomic_load_n(&heap_blocks[k], __ATOMIC_RELAXED));
    return 1;
}
__attribute__((constructor)) static void heap_stats_init(void) {
#ifndef __aarch64__
    const char *e = getenv("VXRT_HEAP_STATS");
    vx_heap_stats = e && *e && *e != '0';
#endif
}
static WINAPI void *HeapAlloc(HANDLE h, DWORD flags, size_t n) {
    (void)h;
    void *p = flags & HEAP_ZERO_MEMORY ? vx_heap_zalloc(n) : vx_heap_alloc(n);
    VX_HEAP_NOTE(p, 1);
    return p;
}
static WINAPI void *HeapReAlloc(HANDLE h, DWORD flags, void *p, size_t n) {
    (void)h;
    size_t old = p ? malloc_size(p) : 0;
    VX_HEAP_NOTE(p, -1);
    void *q = realloc(p, n ? n : 1);
    VX_HEAP_NOTE(q ? q : p, 1);
    if (q && (flags & HEAP_ZERO_MEMORY) && n > old) memset((char *)q + old, 0, n - old);
    return q;
}
// A pointer no heap gave out is reported (with the caller) and left alone, rather than crashing in free().
static int heap_pointer(const void *p, const char *what, void *caller) {
    if (!p || malloc_zone_from_ptr(p) || vx_heap_owns(p)) return 1;
    vx_log("%s of %p, which no heap allocated (called from %p)", what, p, caller);
    vx_set_last_error(87);
    return 0;
}
static WINAPI BOOL HeapFree(HANDLE h, DWORD f, void *p) { (void)h; (void)f; if (!heap_pointer(p, "HeapFree", VX_RETURN_ADDRESS())) return 0; VX_HEAP_NOTE(p, -1); free(p); return 1; }
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
static WINAPI void *LocalAlloc(DWORD flags, size_t n) {
    void *p = flags & 0x40 /* LMEM_ZEROINIT */ ? vx_heap_zalloc(n) : vx_heap_alloc(n);
    VX_HEAP_NOTE(p, 1);
    return p;
}
static WINAPI void *LocalFree(void *p) { if (!heap_pointer(p, "LocalFree", VX_RETURN_ADDRESS())) return p; VX_HEAP_NOTE(p, -1); free(p); return NULL; }
// VXRT_DEBUG_OUTPUT=1: what programs send to OutputDebugString, without the rest of VXRT_TRACE (with VXRT_DEBUGGER=1,
// a game's own console log: why it quit, for one).
int vx_debug_output(void) { static int on = -1; if (on < 0) on = vx_trace || (getenv("VXRT_DEBUG_OUTPUT") && atoi(getenv("VXRT_DEBUG_OUTPUT"))); return on; }
static WINAPI void OutputDebugStringA(const char *s) { if (vx_debug_output()) vx_log("debug: %s", s); }

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
    VX_EXPORT(GetStdHandle), VX_EXPORT(WriteFile), VX_EXPORT(ReadFileEx), VX_EXPORT(WriteFileEx), VX_EXPORT(WriteConsoleA), VX_EXPORT(WriteConsoleW),
    VX_EXPORT(ExitProcess), VX_EXPORT(TerminateProcess),
    VX_EXPORT(GetLastError), VX_EXPORT(SetLastError), VX_EXPORT(IsDebuggerPresent), VX_EXPORT(CheckRemoteDebuggerPresent),
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
    VX_EXPORT(GetExitCodeProcess), VX_EXPORT(GetProcessId), VX_EXPORT(EncodePointer), VX_EXPORT(DecodePointer),
    { "EncodeSystemPointer", (void *)EncodePointer }, { "DecodeSystemPointer", (void *)DecodePointer },
    VX_EXPORT(IsProcessorFeaturePresent), VX_EXPORT(GetLocalTime), VX_EXPORT(GetSystemTime),
    VX_EXPORT(GetVersion), VX_EXPORT(GetVersionExA), VX_EXPORT(GetVersionExW),
    VX_EXPORT(SetErrorMode), VX_EXPORT(GetErrorMode), VX_EXPORT(GetCurrentProcessorNumber), VX_EXPORT(GetPriorityClass),
    VX_EXPORT(SetPriorityClass), VX_EXPORT(HeapValidate), VX_EXPORT(DeviceIoControl), VX_EXPORT(K32GetMappedFileNameW),
    VX_EXPORT(CreateProcessA), VX_EXPORT(CreateProcessW), VX_EXPORT(WinExec),
    VX_EXPORT(GlobalMemoryStatusEx), VX_EXPORT(GetComputerNameA), VX_EXPORT(GetComputerNameW), VX_EXPORT(GetComputerNameExA),
    VX_EXPORT(GetComputerNameExW), VX_EXPORT(FileTimeToSystemTime), VX_EXPORT(SystemTimeToFileTime), VX_EXPORT(FileTimeToLocalFileTime),
    VX_EXPORT(GetTimeZoneInformation),                                     // (SystemTimeToTzSpecificLocalTime: kernelx.c)
    VX_EXPORT(VerSetConditionMask), VX_EXPORT(VerifyVersionInfoA), VX_EXPORT(VerifyVersionInfoW),
    VX_EXPORT(GetConsoleScreenBufferInfo), VX_EXPORT(SetConsoleTextAttribute), VX_EXPORT(SetConsoleTitleA), VX_EXPORT(SetConsoleTitleW),
    VX_EXPORT(GetConsoleWindow), VX_EXPORT(AllocConsole), VX_EXPORT(FreeConsole), VX_EXPORT(SetConsoleCtrlHandler), VX_EXPORT(GetConsoleCP),
    VX_EXPORT(HeapSetInformation), VX_EXPORT(SetHandleCount), VX_EXPORT(GetThreadTimes), VX_EXPORT(GetProcessTimes),
};

// shell32's ShellExecute(Ex), as Microsoft documents them: a program (.exe, .com) starts through CreateProcess with the
// parameters after it; a URL (<scheme>:...) or a document (a file there, by its extension's ProgID) runs the command
// registered for the verb in HKEY_CLASSES_ROOT: <class>\shell\<verb>\command, where the verb not given is the
// default one (the shell key's default value, else "open"). HKEY_CLASSES_ROOT is the per-user classes
// (HKCU\Software\Classes) over the machine's (HKLM\Software\Classes); keys a program wrote to HKEY_CLASSES_ROOT
// itself are kept apart here and looked at last. In the command, %1 and %L are the file or URL, %* the parameters;
// it starts with CreateProcess. Nothing registered: SE_ERR_NOASSOC (ERROR_NO_ASSOCIATION from ShellExecuteEx), never
// anything of the Mac's (a URL is not handed to the Mac's browser); a document that is not there: SE_ERR_FNF.
char *vx_reg_get_string(const char *path, const char *name, int *expand);   // registry.c
enum { SE_ERR_FNF = 2, SE_ERR_PNF = 3, SE_ERR_ACCESSDENIED = 5, SE_ERR_OOM = 8, SE_ERR_NOASSOC = 31 };
static const char *const g_class_roots[] = { "HKEY_CURRENT_USER\\Software\\Classes\\", "HKEY_LOCAL_MACHINE\\Software\\Classes\\",
                                             "HKEY_CLASSES_ROOT\\" };
// A value of HKEY_CLASSES_ROOT\<key> (malloc'd, environment variables expanded for REG_EXPAND_SZ), the per-user one first.
static char *class_value(const char *key, const char *name) {
    for (size_t i = 0; i < sizeof g_class_roots / sizeof g_class_roots[0]; i++) {
        char path[2048];
        snprintf(path, sizeof path, "%s%s", g_class_roots[i], key);
        int expand = 0;
        char *v = vx_reg_get_string(path, name, &expand);
        if (!v) continue;
        if (expand) {
            DWORD (WINAPI *exp)(const char *, char *, DWORD) = (DWORD (WINAPI *)(const char *, char *, DWORD))vx_find_export("kernel32.dll", "ExpandEnvironmentStringsA");
            DWORD n = exp ? exp(v, NULL, 0) : 0;
            if (n) { char *e = malloc(n + 1); if (exp(v, e, n + 1)) { free(v); v = e; } else free(e); }
        }
        return v;
    }
    return NULL;
}
// The command registered for a class and verb (NULL or empty: the default verb).
static char *class_command(const char *cls, const char *verb) {
    char key[2048];
    char *def = NULL;
    if (!verb || !*verb) {
        snprintf(key, sizeof key, "%s\\shell", cls);
        def = class_value(key, "");
        verb = def && *def ? def : "open";
    }
    snprintf(key, sizeof key, "%s\\shell\\%s\\command", cls, verb);
    free(def);
    return class_value(key, "");
}
// "<scheme>:" with a scheme of two or more letters, digits, +, - or . that starts with a letter (one letter is a drive).
static int url_scheme(const char *f, char *scheme, size_t cap) {
    size_t n = 0;
    if (!f || !isalpha((unsigned char)f[0])) return 0;
    while (isalnum((unsigned char)f[n]) || f[n] == '+' || f[n] == '-' || f[n] == '.') n++;
    if (n < 2 || f[n] != ':' || n >= cap) return 0;
    memcpy(scheme, f, n); scheme[n] = 0;
    return 1;
}
// The command line: %1 / %L / %l the target, %* the parameters, %% a %; %2..%9 and %I (no item ID list here) nothing.
static char *substitute(const char *cmd, const char *target, const char *params) {
    size_t cap = strlen(cmd) + 1, n = 0;
    for (const char *c = cmd; *c; c++) if (*c == '%') cap += strlen(target) + (params ? strlen(params) : 0);
    char *out = malloc(cap);
    for (const char *c = cmd; *c; c++) {
        if (c[0] != '%' || !c[1]) { out[n++] = *c; continue; }
        char k = *++c;
        const char *put = k == '1' || k == 'L' || k == 'l' ? target : k == '*' ? (params ? params : "") : k == '%' ? "%" :
                          (k >= '2' && k <= '9') || k == 'I' || k == 'i' ? "" : NULL;
        if (!put) { out[n++] = '%'; out[n++] = k; continue; }
        size_t m = strlen(put); memcpy(out + n, put, m); n += m;
    }
    out[n] = 0;
    return out;
}
static int se_error_of_last_error(void) {
    DWORD e = vx_get_last_error();
    return e == 2 ? SE_ERR_FNF : e == 3 ? SE_ERR_PNF : e == 5 ? SE_ERR_ACCESSDENIED : e == 8 ? SE_ERR_OOM : SE_ERR_FNF;
}
// Runs it: > 32 started (pi: process and thread), else the SE_ERR_ code, with the last error ShellExecuteEx reports.
static int shell_execute_1(const char *verb, const char *file, const char *params, const char *dir, const char *cls, void **pi);
static int shell_execute(const char *verb, const char *file, const char *params, const char *dir, const char *cls, void **pi) {
    int r = shell_execute_1(verb, file, params, dir, cls, pi);
    if (r <= 32 || vx_trace)
        vx_log("ShellExecute(verb %s, file \"%s\", parameters \"%s\", directory \"%s\") = %d", verb ? verb : "(default)", file ? file : "",
               params ? params : "", dir ? dir : "", r);
    return r;
}
static int shell_execute_1(const char *verb, const char *file, const char *params, const char *dir, const char *cls, void **pi) {
    if (!file || !*file) { vx_set_last_error(2); return SE_ERR_FNF; }
    char scheme[64], *cmd = NULL, *line = NULL;
    size_t n = strlen(file);
    int program = !cls && n >= 4 && (!strcasecmp(file + n - 4, ".exe") || !strcasecmp(file + n - 4, ".com")) && !url_scheme(file, scheme, sizeof scheme);
    if (program && (!verb || !*verb || !strcasecmp(verb, "open") || !strcasecmp(verb, "runas"))) {
        line = malloc(n + (params ? strlen(params) : 0) + 8);
        sprintf(line, "\"%s\"%s%s", file, params && *params ? " " : "", params && *params ? params : "");
        BOOL ok = create_process(file, line, NULL, 0, dir, NULL, pi);
        free(line);
        return ok ? 42 : se_error_of_last_error();
    }
    if (cls) cmd = class_command(cls, verb);
    else if (url_scheme(file, scheme, sizeof scheme)) cmd = class_command(scheme, verb);
    else {
        char win[2048], unix_path[2048];
        int relative = !(file[0] == '\\' || (file[0] && file[1] == ':'));
        if (relative && dir && *dir) snprintf(win, sizeof win, "%s\\%s", dir, file);
        else snprintf(win, sizeof win, "%s", file);
        struct stat st;
        if (!vx_unix_path(win, unix_path, sizeof unix_path) || stat(unix_path, &st)) { vx_set_last_error(2); return SE_ERR_FNF; }
        const char *base = strrchr(file, '\\'), *dot = strrchr(base ? base : file, '.');
        if (dot && !S_ISDIR(st.st_mode)) {
            char *progid = class_value(dot, "");
            if (progid && *progid) cmd = class_command(progid, verb);
            if (!cmd) cmd = class_command(dot, verb);
            free(progid);
        }
    }
    if (!cmd) {
        vx_log("ShellExecute(%s): no application is registered for it", file);
        vx_set_last_error(1155);                                      // ERROR_NO_ASSOCIATION
        return SE_ERR_NOASSOC;
    }
    line = substitute(cmd, file, params);
    if (vx_trace) vx_log("ShellExecute(%s): %s", file, line);
    BOOL ok = create_process(NULL, line, NULL, 0, dir, NULL, pi);
    free(cmd); free(line);
    return ok ? 42 : se_error_of_last_error();
}
static WINAPI void *ShellExecuteA(void *w, const char *op, const char *file, const char *params, const char *dir, int show) {
    (void)w; (void)show;
    void *pi[3] = { 0 };
    int r = shell_execute(op, file, params, dir, NULL, pi);
    if (r > 32) { vx_handle_close(pi[0]); vx_handle_close(pi[1]); }
    return (void *)(intptr_t)r;
}
static WINAPI void *ShellExecuteW(void *w, const WCHAR *op, const WCHAR *file, const WCHAR *params, const WCHAR *dir, int show) {
    char *o = vx_utf8_dup(op), *f = vx_utf8_dup(file), *p = vx_utf8_dup(params), *d = vx_utf8_dup(dir);
    void *r = ShellExecuteA(w, o, f, p, d, show);
    free(o); free(f); free(p); free(d);
    return r;
}
// SHELLEXECUTEINFO (64-bit): cbSize, fMask, hwnd, lpVerb (16), lpFile (24), lpParameters (32), lpDirectory (40), nShow,
// hInstApp (56), lpIDList, lpClass (72), hkeyClass, dwHotKey, hIcon, hProcess (104). SEE_MASK_CLASSNAME (1): lpClass
// names the class; SEE_MASK_NOCLOSEPROCESS (0x40): hProcess is the process started.
static BOOL shell_execute_ex(uint8_t *info, int wide) {
    DWORD mask = *(DWORD *)(info + 4);
    char *s[5];
    for (int i = 0; i < 5; i++) {
        size_t at = i < 4 ? 16 + 8 * (size_t)i : 72;
        void *v = *(void **)(info + at);
        s[i] = !v || (i == 4 && !(mask & 1)) ? NULL : wide ? vx_utf8_dup(v) : strdup(v);
    }
    void *pi[3] = { 0 };
    int r = shell_execute(s[0], s[1], s[2], s[3], s[4], pi);
    if (mask & 0x40) *(HANDLE *)(info + 104) = NULL;
    if (r > 32) {
        if (mask & 0x40) *(HANDLE *)(info + 104) = pi[0]; else vx_handle_close(pi[0]);
        vx_handle_close(pi[1]);
    }
    *(void **)(info + 56) = (void *)(intptr_t)r;
    for (int i = 0; i < 5; i++) free(s[i]);
    return r > 32;
}
static WINAPI BOOL ShellExecuteExA(uint8_t *info) { return shell_execute_ex(info, 0); }
static WINAPI BOOL ShellExecuteExW(uint8_t *info) { return shell_execute_ex(info, 1); }
// advapi32: event tracing accepts and drops events (security objects: security.c).
static WINAPI DWORD EventRegister(const void *guid, void *cb, void *ctx, uint64_t *h) { (void)guid; (void)cb; (void)ctx; *h = 1; return 0; }
static WINAPI DWORD EventUnregister(uint64_t h) { (void)h; return 0; }
static WINAPI DWORD EventWrite(uint64_t h, const void *d, DWORD n, void *data) { (void)h; (void)d; (void)n; (void)data; return 0; }
static WINAPI DWORD EventWriteTransfer(uint64_t h, const void *d, const void *a, const void *r, DWORD n, void *data) {
    (void)h; (void)d; (void)a; (void)r; (void)n; (void)data; return 0;
}
static WINAPI BOOL EventEnabled(uint64_t h, const void *d) { (void)h; (void)d; return 0; }
// Classic (WPP) providers register too, and are never enabled: no session asks for their events.
static WINAPI DWORD RegisterTraceGuidsW(void *cb, void *ctx, const void *guid, DWORD n, uint8_t *regs, const void *mof,
                                        const void *res, uint64_t *h) {
    (void)cb; (void)ctx; (void)guid; (void)mof; (void)res;
    for (DWORD i = 0; regs && i < n; i++) *(void **)(regs + 16 * i + 8) = (void *)(uintptr_t)(i + 1);   // RegHandle
    *h = 1;
    return 0;
}
static WINAPI DWORD UnregisterTraceGuids(uint64_t h) { (void)h; return 0; }
static WINAPI uint64_t GetTraceLoggerHandle(void *buf) { (void)buf; vx_set_last_error(6); return (uint64_t)-1; }
static WINAPI uint8_t GetTraceEnableLevel(uint64_t h) { (void)h; return 0; }
static WINAPI DWORD GetTraceEnableFlags(uint64_t h) { (void)h; return 0; }
static WINAPI DWORD TraceMessage(uint64_t h, DWORD flags, const void *guid, uint16_t number) { (void)h; (void)flags; (void)guid; (void)number; return 0; }
static WINAPI DWORD TraceEvent(uint64_t h, void *header) { (void)h; (void)header; return 0; }
static const vx_export advapi_misc[] = {
    VX_EXPORT(EventRegister), VX_EXPORT(EventUnregister),
    VX_EXPORT(EventWrite), VX_EXPORT(EventWriteTransfer), VX_EXPORT(EventEnabled), VX_EXPORT(RegisterTraceGuidsW),
    { "RegisterTraceGuidsA", (void *)RegisterTraceGuidsW }, VX_EXPORT(UnregisterTraceGuids), VX_EXPORT(GetTraceLoggerHandle),
    VX_EXPORT(GetTraceEnableLevel), VX_EXPORT(GetTraceEnableFlags), VX_EXPORT(TraceMessage), VX_EXPORT(TraceEvent), VX_EXPORT(GetUserNameA), VX_EXPORT(GetUserNameW),
};

static const vx_export shell32[] = { VX_EXPORT(ShellExecuteA), VX_EXPORT(ShellExecuteW), VX_EXPORT(ShellExecuteExA), VX_EXPORT(ShellExecuteExW) };

static const vx_export ntdll_version[] = { VX_EXPORT(RtlVerifyVersionInfo), VX_EXPORT(RtlGetVersion) };

void vx_kernel32_init(void) {
    vx_register_module("ntdll.dll", ntdll_version, sizeof ntdll_version / sizeof ntdll_version[0]);
    vx_register_module("advapi32.dll", advapi_misc, sizeof advapi_misc / sizeof advapi_misc[0]);
    vx_register_module("kernelbase.dll", advapi_misc, sizeof advapi_misc / sizeof advapi_misc[0]);
    vx_register_module("shell32.dll", shell32, sizeof shell32 / sizeof shell32[0]);
    vx_register_module("kernel32.dll", k32, sizeof k32 / sizeof k32[0]);
    vx_register_module("kernelbase.dll", k32, sizeof k32 / sizeof k32[0]);
}

#ifdef __aarch64__
// vCPU: answered in the guest (translator/guest.c), as these do: the TEB's slots and last error, the counter.
#include "arm64.h"
__attribute__((constructor)) static void k32_guest_add(void) {
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    uint32_t t[2] = { tb.numer, tb.denom };
    vx_guest_add(TlsGetValue, xv_g_tls_get, NULL, 0); vx_guest_add(TlsSetValue, xv_g_tls_set, NULL, 0);
    vx_guest_add(GetLastError, xv_g_get_last_error, NULL, 0); vx_guest_add(SetLastError, xv_g_set_last_error, NULL, 0);
    vx_guest_add(QueryPerformanceCounter, xv_g_qpc, t, sizeof t); vx_guest_add(QueryPerformanceFrequency, xv_g_qpf, NULL, 0);
    vx_guest_add(GetTickCount64, xv_g_tick64, t, sizeof t); vx_guest_add(GetTickCount, xv_g_tick, t, sizeof t);
    uint64_t heap = (uint64_t)(uintptr_t)GetProcessHeap();
    vx_guest_add(GetProcessHeap, xv_g_const, &heap, sizeof heap);
    void *h = vxvm_heap();
    if (!h) return;
    vx_guest_add(HeapAlloc, xv_g_heap_alloc, &h, sizeof h); vx_guest_add(HeapFree, xv_g_heap_free, &h, sizeof h);
    vx_guest_add(HeapSize, xv_g_heap_size, &h, sizeof h); vx_guest_add(HeapReAlloc, xv_g_heap_realloc, &h, sizeof h);
    vx_guest_add(LocalAlloc, xv_g_local_alloc, &h, sizeof h); vx_guest_add(LocalFree, xv_g_local_free, &h, sizeof h);
}
#endif

#ifdef __aarch64__
#include "thunks/kernel32.c.inc"
#endif
