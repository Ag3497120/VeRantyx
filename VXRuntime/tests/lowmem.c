// Memory below 2 GB (NtAllocateVirtualMemory with ZeroBits, as LuaJIT 2.0 asks for its heap), used through the
// runtime's functions: strings, formatting, files, VirtualQuery, a pointer a function returns or stores. The ARM64
// runtime has it only in the vCPU (the host cannot map below 4 GB; vx.h): in-process it is skipped.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef LONG (WINAPI *ntavm_fn)(HANDLE, void **, ULONG_PTR, SIZE_T *, ULONG, ULONG);
static int failed;
#define CHECK(x) do { if (!(x)) { printf("check failed (line %d): %s\n", __LINE__, #x); failed++; } } while (0)
int main(void) {
    ntavm_fn ntavm = (ntavm_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtAllocateVirtualMemory");
    void *base = NULL; SIZE_T size = 1 << 20;
    LONG st = ntavm ? ntavm(GetCurrentProcess(), &base, 1, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) : -1;
    if (st || !base) { printf("lowmem: no memory below 2 GB in this mode -> ok (skipped)\n"); return 0; }
    char *p = base;
    CHECK((ULONG_PTR)p < 0x80000000ull && !((ULONG_PTR)p & 0xffff));
    const char *text = "hello low memory";
    for (int i = 0; text[i] || !i; i++) p[i] = text[i];
    p[strlen(text)] = 0;
    CHECK(strlen(p) == 16 && !strcmp(p, text));
    char buf[64];
    _snprintf(buf, sizeof buf, "[%s]", p);                                   // (a pointer among variadic arguments)
    CHECK(!strcmp(buf, "[hello low memory]"));
    CHECK(strchr(p, 'l') == p + 2);                                         // (a pointer it returns)
    strcpy(p + 200, "12345xyz");
    char *end = NULL;
    CHECK(strtol(p + 200, &end, 10) == 12345 && end == p + 205);            // (a pointer it stores)
    char path[MAX_PATH]; GetTempPathA(sizeof path, path); strcat(path, "vxlowmem.bin");
    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD n = 0;
    CHECK(f != INVALID_HANDLE_VALUE && WriteFile(f, p, 17, &n, NULL) && n == 17);
    SetFilePointer(f, 0, NULL, FILE_BEGIN);
    CHECK(ReadFile(f, p + 4096, 17, &n, NULL) && n == 17 && !memcmp(p + 4096, text, 17));
    CloseHandle(f); DeleteFileA(path);
    MEMORY_BASIC_INFORMATION mbi;
    CHECK(VirtualQuery(p + 5000, &mbi, sizeof mbi) && mbi.BaseAddress == p + 4096 && mbi.AllocationBase == p && mbi.State == MEM_COMMIT);
    memset(p + 8192, 'x', 100); memcpy(p + 9000, p + 8192, 100);
    CHECK(p[9000] == 'x' && p[9099] == 'x');
    CHECK(VirtualFree(p, 0, MEM_RELEASE));
    void *again = NULL; size = 65536;
    CHECK(!ntavm(GetCurrentProcess(), &again, 1, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) && again && (ULONG_PTR)again < 0x80000000ull);
    if (again) { ((volatile char *)again)[0] = 1; VirtualFree(again, 0, MEM_RELEASE); }
    printf("lowmem: memory at %p below 2 GB, strings, formatting, files, VirtualQuery: %d failed checks -> %s\n", (void *)p, failed, failed ? "FAILED" : "ok");
    return failed != 0;
}
