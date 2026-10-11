// glibc symbol layer for Klepton.
//
// Klepton's shim translates bionic (Android libc) names to Darwin. Valve's
// Linux ARM64 build of Half-Life: Alyx is a glibc program, and glibc and bionic
// agree on almost everything that matters here — both are Linux: errno values,
// open/mmap/socket flags, struct stat, dirent, timespec, sockaddr — so the bulk
// of the surface is answered by Klepton as it stands. What glibc adds is
// answered here, in tiers:
//
//   1. a table of glibc-only names (fortify __*_chk, __xstat, __errno_location,
//      __ctype_b_loc, locale internals, sigaction with glibc's struct layout…)
//   2. aliases: a glibc spelling of something Klepton already serves under the
//      bionic/POSIX name (stat64 → stat, __isoc23_strtol → strtol, …)
//   3. named stubs returning 0 for whole libraries that have no business on this
//      host (X11, xcb, Wayland, udev, drm) — the game runs with SDL's dummy
//      video driver and our own OpenVR runtime.
//
// Installed as kl_shim_override, so it is consulted first; a NULL falls through
// to Klepton's tables, and a name nobody knows still reaches the loader's
// unresolved-import report, which is the work list.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include <wctype.h>
#include <wchar.h>
#include <locale.h>
#include <xlocale.h>
#include <langinfo.h>
#include <math.h>
#include <complex.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <libproc.h>
#include <malloc/malloc.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/random.h>
#include <sys/un.h>
#include <sys/event.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/ipc.h>
#include <sys/xattr.h>
#include <spawn.h>
#include <netinet/in.h>
#include <netdb.h>
#include <inttypes.h>
#include <libgen.h>
#include <wchar.h>
#include <sched.h>
#include <limits.h>
#include <dirent.h>
#include <fenv.h>
#include <iconv.h>
#include <resolv.h>
#include <execinfo.h>
#include <mach/mach_time.h>
#include "klepton.h"
#include "kl_fault.h"
#include "kl_va.h"
#include "kl_tls.h"
#include "kl_glibc.h"

// ---------- tiers below us ----------
// A Klepton-served implementation by its bionic/POSIX name: the tier BELOW
// this layer. kl_shim_lookup asks us first, so while a B() lookup is in
// progress our own lookup answers nothing — otherwise a wrapper that forwards
// to the name it overrides (fopen, stat, read…) would call itself forever.
static _Thread_local int g_in_lower;
static void *B(const char *name) {
    g_in_lower++;
    void *v = kl_shim_lookup(name);
    g_in_lower--;
    return v;
}

static unsigned g_stub_calls;
static const char *g_argv0 = "hlvr";
static char g_progname[256] = "hlvr";
static char *g_program_invocation_name = g_progname;
static char *g_program_invocation_short_name = g_progname;
static char g_libc_single_threaded;           // 0: assume threads (safe)
static void *g_libc_stack_end;
static uintptr_t g_stack_chk_guard;
static int g_h_errno;
extern char **environ;

// ---------- diagnostics ----------
static void die(const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    fprintf(stderr, "  [glibc] fatal: ");
    vfprintf(stderr, fmt, a);
    fprintf(stderr, "\n");
    va_end(a);
    kl_fatal_prepare();
    abort();
}

static void klg___assert_fail(const char *expr, const char *file, unsigned line, const char *fn) {
    die("assertion '%s' failed at %s:%u (%s)", expr, file, line, fn ? fn : "?");
}
static void klg___assert_perror_fail(int err, const char *file, unsigned line, const char *fn) {
    die("assertion failed with error %d at %s:%u (%s)", err, file, line, fn ? fn : "?");
}
static void klg___stack_chk_fail(void) { die("stack smashing detected (__stack_chk_fail)"); }

// ---------- errno, program identity ----------
static int *klg___errno_location(void) {
    int *(*fn)(void) = (int *(*)(void))B("__errno");
    return fn();
}
static int *klg___h_errno_location(void) { return &g_h_errno; }

// ---------- ctype tables (glibc bit layout, "C"/UTF-8 locale) ----------
// glibc's _ISbit(n): n < 8 → (1 << n) << 8, else (1 << n) >> 8 (little endian).
enum {
    G_UPPER = 1 << 8, G_LOWER = 1 << 9, G_ALPHA = 1 << 10, G_DIGIT = 1 << 11,
    G_XDIGIT = 1 << 12, G_SPACE = 1 << 13, G_PRINT = 1 << 14, G_GRAPH = 1 << 15,
    G_BLANK = 1 << 0, G_CNTRL = 1 << 1, G_PUNCT = 1 << 2, G_ALNUM = 1 << 3,
};
static unsigned short g_ctype_b[384];
static int32_t g_ctype_tolower[384], g_ctype_toupper[384];
static const unsigned short *g_ctype_b_ptr = g_ctype_b + 128;
static const int32_t *g_ctype_tolower_ptr = g_ctype_tolower + 128;
static const int32_t *g_ctype_toupper_ptr = g_ctype_toupper + 128;

static void ctype_init(void) {
    for (int i = -128; i < 256; i++) {
        int c = i & 0xff;
        unsigned short b = 0;
        if (i >= 0 && i < 128) {
            if (isupper(c)) b |= G_UPPER;
            if (islower(c)) b |= G_LOWER;
            if (isalpha(c)) b |= G_ALPHA;
            if (isdigit(c)) b |= G_DIGIT;
            if (isxdigit(c)) b |= G_XDIGIT;
            if (isspace(c)) b |= G_SPACE;
            if (isprint(c)) b |= G_PRINT;
            if (isgraph(c)) b |= G_GRAPH;
            if (isblank(c)) b |= G_BLANK;
            if (iscntrl(c)) b |= G_CNTRL;
            if (ispunct(c)) b |= G_PUNCT;
            if (isalnum(c)) b |= G_ALNUM;
        }
        g_ctype_b[i + 128] = b;
        g_ctype_tolower[i + 128] = (i >= 0 && i < 128) ? tolower(i) : i;
        g_ctype_toupper[i + 128] = (i >= 0 && i < 128) ? toupper(i) : i;
    }
}
static const unsigned short **klg___ctype_b_loc(void) { return &g_ctype_b_ptr; }
static const int32_t **klg___ctype_tolower_loc(void) { return &g_ctype_tolower_ptr; }
static const int32_t **klg___ctype_toupper_loc(void) { return &g_ctype_toupper_ptr; }
static size_t klg___ctype_get_mb_cur_max(void) { return MB_CUR_MAX; }

// ---------- fortify (_FORTIFY_SOURCE) ----------
static void *klg___memcpy_chk(void *d, const void *s, size_t n, size_t dl) { if (n > dl) die("__memcpy_chk: %zu > %zu", n, dl); return memcpy(d, s, n); }
static void *klg___memmove_chk(void *d, const void *s, size_t n, size_t dl) { if (n > dl) die("__memmove_chk: %zu > %zu", n, dl); return memmove(d, s, n); }
static void *klg___memset_chk(void *d, int c, size_t n, size_t dl) { if (n > dl) die("__memset_chk: %zu > %zu", n, dl); return memset(d, c, n); }
static void *klg___mempcpy_chk(void *d, const void *s, size_t n, size_t dl) { if (n > dl) die("__mempcpy_chk"); return (char *)memcpy(d, s, n) + n; }
static char *klg___strcpy_chk(char *d, const char *s, size_t dl) { size_t n = strlen(s) + 1; if (n > dl) die("__strcpy_chk: %zu > %zu", n, dl); return memcpy(d, s, n); }
static char *klg___stpcpy_chk(char *d, const char *s, size_t dl) { size_t n = strlen(s) + 1; if (n > dl) die("__stpcpy_chk"); return (char *)memcpy(d, s, n) + n - 1; }
static char *klg___strncpy_chk(char *d, const char *s, size_t n, size_t dl) { if (n > dl) die("__strncpy_chk: %zu > %zu", n, dl); return strncpy(d, s, n); }
static char *klg___stpncpy_chk(char *d, const char *s, size_t n, size_t dl) { if (n > dl) die("__stpncpy_chk"); return stpncpy(d, s, n); }
static char *klg___strcat_chk(char *d, const char *s, size_t dl) { if (strlen(d) + strlen(s) + 1 > dl) die("__strcat_chk"); return strcat(d, s); }
static char *klg___strncat_chk(char *d, const char *s, size_t n, size_t dl) { return strncat(d, s, n); }
static void klg___explicit_bzero_chk(void *b, size_t n, size_t dl) { if (n > dl) die("__explicit_bzero_chk"); memset(b, 0, n); __asm__ volatile("" ::: "memory"); }
static wchar_t *klg___wcscpy_chk(wchar_t *d, const wchar_t *s, size_t dl) { return wcscpy(d, s); }
static wchar_t *klg___wcsncpy_chk(wchar_t *d, const wchar_t *s, size_t n, size_t dl) { return wcsncpy(d, s, n); }
static wchar_t *klg___wmemcpy_chk(wchar_t *d, const wchar_t *s, size_t n, size_t dl) { return wmemcpy(d, s, n); }
static wchar_t *klg___wmemmove_chk(wchar_t *d, const wchar_t *s, size_t n, size_t dl) { return wmemmove(d, s, n); }
static wchar_t *klg___wmemset_chk(wchar_t *d, wchar_t c, size_t n, size_t dl) { return wmemset(d, c, n); }
static size_t klg___mbstowcs_chk(wchar_t *d, const char *s, size_t n, size_t dl) { return mbstowcs(d, s, n); }
static size_t klg___wcstombs_chk(char *d, const wchar_t *s, size_t n, size_t dl) { return wcstombs(d, s, n); }
static size_t klg___mbsrtowcs_chk(wchar_t *d, const char **s, size_t n, mbstate_t *ps, size_t dl) { return mbsrtowcs(d, s, n, ps); }
static size_t klg___wcsrtombs_chk(char *d, const wchar_t **s, size_t n, mbstate_t *ps, size_t dl) { return wcsrtombs(d, s, n, ps); }
static size_t klg___wcrtomb_chk(char *d, wchar_t wc, mbstate_t *ps, size_t dl) { return wcrtomb(d, wc, ps); }
static char *klg___fgets_chk(char *buf, size_t size, int n, void *f) { return fgets(buf, n, kl_host_file(f)); }
static size_t klg___fread_chk(void *p, size_t plen, size_t sz, size_t n, void *f) { return fread(p, sz, n, kl_host_file(f)); }
static ssize_t klg_read(int fd, void *buf, size_t n);
static ssize_t klg___read_chk(int fd, void *buf, size_t n, size_t bl) { return klg_read(fd, buf, n); }
static ssize_t klg___pread_chk(int fd, void *buf, size_t n, off_t off, size_t bl) { return ((ssize_t (*)(int, void *, size_t, off_t))B("pread"))(fd, buf, n, off); }
static ssize_t klg___recv_chk(int fd, void *buf, size_t n, size_t bl, int flags) { return ((ssize_t (*)(int, void *, size_t, int))B("recv"))(fd, buf, n, flags); }
static ssize_t klg___recvfrom_chk(int fd, void *buf, size_t n, size_t bl, int flags, void *addr, void *alen) { return ((ssize_t (*)(int, void *, size_t, int, void *, void *))B("recvfrom"))(fd, buf, n, flags, addr, alen); }
static int klg___poll_chk(void *fds, unsigned n, int timeout, size_t len) { return ((int (*)(void *, unsigned, int))B("poll"))(fds, n, timeout); }
static char *klg___realpath_chk(const char *p, char *res, size_t len) { return ((char *(*)(const char *, char *))B("realpath"))(p, res); }
static char *klg___getcwd_chk(char *buf, size_t len, size_t bl) { return ((char *(*)(char *, size_t))B("getcwd"))(buf, len); }
static ssize_t klg_readlink(const char *path, char *buf, size_t n);
static ssize_t klg___readlink_chk(const char *p, char *buf, size_t len, size_t bl) { return klg_readlink(p, buf, len); }
static int klg___gethostname_chk(char *buf, size_t len, size_t bl) { return gethostname(buf, len); }
static int klg___ttyname_r_chk(int fd, char *buf, size_t len, size_t bl) { return ttyname_r(fd, buf, len); }
static long klg___fdelt_chk(long d) { if (d < 0 || d >= 1024) die("__fdelt_chk: fd %ld outside fd_set", d); return d / 64; }
static void klg___longjmp_chk(void *env, int val) { longjmp(env, val); }

// ---------- /proc/self: exe and cmdline ----------
// tier0 finds the game directory from the launcher's own path (lstat +
// readlink of /proc/self/exe) and reads /proc/self/cmdline; the host names
// both. A Linux aarch64 struct stat is 128 bytes: st_mode at 16, st_nlink at
// 20, st_size at 48, st_blksize at 56.
static char g_exe_path[PATH_MAX];
static char *g_cmdline; static size_t g_cmdline_len;
void kl_glibc_set_exe_path(const char *path) { snprintf(g_exe_path, sizeof g_exe_path, "%s", path); }
void kl_glibc_set_cmdline(int argc, char **argv) {
    size_t n = 0;
    for (int i = 0; i < argc; i++) n += strlen(argv[i]) + 1;
    g_cmdline = calloc(n + 1, 1); g_cmdline_len = n;
    for (int i = 0, o = 0; i < argc; i++) { size_t l = strlen(argv[i]) + 1; memcpy(g_cmdline + o, argv[i], l); o += (int)l; }
}
static int is_self_exe(const char *p) { return g_exe_path[0] && p && !strcmp(p, "/proc/self/exe"); }
static int fake_link_stat(void *buf) {
    memset(buf, 0, 128);
    *(uint32_t *)((char *)buf + 16) = 0120777;                  // S_IFLNK | 0777
    *(uint32_t *)((char *)buf + 20) = 1;
    *(int64_t *)((char *)buf + 48) = (int64_t)strlen(g_exe_path);
    *(int32_t *)((char *)buf + 56) = 4096;
    return 0;
}
static int klg_stat(const char *p, void *buf) { if (is_self_exe(p)) return ((int (*)(const char *, void *))B("stat"))(g_exe_path, buf); return ((int (*)(const char *, void *))B("stat"))(p, buf); }
static int klg_lstat(const char *p, void *buf) { if (is_self_exe(p)) return fake_link_stat(buf); return ((int (*)(const char *, void *))B("lstat"))(p, buf); }
static int klg_fstatat(int dfd, const char *p, void *buf, int flags) {
    if (is_self_exe(p)) return (flags & 0x100) ? fake_link_stat(buf) : klg_stat(p, buf);
    return ((int (*)(int, const char *, void *, int))B("fstatat"))(dfd, p, buf, flags);
}
static void *klg_fopen(const char *p, const char *mode) {
    static char version[] = "Linux version 6.1.0-steamrt (steam@frame) (gcc 13) #1 SMP PREEMPT_DYNAMIC\n";
    if (g_cmdline && p && !strcmp(p, "/proc/self/cmdline")) return fmemopen(g_cmdline, g_cmdline_len, "rb");
    if (p && !strcmp(p, "/proc/version")) return fmemopen(version, sizeof version - 1, "rb");
    return ((void *(*)(const char *, const char *))B("fopen"))(p, mode);
}

// ---------- stat family (glibc's versioned entry points) ----------
static int klg___xstat(int ver, const char *p, void *buf) { return klg_stat(p, buf); }
static int klg___lxstat(int ver, const char *p, void *buf) { return klg_lstat(p, buf); }
static int klg___fxstat(int ver, int fd, void *buf) { return ((int (*)(int, void *))B("fstat"))(fd, buf); }
static int klg___fxstatat(int ver, int dfd, const char *p, void *buf, int flags) { return klg_fstatat(dfd, p, buf, flags); }
static int klg___xmknod(int ver, const char *p, mode_t mode, dev_t *dev) { return mknod(p, mode, dev ? *dev : 0); }
static int klg___open_2(const char *p, int flags) { return ((int (*)(const char *, int, ...))B("open"))(p, flags); }
static int klg___openat_2(int dfd, const char *p, int flags) { return ((int (*)(int, const char *, int, ...))B("openat"))(dfd, p, flags); }

// ---------- stdio internals ----------
static ssize_t klg___getdelim(char **lp, size_t *n, int delim, void *f) { return getdelim(lp, n, delim, kl_host_file(f)); }
static int klg___uflow(void *f) { return fgetc(kl_host_file(f)); }
static int klg___overflow(void *f, int c) { return fputc(c, kl_host_file(f)); }
static int klg___fsetlocking(void *f, int type) { return 1; /* FSETLOCKING_INTERNAL */ }
static void klg___fpurge(void *f) { fpurge(kl_host_file(f)); }
static int klg_getc_unlocked(void *f) { return getc_unlocked(kl_host_file(f)); }
static int klg_putc_unlocked(int c, void *f) { return putc_unlocked(c, kl_host_file(f)); }
static size_t klg_fread_unlocked(void *p, size_t s, size_t n, void *f) { return fread(p, s, n, kl_host_file(f)); }
static size_t klg_fwrite_unlocked(const void *p, size_t s, size_t n, void *f) { return fwrite(p, s, n, kl_host_file(f)); }
static int klg_fputs_unlocked(const char *s, void *f) { return fputs(s, kl_host_file(f)); }
static char *klg_fgets_unlocked(char *b, int n, void *f) { return fgets(b, n, kl_host_file(f)); }
static int klg_fflush_unlocked(void *f) { return fflush(f ? kl_host_file(f) : NULL); }
static int klg_feof_unlocked(void *f) { return feof(kl_host_file(f)); }
static int klg_ferror_unlocked(void *f) { return ferror(kl_host_file(f)); }
static void klg_clearerr_unlocked(void *f) { clearerr(kl_host_file(f)); }
static int klg_fileno_unlocked(void *f) { return fileno(kl_host_file(f)); }

// Variadic fortify printf family (kl_glibc_va.S thunks land here).
#define KLG_MARSHAL(fmt, va)                                               \
    char _m[1024] __attribute__((aligned(16)));                            \
    if (kl_va_marshal((fmt), (va), _m, sizeof _m, KL_VA_PRINTF) == (size_t)-1) \
        return -1;                                                         \
    va_list _ap = (va_list)_m
int klgh_printf_chk(int flag, const char *fmt, kl_va *va) { KLG_MARSHAL(fmt, va); return vprintf(fmt, _ap); }
int klgh_fprintf_chk(void *f, int flag, const char *fmt, kl_va *va) { KLG_MARSHAL(fmt, va); return vfprintf(kl_host_file(f), fmt, _ap); }
int klgh_sprintf_chk(char *s, int flag, size_t slen, const char *fmt, kl_va *va) { KLG_MARSHAL(fmt, va); return slen ? vsnprintf(s, slen, fmt, _ap) : vsprintf(s, fmt, _ap); }
int klgh_snprintf_chk(char *s, size_t n, int flag, size_t slen, const char *fmt, kl_va *va) { KLG_MARSHAL(fmt, va); return vsnprintf(s, n, fmt, _ap); }
int klgh_asprintf_chk(char **out, int flag, const char *fmt, kl_va *va) { KLG_MARSHAL(fmt, va); return vasprintf(out, fmt, _ap); }
int klgh_dprintf_chk(int fd, int flag, const char *fmt, kl_va *va) { KLG_MARSHAL(fmt, va); return vdprintf(fd, fmt, _ap); }
int klgh_syslog_chk(int pri, int flag, const char *fmt, kl_va *va) { KLG_MARSHAL(fmt, va); fprintf(stderr, "[guest syslog %d] ", pri); int n = vfprintf(stderr, fmt, _ap); fputc('\n', stderr); return n; }
int klgh_error(int status, int errnum, const char *fmt, kl_va *va) {
    KLG_MARSHAL(fmt, va);
    fflush(stdout); fprintf(stderr, "%s: ", g_progname); vfprintf(stderr, fmt, _ap);
    if (errnum) fprintf(stderr, ": %s", strerror(kl_errno_from_linux(errnum)));
    fputc('\n', stderr);
    if (status) exit(status);
    return 0;
}
int klgh_err(int status, const char *fmt, kl_va *va) { KLG_MARSHAL(fmt, va); fprintf(stderr, "%s: ", g_progname); vfprintf(stderr, fmt, _ap); fprintf(stderr, ": %s\n", strerror(errno)); exit(status); }
int klgh_errx(int status, const char *fmt, kl_va *va) { KLG_MARSHAL(fmt, va); fprintf(stderr, "%s: ", g_progname); vfprintf(stderr, fmt, _ap); fputc('\n', stderr); exit(status); }
int klgh_warn(const char *fmt, kl_va *va) { KLG_MARSHAL(fmt, va); fprintf(stderr, "%s: ", g_progname); vfprintf(stderr, fmt, _ap); fprintf(stderr, ": %s\n", strerror(errno)); return 0; }
int klgh_warnx(const char *fmt, kl_va *va) { KLG_MARSHAL(fmt, va); fprintf(stderr, "%s: ", g_progname); vfprintf(stderr, fmt, _ap); fputc('\n', stderr); return 0; }
// Non-variadic __v*_chk: the guest's va_list arrives by reference (a 32-byte
// AAPCS64 struct), which is exactly a kl_va *.
static int klg___vfprintf_chk(void *f, int flag, const char *fmt, kl_va *va) { KLG_MARSHAL(fmt, va); return vfprintf(kl_host_file(f), fmt, _ap); }
static int klg___vprintf_chk(int flag, const char *fmt, kl_va *va) { KLG_MARSHAL(fmt, va); return vprintf(fmt, _ap); }
static int klg___vsprintf_chk(char *s, int flag, size_t slen, const char *fmt, kl_va *va) { KLG_MARSHAL(fmt, va); return slen ? vsnprintf(s, slen, fmt, _ap) : vsprintf(s, fmt, _ap); }
static int klg___vsnprintf_chk(char *s, size_t n, int flag, size_t slen, const char *fmt, kl_va *va) { KLG_MARSHAL(fmt, va); return vsnprintf(s, n, fmt, _ap); }
static int klg___vasprintf_chk(char **out, int flag, const char *fmt, kl_va *va) { KLG_MARSHAL(fmt, va); return vasprintf(out, fmt, _ap); }
static int klg___vdprintf_chk(int fd, int flag, const char *fmt, kl_va *va) { KLG_MARSHAL(fmt, va); return vdprintf(fd, fmt, _ap); }
static int klg___vsyslog_chk(int pri, int flag, const char *fmt, kl_va *va) { KLG_MARSHAL(fmt, va); fprintf(stderr, "[guest syslog %d] ", pri); int n = vfprintf(stderr, fmt, _ap); fputc('\n', stderr); return n; }

// syscall(2): Linux puts the variadic arguments in registers like named ones,
// so the thunk hands us an AAPCS64 va_list over x1-x6.
static void exit_trace(const char *what, int code);
long klgh_syscall(long nr, kl_va *va) {
    long a[6];
    for (int i = 0; i < 6; i++) a[i] = (long)kl_va_gp(va);
    switch (nr) {
    case 178: { uint64_t t = 0; pthread_threadid_np(NULL, &t); return (long)t; }          // gettid
    case 278: arc4random_buf((void *)a[0], (size_t)a[1]); return a[1];                     // getrandom
    case 101: { struct timespec ts = { 0, 0 }; const struct timespec *rq = (const struct timespec *)a[0]; if (rq) nanosleep(rq, &ts); return 0; } // nanosleep
    case 172: return getpid();                                                               // getpid
    // tgkill(pid, tid, sig) / tkill(tid, sig) with sig 0: "is this thread still
    // alive?". Thread ids here are pthread_threadid_np values (gettid above);
    // proc_pidinfo answers for them. libvideo's per-thread profiler registry
    // reclaims entries whose thread answers ESRCH — returning an error for
    // live threads made it free other decoder threads' objects (crash in the
    // first level, a1_intro_world_2).
    case 131: case 130: {
        long tid = nr == 131 ? a[1] : a[0], sig = nr == 131 ? a[2] : a[1];
        if (sig == 0) {
            struct proc_threadinfo ti;
            if (tid > 0 && proc_pidinfo(getpid(), PROC_PIDTHREADID64INFO, (uint64_t)tid, &ti, sizeof ti) > 0) return 0;
            errno = ESRCH; return -1;
        }
        uint64_t self = 0; pthread_threadid_np(NULL, &self);
        if ((uint64_t)tid == self) return pthread_kill(pthread_self(), (int)sig) == 0 ? 0 : -1;
        long (*lower)(long, long, long, long, long, long, long) = (long (*)(long, long, long, long, long, long, long))B("syscall");
        if (lower) return lower(nr, a[0], a[1], a[2], a[3], a[4], a[5]);   // another thread, real signal
        errno = ENOSYS; return -1;
    }
    case 93: exit_trace("syscall(exit)", (int)a[0]); fflush(NULL); _exit((int)a[0]);      // exit
    case 94: exit_trace("syscall(exit_group)", (int)a[0]); fflush(NULL); _exit((int)a[0]); // exit_group
    default: {
        // Klepton's raw-syscall shim: futex (which Source 2's thread primitives
        // use directly), getrandom and the rest it knows.
        long (*lower)(long, long, long, long, long, long, long) = (long (*)(long, long, long, long, long, long, long))B("syscall");
        if (lower) return lower(nr, a[0], a[1], a[2], a[3], a[4], a[5]);
        fprintf(stderr, "  [glibc] syscall(%ld) not emulated\n", nr);
        errno = ENOSYS; return -1;
    }
    }
}

// ---------- setjmp / longjmp ----------
// glibc's __jmp_buf_tag (312 bytes) is larger than Darwin's jmp_buf (192) and
// sigjmp_buf (196), so Darwin's own routines can be forwarded on the guest's
// buffer.
static int (*g_darwin_setjmp)(void *);
static int (*g_darwin_sigsetjmp)(void *, int);
static void klg_siglongjmp(void *env, int val) { siglongjmp(env, val); }
static void klg_longjmp(void *env, int val) { longjmp(env, val); }

// ---------- signals: glibc's struct sigaction layout ----------
// glibc/aarch64: handler @0, 128-byte mask @8, int flags @136, restorer @144.
struct glibc_sigaction { void *handler; uint64_t mask[16]; int flags; int pad; void *restorer; };
#define LSA_SIGINFO 4
static const int g_sig_l2d[32] = {
    [1] = SIGHUP, [2] = SIGINT, [3] = SIGQUIT, [4] = SIGILL, [5] = SIGTRAP, [6] = SIGABRT,
    [7] = SIGBUS, [8] = SIGFPE, [9] = SIGKILL, [10] = SIGUSR1, [11] = SIGSEGV, [12] = SIGUSR2,
    [13] = SIGPIPE, [14] = SIGALRM, [15] = SIGTERM, [17] = SIGCHLD, [18] = SIGCONT,
    [19] = SIGSTOP, [20] = SIGTSTP, [21] = SIGTTIN, [22] = SIGTTOU, [23] = SIGURG,
    [24] = SIGXCPU, [25] = SIGXFSZ, [26] = SIGVTALRM, [27] = SIGPROF, [28] = SIGWINCH,
    [29] = SIGIO, [31] = SIGSYS,
};
static struct glibc_sigaction g_guest_sa[32];       // what the guest installed, by Linux number
static int g_crash_handlers = -1;

static int sig_to_linux(int d) { for (int i = 1; i < 32; i++) if (g_sig_l2d[i] == d) return i; return 0; }
static void sig_tramp(int dsig, siginfo_t *si, void *uc) {
    int lsig = sig_to_linux(dsig);
    struct glibc_sigaction *sa = &g_guest_sa[lsig];
    if (!sa->handler || sa->handler == (void *)SIG_IGN) return;
    if (sa->handler == (void *)SIG_DFL) { signal(dsig, SIG_DFL); raise(dsig); return; }
    if (sa->flags & LSA_SIGINFO) {
        struct { int signo, errno_, code, pad; int pid, uid; char rest[104]; } lsi;
        memset(&lsi, 0, sizeof lsi);
        lsi.signo = lsig; lsi.code = si ? si->si_code : 0; lsi.pid = si ? si->si_pid : 0; lsi.uid = si ? (int)si->si_uid : 0;
        ((void (*)(int, void *, void *))sa->handler)(lsig, &lsi, NULL);
    } else {
        ((void (*)(int))sa->handler)(lsig);
    }
}
static int klg_sigaction(int lsig, const struct glibc_sigaction *act, struct glibc_sigaction *old) {
    if (lsig <= 0 || lsig >= 32 || !g_sig_l2d[lsig]) { errno = EINVAL; return -1; }
    int dsig = g_sig_l2d[lsig];
    if (old) *old = g_guest_sa[lsig];
    if (!act) return 0;
    if (g_crash_handlers < 0) g_crash_handlers = kl_env_on("KL_GLIBC_GUEST_CRASH_HANDLERS", 0);
    int fatal = dsig == SIGSEGV || dsig == SIGBUS || dsig == SIGILL || dsig == SIGFPE ||
                dsig == SIGABRT || dsig == SIGTRAP;
    g_guest_sa[lsig] = *act;
    if (fatal && !g_crash_handlers) {
        // The guest's crash reporter expects a Linux ucontext; leaving the
        // default disposition keeps the host's own crash report instead.
        static int said;
        if (!said++) fprintf(stderr, "  [glibc] guest crash handlers are not installed (KL_GLIBC_GUEST_CRASH_HANDLERS=1 to install)\n");
        return 0;
    }
    struct sigaction d;
    memset(&d, 0, sizeof d);
    if (act->handler == (void *)SIG_IGN || act->handler == (void *)SIG_DFL) d.sa_handler = (void (*)(int))act->handler;
    else { d.sa_sigaction = sig_tramp; d.sa_flags = SA_SIGINFO; }
    if (act->flags & 0x10000000) d.sa_flags |= SA_RESTART;
    if (act->flags & 0x08000000) d.sa_flags |= SA_ONSTACK;
    if (act->flags & 0x40000000) d.sa_flags |= SA_NODEFER;
    if (act->flags & 0x80000000u) d.sa_flags |= SA_RESETHAND;
    sigemptyset(&d.sa_mask);
    for (int i = 1; i < 32; i++) if ((act->mask[0] >> (i - 1)) & 1 && g_sig_l2d[i]) sigaddset(&d.sa_mask, g_sig_l2d[i]);
    return sigaction(dsig, &d, NULL);
}
static int klg___libc_current_sigrtmin(void) { return 34; }
static int klg___libc_current_sigrtmax(void) { return 64; }

// ---------- locale ----------
// glibc's LC_*_MASK bits and category numbers differ from Darwin's; locale_t is
// an opaque pointer on both, except glibc's LC_GLOBAL_LOCALE (-1).
static int mask_to_darwin(int m) {
    int d = 0;
    if (m & (1 << 0)) d |= LC_CTYPE_MASK;
    if (m & (1 << 1)) d |= LC_NUMERIC_MASK;
    if (m & (1 << 2)) d |= LC_TIME_MASK;
    if (m & (1 << 3)) d |= LC_COLLATE_MASK;
    if (m & (1 << 4)) d |= LC_MONETARY_MASK;
    if (m & (1 << 5)) d |= LC_MESSAGES_MASK;
    return d;
}
// glibc's locale_t is not opaque to its callers: <ctype.h>'s inline
// isxxx_l and libc++ (libv8_libcpp's num_put, for one) read
// locale->__ctype_b at offset 0x68 directly. So the guest gets glibc's
// struct __locale_struct layout, carrying the Darwin locale_t behind it.
struct klg_locale {
    void *locales[13];                               // __locales (unused)
    const unsigned short *ctype_b;                   // 0x68
    const int32_t *ctype_tolower, *ctype_toupper;    // 0x70, 0x78
    const char *names[13];                           // __names
    uint64_t magic;
    locale_t darwin;
};
#define KLG_LOCALE_MAGIC 0x6c636f6c676c6b31ull     // "1kglocl"
static struct klg_locale *klg_locale_wrap(locale_t d, const char *name) {
    struct klg_locale *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->ctype_b = g_ctype_b + 128;
    w->ctype_tolower = g_ctype_tolower + 128;
    w->ctype_toupper = g_ctype_toupper + 128;
    for (int i = 0; i < 13; i++) w->names[i] = "C";
    (void)name;
    w->magic = KLG_LOCALE_MAGIC;
    w->darwin = d;
    return w;
}
static struct klg_locale *klg_locale_of(void *l) {
    struct klg_locale *w = l;
    return (l && l != (void *)(intptr_t)-1 && w->magic == KLG_LOCALE_MAGIC) ? w : NULL;
}
static locale_t loc_in(void *l) {
    if (l == (void *)(intptr_t)-1) return LC_GLOBAL_LOCALE;
    struct klg_locale *w = klg_locale_of(l);
    return w ? w->darwin : (locale_t)l;
}
static _Thread_local struct klg_locale *t_cur_locale;   // NULL: the global locale
static void *klg_newlocale(int mask, const char *name, void *base) {
    // The game asks for "C" and "C.UTF-8" / "en_US.UTF-8"; Darwin spells the
    // latter en_US.UTF-8 and has no C.UTF-8.
    const char *n = name;
    if (n && (!strcmp(n, "C.UTF-8") || !strcmp(n, "C.utf8") || !strcmp(n, "POSIX"))) n = "en_US.UTF-8";
    struct klg_locale *bw = klg_locale_of(base);
    locale_t db = base ? loc_in(base) : NULL;
    locale_t r = newlocale(mask_to_darwin(mask), n, db);
    if (!r && n != name) r = newlocale(mask_to_darwin(mask), "C", db);
    if (!r) return NULL;
    if (bw) { bw->darwin = r; return bw; }             // glibc: base is modified and returned
    return klg_locale_wrap(r, name);
}
static void *klg_uselocale(void *l) {
    void *prev = t_cur_locale ? (void *)t_cur_locale : (void *)(intptr_t)-1;
    if (l) {
        uselocale(loc_in(l));
        t_cur_locale = klg_locale_of(l);
    }
    return prev;
}
static void *klg_duplocale(void *l) {
    locale_t d = duplocale(loc_in(l));
    return d ? klg_locale_wrap(d, NULL) : NULL;
}
static void klg_freelocale(void *l) {
    struct klg_locale *w = klg_locale_of(l);
    if (w) { if (w->darwin) freelocale(w->darwin); if (t_cur_locale == w) t_cur_locale = NULL; w->magic = 0; free(w); return; }
    if (l && l != (void *)(intptr_t)-1) freelocale((locale_t)l);
}
#define LOC1(ret, name, args, call) static ret klg_##name args { return call; }
LOC1(double, strtod_l, (const char *s, char **e, void *l), strtod_l(s, e, loc_in(l)))
LOC1(float, strtof_l, (const char *s, char **e, void *l), strtof_l(s, e, loc_in(l)))
LOC1(long double, strtold_l, (const char *s, char **e, void *l), strtold_l(s, e, loc_in(l)))
LOC1(long, strtol_l, (const char *s, char **e, int b, void *l), strtol_l(s, e, b, loc_in(l)))
LOC1(unsigned long, strtoul_l, (const char *s, char **e, int b, void *l), strtoul_l(s, e, b, loc_in(l)))
LOC1(long long, strtoll_l, (const char *s, char **e, int b, void *l), strtoll_l(s, e, b, loc_in(l)))
LOC1(unsigned long long, strtoull_l, (const char *s, char **e, int b, void *l), strtoull_l(s, e, b, loc_in(l)))
LOC1(int, strcoll_l, (const char *a, const char *b, void *l), strcoll_l(a, b, loc_in(l)))
LOC1(size_t, strxfrm_l, (char *d, const char *s, size_t n, void *l), strxfrm_l(d, s, n, loc_in(l)))
LOC1(int, wcscoll_l, (const wchar_t *a, const wchar_t *b, void *l), wcscoll_l(a, b, loc_in(l)))
LOC1(size_t, wcsxfrm_l, (wchar_t *d, const wchar_t *s, size_t n, void *l), wcsxfrm_l(d, s, n, loc_in(l)))
LOC1(wint_t, towlower_l, (wint_t c, void *l), towlower_l(c, loc_in(l)))
LOC1(wint_t, towupper_l, (wint_t c, void *l), towupper_l(c, loc_in(l)))
LOC1(int, iswctype_l, (wint_t c, unsigned long t, void *l), iswctype_l(c, (wctype_t)t, loc_in(l)))
LOC1(unsigned long, wctype_l, (const char *n, void *l), (unsigned long)wctype_l(n, loc_in(l)))
LOC1(size_t, strftime_l, (char *s, size_t n, const char *f, const struct tm *t, void *l), strftime_l(s, n, f, t, loc_in(l)))
LOC1(size_t, wcsftime_l, (wchar_t *s, size_t n, const wchar_t *f, const struct tm *t, void *l), wcsftime_l(s, n, f, t, loc_in(l)))
LOC1(int, toupper_l, (int c, void *l), toupper_l(c, loc_in(l)))
LOC1(int, tolower_l, (int c, void *l), tolower_l(c, loc_in(l)))
LOC1(int, isspace_l, (int c, void *l), isspace_l(c, loc_in(l)))
LOC1(int, isalpha_l, (int c, void *l), isalpha_l(c, loc_in(l)))
LOC1(int, isdigit_l, (int c, void *l), isdigit_l(c, loc_in(l)))
LOC1(int, isalnum_l, (int c, void *l), isalnum_l(c, loc_in(l)))
LOC1(int, isupper_l, (int c, void *l), isupper_l(c, loc_in(l)))
LOC1(int, islower_l, (int c, void *l), islower_l(c, loc_in(l)))
LOC1(int, isxdigit_l, (int c, void *l), isxdigit_l(c, loc_in(l)))
LOC1(int, ispunct_l, (int c, void *l), ispunct_l(c, loc_in(l)))
LOC1(int, isprint_l, (int c, void *l), isprint_l(c, loc_in(l)))
LOC1(int, iswalpha_l, (wint_t c, void *l), iswalpha_l(c, loc_in(l)))
LOC1(int, iswdigit_l, (wint_t c, void *l), iswdigit_l(c, loc_in(l)))
LOC1(int, iswspace_l, (wint_t c, void *l), iswspace_l(c, loc_in(l)))
LOC1(int, iswupper_l, (wint_t c, void *l), iswupper_l(c, loc_in(l)))
LOC1(int, iswlower_l, (wint_t c, void *l), iswlower_l(c, loc_in(l)))
LOC1(int, iswalnum_l, (wint_t c, void *l), iswalnum_l(c, loc_in(l)))
LOC1(int, iswpunct_l, (wint_t c, void *l), iswpunct_l(c, loc_in(l)))
LOC1(int, iswprint_l, (wint_t c, void *l), iswprint_l(c, loc_in(l)))
LOC1(int, iswcntrl_l, (wint_t c, void *l), iswcntrl_l(c, loc_in(l)))
LOC1(int, iswxdigit_l, (wint_t c, void *l), iswxdigit_l(c, loc_in(l)))
LOC1(int, iswblank_l, (wint_t c, void *l), iswblank_l(c, loc_in(l)))
LOC1(int, iswgraph_l, (wint_t c, void *l), iswgraph_l(c, loc_in(l)))
LOC1(wint_t, btowc_l, (int c, void *l), btowc_l(c, loc_in(l)))
LOC1(int, wctob_l, (wint_t c, void *l), wctob_l(c, loc_in(l)))
LOC1(size_t, mbrtowc_l, (wchar_t *w, const char *s, size_t n, mbstate_t *ps, void *l), mbrtowc_l(w, s, n, ps, loc_in(l)))
LOC1(size_t, wcrtomb_l, (char *s, wchar_t w, mbstate_t *ps, void *l), wcrtomb_l(s, w, ps, loc_in(l)))
LOC1(size_t, mbsrtowcs_l, (wchar_t *d, const char **s, size_t n, mbstate_t *ps, void *l), mbsrtowcs_l(d, s, n, ps, loc_in(l)))
LOC1(size_t, wcsrtombs_l, (char *d, const wchar_t **s, size_t n, mbstate_t *ps, void *l), wcsrtombs_l(d, s, n, ps, loc_in(l)))
LOC1(size_t, mbrlen_l, (const char *s, size_t n, mbstate_t *ps, void *l), mbrlen_l(s, n, ps, loc_in(l)))
LOC1(wctrans_t, wctrans_l, (const char *n, void *l), wctrans_l(n, loc_in(l)))
LOC1(wint_t, towctrans_l, (wint_t c, wctrans_t t, void *l), towctrans_l(c, t, loc_in(l)))
LOC1(struct lconv *, localeconv_l, (void *l), localeconv_l(loc_in(l)))

// glibc's nl_item encoding: category << 16 | index.
static nl_item langinfo_to_darwin(int item) {
    int cat = item >> 16, idx = item & 0xffff;
    switch (cat) {
    case 0: return idx == 14 ? CODESET : -1;                       // LC_CTYPE: CODESET
    case 1: return idx == 0 ? RADIXCHAR : idx == 1 ? THOUSEP : -1;  // LC_NUMERIC
    case 2:                                                         // LC_TIME
        if (idx < 7) return ABDAY_1 + idx;
        if (idx < 14) return DAY_1 + idx - 7;
        if (idx < 26) return ABMON_1 + idx - 14;
        if (idx < 38) return MON_1 + idx - 26;
        switch (idx) {
        case 38: return AM_STR; case 39: return PM_STR; case 40: return D_T_FMT;
        case 41: return D_FMT; case 42: return T_FMT; case 43: return T_FMT_AMPM;
        case 44: return ERA; case 45: return ERA_D_FMT; case 46: return ALT_DIGITS;
        case 47: return ERA_D_T_FMT; case 48: return ERA_T_FMT;
        }
        return -1;
    case 4: return idx == 15 ? CRNCYSTR : -1;                       // LC_MONETARY
    case 5: return idx == 0 ? YESEXPR : idx == 1 ? NOEXPR : -1;     // LC_MESSAGES
    }
    return -1;
}
static char *klg_nl_langinfo_l(int item, void *l) {
    nl_item d = langinfo_to_darwin(item);
    if (d < 0) return (char *)"";
    if (d == CODESET) return (char *)"UTF-8";
    return nl_langinfo_l(d, loc_in(l));
}
static char *klg_nl_langinfo(int item) { return klg_nl_langinfo_l(item, (void *)(intptr_t)-1); }

// ---------- strings, memory, math ----------
static void *klg___rawmemchr(const void *s, int c) { const unsigned char *p = s; while (*p != (unsigned char)c) p++; return (void *)p; }
static void *klg_mempcpy(void *d, const void *s, size_t n) { return (char *)memcpy(d, s, n) + n; }
static char *klg_strerror_r(int err, char *buf, size_t len) {
    // GNU flavour: returns the string (XSI, which Klepton serves, returns int).
    int (*xpg)(int, char *, size_t) = (int (*)(int, char *, size_t))B("strerror_r");
    if (xpg) { if (xpg(err, buf, len) != 0 && len) snprintf(buf, len, "Unknown error %d", err); return buf; }
    if (len) snprintf(buf, len, "%s", strerror(kl_errno_from_linux(err)));
    return buf;
}
struct klg_qsort_ctx { int (*cmp)(const void *, const void *, void *); void *arg; };
static int klg_qsort_thunk(void *c, const void *a, const void *b) { struct klg_qsort_ctx *x = c; return x->cmp(a, b, x->arg); }
static void klg_qsort_r(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *, void *), void *arg) {
    // glibc: cmp(a, b, arg); Darwin: cmp(arg, a, b) and argument order (base, n, sz, arg, cmp).
    struct klg_qsort_ctx c = { cmp, arg };
    qsort_r(base, n, sz, &c, klg_qsort_thunk);
}
static char *klg_strchrnul(const char *s, int c) { while (*s && *s != (char)c) s++; return (char *)s; }
static int klg___isnan(double x) { return isnan(x); }
static int klg___isnanf(float x) { return isnan(x); }
static int klg___isinf(double x) { return isinf(x); }
static int klg___isinff(float x) { return isinf(x); }
static int klg___finite(double x) { return isfinite(x); }
static int klg___finitef(float x) { return isfinite(x); }
static int klg___signbit(double x) { return signbit(x); }
static int klg___signbitf(float x) { return signbit(x); }
static int klg___fpclassify(double x) { return fpclassify(x); }
static int klg___fpclassifyf(float x) { return fpclassify(x); }
static const char *klg_gnu_get_libc_version(void) { return "2.38"; }
static const char *klg_gnu_get_libc_release(void) { return "stable"; }

// ---------- process, scheduling, misc Linux ----------
static unsigned long klg___getauxval(unsigned long type) {
    static unsigned char rnd[16]; static int have_rnd;
    switch (type) {
    case 6: return (unsigned long)getpagesize();                   // AT_PAGESZ
    case 15: return (unsigned long)(uintptr_t)"aarch64";           // AT_PLATFORM
    case 16: return 0;                                              // AT_HWCAP: no optional features
    case 17: return 100;                                            // AT_CLKTCK
    case 23: return 0;                                              // AT_SECURE
    case 25: if (!have_rnd) { arc4random_buf(rnd, sizeof rnd); have_rnd = 1; } return (unsigned long)(uintptr_t)rnd;   // AT_RANDOM
    case 26: return 0;                                              // AT_HWCAP2
    case 31: return (unsigned long)(uintptr_t)g_argv0;             // AT_EXECFN
    case 51: return 4096;                                           // AT_MINSIGSTKSZ
    }
    errno = ENOENT; return 0;
}
static int klg___register_atfork(void (*prep)(void), void (*parent)(void), void (*child)(void), void *dso) { return 0; }
static int klg___cxa_thread_atexit_impl(void (*dtor)(void *), void *obj, void *dso) { return kl_tls_thread_atexit(dtor, obj, dso); }
static int klg__dl_find_object(void *pc, void *result) { return -1; }   // libgcc then uses dl_iterate_phdr
static void *klg_dlmopen(long ns, const char *f, int mode) { return ((void *(*)(const char *, int))B("dlopen"))(f, mode); }
static void *klg_dlvsym(void *h, const char *name, const char *ver) { return ((void *(*)(void *, const char *))B("dlsym"))(h, name); }
static int klg_dlinfo(void *h, int req, void *info) { errno = EINVAL; return -1; }
static int klg_dladdr1(const void *addr, void *info, void **extra, int flags) { if (extra) *extra = NULL; return ((int (*)(const void *, void *))B("dladdr"))(addr, info); }
static int klg___sched_cpucount(size_t setsize, const uint64_t *set) { int n = 0; for (size_t i = 0; i < setsize / 8; i++) n += __builtin_popcountll(set[i]); return n; }
static int ncpu(void) { int n = (int)sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? n : 1; }
static int klg_sched_getaffinity(int pid, size_t size, uint64_t *set) { memset(set, 0, size); int n = ncpu(); for (int i = 0; i < n && (size_t)i < size * 8; i++) set[i / 64] |= 1ull << (i % 64); return 0; }
static int klg_sched_setaffinity(int pid, size_t size, const void *set) { return 0; }
static int klg_pthread_setaffinity_np(pthread_t t, size_t size, const void *set) { return 0; }
static int klg_pthread_getaffinity_np(pthread_t t, size_t size, uint64_t *set) { return klg_sched_getaffinity(0, size, set); }
static int klg_get_nprocs(void) { return ncpu(); }
static int klg_sched_getcpu(void) { return 0; }
static int klg_gettid(void) { uint64_t t = 0; pthread_threadid_np(NULL, &t); return (int)t; }
static int klg_prctl(int opt, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5) {
    if (opt == 15) { pthread_setname_np((const char *)(uintptr_t)a2); return 0; }    // PR_SET_NAME
    if (opt == 16) { pthread_getname_np(pthread_self(), (char *)(uintptr_t)a2, 16); return 0; }
    return 0;
}
static int klg_pipe2(int fds[2], int flags) {
    if (pipe(fds) != 0) return -1;
    for (int i = 0; i < 2; i++) {
        if (flags & 0x80000) fcntl(fds[i], F_SETFD, FD_CLOEXEC);               // O_CLOEXEC
        if (flags & 0x800) fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);   // O_NONBLOCK
    }
    return 0;
}
static int klg_dup3(int a, int b, int flags) { int r = dup2(a, b); if (r >= 0 && (flags & 0x80000)) fcntl(r, F_SETFD, FD_CLOEXEC); return r; }
static int klg_accept4(int fd, void *addr, void *len, int flags) {
    int r = ((int (*)(int, void *, void *))B("accept"))(fd, addr, len);
    if (r >= 0) {
        if (flags & 0x80000) fcntl(r, F_SETFD, FD_CLOEXEC);
        if (flags & 0x800) fcntl(r, F_SETFL, fcntl(r, F_GETFL) | O_NONBLOCK);
    }
    return r;
}
static ssize_t klg_getrandom(void *buf, size_t n, unsigned flags) { arc4random_buf(buf, n); return (ssize_t)n; }
static int klg_close_range(unsigned first, unsigned last, int flags) { if (last > 65535) last = 65535; for (unsigned i = first; i <= last; i++) close((int)i); return 0; }
static int klg_enosys(void) { errno = ENOSYS; return -1; }
static void *klg_mremap(void *old, size_t olds, size_t news, int flags, void *newaddr) { errno = ENOSYS; return (void *)-1; }
static int klg_memfd_create(const char *name, unsigned flags) {
    char tmpl[] = "/tmp/klepton-memfd-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) unlink(tmpl);
    return fd;
}
static int klg_malloc_trim(size_t pad) { return 0; }
static void klg_malloc_stats(void) {}
static int klg_mallopt(int p, int v) { return 1; }
struct glibc_mallinfo { int arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost; };
static struct glibc_mallinfo klg_mallinfo(void) { struct glibc_mallinfo m; memset(&m, 0, sizeof m); return m; }
static int klg_res_init(void) { return 0; }
static int klg___res_ninit(void *st) { return 0; }
static void klg___res_nclose(void *st) {}
static void *klg___res_state(void) { static char st[512]; return st; }

// pthread extensions glibc has and bionic lacks.
static void realtime_from_clock(int clockid, const struct timespec *abs, struct timespec *out) {
    // Linux CLOCK_MONOTONIC = 1, CLOCK_REALTIME = 0, BOOTTIME = 7.
    if (clockid == 0) { *out = *abs; return; }
    struct timespec now_m, now_r;
    clock_gettime(CLOCK_MONOTONIC, &now_m);
    clock_gettime(CLOCK_REALTIME, &now_r);
    int64_t delta = ((int64_t)abs->tv_sec - now_m.tv_sec) * 1000000000LL + (abs->tv_nsec - now_m.tv_nsec);
    int64_t t = (int64_t)now_r.tv_sec * 1000000000LL + now_r.tv_nsec + delta;
    if (t < 0) t = 0;
    out->tv_sec = t / 1000000000LL; out->tv_nsec = t % 1000000000LL;
}
static int klg_pthread_cond_clockwait(void *c, void *m, int clockid, const struct timespec *abs) {
    struct timespec rt; realtime_from_clock(clockid, abs, &rt);
    return ((int (*)(void *, void *, const struct timespec *))B("pthread_cond_timedwait"))(c, m, &rt);
}
static int klg_pthread_mutex_clocklock(void *m, int clockid, const struct timespec *abs) {
    int (*timed)(void *, const struct timespec *) = (int (*)(void *, const struct timespec *))B("pthread_mutex_timedlock");
    struct timespec rt; realtime_from_clock(clockid, abs, &rt);
    if (timed) return timed(m, &rt);
    int (*trylock)(void *) = (int (*)(void *))B("pthread_mutex_trylock");
    for (;;) {
        int r = trylock(m);
        if (r != 16 /* EBUSY (Linux) */ && r != EBUSY) return r;
        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > rt.tv_sec || (now.tv_sec == rt.tv_sec && now.tv_nsec >= rt.tv_nsec)) return 110; /* ETIMEDOUT (Linux) */
        usleep(200);
    }
}
static int klg_pthread_rwlock_clockrdlock(void *l, int clockid, const struct timespec *abs) { return ((int (*)(void *))B("pthread_rwlock_rdlock"))(l); }
static int klg_pthread_rwlock_clockwrlock(void *l, int clockid, const struct timespec *abs) { return ((int (*)(void *))B("pthread_rwlock_wrlock"))(l); }
static int klg_sem_clockwait(void *s, int clockid, const struct timespec *abs) {
    struct timespec rt; realtime_from_clock(clockid, abs, &rt);
    return ((int (*)(void *, const struct timespec *))B("sem_timedwait"))(s, &rt);
}
static int klg_pthread_tryjoin_np(pthread_t t, void **ret) { return ((int (*)(pthread_t, void **))B("pthread_join"))(t, ret); }
static int klg_pthread_timedjoin_np(pthread_t t, void **ret, const struct timespec *abs) { return ((int (*)(pthread_t, void **))B("pthread_join"))(t, ret); }
static int klg_pthread_setname_np(pthread_t t, const char *name) { return ((int (*)(pthread_t, const char *))B("pthread_setname_np"))(t, name); }
static int klg_pthread_getname_np(pthread_t t, char *buf, size_t len) { return pthread_getname_np(t, buf, len); }
static int klg_pthread_yield(void) { sched_yield(); return 0; }

// ---------- eventfd(2) ----------
// An eventfd is one descriptor that is written and read (8-byte counter) and
// polled. A stream socketpair stands in: the guest's fd is one end, writes to
// it are redirected to the other, so a read or poll on the guest's fd sees
// what was written. read/write/close are routed through here for that.
static struct { int a, b; } g_evfd[256];
static pthread_mutex_t g_evfd_mu = PTHREAD_MUTEX_INITIALIZER;
static int evfd_peer(int fd) {
    for (size_t i = 0; i < sizeof g_evfd / sizeof g_evfd[0]; i++) if (g_evfd[i].a == fd && fd > 0) return g_evfd[i].b;
    return -1;
}
// mmap through Klepton's flag translation, with KL_GLIBC_TRACE_MMAP=1 logging
// every call (hint, length, flags, result, errno) for allocator debugging.
static void *klg_mmap_traced(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    static void *(*lower)(void *, size_t, int, int, int, off_t);
    static int trace = -1;
    if (!lower) lower = (void *(*)(void *, size_t, int, int, int, off_t))B("mmap");
    if (trace < 0) { const char *e = getenv("KL_GLIBC_TRACE_MMAP"); trace = e && *e && *e != '0'; }
    void *p = lower ? lower(addr, len, prot, flags, fd, off) : MAP_FAILED;
    if (trace) fprintf(stderr, "  [glibc] mmap(%p, %zu, prot %d, flags %#x, fd %d, off %lld) -> %p errno %d\n",
                       addr, len, prot, flags, fd, (long long)off, p, p == MAP_FAILED ? errno : 0);
    return p;
}
// KL_TRACE_TSD_IMAGE=<lib>: log pthread_setspecific/getspecific made from
// that image (key, value, thread, caller offset), to follow per-thread objects.
static const char *tsd_trace_image(void) {
    static const char *img = (const char *)-1;
    if (img == (const char *)-1) img = getenv("KL_TRACE_TSD_IMAGE");
    return img;
}
static int tsd_from_traced(void *ra, size_t *off) {
    const char *want = tsd_trace_image();
    if (!want) return 0;
    const char *who = kl_addr_image(ra, off);
    return who && strstr(who, want);
}
// KL_TRACE_FREE_SIZE=<bytes> with KL_TRACE_TSD_IMAGE: frees of blocks of that
// size made from the traced image, with the caller chain.
static void klg_free_trace(void *p) {
    static void (*real)(void *);
    static long want = -1;
    if (!real) real = (void (*)(void *))B("free");
    if (want < 0) { const char *e = getenv("KL_TRACE_FREE_SIZE"); want = e ? atol(e) : 0; }
    if (p && want > 0) {
        size_t sz = malloc_size(p), off = 0;
        static int nlog;
        if (sz >= (size_t)want && sz <= (size_t)want + 128 && nlog < 200 && tsd_from_traced(__builtin_return_address(0), &off)) {
            nlog++;
            uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
            fprintf(stderr, "  [free] %p (%zu bytes) thread %llu from +%#zx\n", p, sz, (unsigned long long)tid, off);
            kl_fault_print_frames(stderr, __builtin_frame_address(0));
        }
    }
    if (real) real(p);
}
static int klg_setspecific_trace(unsigned key, const void *v) {
    int (*real)(unsigned, const void *) = (int (*)(unsigned, const void *))B("pthread_setspecific");
    size_t off = 0;
    if (tsd_from_traced(__builtin_return_address(0), &off)) {
        uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
        fprintf(stderr, "  [tsd] set key %u = %p (thread %llu, from +%#zx)\n", key, v, (unsigned long long)tid, off);
    }
    return real ? real(key, v) : -1;
}
static void *klg_getspecific_trace(unsigned key) {
    void *(*real)(unsigned) = (void *(*)(unsigned))B("pthread_getspecific");
    void *v = real ? real(key) : NULL;
    size_t off = 0;
    if (v && tsd_from_traced(__builtin_return_address(0), &off)) {
        static int n; if (n++ < 400) {
            uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
            const uint64_t *o = v;
            fprintf(stderr, "  [tsd] get key %u -> %p [8]=%#llx [0x18]=%#llx (thread %llu, from +%#zx)\n", key, v,
                    (unsigned long long)(o[1] & 0xffffffff), (unsigned long long)o[3], (unsigned long long)tid, off);
        }
    }
    return v;
}
static int klg_eventfd(unsigned initval, int flags) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    if (flags & 0x800) fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);   // EFD_NONBLOCK
    fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK);
    if (flags & 0x80000) { fcntl(sv[0], F_SETFD, FD_CLOEXEC); fcntl(sv[1], F_SETFD, FD_CLOEXEC); }
    pthread_mutex_lock(&g_evfd_mu);
    size_t i; for (i = 0; i < sizeof g_evfd / sizeof g_evfd[0] && g_evfd[i].a; i++);
    if (i == sizeof g_evfd / sizeof g_evfd[0]) { pthread_mutex_unlock(&g_evfd_mu); close(sv[0]); close(sv[1]); errno = EMFILE; return -1; }
    g_evfd[i].a = sv[0]; g_evfd[i].b = sv[1];
    pthread_mutex_unlock(&g_evfd_mu);
    if (initval) { uint64_t v = initval; write(sv[1], &v, 8); }
    return sv[0];
}
static ssize_t (*g_read)(int, void *, size_t);
static ssize_t (*g_write)(int, const void *, size_t);
static int (*g_close)(int);
static ssize_t klg_read(int fd, void *buf, size_t n) { return g_read(fd, buf, n); }
static ssize_t klg_write(int fd, const void *buf, size_t n) {
    int peer = evfd_peer(fd);
    if (peer >= 0) { ssize_t r = write(peer, buf, n); return r < 0 ? r : (ssize_t)n; }
    return g_write(fd, buf, n);
}
static int klg_close(int fd) {
    pthread_mutex_lock(&g_evfd_mu);
    for (size_t i = 0; i < sizeof g_evfd / sizeof g_evfd[0]; i++)
        if (g_evfd[i].a == fd && fd > 0) { close(g_evfd[i].b); g_evfd[i].a = g_evfd[i].b = 0; break; }
    pthread_mutex_unlock(&g_evfd_mu);
    return g_close(fd);
}
static int klg_eventfd_read(int fd, uint64_t *v) { return read(fd, v, 8) == 8 ? 0 : -1; }
static int klg_eventfd_write(int fd, uint64_t v) { return klg_write(fd, &v, 8) == 8 ? 0 : -1; }

static ssize_t klg_readlink(const char *path, char *buf, size_t n) {
    if (is_self_exe(path)) {
        size_t l = strlen(g_exe_path); if (l > n) l = n;
        memcpy(buf, g_exe_path, l); return (ssize_t)l;
    }
    return ((ssize_t (*)(const char *, char *, size_t))B("readlink"))(path, buf, n);
}
static ssize_t klg_readlinkat(int dfd, const char *path, char *buf, size_t n) {
    if (is_self_exe(path)) return klg_readlink(path, buf, n);
    return ((ssize_t (*)(int, const char *, char *, size_t))B("readlinkat"))(dfd, path, buf, n);
}

// ---------- sysconf: glibc's _SC_* numbering (bionic's differs) ----------
static long sysctl_long(const char *name, long dflt) {
    int64_t v = 0; size_t n = sizeof v;
    return sysctlbyname(name, &v, &n, NULL, 0) == 0 ? (long)v : dflt;
}
static long klg_sysconf(int name) {
    switch (name) {
    case 0: return sysconf(_SC_ARG_MAX);
    case 1: return sysconf(_SC_CHILD_MAX);
    case 2: return 100;                                  // _SC_CLK_TCK
    case 3: return sysconf(_SC_NGROUPS_MAX);
    case 4: return sysconf(_SC_OPEN_MAX);
    case 5: return sysconf(_SC_STREAM_MAX);
    case 6: return sysconf(_SC_TZNAME_MAX);
    case 7: return 1;                                    // _SC_JOB_CONTROL
    case 8: return 1;                                    // _SC_SAVED_IDS
    case 9: return 200809L;                              // _SC_REALTIME_SIGNALS
    case 29: return 200809L;                             // _SC_VERSION
    case 30: return getpagesize();                       // _SC_PAGESIZE
    case 31: return 32;                                  // _SC_RTSIG_MAX
    case 32: return 256;                                 // _SC_SEM_NSEMS_MAX
    case 33: return 32767;                               // _SC_SEM_VALUE_MAX
    case 34: return 32;                                  // _SC_SIGQUEUE_MAX
    case 43: return 2048;                                // _SC_LINE_MAX
    case 60: return sysconf(_SC_IOV_MAX);
    case 69: return 1024;                                // _SC_GETGR_R_SIZE_MAX
    case 70: return 1024;                                // _SC_GETPW_R_SIZE_MAX
    case 71: return 32;                                  // _SC_TTY_NAME_MAX
    case 72: return 256;                                 // _SC_LOGIN_NAME_MAX
    case 73: return 4;                                   // _SC_THREAD_DESTRUCTOR_ITERATIONS
    case 74: return 512;                                 // _SC_THREAD_KEYS_MAX
    case 75: return 16384;                               // _SC_THREAD_STACK_MIN
    case 76: return sysconf(_SC_THREAD_THREADS_MAX);
    case 83: return sysconf(_SC_NPROCESSORS_CONF);
    case 84: return sysconf(_SC_NPROCESSORS_ONLN);
    case 85: return sysconf(_SC_PHYS_PAGES);
    case 86: return sysconf(_SC_PHYS_PAGES) / 2;         // _SC_AVPHYS_PAGES
    case 149: return 200809L;                            // _SC_MONOTONIC_CLOCK
    case 180: return 255;                                // _SC_HOST_NAME_MAX
    case 185: return sysctl_long("hw.l1icachesize", 131072);   // _SC_LEVEL1_ICACHE_SIZE
    case 186: return 8;
    case 187: return sysctl_long("hw.cachelinesize", 128);
    case 188: return sysctl_long("hw.l1dcachesize", 65536);    // _SC_LEVEL1_DCACHE_SIZE
    case 189: return 8;                                        // _SC_LEVEL1_DCACHE_ASSOC
    case 190: return sysctl_long("hw.cachelinesize", 128);     // _SC_LEVEL1_DCACHE_LINESIZE
    case 191: return sysctl_long("hw.l2cachesize", 4194304);   // _SC_LEVEL2_CACHE_SIZE
    case 192: return 16;
    case 193: return sysctl_long("hw.cachelinesize", 128);     // _SC_LEVEL2_CACHE_LINESIZE
    case 194: case 195: case 196: return 0;                    // no L3
    }
    static int said[8]; static int nsaid;
    for (int i = 0; i < nsaid; i++) if (said[i] == name) { errno = EINVAL; return -1; }
    if (nsaid < 8) said[nsaid++] = name;
    fprintf(stderr, "  [glibc] sysconf(%d): no mapping\n", name);
    errno = EINVAL; return -1;
}

// ---------- epoll(7) on kqueue ----------
// Linux/aarch64 epoll_event is 16 bytes: uint32 events @0, uint64 data @8.
struct lx_epoll_event { uint32_t events; uint32_t pad; uint64_t data; };
enum { LX_EPOLLIN = 1, LX_EPOLLPRI = 2, LX_EPOLLOUT = 4, LX_EPOLLERR = 8, LX_EPOLLHUP = 0x10,
       LX_EPOLLRDHUP = 0x2000, LX_EPOLLONESHOT = 1u << 30, LX_EPOLLET = 1u << 31 };
static int klg_epoll_create1(int flags) { int kq = kqueue(); if (kq >= 0 && (flags & 0x80000)) fcntl(kq, F_SETFD, FD_CLOEXEC); return kq; }
static int klg_epoll_create(int size) { return klg_epoll_create1(0); }
static int klg_epoll_ctl(int epfd, int op, int fd, const struct lx_epoll_event *ev) {
    struct kevent64_s kev[2]; int n = 0;
    uint16_t fl = (op == 2 /* EPOLL_CTL_DEL */) ? EV_DELETE : EV_ADD | EV_ENABLE;
    if (op != 2 && ev) { if (ev->events & LX_EPOLLET) fl |= EV_CLEAR; if (ev->events & LX_EPOLLONESHOT) fl |= EV_ONESHOT; }
    uint64_t data = ev ? ev->data : 0;
    if (op == 2 || (ev->events & LX_EPOLLIN)) EV_SET64(&kev[n++], (uint64_t)fd, EVFILT_READ, fl, 0, 0, data, 0, 0);
    if (op == 2 || (ev->events & LX_EPOLLOUT)) EV_SET64(&kev[n++], (uint64_t)fd, EVFILT_WRITE, fl, 0, 0, data, 0, 0);
    if (op == 3 /* MOD */) {   // drop the filters the new mask no longer asks for
        struct kevent64_s del[2]; int m = 0;
        if (!(ev->events & LX_EPOLLIN)) EV_SET64(&del[m++], (uint64_t)fd, EVFILT_READ, EV_DELETE, 0, 0, 0, 0, 0);
        if (!(ev->events & LX_EPOLLOUT)) EV_SET64(&del[m++], (uint64_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, 0, 0, 0);
        if (m) kevent64(epfd, del, m, NULL, 0, 0, NULL);
    }
    int r = 0;
    for (int i = 0; i < n; i++) {
        if (kevent64(epfd, &kev[i], 1, NULL, 0, 0, NULL) != 0 && !(op == 2 && errno == ENOENT)) r = -1;
    }
    if (r == 0 && op == 2) errno = 0;
    return r;
}
static int klg_epoll_wait(int epfd, struct lx_epoll_event *out, int max, int timeout_ms) {
    if (max <= 0) { errno = EINVAL; return -1; }
    if (max > 64) max = 64;
    struct kevent64_s kev[64];
    struct timespec ts, *tp = NULL;
    if (timeout_ms >= 0) { ts.tv_sec = timeout_ms / 1000; ts.tv_nsec = (timeout_ms % 1000) * 1000000L; tp = &ts; }
    int n = kevent64(epfd, NULL, 0, kev, max, 0, tp);
    if (n < 0) return -1;
    int o = 0;
    for (int i = 0; i < n; i++) {
        uint32_t e = 0;
        if (kev[i].filter == EVFILT_READ) e |= LX_EPOLLIN;
        if (kev[i].filter == EVFILT_WRITE) e |= LX_EPOLLOUT;
        if (kev[i].flags & EV_EOF) e |= LX_EPOLLHUP | LX_EPOLLRDHUP;
        if (kev[i].flags & EV_ERROR) e |= LX_EPOLLERR;
        // Merge a read and a write report for the same fd+data into one entry.
        int merged = 0;
        for (int k = 0; k < o; k++) if (out[k].data == kev[i].udata) { out[k].events |= e; merged = 1; break; }
        if (!merged) { out[o].events = e; out[o].pad = 0; out[o].data = kev[i].udata; o++; }
    }
    return o;
}
static int klg_epoll_pwait(int epfd, struct lx_epoll_event *out, int max, int timeout_ms, const void *sigmask) { return klg_epoll_wait(epfd, out, max, timeout_ms); }

// ---------- inotify(7): accepted, never fires ----------
// The engine watches directories for hot reloading; a watch that never
// reports is correct for a shipping game. The fd is a socket end so poll and
// epoll accept it.
static int klg_inotify_init1(int flags) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    if (flags & 0x800) fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    if (flags & 0x80000) fcntl(sv[0], F_SETFD, FD_CLOEXEC);
    // sv[1] is leaked on purpose: closing it would make sv[0] readable (EOF).
    return sv[0];
}
static int klg_inotify_init(void) { return klg_inotify_init1(0); }
static int klg_inotify_add_watch(int fd, const char *path, uint32_t mask) { static int wd; return ++wd; }
static int klg_inotify_rm_watch(int fd, int wd) { return 0; }

// The engine ends with _exit(); flush the host-side FILE buffers first so its
// console.log (-condebug) and stdout survive.
static int (*g__exit)(int);
// KL_GLIBC_TRACE_EXIT=1: who ends the process (frames from the caller's fp).
static int g_trace_exit = -1;   // read at install: the engine edits its environment
static void exit_trace(const char *what, int code) {
    if (g_trace_exit < 0) { const char *e = getenv("KL_GLIBC_TRACE_EXIT"); g_trace_exit = e && *e && *e != '0'; }
    if (!g_trace_exit) return;
    fprintf(stderr, "  [glibc] %s(%d) called:\n", what, code);
    kl_fault_print_frames(stderr, __builtin_frame_address(0));
}
static void klg__exit(int code) { exit_trace("_exit", code); fflush(NULL); if (g__exit) g__exit(code); _exit(code); }
static void klg_exit(int code) { exit_trace("exit", code); exit(code); }

// dlsym/dlopen with a trace (KL_GLIBC_TRACE_DLSYM=1): what the guest asked, on
// which handle, and what it got.
static void *klg_dlsym(void *h, const char *name) {
    void *v = ((void *(*)(void *, const char *))B("dlsym"))(h, name);
    // glibc's dlsym(handle) searches the handle's whole dependency scope (the
    // engine asks libpangoft2's handle for fontconfig and FreeType symbols);
    // Klepton's answers for the one image. Widen a miss to every loaded image.
    if (!v && h && h != (void *)-1 && name) { v = kl_guest_sym_global(name); if (!v) v = kl_shim_lookup(name); }
    static int on = -1; if (on < 0) on = kl_env_on("KL_GLIBC_TRACE_DLSYM", 0);
    if (on) { size_t off = 0; const char *img = kl_addr_image(v, &off);
              fprintf(stderr, "  [dlsym] %p %s -> %p (%s+%#zx)\n", h, name ? name : "?", v, img ? img : "-", off); }
    return v;
}
static void *klg_dlopen(const char *path, int flags) {
    void *v = ((void *(*)(const char *, int))B("dlopen"))(path, flags);
    static int on = -1; if (on < 0) on = kl_env_on("KL_GLIBC_TRACE_DLSYM", 0);
    if (on) fprintf(stderr, "  [dlopen] %s flags %#x -> %p\n", path ? path : "(self)", flags, v);
    return v;
}

// fstatfs: Klepton converts statfs(path) to the Linux layout; reach it by path.
static int klg_fstatfs(int fd, void *buf) {
    char path[PATH_MAX];
    if (fcntl(fd, F_GETPATH, path) != 0) { errno = EBADF; return -1; }
    return ((int (*)(const char *, void *))B("statfs"))(path, buf);
}

// ---------- more of glibc: what glib, gio, fontconfig, libmount and friends import ----------
// random_r family: glibc's struct random_data (48 bytes) holds a xorshift state here.
struct lx_random_data { uint64_t state; uint64_t pad[5]; };
static int klg_srandom_r(unsigned seed, struct lx_random_data *b) { if (!b) { errno = EINVAL; return -1; } b->state = (uint64_t)seed * 0x9E3779B97F4A7C15ull + 0x12345; if (!b->state) b->state = 1; return 0; }
static int klg_initstate_r(unsigned seed, char *statebuf, size_t n, struct lx_random_data *b) { (void)statebuf; (void)n; return klg_srandom_r(seed, b); }
static int klg_setstate_r(char *statebuf, struct lx_random_data *b) { (void)statebuf; (void)b; return 0; }
static int klg_random_r(struct lx_random_data *b, int32_t *out) {
    if (!b || !out) { errno = EINVAL; return -1; }
    uint64_t x = b->state ? b->state : 1; x ^= x << 13; x ^= x >> 7; x ^= x << 17; b->state = x;
    *out = (int32_t)(x & 0x7fffffff); return 0;
}
static int klg_mkostemp(char *tmpl, int flags) { return mkostemp(tmpl, (flags & 0x80000) ? O_CLOEXEC : 0); }
static int klg_creat(const char *p, mode_t m) { return ((int (*)(const char *, int, ...))B("open"))(p, 01 | 0100 | 01000, m); }   // O_WRONLY|O_CREAT|O_TRUNC (Linux)
static char *klg___xpg_basename(char *p) { return basename(p); }
static int klg_pthread_attr_setinheritsched(void *attr, int inherit) { return 0; }
static int klg_enotsup(void) { errno = ENOTSUP; return -1; }
static int klg_eperm(void) { errno = EPERM; return -1; }
static void *klg_null(void) { return NULL; }
static int klg_zero(void) { return 0; }
static void *klg___sched_cpualloc(size_t count) { return calloc((count + 63) / 64, 8); }
static void klg___sched_cpufree(void *set) { free(set); }
static int klg_sched_getscheduler(int pid) { return 0; }
static int klg_sched_setscheduler(int pid, int policy, const void *param) { return 0; }
static int klg_sched_getparam(int pid, int *param) { if (param) *param = 0; return 0; }
// statx (glib's file info): built from fstatat's Linux stat.
struct lx_statx_ts { int64_t sec; uint32_t nsec; int32_t pad; };
struct lx_statx {
    uint32_t mask, blksize; uint64_t attributes; uint32_t nlink, uid, gid; uint16_t mode, pad0; uint64_t ino, size, blocks, attributes_mask;
    struct lx_statx_ts atime, btime, ctime, mtime; uint32_t rdev_major, rdev_minor, dev_major, dev_minor; uint64_t spare[14];
};
static int klg_statx(int dfd, const char *path, int flags, unsigned mask, struct lx_statx *out) {
    uint8_t st[128];
    int atflags = flags & 0x100;     // AT_SYMLINK_NOFOLLOW
    int r;
    if (flags & 0x1000 /* AT_EMPTY_PATH */ && (!path || !*path)) r = ((int (*)(int, void *))B("fstat"))(dfd, st);
    else r = klg_fstatat(dfd, path, st, atflags);
    if (r != 0) return r;
    memset(out, 0, sizeof *out);
    uint64_t dev = *(uint64_t *)(st + 0), ino = *(uint64_t *)(st + 8); uint32_t mode = *(uint32_t *)(st + 16), nlink = *(uint32_t *)(st + 20);
    uint32_t uid = *(uint32_t *)(st + 24), gid = *(uint32_t *)(st + 28); uint64_t rdev = *(uint64_t *)(st + 32);
    int64_t size = *(int64_t *)(st + 48); int32_t blksize = *(int32_t *)(st + 56); int64_t blocks = *(int64_t *)(st + 64);
    const int64_t *ts = (const int64_t *)(st + 72);    // atim, mtim, ctim as {sec, nsec}
    out->mask = 0x7ff; out->blksize = (uint32_t)blksize; out->nlink = nlink; out->uid = uid; out->gid = gid; out->mode = (uint16_t)mode;
    out->ino = ino; out->size = (uint64_t)size; out->blocks = (uint64_t)blocks;
    out->atime.sec = ts[0]; out->atime.nsec = (uint32_t)ts[1]; out->mtime.sec = ts[2]; out->mtime.nsec = (uint32_t)ts[3];
    out->ctime.sec = ts[4]; out->ctime.nsec = (uint32_t)ts[5]; out->btime = out->ctime;
    out->rdev_major = (uint32_t)((rdev >> 8) & 0xfff); out->rdev_minor = (uint32_t)(rdev & 0xff); out->dev_major = (uint32_t)((dev >> 8) & 0xfff); out->dev_minor = (uint32_t)(dev & 0xff);
    return 0;
}
// sendmmsg/recvmmsg as loops over Klepton's sendmsg/recvmsg.
struct lx_mmsghdr { struct { void *name; uint32_t namelen; uint32_t pad; void *iov; size_t iovlen; void *control; size_t controllen; int flags; int pad2; } hdr; unsigned len; unsigned pad; };
static int klg_sendmmsg(int fd, struct lx_mmsghdr *v, unsigned n, int flags) {
    ssize_t (*sendmsg_)(int, void *, int) = (ssize_t (*)(int, void *, int))B("sendmsg");
    unsigned i; for (i = 0; i < n; i++) { ssize_t r = sendmsg_(fd, &v[i].hdr, flags); if (r < 0) return i ? (int)i : -1; v[i].len = (unsigned)r; }
    return (int)i;
}
static int klg_recvmmsg(int fd, struct lx_mmsghdr *v, unsigned n, int flags, void *timeout) {
    ssize_t (*recvmsg_)(int, void *, int) = (ssize_t (*)(int, void *, int))B("recvmsg");
    unsigned i; for (i = 0; i < n; i++) { ssize_t r = recvmsg_(fd, &v[i].hdr, flags | (i ? 0x40 : 0)); if (r < 0) return i ? (int)i : -1; v[i].len = (unsigned)r; }
    return (int)i;
}
static int klg_strverscmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            unsigned long x = strtoul(a, (char **)&a, 10), y = strtoul(b, (char **)&b, 10);
            if (x != y) return x < y ? -1 : 1;
        } else { if (*a != *b) return (unsigned char)*a - (unsigned char)*b; a++; b++; }
    }
    return (unsigned char)*a - (unsigned char)*b;
}
static void *klg_catopen(const char *n, int f) { return (void *)(intptr_t)-1; }
static char *klg_catgets(void *c, int set, int msg, const char *s) { return (char *)s; }
static int klg_catclose(void *c) { return 0; }
static size_t klg___fpending(void *f) { return 0; }
static int klg_getservbyname_r(const char *name, const char *proto, struct servent *out, char *buf, size_t len, struct servent **res) {
    struct servent *s = getservbyname(name, proto);
    if (!s) { *res = NULL; return ENOENT; }
    *out = *s; *res = out; return 0;
}
// posix_spawn: glibc's attr/file_actions are structs; Darwin's are pointers kept in their first word.
#define SPAWN_PTR(x) (*(void **)(x))
static int klg_posix_spawnattr_init(void *a) { return posix_spawnattr_init((posix_spawnattr_t *)a); }
static int klg_posix_spawnattr_destroy(void *a) { return posix_spawnattr_destroy((posix_spawnattr_t *)a); }
static int klg_posix_spawnattr_setflags(void *a, short f) { return posix_spawnattr_setflags((posix_spawnattr_t *)a, (short)(f & 0x0f)); }
static int klg_posix_spawnattr_setsigdefault(void *a, const uint64_t *lset) {
    sigset_t d; sigemptyset(&d);
    for (int i = 1; i < 32; i++) if (((*lset) >> (i - 1)) & 1 && g_sig_l2d[i]) sigaddset(&d, g_sig_l2d[i]);
    return posix_spawnattr_setsigdefault((posix_spawnattr_t *)a, &d);
}
static int klg_posix_spawnattr_setsigmask(void *a, const uint64_t *lset) {
    sigset_t d; sigemptyset(&d);
    for (int i = 1; i < 32; i++) if (((*lset) >> (i - 1)) & 1 && g_sig_l2d[i]) sigaddset(&d, g_sig_l2d[i]);
    return posix_spawnattr_setsigmask((posix_spawnattr_t *)a, &d);
}
static int klg_posix_spawn_file_actions_init(void *fa) { return posix_spawn_file_actions_init((posix_spawn_file_actions_t *)fa); }
static int klg_posix_spawn_file_actions_destroy(void *fa) { return posix_spawn_file_actions_destroy((posix_spawn_file_actions_t *)fa); }
static int klg_posix_spawn_file_actions_adddup2(void *fa, int a, int b) { return posix_spawn_file_actions_adddup2((posix_spawn_file_actions_t *)fa, a, b); }
static int klg_posix_spawn_file_actions_addclose(void *fa, int fd) { return posix_spawn_file_actions_addclose((posix_spawn_file_actions_t *)fa, fd); }
static int klg_posix_spawn(int *pid, const char *path, void *fa, void *attr, char *const argv[], char *const envp[]) {
    fprintf(stderr, "  [glibc] posix_spawn(%s)\n", path ? path : "?");
    return posix_spawn(pid, path, (posix_spawn_file_actions_t *)fa, (posix_spawnattr_t *)attr, argv, envp);
}
static int klg_posix_spawnp(int *pid, const char *file, void *fa, void *attr, char *const argv[], char *const envp[]) {
    fprintf(stderr, "  [glibc] posix_spawnp(%s)\n", file ? file : "?");
    return posix_spawnp(pid, file, (posix_spawn_file_actions_t *)fa, (posix_spawnattr_t *)attr, argv, envp);
}
static int klg_sigtimedwait(const uint64_t *lset, void *info, const struct timespec *timeout) {
    sigset_t d; sigemptyset(&d);
    for (int i = 1; i < 32; i++) if (((*lset) >> (i - 1)) & 1 && g_sig_l2d[i]) sigaddset(&d, g_sig_l2d[i]);
    double deadline = timeout ? (double)timeout->tv_sec + timeout->tv_nsec * 1e-9 : 1e9;
    struct timespec step = { 0, 1000000 };
    for (double waited = 0; waited <= deadline; waited += 0.001) {
        sigset_t pend; sigpending(&pend);
        for (int i = 1; i < 32; i++) if (g_sig_l2d[i] && sigismember(&d, g_sig_l2d[i]) && sigismember(&pend, g_sig_l2d[i])) {
            int got = 0; sigset_t one; sigemptyset(&one); sigaddset(&one, g_sig_l2d[i]); sigwait(&one, &got);
            if (info) memset(info, 0, 128);
            return i;
        }
        nanosleep(&step, NULL);
    }
    errno = EAGAIN; return -1;
}
static int klg_shmctl(int id, int cmd, void *buf) { if (cmd == 0 /* IPC_RMID */) return shmctl(id, IPC_RMID, NULL); errno = ENOSYS; return -1; }
static int klg_umount2(const char *t, int f) { errno = EPERM; return -1; }

// ---------- System V semaphores (tier0's CThreadEvent) ----------
// semctl is variadic: Linux passes the union semun in x3, Darwin's reads it from
// the stack, so a direct forward answers IPC_STAT with a garbage pointer and
// writes the result over the caller's stack. Fixed four arguments here; the
// command numbers and semid_ds layouts are translated.
struct lx_ipc_perm { int32_t key; uint32_t uid, gid, cuid, cgid, mode; uint16_t seq, pad; uint64_t unused1, unused2; };
struct lx_semid_ds { struct lx_ipc_perm perm; int64_t otime, ctime; uint64_t nsems; uint64_t unused[2]; };
static int klg_semctl(int semid, int semnum, int cmd, uint64_t arg) {
    union semun u;
    switch (cmd) {
    case 0: return semctl(semid, semnum, IPC_RMID);
    case 1: {   // IPC_SET
        const struct lx_semid_ds *lx = (const struct lx_semid_ds *)(uintptr_t)arg;
        struct semid_ds d; memset(&d, 0, sizeof d);
        u.buf = &d;
        if (semctl(semid, semnum, IPC_STAT, u) != 0) return -1;
        d.sem_perm.uid = lx->perm.uid; d.sem_perm.gid = lx->perm.gid; d.sem_perm.mode = (mode_t)lx->perm.mode;
        return semctl(semid, semnum, IPC_SET, u);
    }
    case 2: {   // IPC_STAT
        struct lx_semid_ds *lx = (struct lx_semid_ds *)(uintptr_t)arg;
        struct semid_ds d; memset(&d, 0, sizeof d);
        u.buf = &d;
        int r = semctl(semid, semnum, IPC_STAT, u);
        if (r != 0) return r;
        memset(lx, 0, sizeof *lx);
        lx->perm.key = d.sem_perm._key; lx->perm.uid = d.sem_perm.uid; lx->perm.gid = d.sem_perm.gid;
        lx->perm.cuid = d.sem_perm.cuid; lx->perm.cgid = d.sem_perm.cgid; lx->perm.mode = d.sem_perm.mode; lx->perm.seq = d.sem_perm._seq;
        lx->otime = d.sem_otime; lx->ctime = d.sem_ctime; lx->nsems = d.sem_nsems;
        return 0;
    }
    case 11: return semctl(semid, semnum, GETPID);
    case 12: return semctl(semid, semnum, GETVAL);
    case 13: u.array = (unsigned short *)(uintptr_t)arg; return semctl(semid, semnum, GETALL, u);
    case 14: return semctl(semid, semnum, GETNCNT);
    case 15: return semctl(semid, semnum, GETZCNT);
    case 16: u.val = (int)arg; return semctl(semid, semnum, SETVAL, u);
    case 17: u.array = (unsigned short *)(uintptr_t)arg; return semctl(semid, semnum, SETALL, u);
    }
    errno = EINVAL; return -1;
}
static int klg_semget(int key, int nsems, int flags) { return semget((key_t)key, nsems, flags); }
static int klg_semop(int semid, void *ops, size_t n) { return semop(semid, (struct sembuf *)ops, n); }
static int klg_semtimedop(int semid, void *ops, size_t n, const struct timespec *ts) {
    (void)ts; return semop(semid, (struct sembuf *)ops, n);
}

// ---------- odds and ends the engine imports ----------
static int klg_faccessat(int dfd, const char *p, int mode, int flags) {
    int d = 0;
    if (flags & 0x100) d |= AT_SYMLINK_NOFOLLOW;      // Linux AT_SYMLINK_NOFOLLOW
    if (flags & 0x200) d |= AT_EACCESS;               // Linux AT_EACCESS
    return faccessat(dfd == -100 ? AT_FDCWD : dfd, p, mode, d);
}
static int klg_posix_fadvise(int fd, off_t off, off_t len, int advice) { return 0; }
static int klg_mount(const char *s, const char *t, const char *fs, unsigned long f, const void *d) { errno = EPERM; return -1; }
static int klg_exec_fail(void) { errno = ENOSYS; return -1; }
static int klg_mkostemps(char *tmpl, int suffixlen, int flags) { return mkostemps(tmpl, suffixlen, (flags & 0x80000) ? O_CLOEXEC : 0); }
static int klg_vfscanf(void *f, const char *fmt, kl_va *va) {
    char m[1024] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, va, m, sizeof m, KL_VA_SCANF) == (size_t)-1) return -1;
    return vfscanf(kl_host_file(f), fmt, (va_list)m);
}
static int klg_vswscanf(const wchar_t *s, const wchar_t *fmt, kl_va *va) {
    fprintf(stderr, "  [glibc] vswscanf (wide format) is not marshalled; returning 0 matches\n");
    return 0;
}

// ---------- gettext (no catalogs: identity) ----------
static char *klg_gettext(const char *s) { return (char *)s; }
static char *klg_dgettext(const char *d, const char *s) { return (char *)s; }
static char *klg_dcgettext(const char *d, const char *s, int c) { return (char *)s; }
static char *klg_ngettext(const char *s, const char *p, unsigned long n) { return (char *)(n == 1 ? s : p); }
static char *klg_dngettext(const char *d, const char *s, const char *p, unsigned long n) { return (char *)(n == 1 ? s : p); }
static char *klg_dcngettext(const char *d, const char *s, const char *p, unsigned long n, int c) { return (char *)(n == 1 ? s : p); }
static char *klg_textdomain(const char *d) { return (char *)"messages"; }
static char *klg_bindtextdomain(const char *d, const char *dir) { return (char *)dir; }
static char *klg_bind_textdomain_codeset(const char *d, const char *cs) { return (char *)cs; }

// ---------- zero-returning named stubs for absent libraries ----------
static uint64_t klg_zero_stub(const char *name) {
    static unsigned said;
    g_stub_calls++;
    if (said < 40 && kl_env_on("KL_GLIBC_TRACE_STUBS", 1)) { said++; fprintf(stderr, "  [glibc] stub %s() -> 0\n", name); }
    return 0;
}
static int stub_prefix(const char *n) {
    if (n[0] == 'X' && n[1] >= 'A' && n[1] <= 'Z' && strncmp(n, "XML", 3) != 0) return 1;   // Xlib (not expat's XML_*)
    if (!strncmp(n, "xcb_", 4) || !strncmp(n, "wl_", 3) || !strncmp(n, "udev_", 5) ||
        !strncmp(n, "drm", 3) || !strncmp(n, "gbm_", 4) || !strncmp(n, "__gcov_", 7) ||
        !strncmp(n, "__lsan_", 7) || !strncmp(n, "__asan_", 7) || !strncmp(n, "__tsan_", 7) ||
        !strncmp(n, "__ubsan_", 8))
        return 1;
    return 0;
}

// Variadic entry points (kl_glibc_va.S).
extern void klg___printf_chk(void), klg___fprintf_chk(void), klg___sprintf_chk(void), klg___snprintf_chk(void),
            klg___asprintf_chk(void), klg___dprintf_chk(void), klg___syslog_chk(void), klg_error(void), klg_err(void),
            klg_errx(void), klg_warn(void), klg_warnx(void), klg_syscall(void);
static void klg_explicit_bzero(void *b, size_t n) { memset(b, 0, n); __asm__ volatile("" ::: "memory"); }
static void *klg_reallocarray(void *p, size_t n, size_t sz) { if (sz && n > SIZE_MAX / sz) { errno = ENOMEM; return NULL; } return realloc(p, n * sz); }

// ---------- tables ----------
#define E(n, f) { n, (void *)(f) }
#define D(n, p) { n, (void *)(p) }
static struct { const char *name; void *fn; } g_table[] = {
    // identity, errno, environment
    E("__errno_location", klg___errno_location), E("__h_errno_location", klg___h_errno_location),
    D("__environ", &environ), D("environ", &environ), D("_environ", &environ),
    D("__progname", &g_program_invocation_short_name), D("__progname_full", &g_program_invocation_name),
    D("program_invocation_name", &g_program_invocation_name),
    D("program_invocation_short_name", &g_program_invocation_short_name),
    D("__libc_single_threaded", &g_libc_single_threaded), D("__libc_stack_end", &g_libc_stack_end),
    D("__stack_chk_guard", &g_stack_chk_guard), E("__stack_chk_fail", klg___stack_chk_fail),
    E("__assert_fail", klg___assert_fail), E("__assert_perror_fail", klg___assert_perror_fail),
    E("__getauxval", klg___getauxval), E("getauxval", klg___getauxval),
    E("gnu_get_libc_version", klg_gnu_get_libc_version), E("gnu_get_libc_release", klg_gnu_get_libc_release),
    // ctype
    E("__ctype_b_loc", klg___ctype_b_loc), E("__ctype_tolower_loc", klg___ctype_tolower_loc),
    E("__ctype_toupper_loc", klg___ctype_toupper_loc), E("__ctype_get_mb_cur_max", klg___ctype_get_mb_cur_max),
    // fortify
    E("__memcpy_chk", klg___memcpy_chk), E("__memmove_chk", klg___memmove_chk), E("__memset_chk", klg___memset_chk),
    E("__mempcpy_chk", klg___mempcpy_chk), E("__strcpy_chk", klg___strcpy_chk), E("__stpcpy_chk", klg___stpcpy_chk),
    E("__strncpy_chk", klg___strncpy_chk), E("__stpncpy_chk", klg___stpncpy_chk), E("__strcat_chk", klg___strcat_chk),
    E("__strncat_chk", klg___strncat_chk), E("__explicit_bzero_chk", klg___explicit_bzero_chk),
    E("__wcscpy_chk", klg___wcscpy_chk), E("__wcsncpy_chk", klg___wcsncpy_chk), E("__wmemcpy_chk", klg___wmemcpy_chk),
    E("__wmemmove_chk", klg___wmemmove_chk), E("__wmemset_chk", klg___wmemset_chk),
    E("__mbstowcs_chk", klg___mbstowcs_chk), E("__wcstombs_chk", klg___wcstombs_chk),
    E("__mbsrtowcs_chk", klg___mbsrtowcs_chk), E("__wcsrtombs_chk", klg___wcsrtombs_chk), E("__wcrtomb_chk", klg___wcrtomb_chk),
    E("__fgets_chk", klg___fgets_chk), E("__fread_chk", klg___fread_chk), E("__read_chk", klg___read_chk),
    E("__pread_chk", klg___pread_chk), E("__pread64_chk", klg___pread_chk), E("__recv_chk", klg___recv_chk),
    E("__recvfrom_chk", klg___recvfrom_chk), E("__poll_chk", klg___poll_chk), E("__realpath_chk", klg___realpath_chk),
    E("__getcwd_chk", klg___getcwd_chk), E("__readlink_chk", klg___readlink_chk), E("__gethostname_chk", klg___gethostname_chk),
    E("__ttyname_r_chk", klg___ttyname_r_chk), E("__fdelt_chk", klg___fdelt_chk), E("__fdelt_warn", klg___fdelt_chk),
    E("__longjmp_chk", klg___longjmp_chk),
    E("__printf_chk", klg___printf_chk), E("__fprintf_chk", klg___fprintf_chk), E("__sprintf_chk", klg___sprintf_chk),
    E("__snprintf_chk", klg___snprintf_chk), E("__asprintf_chk", klg___asprintf_chk), E("__dprintf_chk", klg___dprintf_chk),
    E("__syslog_chk", klg___syslog_chk), E("__vfprintf_chk", klg___vfprintf_chk), E("__vprintf_chk", klg___vprintf_chk),
    E("__vsprintf_chk", klg___vsprintf_chk), E("__vsnprintf_chk", klg___vsnprintf_chk), E("__vasprintf_chk", klg___vasprintf_chk),
    E("__vdprintf_chk", klg___vdprintf_chk), E("__vsyslog_chk", klg___vsyslog_chk),
    E("error", klg_error), E("err", klg_err), E("errx", klg_errx), E("warn", klg_warn), E("warnx", klg_warnx),
    E("syscall", klg_syscall),
    // stat family, open
    E("__xstat", klg___xstat), E("__xstat64", klg___xstat), E("__lxstat", klg___lxstat), E("__lxstat64", klg___lxstat),
    E("__fxstat", klg___fxstat), E("__fxstat64", klg___fxstat), E("__fxstatat", klg___fxstatat), E("__fxstatat64", klg___fxstatat),
    E("__xmknod", klg___xmknod), E("__open_2", klg___open_2), E("__open64_2", klg___open_2),
    E("__openat_2", klg___openat_2), E("__openat64_2", klg___openat_2),
    // stdio internals
    E("__getdelim", klg___getdelim), E("__uflow", klg___uflow), E("__overflow", klg___overflow),
    E("__fsetlocking", klg___fsetlocking), E("__fpurge", klg___fpurge),
    E("getc_unlocked", klg_getc_unlocked), E("putc_unlocked", klg_putc_unlocked), E("fread_unlocked", klg_fread_unlocked),
    E("fwrite_unlocked", klg_fwrite_unlocked), E("fputs_unlocked", klg_fputs_unlocked), E("fgets_unlocked", klg_fgets_unlocked),
    E("fflush_unlocked", klg_fflush_unlocked), E("feof_unlocked", klg_feof_unlocked), E("ferror_unlocked", klg_ferror_unlocked),
    E("clearerr_unlocked", klg_clearerr_unlocked), E("fileno_unlocked", klg_fileno_unlocked),
    E("_IO_getc", klg_getc_unlocked), E("_IO_putc", klg_putc_unlocked),
    // setjmp / signals
    E("siglongjmp", klg_siglongjmp), E("__libc_longjmp", klg_longjmp),
    E("sigaction", klg_sigaction), E("__sigaction", klg_sigaction),
    E("__libc_current_sigrtmin", klg___libc_current_sigrtmin), E("__libc_current_sigrtmax", klg___libc_current_sigrtmax),
    // locale
    E("newlocale", klg_newlocale), E("__newlocale", klg_newlocale), E("uselocale", klg_uselocale), E("__uselocale", klg_uselocale),
    E("duplocale", klg_duplocale), E("__duplocale", klg_duplocale), E("freelocale", klg_freelocale), E("__freelocale", klg_freelocale),
    E("strtod_l", klg_strtod_l), E("__strtod_l", klg_strtod_l), E("strtof_l", klg_strtof_l), E("__strtof_l", klg_strtof_l),
    E("strtold_l", klg_strtold_l), E("__strtold_l", klg_strtold_l), E("strtol_l", klg_strtol_l), E("__strtol_l", klg_strtol_l),
    E("strtoul_l", klg_strtoul_l), E("__strtoul_l", klg_strtoul_l), E("strtoll_l", klg_strtoll_l), E("__strtoll_l", klg_strtoll_l),
    E("strtoull_l", klg_strtoull_l), E("__strtoull_l", klg_strtoull_l),
    E("strcoll_l", klg_strcoll_l), E("__strcoll_l", klg_strcoll_l), E("strxfrm_l", klg_strxfrm_l), E("__strxfrm_l", klg_strxfrm_l),
    E("wcscoll_l", klg_wcscoll_l), E("__wcscoll_l", klg_wcscoll_l), E("wcsxfrm_l", klg_wcsxfrm_l), E("__wcsxfrm_l", klg_wcsxfrm_l),
    E("towlower_l", klg_towlower_l), E("__towlower_l", klg_towlower_l), E("towupper_l", klg_towupper_l), E("__towupper_l", klg_towupper_l),
    E("iswctype_l", klg_iswctype_l), E("__iswctype_l", klg_iswctype_l), E("wctype_l", klg_wctype_l), E("__wctype_l", klg_wctype_l),
    E("strftime_l", klg_strftime_l), E("__strftime_l", klg_strftime_l), E("wcsftime_l", klg_wcsftime_l), E("__wcsftime_l", klg_wcsftime_l),
    E("toupper_l", klg_toupper_l), E("__toupper_l", klg_toupper_l), E("tolower_l", klg_tolower_l), E("__tolower_l", klg_tolower_l),
    E("isspace_l", klg_isspace_l), E("isalpha_l", klg_isalpha_l), E("isdigit_l", klg_isdigit_l), E("isalnum_l", klg_isalnum_l),
    E("isupper_l", klg_isupper_l), E("islower_l", klg_islower_l), E("isxdigit_l", klg_isxdigit_l), E("ispunct_l", klg_ispunct_l),
    E("isprint_l", klg_isprint_l),
    E("iswalpha_l", klg_iswalpha_l), E("iswdigit_l", klg_iswdigit_l), E("iswspace_l", klg_iswspace_l), E("iswupper_l", klg_iswupper_l),
    E("iswlower_l", klg_iswlower_l), E("iswalnum_l", klg_iswalnum_l), E("iswpunct_l", klg_iswpunct_l), E("iswprint_l", klg_iswprint_l),
    E("iswcntrl_l", klg_iswcntrl_l), E("iswxdigit_l", klg_iswxdigit_l), E("iswblank_l", klg_iswblank_l), E("iswgraph_l", klg_iswgraph_l),
    E("btowc_l", klg_btowc_l), E("wctob_l", klg_wctob_l), E("mbrtowc_l", klg_mbrtowc_l), E("wcrtomb_l", klg_wcrtomb_l),
    E("mbsrtowcs_l", klg_mbsrtowcs_l), E("wcsrtombs_l", klg_wcsrtombs_l), E("mbrlen_l", klg_mbrlen_l), E("__mbrlen", mbrlen),
    E("wctrans_l", klg_wctrans_l), E("towctrans_l", klg_towctrans_l), E("localeconv_l", klg_localeconv_l),
    E("nl_langinfo_l", klg_nl_langinfo_l), E("__nl_langinfo_l", klg_nl_langinfo_l), E("nl_langinfo", klg_nl_langinfo),
    // strings, memory, math
    E("__rawmemchr", klg___rawmemchr), E("rawmemchr", klg___rawmemchr), E("mempcpy", klg_mempcpy), E("__mempcpy", klg_mempcpy),
    E("strerror_r", klg_strerror_r), E("qsort_r", klg_qsort_r),
    E("__isnan", klg___isnan), E("__isnanf", klg___isnanf), E("__isinf", klg___isinf), E("__isinff", klg___isinff),
    E("__finite", klg___finite), E("__finitef", klg___finitef), E("__signbit", klg___signbit), E("__signbitf", klg___signbitf),
    E("__fpclassify", klg___fpclassify), E("__fpclassifyf", klg___fpclassifyf),
    // process, scheduling, Linux-only calls
    E("__register_atfork", klg___register_atfork), E("__cxa_thread_atexit_impl", klg___cxa_thread_atexit_impl),
    E("_dl_find_object", klg__dl_find_object), E("dlmopen", klg_dlmopen), E("dlvsym", klg_dlvsym), E("dlinfo", klg_dlinfo),
    E("dladdr1", klg_dladdr1),
    E("__sched_cpucount", klg___sched_cpucount), E("sched_getaffinity", klg_sched_getaffinity),
    E("sched_setaffinity", klg_sched_setaffinity), E("pthread_setaffinity_np", klg_pthread_setaffinity_np),
    E("pthread_getaffinity_np", klg_pthread_getaffinity_np), E("get_nprocs", klg_get_nprocs), E("get_nprocs_conf", klg_get_nprocs),
    E("sched_getcpu", klg_sched_getcpu), E("gettid", klg_gettid), E("prctl", klg_prctl), E("pipe2", klg_pipe2), E("dup3", klg_dup3),
    E("accept4", klg_accept4), E("getrandom", klg_getrandom), E("close_range", klg_close_range), E("mremap", klg_mremap),
    E("memfd_create", klg_memfd_create),
    E("timerfd_create", klg_enosys), E("timerfd_settime", klg_enosys), E("signalfd", klg_enosys), E("copy_file_range", klg_enosys),
    E("splice", klg_enosys), E("sendfile", klg_enosys), E("sendfile64", klg_enosys), E("fallocate", klg_enosys), E("fallocate64", klg_enosys),
    E("posix_fallocate", klg_enosys), E("posix_fallocate64", klg_enosys), E("readahead", klg_enosys), E("membarrier", klg_enosys),
    E("malloc_trim", klg_malloc_trim), E("malloc_stats", klg_malloc_stats), E("mallopt", klg_mallopt), E("mallinfo", klg_mallinfo),
    E("mallinfo2", klg_mallinfo), E("res_init", klg_res_init), E("__res_init", klg_res_init), E("__res_ninit", klg___res_ninit),
    E("__res_nclose", klg___res_nclose), E("__res_state", klg___res_state),
    E("pthread_cond_clockwait", klg_pthread_cond_clockwait), E("pthread_mutex_clocklock", klg_pthread_mutex_clocklock),
    E("pthread_rwlock_clockrdlock", klg_pthread_rwlock_clockrdlock), E("pthread_rwlock_clockwrlock", klg_pthread_rwlock_clockwrlock),
    E("sem_clockwait", klg_sem_clockwait), E("pthread_tryjoin_np", klg_pthread_tryjoin_np), E("pthread_timedjoin_np", klg_pthread_timedjoin_np),
    E("pthread_setname_np", klg_pthread_setname_np), E("pthread_getname_np", klg_pthread_getname_np), E("pthread_yield", klg_pthread_yield),
    E("sysconf", klg_sysconf), E("__sysconf", klg_sysconf), E("_exit", klg__exit), E("_Exit", klg__exit), E("exit", klg_exit),
    E("dlsym", klg_dlsym), E("dlopen", klg_dlopen), E("pthread_cancel", pthread_cancel),
    E("semctl", klg_semctl), E("semget", klg_semget), E("semop", klg_semop), E("semtimedop", klg_semtimedop),
    // complex math: float complex travels in s0/s1 on both ABIs
    E("cabsf", cabsf), E("cexpf", cexpf), E("cargf", cargf), E("csqrtf", csqrtf), E("clogf", clogf), E("cpowf", cpowf),
    E("cabs", cabs), E("cexp", cexp), E("carg", carg), E("csqrt", csqrt), E("clog", clog), E("cpow", cpow), E("fstatfs", klg_fstatfs), E("fstatfs64", klg_fstatfs),
    E("random_r", klg_random_r), E("srandom_r", klg_srandom_r), E("initstate_r", klg_initstate_r), E("setstate_r", klg_setstate_r),
    E("mkostemp", klg_mkostemp), E("mkostemp64", klg_mkostemp), E("creat", klg_creat), E("creat64", klg_creat),
    E("__xpg_basename", klg___xpg_basename), E("pthread_attr_setinheritsched", klg_pthread_attr_setinheritsched),
    E("memmem", memmem), E("strndup", strndup), E("bcmp", bcmp), E("wcwidth", wcwidth), E("wcswidth", wcswidth), E("wctomb", wctomb),
    E("ttyname", ttyname), E("wcsstr", wcsstr), E("wcslcpy", wcslcpy), E("wcslcat", wcslcat), E("lround", lround), E("lroundf", lroundf),
    E("nextafterf", nextafterf), E("posix_madvise", posix_madvise), E("lchmod", lchmod), E("eaccess", access), E("endservent", endservent),
    E("strtoumax", strtoumax), E("strtoimax", strtoimax), E("pthread_setcanceltype", pthread_setcanceltype),
    E("getservbyname_r", klg_getservbyname_r), D("in6addr_loopback", &in6addr_loopback), D("in6addr_any", &in6addr_any),
    D("__timezone", &timezone), D("timezone", &timezone), D("__tzname", tzname), D("tzname", tzname), D("__daylight", &daylight), D("daylight", &daylight),
    E("nftw", klg_enotsup), E("nftw64", klg_enotsup), E("getmntent_r", klg_null), E("setmntent", klg_null), E("endmntent", klg_zero),
    E("hasmntopt", klg_null), E("getttynam", klg_null), E("umount2", klg_umount2),
    E("__sched_cpualloc", klg___sched_cpualloc), E("__sched_cpufree", klg___sched_cpufree),
    E("sched_getscheduler", klg_sched_getscheduler), E("sched_setscheduler", klg_sched_setscheduler), E("sched_getparam", klg_sched_getparam),
    E("getxattr", klg_enotsup), E("lgetxattr", klg_enotsup), E("fgetxattr", klg_enotsup), E("setxattr", klg_enotsup), E("lsetxattr", klg_enotsup),
    E("fsetxattr", klg_enotsup), E("removexattr", klg_enotsup), E("lremovexattr", klg_enotsup), E("fremovexattr", klg_enotsup),
    E("listxattr", klg_enotsup), E("llistxattr", klg_enotsup), E("flistxattr", klg_enotsup),
    E("statx", klg_statx), E("shmget", shmget), E("shmat", shmat), E("shmdt", shmdt), E("shmctl", klg_shmctl),
    E("sendmmsg", klg_sendmmsg), E("recvmmsg", klg_recvmmsg), E("strverscmp", klg_strverscmp),
    E("fts64_open", klg_null), E("fts64_read", klg_null), E("fts64_set", klg_zero), E("fts64_close", klg_zero),
    E("catopen", klg_catopen), E("catgets", klg_catgets), E("catclose", klg_catclose), E("__fpending", klg___fpending),
    E("unshare", klg_eperm), E("setns", klg_eperm), E("scandirat", klg_enosys), E("open_tree", klg_enosys), E("move_mount", klg_enosys),
    E("mount_setattr", klg_enosys), E("fspick", klg_enosys), E("fsopen", klg_enosys), E("fsmount", klg_enosys), E("fsconfig", klg_enosys),
    E("getcontext", klg_enosys), E("setcontext", klg_enosys), E("makecontext", klg_zero), E("swapcontext", klg_enosys),
    E("res_nquery", klg_enosys), E("__res_nquery", klg_enosys), E("sigtimedwait", klg_sigtimedwait),
    E("posix_spawn", klg_posix_spawn), E("posix_spawnp", klg_posix_spawnp), E("posix_spawnattr_init", klg_posix_spawnattr_init),
    E("posix_spawnattr_destroy", klg_posix_spawnattr_destroy), E("posix_spawnattr_setflags", klg_posix_spawnattr_setflags),
    E("posix_spawnattr_setsigdefault", klg_posix_spawnattr_setsigdefault), E("posix_spawnattr_setsigmask", klg_posix_spawnattr_setsigmask),
    E("posix_spawn_file_actions_init", klg_posix_spawn_file_actions_init), E("posix_spawn_file_actions_destroy", klg_posix_spawn_file_actions_destroy),
    E("posix_spawn_file_actions_adddup2", klg_posix_spawn_file_actions_adddup2), E("posix_spawn_file_actions_addclose", klg_posix_spawn_file_actions_addclose),
    E("epoll_create", klg_epoll_create), E("epoll_create1", klg_epoll_create1), E("epoll_ctl", klg_epoll_ctl),
    E("epoll_wait", klg_epoll_wait), E("epoll_pwait", klg_epoll_pwait),
    E("inotify_init", klg_inotify_init), E("inotify_init1", klg_inotify_init1), E("inotify_add_watch", klg_inotify_add_watch),
    E("inotify_rm_watch", klg_inotify_rm_watch),
    E("readlink", klg_readlink), E("readlinkat", klg_readlinkat), E("stat", klg_stat), E("lstat", klg_lstat),
    E("fstatat", klg_fstatat), E("fopen", klg_fopen),
    E("tmpnam", tmpnam), E("getloadavg", getloadavg), E("putenv", putenv), E("asctime", asctime), E("asctime_r", asctime_r),
    E("getdtablesize", getdtablesize), E("futimes", futimes), E("ctermid", ctermid), E("faccessat", klg_faccessat),
    E("posix_fadvise", klg_posix_fadvise), E("posix_fadvise64", klg_posix_fadvise), E("mount", klg_mount), E("umount", klg_mount),
    E("execlp", klg_exec_fail), E("execvpe", klg_exec_fail), E("execvp", klg_exec_fail), E("execl", klg_exec_fail),
    E("mkostemps", klg_mkostemps), E("mkostemps64", klg_mkostemps), E("vfscanf", klg_vfscanf), E("vswscanf", klg_vswscanf),
    E("free", klg_free_trace), E("pthread_setspecific", klg_setspecific_trace), E("pthread_getspecific", klg_getspecific_trace),
    E("mmap", klg_mmap_traced), E("mmap64", klg_mmap_traced),
    E("eventfd", klg_eventfd), E("eventfd_read", klg_eventfd_read), E("eventfd_write", klg_eventfd_write),
    E("read", klg_read), E("write", klg_write), E("close", klg_close),
    E("gettext", klg_gettext), E("dgettext", klg_dgettext), E("dcgettext", klg_dcgettext), E("ngettext", klg_ngettext),
    E("dngettext", klg_dngettext), E("dcngettext", klg_dcngettext), E("textdomain", klg_textdomain),
    E("bindtextdomain", klg_bindtextdomain), E("bind_textdomain_codeset", klg_bind_textdomain_codeset),
    // AArch64 FPCR/FPSR encodings are what both libcs expose: direct forwards.
    E("fesetround", fesetround), E("fegetround", fegetround), E("fegetenv", fegetenv), E("fesetenv", fesetenv),
    E("feholdexcept", feholdexcept), E("feupdateenv", feupdateenv), E("feraiseexcept", feraiseexcept),
    E("fegetexceptflag", fegetexceptflag), E("fesetexceptflag", fesetexceptflag),
    E("iconv_open", iconv_open), E("iconv", iconv), E("iconv_close", iconv_close), E("dirfd", dirfd),
    // Darwin has these under the same name with the same shape.
    E("backtrace", backtrace), E("backtrace_symbols", backtrace_symbols), E("backtrace_symbols_fd", backtrace_symbols_fd),
    E("fmemopen", fmemopen), E("open_memstream", open_memstream), E("open_wmemstream", open_wmemstream),
    E("strlcpy", strlcpy), E("strlcat", strlcat), E("explicit_bzero", klg_explicit_bzero), E("reallocarray", klg_reallocarray),
    E("stpcpy", stpcpy), E("stpncpy", stpncpy), E("strchrnul", klg_strchrnul), E("dn_expand", dn_expand), E("__dn_expand", dn_expand),
    E("getentropy", getentropy), E("arc4random", arc4random), E("arc4random_buf", arc4random_buf), E("arc4random_uniform", arc4random_uniform),
    E("ffsl", ffsl), E("ffsll", ffsll), E("timegm", timegm), E("timelocal", timelocal),
    E("wcscasecmp", wcscasecmp), E("wcsncasecmp", wcsncasecmp), E("wcsdup", wcsdup), E("wcsnlen", wcsnlen),
    E("mbsnrtowcs", mbsnrtowcs), E("wcsnrtombs", wcsnrtombs), E("wmemchr", wmemchr), E("wmemcmp", wmemcmp),
};

// glibc name → the bionic/POSIX name Klepton serves it under.
static const char *g_alias[][2] = {
    {"__libc_malloc", "malloc"}, {"__libc_free", "free"}, {"__libc_calloc", "calloc"}, {"__libc_realloc", "realloc"},
    {"__libc_memalign", "memalign"}, {"__strdup", "strdup"}, {"__strtok_r", "strtok_r"}, {"__stpcpy", "stpcpy"},
    {"__pthread_key_create", "pthread_key_create"}, {"__xpg_strerror_r", "strerror_r"},
    {"__clock_gettime", "clock_gettime"}, {"__nanosleep", "nanosleep"}, {"__poll", "poll"}, {"__read", "read"},
    {"__write", "write"}, {"__close", "close"}, {"__open", "open"}, {"__open64", "open"}, {"__dup2", "dup2"},
    {"__getpagesize", "getpagesize"}, {"__sigsetjmp", "sigsetjmp"}, {"_setjmp", "setjmp"}, {"__setjmp", "setjmp"},
    {"_longjmp", "longjmp"}, {"__pthread_mutex_lock", "pthread_mutex_lock"}, {"__pthread_mutex_unlock", "pthread_mutex_unlock"},
    {"__pthread_once", "pthread_once"}, {"__pthread_getspecific", "pthread_getspecific"}, {"__pthread_setspecific", "pthread_setspecific"},
    {"__getdents64", "getdents64"}, {"__exit", "_exit"}, {"secure_getenv", "getenv"}, {"__secure_getenv", "getenv"},
    {"__environ_ptr", "environ"}, {"__sched_yield", "sched_yield"}, {"__gettimeofday", "gettimeofday"}, {"__select", "select"},
    {"__isoc99_vsnprintf", "vsnprintf"}, {"__wcstod_internal", "wcstod"}, {"__strtol_internal", "strtol"}, {"__strtoul_internal", "strtoul"},
    {"__strtod_internal", "strtod"}, {"__strtoll_internal", "strtoll"}, {"__strtoull_internal", "strtoull"},
    {"readdir64_r", "readdir_r"}, {"scandir64", "scandir"}, {"alphasort64", "alphasort"}, {"versionsort", "alphasort"},
    {"__sigaddset", "sigaddset"}, {"__sigdelset", "sigdelset"}, {"__sigismember", "sigismember"},
    {"__wcstombs_internal", "wcstombs"}, {"__fgets_unlocked", "fgets"}, {"__fprintf", "fprintf"}, {"__vfprintf", "vfprintf"},
    {"__sprintf", "sprintf"}, {"__snprintf", "snprintf"}, {"__vsnprintf", "vsnprintf"}, {"__asprintf", "asprintf"},
    {"__sigsuspend", "sigsuspend"}, {"__sigprocmask", "sigprocmask"}, {"__cmsg_nxthdr", "__cmsg_nxthdr"},
};

// Names Klepton serves under a bionic spelling that glibc writes with a
// different suffix: "__isoc23_sscanf" is sscanf; "fopen64" is fopen.
static void *suffix_alias(const char *name) {
    char buf[256];
    size_t n = strlen(name);
    if (n >= sizeof buf) return NULL;
    if (!strncmp(name, "__isoc23_", 9)) return kl_shim_lookup(name + 9);
    if (!strncmp(name, "__isoc99_", 9)) return kl_shim_lookup(name + 9);
    if (n > 2 && !strcmp(name + n - 2, "64")) {
        memcpy(buf, name, n - 2); buf[n - 2] = 0;
        return kl_shim_lookup(buf);
    }
    return NULL;
}

static void *klg_lookup(const char *name) {
    if (g_in_lower) return NULL;
    for (size_t i = 0; i < sizeof g_table / sizeof g_table[0]; i++)
        if (!strcmp(g_table[i].name, name)) return g_table[i].fn;
    for (size_t i = 0; i < sizeof g_alias / sizeof g_alias[0]; i++)
        if (!strcmp(g_alias[i][0], name)) return kl_shim_lookup(g_alias[i][1]);
    void *s = suffix_alias(name);
    if (s) return s;
    if (stub_prefix(name)) return kl_named_stub(name, (void *)klg_zero_stub);
    return NULL;
}

void kl_glibc_install(const char *argv0) {
    if (argv0 && *argv0) {
        g_argv0 = argv0;
        const char *b = strrchr(argv0, '/');
        snprintf(g_progname, sizeof g_progname, "%s", b ? b + 1 : argv0);
    }
    ctype_init();
    { const char *e = getenv("KL_GLIBC_TRACE_EXIT"); g_trace_exit = e && *e && *e != '0'; }
    arc4random_buf(&g_stack_chk_guard, sizeof g_stack_chk_guard);
    ((unsigned char *)&g_stack_chk_guard)[0] = 0;     // glibc keeps a zero byte in the canary
    g_libc_stack_end = pthread_get_stackaddr_np(pthread_self());
    g_darwin_setjmp = (int (*)(void *))setjmp;
    g_darwin_sigsetjmp = (int (*)(void *, int))sigsetjmp;
    (void)g_darwin_setjmp; (void)g_darwin_sigsetjmp;
    // Klepton's own read/write/close, looked up before our override is live.
    g_read = (ssize_t (*)(int, void *, size_t))kl_shim_lookup("read");
    g_write = (ssize_t (*)(int, const void *, size_t))kl_shim_lookup("write");
    g_close = (int (*)(int))kl_shim_lookup("close");
    g__exit = (int (*)(int))kl_shim_lookup("_exit");
    if (!g_read) g_read = read;
    if (!g_write) g_write = write;
    if (!g_close) g_close = close;
    kl_shim_override = klg_lookup;
}

void kl_glibc_report(FILE *f) {
    fprintf(f, "  [glibc] %zu table entries, %zu aliases, %u stub calls\n",
            sizeof g_table / sizeof g_table[0], sizeof g_alias / sizeof g_alias[0], g_stub_calls);
}
