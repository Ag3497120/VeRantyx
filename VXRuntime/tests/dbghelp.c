// dbghelp (src/dbghelp.c): StackWalk64 through this program's own x64 frames (its unwind data), symbols from its
// export table, modules and function table entries, the search path, and the failures without PDBs.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s (last error %lu)\n", __LINE__, #c, GetLastError()); failures++; } } while (0)

static DWORD64 g_pc[64], g_ret[64];
static int g_frames;
static CONTEXT g_start;

// Four exported frames: vxrt_level3 walks the stack from where it is.
__declspec(dllexport) __attribute__((noinline)) int vxrt_level3(int x) {
    CONTEXT c;
    RtlCaptureContext(&c);
    g_start = c;
    STACKFRAME64 f;
    memset(&f, 0, sizeof f);
    f.AddrPC.Offset = c.Rip; f.AddrPC.Mode = AddrModeFlat;
    f.AddrFrame.Offset = c.Rbp; f.AddrFrame.Mode = AddrModeFlat;
    f.AddrStack.Offset = c.Rsp; f.AddrStack.Mode = AddrModeFlat;
    g_frames = 0;
    while (g_frames < 64 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &f, &c, NULL,
                                        SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
        g_pc[g_frames] = f.AddrPC.Offset; g_ret[g_frames] = f.AddrReturn.Offset;
        if (f.AddrPC.Mode != AddrModeFlat || !f.AddrStack.Offset) failures++;
        g_frames++;
    }
    return x + g_frames;
}
__declspec(dllexport) __attribute__((noinline)) int vxrt_level2(int x) { volatile int y = x * 3; return vxrt_level3(y) + 1; }
__declspec(dllexport) __attribute__((noinline)) int vxrt_level1(int x) { volatile int y = x + 7; return vxrt_level2(y) + 2; }
__declspec(dllexport) __attribute__((noinline)) int vxrt_level0(int x) { volatile int y = x ^ 5; return vxrt_level1(y) + 3; }

static void check_symbol(DWORD64 address, const char *name, int inside) {
    union { SYMBOL_INFO info; char space[sizeof(SYMBOL_INFO) + 256]; } s;
    memset(&s, 0, sizeof s);
    s.info.SizeOfStruct = sizeof(SYMBOL_INFO);
    s.info.MaxNameLen = 256;
    DWORD64 displacement = ~0ull;
    if (!SymFromAddr(GetCurrentProcess(), address, &displacement, &s.info)) { printf("no symbol at %#llx (%s)\n", (unsigned long long)address, name); failures++; return; }
    if (strcmp(s.info.Name, name) || s.info.NameLen != strlen(name) || s.info.Address + displacement != address ||
        (inside && !displacement) || !(s.info.Flags & SYMFLAG_EXPORT) || s.info.ModBase != (DWORD64)(ULONG_PTR)GetModuleHandleW(NULL)) {
        printf("symbol at %#llx: %s+%#llx (wanted %s)\n", (unsigned long long)address, s.info.Name, (unsigned long long)displacement, name);
        failures++;
    }
}

int main(void) {
    HANDLE me = GetCurrentProcess();
    union { SYMBOL_INFO info; char space[sizeof(SYMBOL_INFO) + 64]; } s;
    memset(&s, 0, sizeof s); s.info.SizeOfStruct = sizeof(SYMBOL_INFO); s.info.MaxNameLen = 64;
    SetLastError(0);
    CHECK(!SymFromAddr(me, (DWORD64)(ULONG_PTR)vxrt_level1, NULL, &s.info) && GetLastError() == ERROR_INVALID_HANDLE);   // before SymInitialize
    DWORD options = SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    CHECK(options == (SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES) && SymGetOptions() == options);
    CHECK(SymInitialize(me, NULL, TRUE));
    SetLastError(0);
    CHECK(!SymInitialize(me, NULL, TRUE) && GetLastError() == ERROR_INVALID_PARAMETER);   // once per process handle

    // the search path: the current directory first by default; set and read back; too small a buffer
    WCHAR path[1024];
    CHECK(SymGetSearchPathW(me, path, 1024) && path[0] == '.');
    CHECK(SymSetSearchPathW(me, L"C:\\made-up\\symbols;."));
    CHECK(SymGetSearchPathW(me, path, 1024) && !wcscmp(path, L"C:\\made-up\\symbols;."));
    SetLastError(0);
    CHECK(!SymGetSearchPathW(me, path, 5) && GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    CHECK(SymSetSearchPathW(me, NULL) && SymGetSearchPathW(me, path, 1024) && path[0] == '.');

    // the walk: level3 (where it started), level2, level1, level0, main, then the C runtime's start-up, and an end
    int r = vxrt_level0(1);
    CHECK(r > 0);
    CHECK(g_frames >= 6 && g_frames < 64);
    if (g_frames >= 5) {
        check_symbol(g_pc[0], "vxrt_level3", 1);
        check_symbol(g_pc[1], "vxrt_level2", 1);
        check_symbol(g_pc[2], "vxrt_level1", 1);
        check_symbol(g_pc[3], "vxrt_level0", 1);
        CHECK(g_pc[0] == g_start.Rip);
        CHECK(g_pc[4] > (DWORD64)(ULONG_PTR)main && g_pc[4] < (DWORD64)(ULONG_PTR)main + 0x1000);   // a return address in main
    }
    for (int i = 0; i + 1 < g_frames; i++) if (g_ret[i] != g_pc[i + 1]) { printf("frame %d returns to %#llx, frame %d is at %#llx\n", i, (unsigned long long)g_ret[i], i + 1, (unsigned long long)g_pc[i + 1]); failures++; }
    check_symbol((DWORD64)(ULONG_PTR)vxrt_level2, "vxrt_level2", 0);   // an export's own address: displacement 0

    // modules and function table entries
    DWORD64 base = (DWORD64)(ULONG_PTR)GetModuleHandleW(NULL);
    CHECK(SymGetModuleBase64(me, g_pc[1]) == base);
    CHECK(SymGetModuleBase64(me, (DWORD64)(ULONG_PTR)&s) == 0);              // the stack is in no module
    PRUNTIME_FUNCTION fn = SymFunctionTableAccess64(me, g_pc[1]);
    CHECK(fn && fn->BeginAddress <= g_pc[1] - base && g_pc[1] - base < fn->EndAddress);
    CHECK(fn && fn->BeginAddress == (DWORD64)(ULONG_PTR)vxrt_level2 - base);
    void *heap = malloc(64);
    SetLastError(0);
    CHECK(!SymFromAddr(me, (DWORD64)(ULONG_PTR)heap, NULL, &s.info));
    free(heap);
    IMAGEHLP_LINE64 line; memset(&line, 0, sizeof line); line.SizeOfStruct = sizeof line;
    DWORD column = 0;
    SetLastError(0);
    CHECK(!SymGetLineFromAddr64(me, g_pc[1], &column, &line) && GetLastError() == ERROR_INVALID_ADDRESS);   // no PDBs

    // bad input: another machine, no context; a stack pointer that is not memory ends the walk after its first frame
    STACKFRAME64 f; memset(&f, 0, sizeof f);
    CONTEXT c = g_start;
    CHECK(!StackWalk64(IMAGE_FILE_MACHINE_I386, me, GetCurrentThread(), &f, &c, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL));
    CHECK(!StackWalk64(IMAGE_FILE_MACHINE_AMD64, me, GetCurrentThread(), &f, NULL, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL));
    c.Rsp = 0x10;
    memset(&f, 0, sizeof f);
    CHECK(StackWalk64(IMAGE_FILE_MACHINE_AMD64, me, GetCurrentThread(), &f, &c, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL));
    CHECK(f.AddrPC.Offset == g_start.Rip && f.AddrReturn.Offset == 0);
    CHECK(!StackWalk64(IMAGE_FILE_MACHINE_AMD64, me, GetCurrentThread(), &f, &c, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL));

    CHECK(SymCleanup(me));
    SetLastError(0);
    CHECK(!SymCleanup(me) && GetLastError() == ERROR_INVALID_HANDLE);
    printf("dbghelp: %d frames walked; symbols, modules, function tables, search path: %d failed checks -> %s\n", g_frames, failures, failures ? "FAILED" : "ok");
    return failures;
}
