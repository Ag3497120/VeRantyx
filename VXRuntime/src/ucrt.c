// ucrtbase (and the api-ms-win-crt-* API sets): the Universal C Runtime, implemented on macOS's libc.
//
// Every function here uses the Windows calling convention, so libc calls are wrapped, never exported raw.
// printf and friends arrive as __stdio_common_v*printf with a Windows va_list: a pointer to consecutive 8-byte
// argument slots (doubles included), which vx_format walks itself.
#include <ctype.h>
#include <malloc/malloc.h>
#include <math.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "vx.h"

// ---- formatting -----------------------------------------------------------------------------------------------------
static size_t utf16_to_utf8(const WCHAR *w, long max, char *out, size_t cap) {
    size_t k = 0;
    for (long i = 0; (max < 0 || i < max) && w[i]; i++) {
        uint32_t c = w[i];
        char b[3]; size_t n;
        if (c < 0x80) { b[0] = (char)c; n = 1; }
        else if (c < 0x800) { b[0] = 0xc0 | (c >> 6); b[1] = 0x80 | (c & 0x3f); n = 2; }
        else { b[0] = 0xe0 | (c >> 12); b[1] = 0x80 | ((c >> 6) & 0x3f); b[2] = 0x80 | (c & 0x3f); n = 3; }
        for (size_t j = 0; j < n; j++) { if (k + 1 < cap) out[k] = b[j]; k++; }
    }
    if (cap) out[k < cap ? k : cap - 1] = 0;
    return k;
}

// Formats into out (cap bytes, always NUL-terminated when cap > 0) and returns the full length, like C99 snprintf.
int vx_format(char *out, size_t cap, const char *fmt, const char *ap) {
    size_t len = 0;
#define EMIT(s, n) do { size_t _n = (n); for (size_t _i = 0; _i < _n; _i++) { if (len + 1 < cap) out[len] = (s)[_i]; len++; } } while (0)
#define NEXT64() ({ uint64_t _v; memcpy(&_v, ap, 8); ap += 8; _v; })
    for (const char *p = fmt; *p;) {
        if (*p != '%') { const char *q = strchr(p, '%'); size_t n = q ? (size_t)(q - p) : strlen(p); EMIT(p, n); p += n; continue; }
        const char *start = p++;
        if (*p == '%') { EMIT("%", 1); p++; continue; }
        char spec[48]; size_t sl = 0; spec[sl++] = '%';
        while (strchr("-+ #0", *p) && sl < 8) spec[sl++] = *p++;
        if (*p == '*') { sl += snprintf(spec + sl, sizeof spec - sl, "%d", (int)(int32_t)NEXT64()); p++; }
        else while (isdigit((unsigned char)*p) && sl < 20) spec[sl++] = *p++;
        if (*p == '.') {
            spec[sl++] = *p++;
            if (*p == '*') { sl += snprintf(spec + sl, sizeof spec - sl, "%d", (int)(int32_t)NEXT64()); p++; }
            else while (isdigit((unsigned char)*p) && sl < 30) spec[sl++] = *p++;
        }
        // Length: Windows long is 32-bit; I64/ll/z/j/t are 64-bit; w/l on c/s means wide.
        int size = 32, wide = 0;
        if (!strncmp(p, "I64", 3)) { size = 64; p += 3; }
        else if (!strncmp(p, "I32", 3)) { size = 32; p += 3; }
        else if (*p == 'I' || *p == 'z' || *p == 'j' || *p == 't') { size = 64; p++; }
        else if (!strncmp(p, "ll", 2)) { size = 64; p += 2; }
        else if (!strncmp(p, "hh", 2)) { size = 8; p += 2; }
        else if (*p == 'h') { size = 16; p++; }
        else if (*p == 'l' || *p == 'w') { wide = 1; p++; }
        else if (*p == 'L') p++;
        char conv = *p ? *p++ : 0;
        char piece[512];
        int n = 0;
        switch (conv) {
        case 'd': case 'i': {
            int64_t v = (int64_t)NEXT64();
            if (size == 32) v = (int32_t)v; else if (size == 16) v = (int16_t)v; else if (size == 8) v = (int8_t)v;
            spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0;
            n = snprintf(piece, sizeof piece, spec, (long long)v); break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            uint64_t v = NEXT64();
            if (size == 32) v = (uint32_t)v; else if (size == 16) v = (uint16_t)v; else if (size == 8) v = (uint8_t)v;
            spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0;
            n = snprintf(piece, sizeof piece, spec, (unsigned long long)v); break;
        }
        case 'p': n = snprintf(piece, sizeof piece, "%016llX", (unsigned long long)NEXT64()); break;
        case 'c': case 'C': {
            uint64_t v = NEXT64();
            if (wide || conv == 'C') { WCHAR w[2] = { (WCHAR)v, 0 }; n = (int)utf16_to_utf8(w, 1, piece, sizeof piece); }
            else { spec[sl++] = 'c'; spec[sl] = 0; n = snprintf(piece, sizeof piece, spec, (int)(char)v); }
            break;
        }
        case 's': case 'S': {
            const void *s = (const void *)NEXT64();
            char conv_buf[4096];
            const char *text = s ? (const char *)s : "(null)";
            if (s && (wide || conv == 'S')) { utf16_to_utf8((const WCHAR *)s, -1, conv_buf, sizeof conv_buf); text = conv_buf; }
            spec[sl++] = 's'; spec[sl] = 0;
            int need = snprintf(NULL, 0, spec, text);
            if (need < (int)sizeof piece) n = snprintf(piece, sizeof piece, spec, text);
            else { EMIT(text, strlen(text)); continue; }
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            uint64_t bits = NEXT64(); double d; memcpy(&d, &bits, 8);
            spec[sl++] = conv; spec[sl] = 0;
            n = snprintf(piece, sizeof piece, spec, d); break;
        }
        case 'n': (void)NEXT64(); continue;   // never written (as the UCRT does by default)
        default: EMIT(start, (size_t)(p - start)); continue;
        }
        if (n > 0) EMIT(piece, (size_t)(n < (int)sizeof piece ? n : (int)sizeof piece - 1));
    }
    if (cap) out[len < cap ? len : cap - 1] = 0;
    return (int)len;
#undef EMIT
#undef NEXT64
}

// ---- stdio ----------------------------------------------------------------------------------------------------------------
// A FILE is this runtime's own object. The standard streams write straight to their descriptors (unbuffered, so
// output from the program and from this runtime interleave correctly); files from fopen wrap a macOS FILE.
// Files are always binary: no "\r\n" translation in text mode.
// base/ptr/cnt: what _get_stream_buffer_pointers hands out (the C++ library's file buffers look at them; they stay
// empty, so it reads and writes through the functions).
typedef struct { int fd; FILE *f; char *base, *ptr; int cnt; } vx_file;
static vx_file g_iob[3] = { { 0, NULL, NULL, NULL, 0 }, { 1, NULL, NULL, NULL, 0 }, { 2, NULL, NULL, NULL, 0 } };

static WINAPI vx_file *__acrt_iob_func(unsigned i) { return i < 3 ? &g_iob[i] : NULL; }

static int write_all(int fd, const char *s, size_t n) {
    while (n) { ssize_t w = write(fd, s, n); if (w <= 0) return -1; s += w; n -= (size_t)w; }
    return 0;
}
static int file_write(vx_file *f, const void *p, size_t n) {
    if (f->f) return fwrite(p, 1, n, f->f) == n ? 0 : -1;
    return write_all(f->fd, p, n);
}
// The macOS FILE to read from (stdin is read through macOS's own buffered stream).
static FILE *read_stream(vx_file *f) {
    if (!f->f && f->fd == 0) f->f = stdin;
    return f->f;
}

static WINAPI int __stdio_common_vfprintf(uint64_t options, vx_file *f, const char *fmt, void *locale, const char *ap) {
    (void)options; (void)locale;
    char stackbuf[1024];
    int n = vx_format(stackbuf, sizeof stackbuf, fmt, ap);
    if (n < (int)sizeof stackbuf) return file_write(f, stackbuf, (size_t)n) ? -1 : n;
    char *heap = malloc((size_t)n + 1);
    vx_format(heap, (size_t)n + 1, fmt, ap);
    int r = file_write(f, heap, (size_t)n) ? -1 : n;
    free(heap);
    return r;
}

#define UCRT_STANDARD_SNPRINTF 0x2
static WINAPI int __stdio_common_vsprintf(uint64_t options, char *buf, size_t count, const char *fmt, void *locale, const char *ap) {
    (void)locale;
    int n = vx_format(buf, buf ? count : 0, fmt, ap);
    if (!(options & UCRT_STANDARD_SNPRINTF) && buf && (size_t)n >= count) return -1;   // _vsnprintf semantics
    return n;
}
static WINAPI int __stdio_common_vsnprintf_s(uint64_t options, char *buf, size_t size, size_t count, const char *fmt, void *locale, const char *ap) {
    (void)size;
    return __stdio_common_vsprintf(options, buf, count, fmt, locale, ap);
}

static WINAPI int vx_fflush(vx_file *f) { return f && f->f ? fflush(f->f) : 0; }
static WINAPI int vx_setvbuf(vx_file *f, char *b, int m, size_t s) { (void)f; (void)b; (void)m; (void)s; return 0; }
static WINAPI int vx_puts(const char *s) { return write_all(1, s, strlen(s)) || write_all(1, "\n", 1) ? -1 : 0; }
static WINAPI int vx_fputs(const char *s, vx_file *f) { return file_write(f, s, strlen(s)) ? -1 : 0; }
static WINAPI int vx_putchar(int c) { char ch = (char)c; return write_all(1, &ch, 1) ? -1 : c; }
static WINAPI int vx_fputc(int c, vx_file *f) { char ch = (char)c; return file_write(f, &ch, 1) ? -1 : (uint8_t)c; }
static WINAPI size_t vx_fwrite(const void *p, size_t sz, size_t n, vx_file *f) {
    if (!sz || !n) return 0;
    if (f->f) return fwrite(p, sz, n, f->f);
    return file_write(f, p, sz * n) ? 0 : n;
}

// Opening: Windows path, Windows mode letters (t, c, n, N, S, R, T, D and ",ccs=..." are dropped).
static int g_errno_value(void);
static void set_errno(int e);
static vx_file *open_file(const char *path, const char *mode) {
    char unix_path[4096], m[8]; size_t k = 0;
    for (const char *p = mode; *p && *p != ',' && k < sizeof m - 1; p++)
        if (strchr("rwa+bx", *p)) m[k++] = *p;
    m[k] = 0;
    int writes = strpbrk(m, "wa+") != NULL;
    if (!(writes ? vx_unix_path_write(path, unix_path, sizeof unix_path, m[0] != 'w') : vx_unix_path(path, unix_path, sizeof unix_path))) { set_errno(2); return NULL; }
    FILE *f = fopen(unix_path, m);
    if (!f) { set_errno(errno == ENOENT ? 2 : errno == EACCES ? 13 : errno == EEXIST ? 17 : errno == EISDIR ? 13 : 22); return NULL; }
    vx_file *v = calloc(1, sizeof *v);
    v->fd = fileno(f); v->f = f;
    return v;
}
static WINAPI vx_file *vx_fopen(const char *path, const char *mode) { return open_file(path, mode); }
static WINAPI vx_file *vx_wfopen(const WCHAR *path, const WCHAR *mode) {
    char *p = vx_utf8_dup(path), *m = vx_utf8_dup(mode);
    vx_file *f = open_file(p, m);
    free(p); free(m);
    return f;
}
static WINAPI int vx_fopen_s(vx_file **out, const char *path, const char *mode) { *out = open_file(path, mode); return *out ? 0 : g_errno_value(); }
static WINAPI int vx_wfopen_s(vx_file **out, const WCHAR *path, const WCHAR *mode) { *out = vx_wfopen(path, mode); return *out ? 0 : g_errno_value(); }
static WINAPI int vx_fclose(vx_file *f) {
    if (!f) return -1;
    if (f >= g_iob && f < g_iob + 3) return 0;
    int r = fclose(f->f);
    free(f);
    return r;
}
static WINAPI size_t vx_fread(void *p, size_t sz, size_t n, vx_file *f) { FILE *s = read_stream(f); return s ? fread(p, sz, n, s) : 0; }
static WINAPI char *vx_fgets(char *buf, int n, vx_file *f) { FILE *s = read_stream(f); return s ? fgets(buf, n, s) : NULL; }
static WINAPI int vx_fgetc(vx_file *f) { FILE *s = read_stream(f); return s ? fgetc(s) : -1; }
static WINAPI int vx_ungetc(int c, vx_file *f) { FILE *s = read_stream(f); return s ? ungetc(c, s) : -1; }
static WINAPI int vx_getchar(void) { return fgetc(stdin); }
static WINAPI int vx_feof(vx_file *f) { return f->f ? feof(f->f) : 0; }
static WINAPI int vx_ferror(vx_file *f) { return f->f ? ferror(f->f) : 0; }
static WINAPI void vx_clearerr(vx_file *f) { if (f->f) clearerr(f->f); }
static WINAPI int vx_fseek(vx_file *f, int32_t off, int whence) { return f->f ? fseeko(f->f, off, whence) : (lseek(f->fd, off, whence) < 0 ? -1 : 0); }   // long is 32-bit
static WINAPI int vx_fseeki64(vx_file *f, int64_t off, int whence) { return f->f ? fseeko(f->f, off, whence) : (lseek(f->fd, off, whence) < 0 ? -1 : 0); }
static WINAPI int32_t vx_ftell(vx_file *f) { return (int32_t)(f->f ? ftello(f->f) : lseek(f->fd, 0, SEEK_CUR)); }
static WINAPI int64_t vx_ftelli64(vx_file *f) { return f->f ? ftello(f->f) : lseek(f->fd, 0, SEEK_CUR); }
static WINAPI void vx_rewind(vx_file *f) { if (f->f) rewind(f->f); }

// Path functions of the C runtime: Windows paths, errno values that Windows shares with macOS (2, 13, 17, ...).
static int path_call(const char *path, int (*op)(const char *)) {
    char unix_path[4096];
    if (!vx_unix_path_write(path, unix_path, sizeof unix_path, 0)) { set_errno(2); return -1; }   // changes: through any overlay
    if (op(unix_path)) { set_errno(errno); return -1; }
    return 0;
}
static int mkdir_default(const char *p) { return mkdir(p, 0777); }
static WINAPI int vx_remove(const char *p) { return path_call(p, unlink); }
static WINAPI int vx_rmdir(const char *p) { return path_call(p, rmdir); }
static WINAPI int vx_mkdir(const char *p) { return path_call(p, mkdir_default); }
static WINAPI int vx_rename(const char *from, const char *to) {
    char a[4096], b[4096];
    if (!vx_unix_path(from, a, sizeof a) || !vx_unix_path_write(to, b, sizeof b, 0)) { set_errno(2); return -1; }
    if (rename(a, b)) { set_errno(errno); return -1; }
    return 0;
}
static WINAPI int vx_access(const char *p, int mode) {
    char unix_path[4096];
    if (!vx_unix_path(p, unix_path, sizeof unix_path)) { set_errno(2); return -1; }
    if (access(unix_path, mode & 6)) { set_errno(errno); return -1; }   // Windows modes: 0 exists, 2 write, 4 read
    return 0;
}
static WINAPI char *vx_getcwd(char *buf, int n) {
    char cwd[4096];
    vx_get_cwd(cwd, sizeof cwd);
    if (!buf) return strdup(cwd);
    if ((size_t)n <= strlen(cwd)) { set_errno(34 /* ERANGE */); return NULL; }
    return strcpy(buf, cwd);
}
static WINAPI int vx_chdir(const char *p) { if (vx_set_cwd(p)) return 0; set_errno(2); return -1; }

// ---- process startup and exit ----------------------------------------------------------------------------------------------
static int g_fmode, g_commode;
static WINAPI int *__p__fmode(void) { return &g_fmode; }
static WINAPI int *__p__commode(void) { return &g_commode; }
static WINAPI int *__p___argc(void) { return &vx_proc.argc; }
static WINAPI char ***__p___argv(void) { return &vx_proc.argv; }
static WINAPI char ***__p__environ(void) { return &vx_proc.envp; }
static WINAPI int _configure_narrow_argv(int mode) { (void)mode; return 0; }
static WINAPI int _initialize_narrow_environment(void) { return 0; }
static WINAPI char **_get_initial_narrow_environment(void) { return vx_proc.envp; }
// The wide environment, made once from the narrow one.
static WINAPI WCHAR **_get_initial_wide_environment(void) {
    static WCHAR **wide;
    if (!wide) {
        int n = 0;
        while (vx_proc.envp && vx_proc.envp[n]) n++;
        wide = calloc((size_t)n + 1, sizeof *wide);
        for (int i = 0; i < n; i++) wide[i] = vx_utf16_dup(vx_proc.envp[i]);
    }
    return wide;
}
static WINAPI WCHAR ***__p__wenviron(void) { static WCHAR **w; if (!w) w = _get_initial_wide_environment(); return &w; }
// The wide argv, made once from the narrow one (wmain programs: _configure_wide_argv, then __p___wargv).
static WCHAR **g_wargv;
static WINAPI int _configure_wide_argv(int mode) {
    (void)mode;
    if (!g_wargv) {
        WCHAR **w = calloc((size_t)vx_proc.argc + 1, sizeof *w);
        for (int i = 0; i < vx_proc.argc; i++) w[i] = vx_utf16_dup(vx_proc.argv[i]);
        g_wargv = w;
    }
    return 0;
}
static WINAPI WCHAR ***__p___wargv(void) { _configure_wide_argv(0); return &g_wargv; }
static WINAPI int _initialize_wide_environment(void) { _get_initial_wide_environment(); return 0; }
static WINAPI WCHAR *vx_wcscpy(WCHAR *d, const WCHAR *s) { WCHAR *r = d; while ((*d++ = *s++)) {} return r; }
static WINAPI WCHAR *vx_wcscat(WCHAR *d, const WCHAR *s) { WCHAR *r = d; while (*d) d++; while ((*d++ = *s++)) {} return r; }
static WINAPI WCHAR *vx_wcsrchr(const WCHAR *s, WCHAR c) { const WCHAR *last = NULL; do if (*s == c) last = s; while (*s++); return (WCHAR *)last; }
static WINAPI unsigned char *vx_mbsstr(const unsigned char *h, const unsigned char *n) { return (unsigned char *)strstr((const char *)h, (const char *)n); }
static WINAPI void _set_app_type(int t) { (void)t; }
static WINAPI void *_set_invalid_parameter_handler(void *h) { (void)h; return NULL; }
static WINAPI void __setusermatherr(void *f) { (void)f; }
static WINAPI int _configthreadlocale(int m) { (void)m; return 1; }
static WINAPI int _set_new_mode(int m) { (void)m; return 0; }
static WINAPI void *vx_signal(int sig, void *h) { (void)sig; (void)h; return NULL; }

typedef void (WINAPI *vfn)(void);
typedef int (WINAPI *ifn)(void);
static WINAPI void _initterm(vfn *b, vfn *e) { for (; b < e; b++) if (*b) (*b)(); }
static WINAPI int _initterm_e(ifn *b, ifn *e) { for (; b < e; b++) if (*b) { int r = (*b)(); if (r) return r; } return 0; }

static vfn g_atexit[256];
static int g_atexit_n;
static WINAPI int _crt_atexit(vfn f) { if (g_atexit_n < 256) { g_atexit[g_atexit_n++] = f; return 0; } return -1; }
static WINAPI int vx_atexit(vfn f) { return _crt_atexit(f); }
static void run_atexit(void) { while (g_atexit_n > 0) g_atexit[--g_atexit_n](); }
// A DLL's atexit list (_onexit_table_t: first, last, end), run by its CRT at DLL_PROCESS_DETACH.
typedef struct { vfn *first, *last, *end; } onexit_table;
static pthread_mutex_t g_onexit_lock = PTHREAD_MUTEX_INITIALIZER;
static WINAPI int _initialize_onexit_table(onexit_table *t) { if (!t) return -1; t->first = t->last = t->end = NULL; return 0; }
static WINAPI int _register_onexit_function(onexit_table *t, vfn f) {
    if (!t) return -1;
    pthread_mutex_lock(&g_onexit_lock);
    if (t->last == t->end) {
        size_t n = (size_t)(t->last - t->first), cap = n ? n * 2 : 32;
        vfn *a = realloc(t->first, cap * sizeof *a);
        if (!a) { pthread_mutex_unlock(&g_onexit_lock); return -1; }
        t->first = a; t->last = a + n; t->end = a + cap;
    }
    *t->last++ = f;
    pthread_mutex_unlock(&g_onexit_lock);
    return 0;
}
static WINAPI int _execute_onexit_table(onexit_table *t) {
    if (!t) return -1;
    pthread_mutex_lock(&g_onexit_lock);
    vfn *first = t->first, *last = t->last;
    t->first = t->last = t->end = NULL;
    pthread_mutex_unlock(&g_onexit_lock);
    while (last > first) { vfn f = *--last; if (f) f(); }
    free(first);
    return 0;
}
static WINAPI void _cexit(void) { run_atexit(); }
static void trace_exit(const char *what, int code, void *ret) {
    if (!vx_trace) return;
    const vx_image *m = vx_image_at((uint64_t)(uintptr_t)ret);
    vx_log("%s(%d) from %s+%#llx", what, code, m ? m->name : "?", m ? (unsigned long long)((uint8_t *)ret - m->base) : 0ull);
}
static WINAPI void vx_exit(int code) { trace_exit("exit", code, __builtin_return_address(0)); run_atexit(); _exit(code); }
static WINAPI void vx__exit(int code) { trace_exit("_exit", code, __builtin_return_address(0)); _exit(code); }
static WINAPI void vx_abort(void) {
    const vx_image *m = vx_image_at((uint64_t)(uintptr_t)__builtin_return_address(0));
    vx_log("the program called abort() from %s+%#llx", m ? m->name : "?", m ? (unsigned long long)((uint8_t *)__builtin_return_address(0) - m->base) : 0ull);
    _exit(3);
}


// errno is per thread on Windows too.
static __thread int t_errno;
static int *g_errno_ptr(void) { return &t_errno; }
static int g_errno_value(void) { return t_errno; }
static void set_errno(int e) { t_errno = e; }

// ---- heap, memory, strings ---------------------------------------------------------------------------------------------------
static WINAPI void *vx_malloc(size_t n) { return malloc(n); }
static WINAPI void *vx_calloc(size_t a, size_t b) { return calloc(a, b); }
static WINAPI void *vx_realloc(void *p, size_t n) { return realloc(p, n); }
static WINAPI void vx_free(void *p) { free(p); }
static WINAPI void *vx_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static WINAPI void *vx_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static WINAPI void *vx_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static WINAPI int vx_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static WINAPI size_t vx_strlen(const char *s) { return strlen(s); }
static WINAPI int vx_strcmp(const char *a, const char *b) { return strcmp(a, b); }
static WINAPI int vx_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static WINAPI char *vx_strcpy(char *d, const char *s) { return strcpy(d, s); }
static WINAPI char *vx_strncpy(char *d, const char *s, size_t n) { return strncpy(d, s, n); }
static WINAPI char *vx_strcat(char *d, const char *s) { return strcat(d, s); }
static WINAPI char *vx_strchr(const char *s, int c) { return strchr(s, c); }
static WINAPI char *vx_strrchr(const char *s, int c) { return strrchr(s, c); }
static WINAPI char *vx_strstr(const char *a, const char *b) { return strstr(a, b); }
static WINAPI char *vx_strdup(const char *s) { return strdup(s); }
static WINAPI int vx_atoi(const char *s) { return atoi(s); }
static WINAPI long vx_strtol(const char *s, char **e, int b) { return (long)(int32_t)strtol(s, e, b); }   // Windows long is 32-bit
static WINAPI unsigned long vx_strtoul(const char *s, char **e, int b) { return (uint32_t)strtoul(s, e, b); }
static WINAPI long long vx_strtoll(const char *s, char **e, int b) { return strtoll(s, e, b); }
static WINAPI double vx_strtod(const char *s, char **e) { return strtod(s, e); }
static WINAPI char *vx_getenv(const char *n) { return getenv(n); }
static WINAPI int vx_toupper(int c) { return toupper(c); }
static WINAPI int vx_tolower(int c) { return tolower(c); }
static WINAPI int vx_isspace(int c) { return isspace(c); }
static WINAPI int vx_isdigit(int c) { return isdigit(c); }
static WINAPI int vx_isalpha(int c) { return isalpha(c); }
static WINAPI int vx_rand(void) { return rand() & 0x7fff; }   // RAND_MAX is 0x7fff on Windows
static WINAPI void vx_srand(unsigned s) { srand(s); }
static WINAPI int64_t vx_time64(int64_t *t) { int64_t v = (int64_t)time(NULL); if (t) *t = v; return v; }
static WINAPI long vx_clock(void) { return (long)(clock() * 1000 / CLOCKS_PER_SEC); }   // CLOCKS_PER_SEC is 1000 on Windows
static WINAPI int *vx__errno(void) { return g_errno_ptr(); }

// qsort with a Windows-convention comparator.
typedef int (WINAPI *cmpfn)(const void *, const void *);
static cmpfn g_qsort_cmp;
static int qsort_tramp(const void *a, const void *b) { return g_qsort_cmp(a, b); }
static WINAPI void vx_qsort(void *base, size_t n, size_t sz, cmpfn cmp) {
    cmpfn saved = g_qsort_cmp; g_qsort_cmp = cmp;
    qsort(base, n, sz, qsort_tramp);
    g_qsort_cmp = saved;
}

// ---- more of the C library (what MinGW's C++ runtime and winpthread import) -----------------------------------------------
static WINAPI void vx_assert(const char *msg, const char *file, unsigned line) { vx_log("assertion failed: %s (%s:%u)", msg, file, line); _exit(3); }
static WINAPI int vx_fileno(vx_file *f) { return f->fd; }
static WINAPI void vx_lock_file(vx_file *f) { (void)f; }
static WINAPI int vx_read(int fd, void *buf, unsigned n) { return (int)read(fd, buf, n); }
static WINAPI int vx_setmode(int fd, int mode) { (void)fd; (void)mode; return 0x8000; /* _O_BINARY: no newline translation here */ }
static WINAPI void *vx_memchr(const void *p, int c, size_t n) { return memchr(p, c, n); }
static WINAPI size_t vx_strnlen(const char *s, size_t n) { return strnlen(s, n); }
static WINAPI char *vx_strerror(int e) { return strerror(e); }
static WINAPI size_t crt_wcslen(const WCHAR *s) { size_t n = 0; while (s[n]) n++; return n; }
static WINAPI int vx_wcscmp(const WCHAR *a, const WCHAR *b) { while (*a && *a == *b) { a++; b++; } return (*a > *b) - (*a < *b); }
static WINAPI size_t vx_wcsnlen(const WCHAR *s, size_t max) { size_t n = 0; while (n < max && s[n]) n++; return n; }
static WINAPI int vx_rand_s(unsigned *v) { arc4random_buf(v, sizeof *v); return 0; }

// Windows struct lconv, "C" locale: ten char* members (decimal point, thousands separator, grouping, currency
// strings, signs), eight chars (int_frac_digits ... n_sign_posn, CHAR_MAX), then the same eight strings as
// wchar_t* (from 88).
static uint8_t g_lconv[88 + 8 * 8];
static WINAPI void *vx_localeconv(void) {
    static WCHAR wdot[] = { '.', 0 }, wempty[] = { 0 };
    char **c = (char **)g_lconv;
    if (!c[0]) {
        c[0] = "."; for (int i = 1; i < 10; i++) c[i] = "";
        memset(g_lconv + 80, 127, 8);
        WCHAR **w = (WCHAR **)(g_lconv + 88);
        w[0] = wdot; for (int i = 1; i < 8; i++) w[i] = wempty;
    }
    return g_lconv;
}
// Multibyte conversions in the "C" locale: bytes map to the first 256 code points.
static WINAPI size_t vx_mbrtowc(WCHAR *wc, const char *s, size_t n, void *state) {
    (void)state;
    if (!s) return 0;
    if (!n) return (size_t)-2;
    if (wc) *wc = (uint8_t)*s;
    return *s ? 1 : 0;
}
static WINAPI size_t vx_mbsrtowcs(WCHAR *dst, const char **src, size_t n, void *state) {
    (void)state;
    const char *s = *src; size_t i = 0;
    for (; !dst || i < n; i++) {
        if (dst) dst[i] = (uint8_t)s[i];
        if (!s[i]) { if (dst) *src = NULL; return i; }
    }
    *src = s + i;
    return i;
}
static WINAPI size_t vx_wcrtomb(char *s, WCHAR wc, void *state) {
    (void)state;
    if (!s) return 1;
    if (wc > 0xff) { g_errno_ptr()[0] = 42 /* EILSEQ */; return (size_t)-1; }
    *s = (char)wc;
    return 1;
}

// ---- math (doubles and floats travel in xmm registers in both conventions) ------------------------------------------------
#define MATH1(name) static WINAPI double m_##name(double x) { return name(x); } \
                    static WINAPI float m_##name##f(float x) { return name##f(x); }
#define MATH2(name) static WINAPI double m_##name(double x, double y) { return name(x, y); } \
                    static WINAPI float m_##name##f(float x, float y) { return name##f(x, y); }
MATH1(sqrt) MATH1(sin) MATH1(cos) MATH1(tan) MATH1(asin) MATH1(acos) MATH1(atan) MATH1(exp) MATH1(log) MATH1(log10)
MATH1(log2) MATH1(exp2) MATH1(floor) MATH1(ceil) MATH1(round) MATH1(trunc) MATH1(fabs) MATH1(sinh) MATH1(cosh) MATH1(tanh) MATH1(cbrt)
MATH2(atan2) MATH2(pow) MATH2(fmod) MATH2(hypot) MATH2(fmin) MATH2(fmax)
static WINAPI double vx_ldexp(double x, int e) { return ldexp(x, e); }
static WINAPI double vx_frexp(double x, int *e) { return frexp(x, e); }
static WINAPI double vx_modf(double x, double *i) { return modf(x, i); }
#define MATH_EXPORTS(name) { #name, (void *)m_##name }, { #name "f", (void *)m_##name##f }

// ---- bounds-checked string functions (errno_t results: 0, EINVAL 22, ERANGE 34) ------------------------------------------
static WINAPI int vx_strcpy_s(char *d, size_t n, const char *s) {
    if (!d || !n) return 22;
    if (!s) { d[0] = 0; return 22; }
    size_t l = strlen(s);
    if (l >= n) { d[0] = 0; return 34; }
    memcpy(d, s, l + 1);
    return 0;
}
static WINAPI int vx_strcat_s(char *d, size_t n, const char *s) {
    if (!d || !n || !s) return 22;
    size_t dl = strnlen(d, n);
    if (dl == n) return 22;
    return vx_strcpy_s(d + dl, n - dl, s) ? (d[0] = 0, 34) : 0;
}
static WINAPI int vx_strncpy_s(char *d, size_t n, const char *s, size_t count) {
    if (!d || !n) return 22;
    size_t l = s ? strnlen(s, count) : 0;
    if (count == (size_t)-1) { l = strlen(s); if (l >= n) l = n - 1; }   // _TRUNCATE
    else if (l >= n) { d[0] = 0; return 34; }
    memcpy(d, s, l); d[l] = 0;
    return 0;
}
static WINAPI int vx_memcpy_s(void *d, size_t n, const void *s, size_t count) {
    if (!count) return 0;
    if (!d || !s) return 22;
    if (count > n) { memset(d, 0, n); return 34; }
    memcpy(d, s, count);
    return 0;
}
static WINAPI int vx_memmove_s(void *d, size_t n, const void *s, size_t count) {
    if (!count) return 0;
    if (!d || !s) return 22;
    if (count > n) return 34;
    memmove(d, s, count);
    return 0;
}
static WINAPI int vx_wcscpy_s(WCHAR *d, size_t n, const WCHAR *s) {
    if (!d || !n || !s) return 22;
    size_t l = 0; while (s[l]) l++;
    if (l >= n) { d[0] = 0; return 34; }
    memcpy(d, s, (l + 1) * sizeof(WCHAR));
    return 0;
}
static WINAPI int __stdio_common_vsprintf_s(uint64_t options, char *buf, size_t count, const char *fmt, void *locale, const char *ap) {
    (void)options; (void)locale;
    int n = vx_format(buf, count, fmt, ap);
    if (buf && (size_t)n >= count) { if (count) buf[0] = 0; return -1; }
    return n;
}

// The old msvcrt's numbered internal locks (_lock/_unlock), used by programs built against msvcrt.dll.
static pthread_mutex_t g_crt_locks[64];
static pthread_once_t g_crt_locks_once = PTHREAD_ONCE_INIT;
static void crt_locks_init(void) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    for (int i = 0; i < 64; i++) pthread_mutex_init(&g_crt_locks[i], &a);
}
static WINAPI void vx_lock(int n) { pthread_once(&g_crt_locks_once, crt_locks_init); pthread_mutex_lock(&g_crt_locks[n & 63]); }
static WINAPI void vx_unlock(int n) { pthread_mutex_unlock(&g_crt_locks[n & 63]); }

// msvcrt's __dllonexit: appends to a DLL's own atexit table [*begin, *end), which its startup code allocated
// with this runtime's malloc.
static WINAPI vfn vx_dllonexit(vfn f, vfn **begin, vfn **end) {
    size_t n = (size_t)(*end - *begin);
    vfn *t = realloc(*begin, (n + 1) * sizeof *t);
    if (!t) return NULL;
    t[n] = f;
    *begin = t; *end = t + n + 1;
    return f;
}
// msvcrt's C++ operators new and delete (mangled names), for programs built against msvcrt.dll.
static WINAPI void *vx_new(size_t n) { return malloc(n ? n : 1); }
static WINAPI void vx_delete(void *p) { free(p); }

// Aligned allocations: posix_memalign memory is released with free(), so _aligned_free is free().
static WINAPI void *vx_aligned_malloc(size_t n, size_t align) {
    void *p = NULL;
    if (align < sizeof(void *)) align = sizeof(void *);
    return posix_memalign(&p, align, n ? n : 1) ? NULL : p;
}
static WINAPI void vx_aligned_free(void *p) { free(p); }
static WINAPI size_t vx_aligned_msize(void *p, size_t align, size_t offset) { (void)align; (void)offset; return p ? malloc_size(p) : 0; }
static WINAPI void *vx_aligned_realloc(void *p, size_t n, size_t align) {
    if (!p) return vx_aligned_malloc(n, align);
    if (!n) { free(p); return NULL; }
    void *q = vx_aligned_malloc(n, align);
    if (q) { size_t old = malloc_size(p); memcpy(q, p, old < n ? old : n); free(p); }
    return q;
}

// More of the old msvcrt.dll, as Microsoft's own DLLs of the DirectX SDK era import it.
static WINAPI vfn vx_onexit(vfn f) { return _crt_atexit(f) ? NULL : f; }
static WINAPI void vx_amsg_exit(int n) { vx_log("runtime error %d", n); _exit(255); }
static WINAPI int vx_XcptFilter(DWORD code, void *ep) { (void)code; (void)ep; return 0; }   // EXCEPTION_CONTINUE_SEARCH
static WINAPI uint64_t vx_strtoui64(const char *s, char **e, int b) { return strtoull(s, e, b); }
static WINAPI int64_t vx_strtoi64(const char *s, char **e, int b) { return strtoll(s, e, b); }
static WINAPI int vx_isxdigit(int c) { return isxdigit(c); }
static WINAPI int vx_isalnum(int c) { return isalnum(c); }
static WINAPI int vx_isupper(int c) { return isupper(c); }
static WINAPI int vx_islower(int c) { return islower(c); }
static WINAPI int vx_isprint(int c) { return isprint(c); }
static WINAPI int vx_ispunct(int c) { return ispunct(c); }
static WINAPI double vx_atof(const char *s) { return atof(s); }
static WINAPI char *vx_setlocale(int category, const char *locale) { (void)category; (void)locale; return "C"; }   // only the C locale
static WINAPI size_t vx_mbstrlen(const char *s) { return strlen(s); }
static WINAPI int vx_isnan(double d) { return isnan(d); }
static WINAPI int vx_finite(double d) { return isfinite(d); }
static WINAPI uint32_t vx_clearfp(void) { return 0; }
static WINAPI uint32_t vx_statusfp(void) { return 0; }
static WINAPI uint32_t vx_controlfp(uint32_t value, uint32_t mask) { (void)value; (void)mask; return 0x0009001F; }   // the default control word
static WINAPI int vx_controlfp_s(uint32_t *cur, uint32_t value, uint32_t mask) { (void)value; (void)mask; if (cur) *cur = 0x0009001F; return 0; }
static WINAPI int vx_fpclass(double d) {
    switch (fpclassify(d)) {
    case FP_NAN: return 2;                                     // _FPCLASS_QNAN
    case FP_INFINITE: return signbit(d) ? 4 : 0x200;
    case FP_ZERO: return signbit(d) ? 0x20 : 0x40;
    case FP_SUBNORMAL: return signbit(d) ? 0x10 : 0x80;
    default: return signbit(d) ? 8 : 0x100;
    }
}
static WINAPI int vx_stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
static WINAPI int vx_strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
static WINAPI int vx_vsnprintf(char *buf, size_t n, const char *fmt, const char *ap) {
    int len = vx_format(buf, n, fmt, ap);
    return (size_t)len >= n ? -1 : len;                         // the old _vsnprintf: -1 when it did not fit
}
static WINAPI int vx_snprintf(char *buf, size_t n, const char *fmt, ...) {
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    int r = vx_vsnprintf(buf, n, fmt, ap);
    __builtin_ms_va_end(ap);
    return r;
}
static WINAPI int vx_sprintf(char *buf, const char *fmt, ...) {
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    int r = vx_format(buf, (size_t)1 << 30, fmt, ap);
    __builtin_ms_va_end(ap);
    return r;
}

// ---- what the C++ standard library (msvcp140) needs from the C runtime ----------------------------------------------------
// Locales: only "C", with UTF-8 as its code page; the classification table of <ctype.h> (_UPPER 1, _LOWER 2,
// _DIGIT 4, _SPACE 8, _PUNCT 0x10, _CONTROL 0x20, _BLANK 0x40, _HEX 0x80, _ALPHA 0x100 | 0x103 for letters).
static uint16_t g_ctype[257];
static void init_ctype(void) {
    if (g_ctype[1 + 'A']) return;
    for (int c = 0; c < 256; c++) {
        uint16_t m = 0;
        if (c < 128) {
            if (isupper(c)) m |= 0x1 | 0x100; if (islower(c)) m |= 0x2 | 0x100; if (isdigit(c)) m |= 0x4;
            if (isspace(c)) m |= 0x8; if (ispunct(c)) m |= 0x10; if (iscntrl(c)) m |= 0x20; if (c == ' ' || c == '\t') m |= 0x40;
            if (isxdigit(c)) m |= 0x80;
        }
        g_ctype[1 + c] = m;
    }
}
static WINAPI const uint16_t *vx_pctype_func(void) { init_ctype(); return g_ctype + 1; }
static WINAPI unsigned vx_lc_codepage_func(void) { return 65001; }
static WINAPI WCHAR **vx_lc_locale_name_func(void) { static WCHAR *names[6]; return names; }   // the C locale has no names
static WINAPI void vx_lock_locales(void) {}
static WINAPI WCHAR *vx_wsetlocale(int category, const WCHAR *locale) { (void)category; (void)locale; static WCHAR c[] = { 'C', 0 }; return c; }
typedef int (WINAPI *new_handler)(size_t);
static new_handler g_new_handler;
static WINAPI new_handler vx_set_new_handler(new_handler h) { new_handler old = g_new_handler; g_new_handler = h; return old; }
static WINAPI void vx_terminate(void) { vx_log("terminate() called"); abort(); }
// Wide path functions: through the narrow ones.
#define WIDE_CALL(name, narrow) static WINAPI int name(const WCHAR *p) { char *a = vx_utf8_dup(p); int r = narrow(a); free(a); return r; }
WIDE_CALL(vx_wremove, vx_remove)
WIDE_CALL(vx_wrmdir, vx_rmdir)
static WINAPI int vx_wrename(const WCHAR *a, const WCHAR *b) { char *x = vx_utf8_dup(a), *y = vx_utf8_dup(b); int r = vx_rename(x, y); free(x); free(y); return r; }
static WINAPI int vx_wchdir(const WCHAR *p) { char *a = vx_utf8_dup(p); int r = vx_set_cwd(a) ? 0 : -1; free(a); return r; }
static WINAPI WCHAR *vx_wgetcwd(WCHAR *buf, int n) {
    char c[1024]; vx_get_cwd(c, sizeof c);
    if (!buf) { n = (int)strlen(c) + 1; buf = malloc((size_t)n * 2); }
    vx_utf8_to_utf16(c, strlen(c) + 1, buf, (size_t)n);
    return buf;
}
// Floating point classification (Windows' FP_ values: infinite 1, NaN 2, normal -1, subnormal -2, zero 0).
static short fp_class(int c) { return c == FP_INFINITE ? 1 : c == FP_NAN ? 2 : c == FP_NORMAL ? -1 : c == FP_SUBNORMAL ? -2 : 0; }
static WINAPI short vx_dclass(double x) { return fp_class(fpclassify(x)); }
static WINAPI short vx_fdclass(float x) { return fp_class(fpclassify(x)); }
static WINAPI int vx_dsign(double x) { return signbit(x) ? 0x8000 : 0; }
static WINAPI int vx_fdsign(float x) { return signbit(x) ? 0x8000 : 0; }
static WINAPI double vx_copysign(double a, double b) { return copysign(a, b); }
static WINAPI double vx_lgamma(double x) { return lgamma(x); }
static WINAPI float vx_lgammaf(float x) { return lgammaf(x); }
static WINAPI size_t vx_strncnt(const char *s, size_t n) { return strnlen(s, n); }
static WINAPI WCHAR *vx_wcsdup(const WCHAR *s) { size_t n = (vx_wcslen(s) + 1) * 2; WCHAR *d = malloc(n); memcpy(d, s, n); return d; }
static WINAPI size_t vx_strcspn(const char *a, const char *b) { return strcspn(a, b); }
static WINAPI float vx_strtof(const char *s, char **end) { return strtof(s, end); }
static WINAPI double vx_wcstod(const WCHAR *s, WCHAR **end) {
    char a[128]; size_t i = 0;
    for (; s[i] && s[i] < 128 && i < sizeof a - 1; i++) a[i] = (char)s[i];
    a[i] = 0;
    char *e; double v = strtod(a, &e);
    if (end) *end = (WCHAR *)(s + (e - a));
    return v;
}
static WINAPI float vx_wcstof(const WCHAR *s, WCHAR **end) { return (float)vx_wcstod(s, end); }
// Streams: shared-access opening (sharing is not enforced), buffer pointers, positions, wide characters (UTF-8).
static WINAPI vx_file *vx_fsopen(const char *path, const char *mode, int share) { (void)share; return open_file(path, mode); }
static WINAPI vx_file *vx_wfsopen(const WCHAR *path, const WCHAR *mode, int share) { (void)share; return vx_wfopen(path, mode); }
static WINAPI int vx_get_stream_buffer_pointers(vx_file *f, char ***base, char ***ptr, int **cnt) {
    if (base) *base = &f->base;
    if (ptr) *ptr = &f->ptr;
    if (cnt) *cnt = &f->cnt;
    return 0;
}
static WINAPI int vx_fgetpos(vx_file *f, int64_t *pos) { *pos = f->f ? ftello(f->f) : lseek(f->fd, 0, SEEK_CUR); return *pos < 0 ? -1 : 0; }
static WINAPI int vx_fsetpos(vx_file *f, const int64_t *pos) { return f->f ? fseeko(f->f, *pos, SEEK_SET) : (lseek(f->fd, *pos, SEEK_SET) < 0 ? -1 : 0); }
static WINAPI unsigned vx_fgetwc(vx_file *f) {
    FILE *s = read_stream(f);
    int c = s ? fgetc(s) : -1;
    if (c < 0) return 0xffff;
    if (c < 0x80) return (unsigned)c;
    int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;                // UTF-8
    unsigned v = (unsigned)c & (0x3f >> extra);
    for (int i = 0; i < extra; i++) { int d = fgetc(s); if (d < 0) break; v = v << 6 | ((unsigned)d & 0x3f); }
    return v > 0xffff ? 0xfffd : v;
}
static WINAPI unsigned vx_fputwc(WCHAR c, vx_file *f) {
    char b[4]; size_t n = vx_utf16_to_utf8(&c, 1, b, sizeof b);
    return file_write(f, b, n) ? 0xffff : c;
}
static WINAPI unsigned vx_ungetwc(unsigned c, vx_file *f) { FILE *s = read_stream(f); return s && c < 0x80 && ungetc((int)c, s) >= 0 ? c : 0xffff; }
// Time names for the C++ library's time facets: ":Sun:Sunday:Mon:Monday:..." strings, and __lc_time_data.
static const char *g_days[] = { "Sun", "Sunday", "Mon", "Monday", "Tue", "Tuesday", "Wed", "Wednesday", "Thu", "Thursday", "Fri", "Friday", "Sat", "Saturday" };
static const char *g_months[] = { "Jan", "January", "Feb", "February", "Mar", "March", "Apr", "April", "May", "May", "Jun", "June",
                                  "Jul", "July", "Aug", "August", "Sep", "September", "Oct", "October", "Nov", "November", "Dec", "December" };
static char *join_names(const char **names, int n) {
    size_t len = 1;
    for (int i = 0; i < n; i++) len += strlen(names[i]) + 1;
    char *s = malloc(len), *p = s;
    for (int i = 0; i < n; i++) p += sprintf(p, ":%s", names[i]);
    return s;
}
static WINAPI char *vx_Getdays(void) { return join_names(g_days, 14); }
static WINAPI char *vx_Getmonths(void) { return join_names(g_months, 24); }
static WINAPI WCHAR *vx_W_Getdays(void) { char *a = vx_Getdays(); WCHAR *w = vx_utf16_dup(a); free(a); return w; }
static WINAPI WCHAR *vx_W_Getmonths(void) { char *a = vx_Getmonths(); WCHAR *w = vx_utf16_dup(a); free(a); return w; }
// __lc_time_data: abbreviated and full day and month names, AM/PM, date and time formats (narrow then wide), the
// calendar type and a reference count, the locale name.
static void *time_names(void) {
    static void *data[7 + 7 + 12 + 12 + 2 + 3 + 2 + 7 + 7 + 12 + 12 + 2 + 3 + 1];
    static int done;
    if (done) return data;
    int k = 0;
    for (int i = 0; i < 7; i++) data[k++] = (void *)g_days[2 * i];
    for (int i = 0; i < 7; i++) data[k++] = (void *)g_days[2 * i + 1];
    for (int i = 0; i < 12; i++) data[k++] = (void *)g_months[2 * i];
    for (int i = 0; i < 12; i++) data[k++] = (void *)g_months[2 * i + 1];
    data[k++] = "AM"; data[k++] = "PM";
    data[k++] = "MM/dd/yy"; data[k++] = "dddd, MMMM dd, yyyy"; data[k++] = "HH:mm:ss";
    data[k++] = (void *)(uintptr_t)1; data[k++] = (void *)(uintptr_t)1;   // ww_caltype, refcount (as two 4-byte fields: close enough)
    for (int i = 0; i < 7; i++) data[k++] = vx_utf16_dup(g_days[2 * i]);
    for (int i = 0; i < 7; i++) data[k++] = vx_utf16_dup(g_days[2 * i + 1]);
    for (int i = 0; i < 12; i++) data[k++] = vx_utf16_dup(g_months[2 * i]);
    for (int i = 0; i < 12; i++) data[k++] = vx_utf16_dup(g_months[2 * i + 1]);
    data[k++] = vx_utf16_dup("AM"); data[k++] = vx_utf16_dup("PM");
    data[k++] = vx_utf16_dup("MM/dd/yy"); data[k++] = vx_utf16_dup("dddd, MMMM dd, yyyy"); data[k++] = vx_utf16_dup("HH:mm:ss");
    data[k++] = vx_utf16_dup("en-US");
    done = 1;
    return data;
}
static WINAPI void *vx_Gettnames(void) { return time_names(); }
static WINAPI size_t vx_Strftime(char *out, size_t cap, const char *fmt, const struct tm *t, void *names) { (void)names; return strftime(out, cap, fmt, t); }
static WINAPI size_t vx_Wcsftime(WCHAR *out, size_t cap, const WCHAR *fmt, const struct tm *t, void *names) {
    (void)names;
    char *f = vx_utf8_dup(fmt), buf[1024];
    size_t n = strftime(buf, sizeof buf, f, t);
    free(f);
    if (!n || n >= cap) return 0;
    return vx_utf8_to_utf16(buf, n + 1, out, cap) - 1;
}
// Wide formatting: the format turned into the narrow one (%s and %c take wide arguments in wide functions).
int vx_format_w(WCHAR *out, size_t cap, const WCHAR *fmt, const char *ap) {
    size_t n = vx_wcslen(fmt);
    char *nf = malloc(n * 4 + 8), *q = nf;
    for (size_t i = 0; i < n; i++) {
        WCHAR c = fmt[i];
        if (c != '%') { char b[4]; size_t k = vx_utf16_to_utf8(&c, 1, b, sizeof b); memcpy(q, b, k); q += k; continue; }
        *q++ = '%';
        size_t j = i + 1;
        while (j < n && fmt[j] && strchr("-+ #0123456789.*", (int)fmt[j])) *q++ = (char)fmt[j++];
        int has_len = j < n && (fmt[j] == 'l' || fmt[j] == 'w' || fmt[j] == 'h');
        if (j < n && (fmt[j] == 's' || fmt[j] == 'c')) *q++ = 'l';          // wide by default here
        else if (j < n && (fmt[j] == 'S' || fmt[j] == 'C')) { *q++ = 'h'; *q++ = (char)(fmt[j] == 'S' ? 's' : 'c'); i = j; continue; }
        (void)has_len;
        i = j - 1;
    }
    *q = 0;
    char *tmp = malloc(cap * 4 + 16);
    int r = vx_format(tmp, cap * 4 + 16, nf, ap);
    size_t w = vx_utf8_to_utf16(tmp, strlen(tmp) + 1, out, cap);
    free(nf); free(tmp);
    return r < 0 ? r : (int)(w ? w - 1 : 0);
}
static WINAPI int vx_vsnwprintf(WCHAR *buf, size_t n, const WCHAR *fmt, const char *ap) { return vx_format_w(buf, n, fmt, ap); }
static WINAPI int vx_snwprintf(WCHAR *buf, size_t n, const WCHAR *fmt, ...) {
    __builtin_ms_va_list ap; __builtin_ms_va_start(ap, fmt);
    int r = vx_format_w(buf, n, fmt, ap);
    __builtin_ms_va_end(ap);
    return r;
}
static WINAPI int __stdio_common_vswprintf(uint64_t options, WCHAR *buf, size_t count, const WCHAR *fmt, void *locale, const char *ap) {
    (void)options; (void)locale;
    if (!buf || !count) { WCHAR tmp[4096]; return vx_format_w(tmp, 4096, fmt, ap); }
    return vx_format_w(buf, count, fmt, ap);
}
static WINAPI int __stdio_common_vswprintf_s(uint64_t options, WCHAR *buf, size_t count, const WCHAR *fmt, void *locale, const char *ap) {
    return __stdio_common_vswprintf(options, buf, count, fmt, locale, ap);
}

// ---- stat, time conversion, scanf, strings, process start-up odds and ends -------------------------------------------------
// struct _stat64 (56 bytes): dev, ino, mode, nlink, uid, gid, rdev, size, atime, mtime, ctime; _stat64i32 has a
// 32-bit size (48 bytes).
static void windows_stat(const struct stat *st, uint8_t *out, int wide_size) {
    memset(out, 0, wide_size ? 56 : 48);
    uint16_t mode = S_ISDIR(st->st_mode) ? 0x4000 : 0x8000;
    mode |= 0x100 | 0x20 | 0x4;                                     // readable
    if (st->st_mode & S_IWUSR) mode |= 0x80 | 0x10 | 0x2;
    if (S_ISDIR(st->st_mode) || (st->st_mode & S_IXUSR)) mode |= 0x40 | 0x8 | 0x1;
    *(uint32_t *)out = 2;                                           // drive C:
    *(uint16_t *)(out + 6) = mode; *(int16_t *)(out + 8) = (int16_t)st->st_nlink;
    *(uint32_t *)(out + 16) = 2;
    if (wide_size) {
        *(int64_t *)(out + 24) = st->st_size;
        *(int64_t *)(out + 32) = st->st_atime; *(int64_t *)(out + 40) = st->st_mtime; *(int64_t *)(out + 48) = st->st_ctime;
    } else {
        *(int32_t *)(out + 20) = (int32_t)st->st_size;
        *(int64_t *)(out + 24) = st->st_atime; *(int64_t *)(out + 32) = st->st_mtime; *(int64_t *)(out + 40) = st->st_ctime;
    }
}
static int stat_path(const char *path, uint8_t *out, int wide_size) {
    char unix_path[4096];
    struct stat st;
    if (!vx_unix_path(path, unix_path, sizeof unix_path) || stat(unix_path, &st)) { set_errno(2); return -1; }
    windows_stat(&st, out, wide_size);
    return 0;
}
static WINAPI int vx_stat64(const char *p, uint8_t *out) { return stat_path(p, out, 1); }
static WINAPI int vx_stat64i32(const char *p, uint8_t *out) { return stat_path(p, out, 0); }
static WINAPI int vx_wstat64(const WCHAR *p, uint8_t *out) { char *a = vx_utf8_dup(p); int r = stat_path(a, out, 1); free(a); return r; }
static WINAPI int vx_wstat64i32(const WCHAR *p, uint8_t *out) { char *a = vx_utf8_dup(p); int r = stat_path(a, out, 0); free(a); return r; }
static WINAPI int vx_fstat64(int fd, uint8_t *out) {
    struct stat st;
    if (fstat(fd, &st)) { set_errno(9); return -1; }
    windows_stat(&st, out, 1);
    return 0;
}
// Time conversion: Windows' struct tm is the nine ints (the Mac's has more after them).
static __thread int t_tm[9];
static int *windows_tm(const struct tm *t) { memcpy(t_tm, t, sizeof t_tm); return t_tm; }
static WINAPI int *vx_gmtime64(const int64_t *t) { time_t v = (time_t)*t; struct tm r; return gmtime_r(&v, &r) ? windows_tm(&r) : NULL; }
static WINAPI int *vx_localtime64(const int64_t *t) { time_t v = (time_t)*t; struct tm r; return localtime_r(&v, &r) ? windows_tm(&r) : NULL; }
static WINAPI int vx_gmtime64_s(int *out, const int64_t *t) { time_t v = (time_t)*t; struct tm r; if (!gmtime_r(&v, &r)) return 22; memcpy(out, &r, 36); return 0; }
static WINAPI int vx_localtime64_s(int *out, const int64_t *t) { time_t v = (time_t)*t; struct tm r; if (!localtime_r(&v, &r)) return 22; memcpy(out, &r, 36); return 0; }
// sscanf with a Windows va_list: the format's size modifiers turned into the Mac's, its pointers passed on.
static WINAPI int __stdio_common_vsscanf(uint64_t options, const char *buf, size_t count, const char *fmt, void *locale, const char *ap) {
    (void)options; (void)locale; (void)count;
    char nf[1024]; size_t k = 0; int nargs = 0;
    for (const char *p = fmt; *p && k < sizeof nf - 8; p++) {
        nf[k++] = *p;
        if (*p != '%') continue;
        if (p[1] == '%') { nf[k++] = *++p; continue; }
        int skip = p[1] == '*';
        while (p[1] && strchr("*0123456789", p[1])) nf[k++] = *++p;
        if (!strncmp(p + 1, "I64", 3)) { nf[k++] = 'l'; nf[k++] = 'l'; p += 3; }
        else if (p[1] == 'l' && strchr("dioux", p[2])) p++;          // Windows long is 32-bit
        else if (p[1] == 'l' && strchr("efg", p[2])) nf[k++] = *++p;  // double
        else if (p[1] == 'z' || p[1] == 'I') { nf[k++] = 'l'; p++; }
        else if (p[1] == 'h' || (p[1] == 'l' && p[2] == 'l')) { nf[k++] = *++p; if (*p == 'l') nf[k++] = *++p; }
        if (p[1] == '[') { while (p[1] && p[1] != ']') nf[k++] = *++p; }
        if (p[1]) nf[k++] = *++p;
        if (!skip) nargs++;
    }
    nf[k] = 0;
    void *a[16] = { 0 };
    for (int i = 0; i < nargs && i < 16; i++) memcpy(&a[i], ap + 8 * i, 8);
    return sscanf(buf, nf, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
}
static WINAPI int vx_wcsicmp(const WCHAR *a, const WCHAR *b) {
    for (;; a++, b++) { WCHAR x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b; if (x != y || !x) return x - y; }
}
static WINAPI int vx_wcsnicmp(const WCHAR *a, const WCHAR *b, size_t n) {
    for (; n--; a++, b++) { WCHAR x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b; if (x != y || !x) return x - y; }
    return 0;
}
static WINAPI char *vx_strncat(char *d, const char *s, size_t n) { return strncat(d, s, n); }
static WINAPI char *vx_strpbrk(const char *s, const char *set) { return strpbrk(s, set); }
static WINAPI size_t vx_strspn(const char *s, const char *set) { return strspn(s, set); }
static WINAPI uint64_t vx_strtoull(const char *s, char **e, int b) { return strtoull(s, e, b); }
typedef struct { int32_t quot, rem; } win_ldiv;
static WINAPI win_ldiv vx_ldiv(int32_t a, int32_t b) { win_ldiv r = { a / b, a % b }; return r; }
static WINAPI short vx_dtest(double *x) { return fp_class(fpclassify(*x)); }
static WINAPI short vx_fdtest(float *x) { return fp_class(fpclassify(*x)); }
static WINAPI int32_t vx_lrintf(float x) { return (int32_t)lrintf(x); }
static int g_fmode;
static WINAPI int vx_set_fmode(int m) { g_fmode = m; return 0; }
static WINAPI int vx_get_fmode(int *m) { *m = g_fmode; return 0; }
static WINAPI void vx_c_exit(void) {}
static WINAPI void vx_invalid_parameter_noinfo(void) { if (vx_trace) vx_log("the program reported an invalid parameter to the C runtime"); }
static WINAPI void vx_register_thread_local_exe_atexit_callback(void *cb) { (void)cb; }
// The command line after the program's name, for WinMain.
static WINAPI char *vx_get_narrow_winmain_command_line(void) {
    char *p = vx_proc.command_line;
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
    else while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}
static WINAPI WCHAR *vx_get_wide_winmain_command_line(void) {
    WCHAR *p = vx_proc.command_line_w;
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
    else while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

// ---- more of <math.h> (Windows long is 32 bits), mbrlen, _mktime64, bsearch_s ---------------------------------------------
static WINAPI int32_t m_lround(double x) { return (int32_t)lround(x); }
static WINAPI int32_t m_lroundf(float x) { return (int32_t)lroundf(x); }
static WINAPI int64_t m_llround(double x) { return llround(x); }
static WINAPI int64_t m_llroundf(float x) { return llroundf(x); }
static WINAPI int32_t m_lrint(double x) { return (int32_t)lrint(x); }
static WINAPI int64_t m_llrint(double x) { return llrint(x); }
static WINAPI int64_t m_llrintf(float x) { return llrintf(x); }
static WINAPI double m_nearbyint(double x) { return nearbyint(x); }
static WINAPI float m_nearbyintf(float x) { return nearbyintf(x); }
static WINAPI double m_expm1(double x) { return expm1(x); }
static WINAPI float m_expm1f(float x) { return expm1f(x); }
static WINAPI double m_log1p(double x) { return log1p(x); }
static WINAPI float m_log1pf(float x) { return log1pf(x); }
static WINAPI double m_remainder(double x, double y) { return remainder(x, y); }
static WINAPI double m_fma(double a, double b, double c) { return fma(a, b, c); }
static WINAPI float m_fmaf(float a, float b, float c) { return fmaf(a, b, c); }
static WINAPI double m_nextafter(double a, double b) { return nextafter(a, b); }
static WINAPI float m_nextafterf(float a, float b) { return nextafterf(a, b); }
static WINAPI double m_erf(double x) { return erf(x); }
static WINAPI double m_erfc(double x) { return erfc(x); }
static WINAPI double m_tgamma(double x) { return tgamma(x); }
static WINAPI double m_asinh(double x) { return asinh(x); }
static WINAPI double m_acosh(double x) { return acosh(x); }
static WINAPI double m_atanh(double x) { return atanh(x); }
static WINAPI float m_asinhf(float x) { return asinhf(x); }
static WINAPI float m_acoshf(float x) { return acoshf(x); }
static WINAPI float m_atanhf(float x) { return atanhf(x); }
static WINAPI double m_fdim(double x, double y) { return fdim(x, y); }
static WINAPI double m_logb(double x) { return logb(x); }
static WINAPI int m_ilogb(double x) { return ilogb(x); }
static WINAPI double m_scalbn(double x, int n) { return scalbn(x, n); }
static WINAPI float m_scalbnf(float x, int n) { return scalbnf(x, n); }
static WINAPI float m_modff(float x, float *i) { return modff(x, i); }
static WINAPI double m_remquo(double x, double y, int *q) { return remquo(x, y, q); }
// wcstombs/mbstowcs: UTF-8 (the code page of the "C" locale here).
static WINAPI size_t vx_wcstombs(char *out, const WCHAR *in, size_t n) {
    size_t need = vx_utf16_to_utf8(in, vx_wcslen(in), NULL, 0);
    if (!out) return need;
    size_t w = vx_utf16_to_utf8(in, vx_wcslen(in), out, n);
    if (w < n) out[w] = 0;
    return w < n ? w : n;
}
static WINAPI size_t vx_mbstowcs(WCHAR *out, const char *in, size_t n) {
    size_t need = vx_utf8_to_utf16(in, strlen(in), NULL, 0);
    if (!out) return need;
    size_t w = vx_utf8_to_utf16(in, strlen(in), out, n);
    if (w < n) out[w] = 0;
    return w < n ? w : n;
}
static WINAPI size_t vx_mbrlen(const char *s, size_t n, void *state) {
    (void)state;
    if (!s || !n) return 0;
    uint8_t c = (uint8_t)*s;
    size_t need = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;   // UTF-8
    return !c ? 0 : need > n ? (size_t)-2 : need;
}
static WINAPI int64_t vx_mktime64(int *t) {
    struct tm tm = { 0 };
    memcpy(&tm, t, 36);
    time_t r = mktime(&tm);
    memcpy(t, &tm, 36);
    return (int64_t)r;
}
typedef int (WINAPI *compare_s)(void *, const void *, const void *);
static WINAPI void *vx_bsearch_s(const void *key, const void *base, size_t n, size_t size, compare_s cmp, void *ctx) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const void *e = (const uint8_t *)base + mid * size;
        int c = cmp(ctx, key, e);
        if (!c) return (void *)e;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

static WINAPI int vx_getpid(void) { return (int)getpid(); }
static const vx_export ucrt[] = {
    { "_getpid", (void *)vx_getpid },
    { "lround", (void *)m_lround }, { "lroundf", (void *)m_lroundf }, { "llround", (void *)m_llround }, { "llroundf", (void *)m_llroundf },
    { "lrint", (void *)m_lrint }, { "llrint", (void *)m_llrint }, { "llrintf", (void *)m_llrintf }, { "nearbyint", (void *)m_nearbyint },
    { "nearbyintf", (void *)m_nearbyintf }, { "expm1", (void *)m_expm1 }, { "expm1f", (void *)m_expm1f }, { "log1p", (void *)m_log1p },
    { "log1pf", (void *)m_log1pf }, { "remainder", (void *)m_remainder }, { "fma", (void *)m_fma }, { "fmaf", (void *)m_fmaf },
    { "nextafter", (void *)m_nextafter }, { "nextafterf", (void *)m_nextafterf }, { "_nextafter", (void *)m_nextafter },
    { "erf", (void *)m_erf }, { "erfc", (void *)m_erfc }, { "tgamma", (void *)m_tgamma }, { "asinh", (void *)m_asinh },
    { "acosh", (void *)m_acosh }, { "atanh", (void *)m_atanh }, { "asinhf", (void *)m_asinhf }, { "acoshf", (void *)m_acoshf },
    { "atanhf", (void *)m_atanhf }, { "fdim", (void *)m_fdim }, { "logb", (void *)m_logb }, { "_logb", (void *)m_logb },
    { "ilogb", (void *)m_ilogb }, { "scalbn", (void *)m_scalbn }, { "scalbnf", (void *)m_scalbnf }, { "modff", (void *)m_modff },
    { "remquo", (void *)m_remquo }, { "mbrlen", (void *)vx_mbrlen }, { "wcstombs", (void *)vx_wcstombs }, { "mbstowcs", (void *)vx_mbstowcs }, { "_mktime64", (void *)vx_mktime64 },
    { "bsearch_s", (void *)vx_bsearch_s },
    { "_stat64", (void *)vx_stat64 }, { "_stat64i32", (void *)vx_stat64i32 }, { "_wstat64", (void *)vx_wstat64 },
    { "_wstat64i32", (void *)vx_wstat64i32 }, { "_fstat64", (void *)vx_fstat64 }, { "_gmtime64", (void *)vx_gmtime64 },
    { "_localtime64", (void *)vx_localtime64 }, { "_gmtime64_s", (void *)vx_gmtime64_s }, { "_localtime64_s", (void *)vx_localtime64_s },
    VX_EXPORT(__stdio_common_vsscanf), { "_wcsicmp", (void *)vx_wcsicmp }, { "_wcsnicmp", (void *)vx_wcsnicmp },
    { "strncat", (void *)vx_strncat }, { "strpbrk", (void *)vx_strpbrk }, { "strspn", (void *)vx_strspn }, { "strtoull", (void *)vx_strtoull },
    { "ldiv", (void *)vx_ldiv }, { "_dtest", (void *)vx_dtest }, { "_fdtest", (void *)vx_fdtest }, { "lrintf", (void *)vx_lrintf },
    { "_set_fmode", (void *)vx_set_fmode }, { "_get_fmode", (void *)vx_get_fmode }, { "_c_exit", (void *)vx_c_exit },
    { "_invalid_parameter_noinfo", (void *)vx_invalid_parameter_noinfo },
    { "_register_thread_local_exe_atexit_callback", (void *)vx_register_thread_local_exe_atexit_callback },
    { "_get_narrow_winmain_command_line", (void *)vx_get_narrow_winmain_command_line },
    { "_get_wide_winmain_command_line", (void *)vx_get_wide_winmain_command_line },
    { "__pctype_func", (void *)vx_pctype_func }, { "___lc_codepage_func", (void *)vx_lc_codepage_func },
    { "___lc_collate_cp_func", (void *)vx_lc_codepage_func }, { "___lc_locale_name_func", (void *)vx_lc_locale_name_func },
    { "_lock_locales", (void *)vx_lock_locales }, { "_unlock_locales", (void *)vx_lock_locales }, { "_wsetlocale", (void *)vx_wsetlocale },
    { "_set_new_handler", (void *)vx_set_new_handler }, { "terminate", (void *)vx_terminate },
    { "_wremove", (void *)vx_wremove }, { "_wrmdir", (void *)vx_wrmdir }, { "_wrename", (void *)vx_wrename }, { "_wchdir", (void *)vx_wchdir },
    { "_wgetcwd", (void *)vx_wgetcwd }, { "_dclass", (void *)vx_dclass }, { "_fdclass", (void *)vx_fdclass }, { "_dsign", (void *)vx_dsign },
    { "_fdsign", (void *)vx_fdsign }, { "copysign", (void *)vx_copysign }, { "_copysign", (void *)vx_copysign }, { "lgamma", (void *)vx_lgamma },
    { "lgammal", (void *)vx_lgamma }, { "lgammaf", (void *)vx_lgammaf }, { "__strncnt", (void *)vx_strncnt }, { "_wcsdup", (void *)vx_wcsdup },
    { "strcspn", (void *)vx_strcspn }, { "strtof", (void *)vx_strtof }, { "wcstod", (void *)vx_wcstod }, { "wcstof", (void *)vx_wcstof },
    { "_fsopen", (void *)vx_fsopen }, { "_wfsopen", (void *)vx_wfsopen }, { "_get_stream_buffer_pointers", (void *)vx_get_stream_buffer_pointers },
    { "fgetpos", (void *)vx_fgetpos }, { "fsetpos", (void *)vx_fsetpos }, { "fgetwc", (void *)vx_fgetwc }, { "fputwc", (void *)vx_fputwc },
    { "ungetwc", (void *)vx_ungetwc }, { "_Getdays", (void *)vx_Getdays }, { "_Getmonths", (void *)vx_Getmonths },
    { "_W_Getdays", (void *)vx_W_Getdays }, { "_W_Getmonths", (void *)vx_W_Getmonths }, { "_Gettnames", (void *)vx_Gettnames },
    { "_W_Gettnames", (void *)vx_Gettnames }, { "_Strftime", (void *)vx_Strftime }, { "_Wcsftime", (void *)vx_Wcsftime },
    { "_vsnwprintf", (void *)vx_vsnwprintf }, { "_snwprintf", (void *)vx_snwprintf }, VX_EXPORT(__stdio_common_vswprintf),
    VX_EXPORT(__stdio_common_vswprintf_s),
    { "_onexit", (void *)vx_onexit }, { "_amsg_exit", (void *)vx_amsg_exit }, { "_XcptFilter", (void *)vx_XcptFilter },
    { "_strtoui64", (void *)vx_strtoui64 }, { "_strtoi64", (void *)vx_strtoi64 }, { "isxdigit", (void *)vx_isxdigit },
    { "isalnum", (void *)vx_isalnum }, { "isupper", (void *)vx_isupper }, { "islower", (void *)vx_islower },
    { "isprint", (void *)vx_isprint }, { "ispunct", (void *)vx_ispunct }, { "atof", (void *)vx_atof },
    { "setlocale", (void *)vx_setlocale }, { "_mbstrlen", (void *)vx_mbstrlen }, { "_isnan", (void *)vx_isnan },
    { "_finite", (void *)vx_finite }, { "_clearfp", (void *)vx_clearfp }, { "_statusfp", (void *)vx_statusfp },
    { "_controlfp", (void *)vx_controlfp }, { "_controlfp_s", (void *)vx_controlfp_s }, { "_fpclass", (void *)vx_fpclass },
    { "_stricmp", (void *)vx_stricmp }, { "_strnicmp", (void *)vx_strnicmp }, { "_vsnprintf", (void *)vx_vsnprintf },
    { "_snprintf", (void *)vx_snprintf }, { "sprintf", (void *)vx_sprintf },
    { "_aligned_malloc", (void *)vx_aligned_malloc }, { "_aligned_free", (void *)vx_aligned_free },
    { "_aligned_realloc", (void *)vx_aligned_realloc }, { "_aligned_msize", (void *)vx_aligned_msize },
    { "_lock", (void *)vx_lock }, { "_unlock", (void *)vx_unlock }, { "__dllonexit", (void *)vx_dllonexit },
    { "??2@YAPEAX_K@Z", (void *)vx_new }, { "??_U@YAPEAX_K@Z", (void *)vx_new },
    { "??3@YAXPEAX@Z", (void *)vx_delete }, { "??_V@YAXPEAX@Z", (void *)vx_delete },
    MATH_EXPORTS(sqrt), MATH_EXPORTS(sin), MATH_EXPORTS(cos), MATH_EXPORTS(tan), MATH_EXPORTS(asin), MATH_EXPORTS(acos),
    MATH_EXPORTS(atan), MATH_EXPORTS(exp), MATH_EXPORTS(log), MATH_EXPORTS(log10), MATH_EXPORTS(log2), MATH_EXPORTS(exp2), MATH_EXPORTS(floor),
    MATH_EXPORTS(ceil), MATH_EXPORTS(round), MATH_EXPORTS(trunc), MATH_EXPORTS(fabs), MATH_EXPORTS(sinh), MATH_EXPORTS(cosh),
    MATH_EXPORTS(tanh), MATH_EXPORTS(cbrt), MATH_EXPORTS(atan2), MATH_EXPORTS(pow), MATH_EXPORTS(fmod), MATH_EXPORTS(hypot),
    MATH_EXPORTS(fmin), MATH_EXPORTS(fmax),
    { "ldexp", (void *)vx_ldexp }, { "frexp", (void *)vx_frexp }, { "modf", (void *)vx_modf },
    { "strcpy_s", (void *)vx_strcpy_s }, { "strcat_s", (void *)vx_strcat_s }, { "strncpy_s", (void *)vx_strncpy_s },
    { "memcpy_s", (void *)vx_memcpy_s }, { "memmove_s", (void *)vx_memmove_s }, { "wcscpy_s", (void *)vx_wcscpy_s },
    VX_EXPORT(__stdio_common_vsprintf_s),
    VX_EXPORT(__acrt_iob_func), VX_EXPORT(__stdio_common_vfprintf), VX_EXPORT(__stdio_common_vsprintf),
    VX_EXPORT(__stdio_common_vsnprintf_s),
    { "fflush", (void *)vx_fflush }, { "setvbuf", (void *)vx_setvbuf }, { "puts", (void *)vx_puts },
    { "fputs", (void *)vx_fputs }, { "putchar", (void *)vx_putchar }, { "fputc", (void *)vx_fputc },
    { "fwrite", (void *)vx_fwrite },
    { "fopen", (void *)vx_fopen }, { "_wfopen", (void *)vx_wfopen }, { "fopen_s", (void *)vx_fopen_s }, { "_wfopen_s", (void *)vx_wfopen_s },
    { "fclose", (void *)vx_fclose }, { "fread", (void *)vx_fread }, { "fgets", (void *)vx_fgets }, { "fgetc", (void *)vx_fgetc },
    { "getc", (void *)vx_fgetc }, { "_fgetc_nolock", (void *)vx_fgetc }, { "ungetc", (void *)vx_ungetc }, { "getchar", (void *)vx_getchar },
    { "feof", (void *)vx_feof }, { "ferror", (void *)vx_ferror }, { "clearerr", (void *)vx_clearerr },
    { "fseek", (void *)vx_fseek }, { "_fseeki64", (void *)vx_fseeki64 }, { "ftell", (void *)vx_ftell }, { "_ftelli64", (void *)vx_ftelli64 },
    { "rewind", (void *)vx_rewind }, { "remove", (void *)vx_remove }, { "_unlink", (void *)vx_remove }, { "rename", (void *)vx_rename },
    { "_mkdir", (void *)vx_mkdir }, { "_rmdir", (void *)vx_rmdir }, { "_access", (void *)vx_access },
    { "_getcwd", (void *)vx_getcwd }, { "_chdir", (void *)vx_chdir },
    VX_EXPORT(__p__fmode), VX_EXPORT(__p__commode), VX_EXPORT(__p___argc), VX_EXPORT(__p___argv), VX_EXPORT(__p__environ),
    VX_EXPORT(_configure_narrow_argv), VX_EXPORT(_initialize_narrow_environment), VX_EXPORT(_get_initial_narrow_environment),
    VX_EXPORT(_get_initial_wide_environment), VX_EXPORT(__p__wenviron), VX_EXPORT(_configure_wide_argv), VX_EXPORT(__p___wargv),
    VX_EXPORT(_initialize_wide_environment), { "wcscpy", (void *)vx_wcscpy }, { "wcscat", (void *)vx_wcscat }, { "wcsrchr", (void *)vx_wcsrchr }, { "_mbsstr", (void *)vx_mbsstr }, VX_EXPORT(_set_app_type),
    VX_EXPORT(_set_invalid_parameter_handler), VX_EXPORT(__setusermatherr), VX_EXPORT(_configthreadlocale),
    VX_EXPORT(_set_new_mode), { "signal", (void *)vx_signal },
    VX_EXPORT(_initterm), VX_EXPORT(_initterm_e), VX_EXPORT(_crt_atexit), { "atexit", (void *)vx_atexit },
    VX_EXPORT(_cexit), { "exit", (void *)vx_exit }, { "_exit", (void *)vx__exit }, { "abort", (void *)vx_abort },
    { "malloc", (void *)vx_malloc }, { "calloc", (void *)vx_calloc }, { "realloc", (void *)vx_realloc }, { "free", (void *)vx_free },
    { "memcpy", (void *)vx_memcpy }, { "memmove", (void *)vx_memmove }, { "memset", (void *)vx_memset }, { "memcmp", (void *)vx_memcmp },
    { "strlen", (void *)vx_strlen }, { "strcmp", (void *)vx_strcmp }, { "strncmp", (void *)vx_strncmp },
    { "strcpy", (void *)vx_strcpy }, { "strncpy", (void *)vx_strncpy }, { "strcat", (void *)vx_strcat },
    { "strchr", (void *)vx_strchr }, { "strrchr", (void *)vx_strrchr }, { "strstr", (void *)vx_strstr },
    { "_strdup", (void *)vx_strdup }, { "strdup", (void *)vx_strdup },
    { "atoi", (void *)vx_atoi }, { "strtol", (void *)vx_strtol }, { "strtoul", (void *)vx_strtoul },
    { "strtoll", (void *)vx_strtoll }, { "strtod", (void *)vx_strtod }, { "getenv", (void *)vx_getenv },
    { "toupper", (void *)vx_toupper }, { "tolower", (void *)vx_tolower }, { "isspace", (void *)vx_isspace },
    { "isdigit", (void *)vx_isdigit }, { "isalpha", (void *)vx_isalpha },
    { "rand", (void *)vx_rand }, { "srand", (void *)vx_srand }, { "_time64", (void *)vx_time64 }, { "clock", (void *)vx_clock },
    { "_errno", (void *)vx__errno }, { "qsort", (void *)vx_qsort },
    { "_assert", (void *)vx_assert }, { "_fileno", (void *)vx_fileno }, { "_lock_file", (void *)vx_lock_file },
    { "_unlock_file", (void *)vx_lock_file }, { "_read", (void *)vx_read }, { "_setmode", (void *)vx_setmode },
    { "memchr", (void *)vx_memchr }, { "strnlen", (void *)vx_strnlen }, { "strerror", (void *)vx_strerror },
    { "wcslen", (void *)crt_wcslen }, { "wcsnlen", (void *)vx_wcsnlen }, { "rand_s", (void *)vx_rand_s },
    { "localeconv", (void *)vx_localeconv }, { "mbrtowc", (void *)vx_mbrtowc }, { "mbsrtowcs", (void *)vx_mbsrtowcs },
    { "wcrtomb", (void *)vx_wcrtomb }, { "wcscmp", (void *)vx_wcscmp },
    VX_EXPORT(_initialize_onexit_table), VX_EXPORT(_register_onexit_function), VX_EXPORT(_execute_onexit_table),
};

void vx_ucrt_init(void) {
    vx_register_module("ucrtbase.dll", ucrt, sizeof ucrt / sizeof ucrt[0]);
    vx_register_module("msvcrt.dll", ucrt, sizeof ucrt / sizeof ucrt[0]);
}
