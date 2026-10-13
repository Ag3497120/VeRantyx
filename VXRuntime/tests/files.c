// Milestone 3a: files, directories, paths (C:\ is VXRT_ROOT/drive_c) and the registry.
#include <windows.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("check failed (line %d): %s\n", __LINE__, #cond); failures++; } } while (0)

int main(void) {
    char buf[MAX_PATH];
    DWORD n;
    CreateDirectoryA("C:\\vxtest", NULL);
    CHECK(GetFileAttributesA("C:\\vxtest") & FILE_ATTRIBUTE_DIRECTORY);

    // Win32 files: write, reopen through a differently cased wide path, seek, read.
    HANDLE h = CreateFileA("C:\\vxtest\\a.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(WriteFile(h, "hello file", 10, &n, NULL) && n == 10);
    CloseHandle(h);
    h = CreateFileW(L"c:\\VXTEST\\A.TXT", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    LARGE_INTEGER size; CHECK(GetFileSizeEx(h, &size) && size.QuadPart == 10);
    CHECK(SetFilePointer(h, 6, NULL, FILE_BEGIN) == 6);
    CHECK(ReadFile(h, buf, 4, &n, NULL) && n == 4);
    buf[n] = 0;
    CHECK(!strcmp(buf, "file"));
    CloseHandle(h);
    CHECK(CreateFileA("C:\\vxtest\\missing.txt", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE);
    CHECK(GetLastError() == ERROR_FILE_NOT_FOUND);
    CHECK(GetFileAttributesA("C:\\nowhere\\x.txt") == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_PATH_NOT_FOUND);

    // The C runtime on the same files, with forward slashes.
    FILE *f = fopen("C:/vxtest/b.txt", "w");
    CHECK(f != NULL);
    fprintf(f, "line %d\n", 1);
    fputs("line 2\n", f);
    fclose(f);
    f = fopen("C:\\vxtest\\b.txt", "r");
    char l1[32] = "", l2[32] = "";
    fgets(l1, sizeof l1, f); fgets(l2, sizeof l2, f);
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    fclose(f);
    CHECK(!strcmp(l1, "line 1\n") && !strcmp(l2, "line 2\n") && end == 14);
    CHECK(MoveFileExA("C:\\vxtest\\b.txt", "C:\\vxtest\\c.txt", 0));

    // Directory listing with a wildcard.
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA("C:\\vxtest\\*.TXT", &fd);
    int found = 0;
    if (fh != INVALID_HANDLE_VALUE) {
        do found += !strcmp(fd.cFileName, "a.txt") * 1 + !strcmp(fd.cFileName, "c.txt") * 2; while (FindNextFileA(fh, &fd));
        CHECK(GetLastError() == ERROR_NO_MORE_FILES);
        FindClose(fh);
    }
    CHECK(found == 3);

    // Current directory and full paths.
    CHECK(SetCurrentDirectoryA("C:\\vxtest"));
    GetCurrentDirectoryA(sizeof buf, buf);
    CHECK(!strcmp(buf, "C:\\vxtest"));
    GetFullPathNameA("sub\\..\\.\\x.txt", sizeof buf, buf, NULL);
    CHECK(!strcmp(buf, "C:\\vxtest\\x.txt"));
    CHECK(GetFileAttributesA("a.txt") != INVALID_FILE_ATTRIBUTES);      // relative to the current directory
    // Shared memory by name: two mappings of the same name see the same bytes.
    HANDLE m1 = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 65536, "Local\\vxtest-shared");
    HANDLE m2 = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 65536, "Local\\vxtest-shared");
    CHECK(m1 && m2 && GetLastError() == ERROR_ALREADY_EXISTS);
    char *v1 = MapViewOfFile(m1, FILE_MAP_ALL_ACCESS, 0, 0, 0), *v2 = MapViewOfFile(m2, FILE_MAP_READ, 0, 0, 4096);
    CHECK(v1 && v2);
    if (v1 && v2) { strcpy(v1, "shared"); CHECK(!strcmp(v2, "shared")); }
    CHECK(UnmapViewOfFile(v1) && UnmapViewOfFile(v2));
    CloseHandle(m1); CloseHandle(m2);
    // A file mapping: the view shows the file's bytes.
    h = CreateFileA("C:\\vxtest\\a.txt", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE fm = CreateFileMappingA(h, NULL, PAGE_READONLY, 0, 0, NULL);
    const char *fv = fm ? MapViewOfFile(fm, FILE_MAP_READ, 0, 0, 0) : NULL;
    CHECK(fv && !memcmp(fv, "hello file", 10));
    if (fv) UnmapViewOfFile((void *)fv);
    CloseHandle(fm); CloseHandle(h);
    SetCurrentDirectoryA("C:\\");
    CHECK(DeleteFileA("C:\\vxtest\\a.txt") && DeleteFileA("C:\\vxtest\\c.txt") && RemoveDirectoryA("C:\\vxtest"));

    // Registry: create, write, reopen with other casing, read in both A and W forms, enumerate, delete.
    HKEY k;
    CHECK(RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\VeRantyx\\Test", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL) == ERROR_SUCCESS);
    DWORD v = 1234;
    RegSetValueExA(k, "Number", 0, REG_DWORD, (BYTE *)&v, 4);
    RegSetValueExW(k, L"Name", 0, REG_SZ, (const BYTE *)L"VXRuntime", 20);
    RegCloseKey(k);
    CHECK(RegOpenKeyExA(HKEY_CURRENT_USER, "software\\verantyx\\TEST", 0, KEY_READ, &k) == ERROR_SUCCESS);
    DWORD type, got = 0, cb = sizeof got;
    CHECK(RegQueryValueExA(k, "number", NULL, &type, (BYTE *)&got, &cb) == ERROR_SUCCESS && type == REG_DWORD && got == 1234);
    char name[32]; cb = sizeof name;
    CHECK(RegQueryValueExA(k, "Name", NULL, &type, (BYTE *)name, &cb) == ERROR_SUCCESS && type == REG_SZ && !strcmp(name, "VXRuntime"));
    WCHAR wname[32]; cb = sizeof wname;
    CHECK(RegGetValueW(HKEY_CURRENT_USER, L"Software\\VeRantyx\\Test", L"Name", RRF_RT_REG_SZ, NULL, wname, &cb) == ERROR_SUCCESS && !wcscmp(wname, L"VXRuntime"));
    cb = 2;
    CHECK(RegQueryValueExA(k, "Name", NULL, NULL, (BYTE *)name, &cb) == ERROR_MORE_DATA && cb == 10);
    CHECK(RegQueryValueExA(k, "Missing", NULL, NULL, NULL, NULL) == ERROR_FILE_NOT_FOUND);
    DWORD nvalues = 0; RegQueryInfoKeyA(k, NULL, NULL, NULL, NULL, NULL, NULL, &nvalues, NULL, NULL, NULL, NULL);
    CHECK(nvalues == 2);
    RegCloseKey(k);
    HKEY sw; char sub[64]; DWORD cch = sizeof sub; int saw = 0;
    RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\VeRantyx", 0, KEY_READ, &sw);
    for (DWORD i = 0; cch = sizeof sub, RegEnumKeyExA(sw, i, sub, &cch, NULL, NULL, NULL, NULL) == ERROR_SUCCESS; i++) saw |= !strcmp(sub, "Test");
    RegCloseKey(sw);
    CHECK(saw);
    CHECK(RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\VeRantyx\\Test") == ERROR_SUCCESS);
    CHECK(RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\VeRantyx") == ERROR_SUCCESS);
    CHECK(RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\VeRantyx", 0, KEY_READ, &k) == ERROR_FILE_NOT_FOUND);
    char product[64]; cb = sizeof product;
    CHECK(RegGetValueA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows NT\\CurrentVersion", "ProductName", RRF_RT_REG_SZ, NULL, product, &cb) == ERROR_SUCCESS);

    // ReadProcessMemory: this process's memory is copied; an unmapped address fails instead of crashing.
    {
        static const char src[] = "process memory";
        char dst[16] = { 0 }; SIZE_T got = 0;
        CHECK(ReadProcessMemory(GetCurrentProcess(), src, dst, sizeof src, &got) && got == sizeof src && !strcmp(dst, src));
        CHECK(!ReadProcessMemory(GetCurrentProcess(), (void *)0x10, dst, 4, &got) && GetLastError() == ERROR_PARTIAL_COPY);
    }

    // Virtual memory: an aligned reservation the way allocators make one (reserve more, release, reserve at the
    // aligned address inside), many small blocks, commits inside a reservation.
    {
        SIZE_T size = 8 << 20, align = 4 << 20;
        for (int i = 0; i < 3; i++) {
            char *big = VirtualAlloc(NULL, size + align, MEM_RESERVE, PAGE_READWRITE);
            CHECK(big != NULL);
            char *aligned = (char *)(((ULONG_PTR)big + align - 1) & ~(ULONG_PTR)(align - 1));
            CHECK(VirtualFree(big, 0, MEM_RELEASE));
            char *p = VirtualAlloc(aligned, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            CHECK(p == aligned);
            if (p) { p[size - 1] = 1; CHECK(VirtualFree(p, 0, MEM_RELEASE)); }
        }
        static char *small[2000];
        for (int i = 0; i < 2000; i++) {
            small[i] = VirtualAlloc(NULL, 0x1000 * (1 + i % 7), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            CHECK(small[i] && !((ULONG_PTR)small[i] & 0xffff));
            if (small[i]) small[i][0] = (char)i;
        }
        CHECK(VirtualAlloc(small[5] + 0x2000, 0x1000, MEM_RESERVE, PAGE_READWRITE) == NULL);   // inside another one
        for (int i = 0; i < 2000; i++) if (small[i]) { CHECK(small[i][0] == (char)i); VirtualFree(small[i], 0, MEM_RELEASE); }
        char *r = VirtualAlloc(NULL, 0x30000, MEM_RESERVE, PAGE_NOACCESS);
        CHECK(r && VirtualAlloc(r + 0x11000, 0x1000, MEM_COMMIT, PAGE_READWRITE) == r + 0x11000);
        MEMORY_BASIC_INFORMATION mbi;
        CHECK(VirtualQuery(r + 0x11000, &mbi, sizeof mbi) && mbi.AllocationBase == r && mbi.Protect == PAGE_READWRITE);
        CHECK(VirtualFree(r, 0, MEM_RELEASE));
    }
    printf("files and registry: %d failed checks, windows reports \"%s\" -> %s\n", failures, product, failures ? "FAILED" : "ok");
    return failures;
}
