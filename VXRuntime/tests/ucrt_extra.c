// UCRT functions No Man's Sky imports (ucrt.c): locale objects and their _l variants, the _s conversions and string
// functions, strtok, times, environment. Looked up in ucrtbase.dll by name, with made-up data.
#include <windows.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)
static HMODULE crt;
#define FN(type, name) __typeof__(type) p##name = (__typeof__(type))(void *)GetProcAddress(crt, #name); CHECK(p##name != NULL)
typedef struct { const unsigned short *pctype; int mb_cur_max; unsigned codepage; } locale_public;
typedef struct { locale_public *locinfo; void *mbcinfo; } *locale_t;
typedef struct { int sec, min, hour, mday, mon, year, wday, yday, isdst; } wtm;
int main(void) {
    crt = LoadLibraryA("ucrtbase.dll");
    CHECK(crt != NULL);
    FN(locale_t (__cdecl *)(int, const char *), _create_locale);
    FN(locale_t (__cdecl *)(int, const wchar_t *), _wcreate_locale);
    FN(locale_t (__cdecl *)(void), _get_current_locale);
    FN(void (__cdecl *)(locale_t), _free_locale);
    FN(long long (__cdecl *)(const char *, char **, int, locale_t), _strtoi64_l);
    FN(unsigned long long (__cdecl *)(const char *, char **, int, locale_t), _strtoui64_l);
    FN(double (__cdecl *)(const char *, char **, locale_t), _strtod_l);
    if (!failures) {
        locale_t c = p_create_locale(0 /* LC_ALL */, "C");
        CHECK(c && c->locinfo && c->locinfo->pctype && c->locinfo->mb_cur_max == 1);
        CHECK(c && (c->locinfo->pctype['7'] & 4) && (c->locinfo->pctype['q'] & 2) && !(c->locinfo->pctype['#'] & 0x103));
        CHECK(p_create_locale(9, "C") == NULL && p_create_locale(0, NULL) == NULL);
        char *end;
        CHECK(p_strtoi64_l("-81985529216486895 tail", &end, 10, c) == -81985529216486895LL && !strcmp(end, " tail"));
        CHECK(p_strtoui64_l("fedcba9876543210", NULL, 16, c) == 0xfedcba9876543210ULL);
        CHECK(p_strtod_l("2.75e3x", &end, c) == 2750.0 && *end == 'x');
        locale_t w = p_wcreate_locale(0, L"C"), cur = p_get_current_locale();
        CHECK(w && cur && w != cur);
        p_free_locale(c); p_free_locale(w); p_free_locale(cur);
    }
    FN(long long (__cdecl *)(const char *), _atoi64);
    FN(int (__cdecl *)(unsigned long, char *, size_t, int), _ultoa_s);
    FN(int (__cdecl *)(long long, char *, size_t, int), _i64toa_s);
    FN(int (__cdecl *)(unsigned long long, char *, size_t, int), _ui64toa_s);
    if (p_atoi64 && p_ultoa_s && p_i64toa_s && p_ui64toa_s) {
        char b[32];
        CHECK(p_atoi64("  -4611686018427387904") == -4611686018427387904LL);
        CHECK(p_ultoa_s(0xbeefu, b, sizeof b, 16) == 0 && !strcmp(b, "beef"));
        CHECK(p_i64toa_s(-90071992547409LL, b, sizeof b, 10) == 0 && !strcmp(b, "-90071992547409"));
        CHECK(p_i64toa_s(-1, b, sizeof b, 16) == 0 && !strcmp(b, "ffffffffffffffff"));
        CHECK(p_ui64toa_s(37, b, sizeof b, 2) == 0 && !strcmp(b, "100101"));
        CHECK(p_ui64toa_s(123456, b, 4, 10) == 34 /* ERANGE */ && b[0] == 0);
        CHECK(p_ultoa_s(5, b, sizeof b, 40) == 22 /* EINVAL */);
    }
    FN(int (__cdecl *)(float, float), _fdpcomp);
    FN(int (__cdecl *)(double, double), _dpcomp);
    if (p_fdpcomp && p_dpcomp) {
        volatile float nan = 0.0f; nan = nan / nan;
        CHECK(p_fdpcomp(1.5f, 2.5f) == 1 && p_fdpcomp(3.0f, 3.0f) == 2 && p_fdpcomp(4.0f, -4.0f) == 4 && p_fdpcomp(nan, 1.0f) == 0);
        CHECK(p_dpcomp(-7.25, -7.0) == 1);
    }
    FN(char **(__cdecl *)(void), __sys_errlist);
    if (p__sys_errlist) CHECK(!strcmp(p__sys_errlist()[2], "No such file or directory") && !strcmp(p__sys_errlist()[22], "Invalid argument"));
    FN(int (__cdecl *)(const char *), system);
    if (psystem) CHECK(psystem(NULL) == 0);
    FN(char *(__cdecl *)(char *), _strupr);
    FN(char *(__cdecl *)(char *), _strrev);
    if (p_strupr && p_strrev) { char s[] = "Gamma-7b"; CHECK(!strcmp(p_strupr(s), "GAMMA-7B") && !strcmp(p_strrev(s), "B7-AMMAG")); }
    FN(int (__cdecl *)(wchar_t *, size_t, const wchar_t *, size_t), wcsncpy_s);
    FN(int (__cdecl *)(wchar_t *, size_t, const wchar_t *, size_t), wcsncat_s);
    FN(int (__cdecl *)(wchar_t *, size_t, const wchar_t *), wcscat_s);
    if (pwcsncpy_s && pwcsncat_s && pwcscat_s) {
        wchar_t w[8];
        CHECK(pwcsncpy_s(w, 8, L"orbital", 3) == 0 && !wcscmp(w, L"orb"));
        CHECK(pwcsncat_s(w, 8, L"iter!", 4) == 0 && !wcscmp(w, L"orbiter"));
        CHECK(pwcscat_s(w, 8, L"x") == 34 && w[0] == 0);
        CHECK(pwcsncpy_s(w, 4, L"freighter", (size_t)-1) == 80 /* STRUNCATE */ && !wcscmp(w, L"fre"));
        CHECK(pwcscat_s(w, 8, L"ak") == 0 && !wcscmp(w, L"freak"));
    }
    FN(char *(__cdecl *)(char *, const char *), strtok);
    FN(char *(__cdecl *)(char *, const char *, char **), strtok_s);
    FN(wchar_t *(__cdecl *)(wchar_t *, const wchar_t *, wchar_t **), wcstok_s);
    if (pstrtok && pstrtok_s && pwcstok_s) {
        char a[] = ",,alpha,,beta;gamma,", b[] = "x y";
        CHECK(!strcmp(pstrtok(a, ",;"), "alpha"));
        char *ctx = NULL;
        CHECK(!strcmp(pstrtok_s(b, " ", &ctx), "x"));                 // independent of strtok's own position
        CHECK(!strcmp(pstrtok(NULL, ",;"), "beta") && !strcmp(pstrtok(NULL, ",;"), "gamma") && pstrtok(NULL, ",;") == NULL);
        CHECK(!strcmp(pstrtok_s(NULL, " ", &ctx), "y") && pstrtok_s(NULL, " ", &ctx) == NULL);
        wchar_t w[] = L"k=v|q", *wc = NULL;
        CHECK(!wcscmp(pwcstok_s(w, L"=|", &wc), L"k") && !wcscmp(pwcstok_s(NULL, L"=|", &wc), L"v") && !wcscmp(pwcstok_s(NULL, L"=|", &wc), L"q"));
        CHECK(pwcstok_s(NULL, L"=|", &wc) == NULL);
    }
    FN(long long (__cdecl *)(wtm *), _mkgmtime64);
    FN(double (__cdecl *)(long long, long long), _difftime64);
    FN(char *(__cdecl *)(const wtm *), asctime);
    if (p_mkgmtime64 && p_difftime64 && pasctime) {
        wtm t = { 5, 4, 3, 31, 0, 101, 0, 0, 0 };                     // 2001-01-31 03:04:05 UTC
        CHECK(p_mkgmtime64(&t) == 980910245LL && t.wday == 3 && t.yday == 30);
        wtm n = { 0, 0, 0, 32, 0, 101, 0, 0, 0 };                     // January 32nd: February 1st
        CHECK(p_mkgmtime64(&n) == 980985600LL && n.mon == 1 && n.mday == 1);
        CHECK(!strcmp(pasctime(&t), "Wed Jan 31 03:04:05 2001\n"));
        CHECK(p_difftime64(1000, 250) == 750.0);
    }
    FN(int (__cdecl *)(char **, size_t *, const char *), _dupenv_s);
    FN(int (__cdecl *)(size_t *, wchar_t *, size_t, const wchar_t *), _wgetenv_s);
    FN(int (__cdecl *)(const wchar_t *, int), _waccess);
    FN(int (__cdecl *)(void (__cdecl *)(void)), _crt_at_quick_exit);
    FN(void *, __stdio_common_vfprintf_s);
    if (p_dupenv_s && p_wgetenv_s && p_waccess) {
        SetEnvironmentVariableA("VXRT_UCRT_TEST", "nebula");
        char *v = NULL; size_t len = 0;
        CHECK(p_dupenv_s(&v, &len, "VXRT_UCRT_TEST") == 0 && v && !strcmp(v, "nebula") && len == 7);
        free(v);
        CHECK(p_dupenv_s(&v, &len, "VXRT_UCRT_NOT_SET") == 0 && v == NULL && len == 0);
        wchar_t w[16]; size_t wl = 0;
        CHECK(p_wgetenv_s(&wl, w, 16, L"VXRT_UCRT_TEST") == 0 && wl == 7 && !wcscmp(w, L"nebula"));
        CHECK(p_wgetenv_s(&wl, w, 3, L"VXRT_UCRT_TEST") == 34 && wl == 7);
        CHECK(p_wgetenv_s(&wl, NULL, 0, L"VXRT_UCRT_TEST") == 0 && wl == 7);
        CHECK(p_waccess(L"C:\\vxrt_no_such_file.bin", 0) == -1);
    }
    printf("ucrt extra: locales, _s conversions, strtok, times, environment -> %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
