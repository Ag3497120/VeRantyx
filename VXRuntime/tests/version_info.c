// version.dll (src/version.c): a file's version resource and VerQueryValue over it.
// - this program's own file (tests/version_info.rc, made-up data): VS_FIXEDFILEINFO, the translation, strings (W and A,
//   sub-block names in any case);
// - kernel32.dll where GetModuleFileName puts it (one of the runtime's own DLLs, no file): a block for 10.0.19045;
// - a file with no version resource, and no file: 0 with ERROR_RESOURCE_TYPE_NOT_FOUND / ERROR_FILE_NOT_FOUND;
// - a buffer smaller than the size asked: the start of the block.
// Chromium (Steam's web helper) stopped at an int3 when kernel32.dll had no version information.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail = 1; } } while (0)

static void *read_info(const WCHAR *path, DWORD *size) {
    DWORD handle = 7;
    *size = GetFileVersionInfoSizeW(path, &handle);
    CHECK(*size > 0, "GetFileVersionInfoSizeW(%ls) = 0, error %lu", path, GetLastError());
    CHECK(handle == 0, "handle %lu", handle);
    if (!*size) return NULL;
    void *b = malloc(*size);
    CHECK(GetFileVersionInfoW(path, 0, *size, b), "GetFileVersionInfoW(%ls) failed, error %lu", path, GetLastError());
    return b;
}

static void own_file(void) {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    DWORD size;
    void *b = read_info(path, &size);
    if (!b) return;
    VS_FIXEDFILEINFO *f = NULL; UINT n = 0;
    CHECK(VerQueryValueW(b, L"\\", (void **)&f, &n) && f && n == sizeof *f, "root: %p %u", (void *)f, n);
    if (f) {
        CHECK(f->dwSignature == 0xfeef04bd, "signature %#lx", f->dwSignature);
        CHECK(f->dwFileVersionMS == 0x00010002 && f->dwFileVersionLS == 0x00030004, "file version %#lx %#lx", f->dwFileVersionMS, f->dwFileVersionLS);
        CHECK(f->dwProductVersionMS == 0x00050006 && f->dwProductVersionLS == 0x00070008, "product version %#lx %#lx", f->dwProductVersionMS, f->dwProductVersionLS);
        CHECK(f->dwFileType == 1, "file type %lu", f->dwFileType);
    }
    WORD *tr = NULL;
    CHECK(VerQueryValueW(b, L"\\VarFileInfo\\Translation", (void **)&tr, &n) && n == 4 && tr && tr[0] == 0x411 && tr[1] == 1200,
          "translation %u: %04x %04x", n, tr ? tr[0] : 0, tr ? tr[1] : 0);
    WCHAR *s = NULL;
    CHECK(VerQueryValueW(b, L"\\StringFileInfo\\041104B0\\CompanyName", (void **)&s, &n) && s && !wcscmp(s, L"Example Test Works") && n >= 18,
          "CompanyName %ls (%u)", s ? s : L"(none)", n);
    CHECK(VerQueryValueW(b, L"\\stringfileinfo\\041104b0\\productversion", (void **)&s, &n) && s && !wcscmp(s, L"5.6.7.8"),
          "ProductVersion (lower case path) %ls", s ? s : L"(none)");
    CHECK(!VerQueryValueW(b, L"\\StringFileInfo\\041104B0\\Missing", (void **)&s, &n), "a missing value was found");
    char *a = NULL;
    CHECK(VerQueryValueA(b, "\\StringFileInfo\\041104B0\\FileDescription", (void **)&a, &n) && a && !strcmp(a, "Version resource test") && n == 22,
          "FileDescription (A) %s (%u)", a ? a : "(none)", n);
    // A wide string asked again is still there (the ANSI copy is beside it, not over it).
    CHECK(VerQueryValueW(b, L"\\StringFileInfo\\041104B0\\FileDescription", (void **)&s, &n) && s && !wcscmp(s, L"Version resource test"),
          "FileDescription (W) %ls", s ? s : L"(none)");
    // A buffer smaller than asked: the start of the block (Windows copies what fits).
    char small[16];
    memset(small, 0xcc, sizeof small);
    CHECK(GetFileVersionInfoW(path, 0, sizeof small, small) && !memcmp(small, b, sizeof small), "small buffer");
    free(b);
}

static void runtime_dll(void) {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(GetModuleHandleW(L"kernel32.dll"), path, MAX_PATH);
    DWORD size;
    void *b = read_info(path, &size);
    if (!b) return;
    VS_FIXEDFILEINFO *f = NULL; UINT n = 0;
    CHECK(VerQueryValueW(b, L"\\", (void **)&f, &n) && f && f->dwSignature == 0xfeef04bd && HIWORD(f->dwFileVersionMS) == 10 &&
          HIWORD(f->dwFileVersionLS) == 19045 && f->dwFileType == 2, "kernel32 fixed info");
    WCHAR *s = NULL;
    CHECK(VerQueryValueW(b, L"\\StringFileInfo\\040904B0\\OriginalFilename", (void **)&s, &n) && s && !_wcsicmp(s, L"kernel32.dll"),
          "kernel32 OriginalFilename %ls", s ? s : L"(none)");
    WORD *tr = NULL;
    CHECK(VerQueryValueW(b, L"\\VarFileInfo\\Translation", (void **)&tr, &n) && n == 4 && tr && tr[0] == 0x409, "kernel32 translation");
    free(b);
    // By name alone, too.
    CHECK(GetFileVersionInfoSizeW(L"kernel32.dll", NULL) > 0, "GetFileVersionInfoSizeW(kernel32.dll) failed, error %lu", GetLastError());
}

static void failures(const char *no_resource) {
    DWORD h = 5;
    SetLastError(0);
    CHECK(GetFileVersionInfoSizeA(no_resource, &h) == 0 && GetLastError() == ERROR_RESOURCE_TYPE_NOT_FOUND && h == 0,
          "file without a version resource: error %lu", GetLastError());
    SetLastError(0);
    CHECK(GetFileVersionInfoSizeW(L"C:\\no\\such\\file.dll", NULL) == 0 && GetLastError() == ERROR_FILE_NOT_FOUND,
          "missing file: error %lu", GetLastError());
}

int main(int argc, char **argv) {
    own_file();
    runtime_dll();
    if (argc > 1) failures(argv[1]);
    printf(g_fail ? "version_info: FAILED\n" : "version_info: own file, a runtime DLL, missing ones -> ok\n");
    return g_fail;
}
