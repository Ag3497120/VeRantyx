// version.dll (src/version.c): a file's version resource and VerQueryValue over it.
// - this program's own file (tests/version_info.rc, made-up data): VS_FIXEDFILEINFO, the translation, strings (W and A,
//   sub-block names in any case);
// - kernel32.dll where GetModuleFileName puts it (one of the runtime's own DLLs, no file): a block for 10.0.19045;
// - a file with no version resource, and no file: 0 with ERROR_RESOURCE_TYPE_NOT_FOUND / ERROR_FILE_NOT_FOUND;
// - a buffer smaller than the size asked: the start of the block;
// - a made-up block whose strings' wValueLength says more than the block holds (a byte count, as some resource tools
//   write, and a far too large one): VerQueryValueA/W give what the block holds and write nothing outside the buffer.
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

// Building a version block by hand (Microsoft's VS_VERSIONINFO layout: wLength, wValueLength, wType, the key in UTF-16
// with its NUL, padding to 32 bits, the value, padding, the children).
typedef struct { unsigned char *b; size_t n; } maker;
static void mk_pad(maker *m) { while (m->n & 3) m->b[m->n++] = 0; }
static size_t mk_begin(maker *m, const WCHAR *key, WORD value_length, WORD type, const void *value, size_t value_bytes) {
    mk_pad(m);
    size_t at = m->n;
    WORD header[3] = { 0, value_length, type };
    memcpy(m->b + at, header, 6);
    m->n += 6;
    size_t key_bytes = 2 * (wcslen(key) + 1);
    memcpy(m->b + m->n, key, key_bytes);
    m->n += key_bytes;
    mk_pad(m);
    if (value_bytes) memcpy(m->b + m->n, value, value_bytes);
    m->n += value_bytes;
    return at;
}
static void mk_end(maker *m, size_t at) { WORD length = (WORD)(m->n - at); memcpy(m->b + at, &length, 2); }

static void wrong_value_length(void) {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    DWORD size = GetFileVersionInfoSizeW(path, NULL);
    CHECK(size > 0, "GetFileVersionInfoSizeW(own file) = 0, error %lu", GetLastError());
    if (!size) return;
    // A buffer of the size asked for, then guard bytes; the version information of this program is replaced by the
    // made-up block below, of the same root length (the buffer's room after the block stays as GetFileVersionInfo
    // left it).
    enum { GUARD = 128 * 1024 };
    unsigned char *b = malloc(size + GUARD);
    if (!b) return;
    memset(b, 0xcc, size + GUARD);
    CHECK(GetFileVersionInfoW(path, 0, size, b), "GetFileVersionInfoW(own file) failed, error %lu", GetLastError());
    WORD root_length;
    memcpy(&root_length, b, 2);
    CHECK(root_length >= 256 && root_length <= size, "own root block of %u bytes", root_length);
    if (root_length < 256 || root_length > size) { free(b); return; }
    memset(b, 0, root_length);
    maker m = { b, 0 };
    size_t root = mk_begin(&m, L"VS_VERSION_INFO", 0, 0, NULL, 0);
    size_t sfi = mk_begin(&m, L"StringFileInfo", 0, 1, NULL, 0);
    size_t table = mk_begin(&m, L"040904B0", 0, 1, NULL, 0);
    static const WCHAR made_up[] = L"made-up", short_one[] = L"short";
    mk_end(&m, mk_begin(&m, L"Comments", sizeof made_up, 1, made_up, sizeof made_up));      // bytes, not characters
    mk_end(&m, mk_begin(&m, L"Hostile", 0x4000, 1, short_one, sizeof short_one));           // far more than the block
    mk_end(&m, table);
    mk_end(&m, sfi);
    WORD keep = root_length;                                       // (the rest of the root block: zeros)
    memcpy(b + root, &keep, 2);

    char *a = NULL; WCHAR *w = NULL; UINT n = 0;
    CHECK(VerQueryValueA(b, "\\StringFileInfo\\040904B0\\Comments", (void **)&a, &n) && a && !strcmp(a, "made-up") && n == 8,
          "Comments (A, byte count) %s (%u)", a ? a : "(none)", n);
    CHECK(!a || ((unsigned char *)a >= b && (unsigned char *)a + n <= b + size), "Comments (A) outside the buffer");
    a = NULL; n = 0;
    CHECK(VerQueryValueA(b, "\\StringFileInfo\\040904B0\\Hostile", (void **)&a, &n) && a && !strcmp(a, "short") && n == 6,
          "Hostile (A) %s (%u)", a ? a : "(none)", n);
    CHECK(!a || ((unsigned char *)a >= b && (unsigned char *)a + n <= b + size), "Hostile (A) outside the buffer");
    CHECK(VerQueryValueW(b, L"\\StringFileInfo\\040904B0\\Hostile", (void **)&w, &n) && w && !wcscmp(w, L"short") && n == 6,
          "Hostile (W) %ls (%u)", w ? w : L"(none)", n);
    CHECK(VerQueryValueW(b, L"\\StringFileInfo\\040904B0\\Comments", (void **)&w, &n) && w && !wcscmp(w, L"made-up") && n == 8,
          "Comments (W, byte count) %ls (%u)", w ? w : L"(none)", n);
    size_t touched = 0;
    for (size_t i = size; i < size + GUARD; i++) touched += b[i] != 0xcc;
    CHECK(!touched, "%lu bytes written after the buffer GetFileVersionInfoSize asked for", (unsigned long)touched);
    free(b);
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
    wrong_value_length();
    if (argc > 1) failures(argv[1]);
    printf(g_fail ? "version_info: FAILED\n" : "version_info: own file, a runtime DLL, missing ones -> ok\n");
    return g_fail;
}
