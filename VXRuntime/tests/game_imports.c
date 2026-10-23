// Functions games look up by name (GetProcAddress) before their own code runs: the Steam DRM stub of No Man's Sky
// takes msvcrt.dll!vsprintf, and its libraries register classic (WPP) event-trace providers.
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)

typedef int (__cdecl *vsprintf_fn)(char *, const char *, va_list);
static vsprintf_fn msvcrt_vsprintf;
static int call_vsprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = msvcrt_vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

typedef struct { const GUID *guid; HANDLE handle; } guid_registration;
typedef ULONG (WINAPI *register_fn)(void *, void *, const GUID *, ULONG, guid_registration *, const WCHAR *, const WCHAR *, ULONG64 *);
typedef ULONG (WINAPI *unregister_fn)(ULONG64);
typedef UCHAR (WINAPI *level_fn)(ULONG64);

int main(void) {
    msvcrt_vsprintf = (vsprintf_fn)GetProcAddress(LoadLibraryA("msvcrt.dll"), "vsprintf");
    CHECK(msvcrt_vsprintf != NULL);
    if (msvcrt_vsprintf) {
        char buf[64];
        CHECK(call_vsprintf(buf, "%s:%08X:%d", "load", 0x10016u, -7) == 16 && !strcmp(buf, "load:00010016:-7"));
    }

    HMODULE advapi = LoadLibraryA("advapi32.dll");
    register_fn reg = (register_fn)GetProcAddress(advapi, "RegisterTraceGuidsW");
    unregister_fn unreg = (unregister_fn)GetProcAddress(advapi, "UnregisterTraceGuids");
    level_fn level = (level_fn)GetProcAddress(advapi, "GetTraceEnableLevel");
    CHECK(reg && unreg && level);
    if (reg && unreg && level) {
        static const GUID control = { 0x12345678, 0x1234, 0x5678, { 1, 2, 3, 4, 5, 6, 7, 8 } };
        guid_registration regs[2] = { { &control, NULL }, { &control, NULL } };
        ULONG64 handle = 0;
        CHECK(reg(NULL, NULL, &control, 2, regs, NULL, NULL, &handle) == ERROR_SUCCESS);
        CHECK(handle != 0 && regs[0].handle != NULL && regs[1].handle != NULL);
        CHECK(level(handle) == 0);                                   // no session enables the provider
        CHECK(unreg(handle) == ERROR_SUCCESS);
    }
    printf("game imports: msvcrt vsprintf, classic event tracing -> %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
