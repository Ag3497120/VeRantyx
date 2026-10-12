// ucrtbase (and the api-ms-win-crt-* API sets): the Universal C Runtime, implemented on macOS's libc.
//
// Every function here uses the Windows calling convention, so libc calls are wrapped, never exported raw.
// printf and friends arrive as __stdio_common_v*printf with a Windows va_list: a pointer to consecutive 8-byte
// argument slots (doubles included), which vx_format walks itself.
#include <ctype.h>
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
typedef struct { int fd; } vx_file;
static vx_file g_iob[3] = { { 0 }, { 1 }, { 2 } };

static WINAPI vx_file *__acrt_iob_func(unsigned i) { return i < 3 ? &g_iob[i] : NULL; }

static int write_all(int fd, const char *s, size_t n) {
    while (n) { ssize_t w = write(fd, s, n); if (w <= 0) return -1; s += w; n -= (size_t)w; }
    return 0;
}

static WINAPI int __stdio_common_vfprintf(uint64_t options, vx_file *f, const char *fmt, void *locale, const char *ap) {
    (void)options; (void)locale;
    char stackbuf[1024];
    int n = vx_format(stackbuf, sizeof stackbuf, fmt, ap);
    if (n < (int)sizeof stackbuf) return write_all(f->fd, stackbuf, (size_t)n) ? -1 : n;
    char *heap = malloc((size_t)n + 1);
    vx_format(heap, (size_t)n + 1, fmt, ap);
    int r = write_all(f->fd, heap, (size_t)n) ? -1 : n;
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

static WINAPI int vx_fflush(vx_file *f) { (void)f; return 0; }
static WINAPI int vx_setvbuf(vx_file *f, char *b, int m, size_t s) { (void)f; (void)b; (void)m; (void)s; return 0; }
static WINAPI int vx_puts(const char *s) { return write_all(1, s, strlen(s)) || write_all(1, "\n", 1) ? -1 : 0; }
static WINAPI int vx_fputs(const char *s, vx_file *f) { return write_all(f->fd, s, strlen(s)) ? -1 : 0; }
static WINAPI int vx_putchar(int c) { char ch = (char)c; return write_all(1, &ch, 1) ? -1 : c; }
static WINAPI int vx_fputc(int c, vx_file *f) { char ch = (char)c; return write_all(f->fd, &ch, 1) ? -1 : c; }
static WINAPI size_t vx_fwrite(const void *p, size_t sz, size_t n, vx_file *f) { return write_all(f->fd, p, sz * n) ? 0 : n; }

// ---- process startup and exit ----------------------------------------------------------------------------------------------
static int g_fmode, g_commode;
static WINAPI int *__p__fmode(void) { return &g_fmode; }
static WINAPI int *__p__commode(void) { return &g_commode; }
static WINAPI int *__p___argc(void) { return &vx_proc.argc; }
static WINAPI char ***__p___argv(void) { return &vx_proc.argv; }
static WINAPI char ***__p__environ(void) { return &vx_proc.envp; }
static WINAPI int _configure_narrow_argv(int mode) { (void)mode; return 0; }
static WINAPI int _initialize_narrow_environment(void) { return 0; }
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
static WINAPI void _cexit(void) { run_atexit(); }
static WINAPI void vx_exit(int code) { run_atexit(); _exit(code); }
static WINAPI void vx__exit(int code) { _exit(code); }
static WINAPI void vx_abort(void) { vx_log("the program called abort()"); _exit(3); }


// errno is per thread on Windows too.
static __thread int t_errno;
static int *g_errno_ptr(void) { return &t_errno; }

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
static WINAPI size_t vx_wcslen(const WCHAR *s) { size_t n = 0; while (s[n]) n++; return n; }
static WINAPI size_t vx_wcsnlen(const WCHAR *s, size_t max) { size_t n = 0; while (n < max && s[n]) n++; return n; }
static WINAPI int vx_rand_s(unsigned *v) { arc4random_buf(v, sizeof *v); return 0; }

// Windows struct lconv: the char* members first (decimal_point, thousands_sep, grouping, ...), all "C" locale.
static char *g_lconv[32] = { ".", "", "" };
static WINAPI void *vx_localeconv(void) {
    for (int i = 3; i < 10; i++) if (!g_lconv[i]) g_lconv[i] = "";
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

static const vx_export ucrt[] = {
    VX_EXPORT(__acrt_iob_func), VX_EXPORT(__stdio_common_vfprintf), VX_EXPORT(__stdio_common_vsprintf),
    VX_EXPORT(__stdio_common_vsnprintf_s),
    { "fflush", (void *)vx_fflush }, { "setvbuf", (void *)vx_setvbuf }, { "puts", (void *)vx_puts },
    { "fputs", (void *)vx_fputs }, { "putchar", (void *)vx_putchar }, { "fputc", (void *)vx_fputc },
    { "fwrite", (void *)vx_fwrite },
    VX_EXPORT(__p__fmode), VX_EXPORT(__p__commode), VX_EXPORT(__p___argc), VX_EXPORT(__p___argv), VX_EXPORT(__p__environ),
    VX_EXPORT(_configure_narrow_argv), VX_EXPORT(_initialize_narrow_environment), VX_EXPORT(_set_app_type),
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
    { "wcslen", (void *)vx_wcslen }, { "wcsnlen", (void *)vx_wcsnlen }, { "rand_s", (void *)vx_rand_s },
    { "localeconv", (void *)vx_localeconv }, { "mbrtowc", (void *)vx_mbrtowc }, { "mbsrtowcs", (void *)vx_mbsrtowcs },
    { "wcrtomb", (void *)vx_wcrtomb },
};

void vx_ucrt_init(void) {
    vx_register_module("ucrtbase.dll", ucrt, sizeof ucrt / sizeof ucrt[0]);
    vx_register_module("msvcrt.dll", ucrt, sizeof ucrt / sizeof ucrt[0]);
}
