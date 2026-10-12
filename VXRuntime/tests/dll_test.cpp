// Milestone 3b: real PE DLLs. An import-time DLL (vxdll.dll) and a run-time one (vxplugin.dll).
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
extern "C" __declspec(dllimport) int vxdll_add(int, int);
extern "C" __declspec(dllimport) int vxdll_attaches(void);
extern "C" __declspec(dllimport) void vxdll_throw(int);
static int failures;
#define CHECK(cond) do { if (!(cond)) { std::printf("check failed (line %d): %s\n", __LINE__, #cond); failures++; } } while (0)
static DWORD WINAPI nothing(void *) { return 0; }
int main() {
    CHECK(vxdll_add(2, 3) == 5);
    HANDLE t = CreateThread(nullptr, 0, nothing, nullptr, 0, nullptr);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CHECK(vxdll_attaches() == 101);                    // one process attach, one thread attach
    bool caught = false;
    try { vxdll_throw(9); } catch (const std::exception &e) { caught = !std::strcmp(e.what(), "from the dll 9"); }
    CHECK(caught);

    HMODULE dll = GetModuleHandleA("vxdll.dll");
    CHECK(dll && dll == LoadLibraryA("VXDLL"));
    CHECK((void *)GetProcAddress(dll, "vxdll_add") == (void *)vxdll_add);
    HMODULE plugin = LoadLibraryA("vxplugin.dll");
    CHECK(plugin != nullptr);
    auto by_name = (int (*)(void))GetProcAddress(plugin, "plugin_value");
    auto by_ordinal = (int (*)(void))GetProcAddress(plugin, MAKEINTRESOURCEA(7));
    CHECK(by_name && by_name == by_ordinal && by_name() == 7);
    CHECK((void *)GetProcAddress(plugin, "forwarded") == (void *)vxdll_add);
    CHECK(GetProcAddress(plugin, "nothing_here") == nullptr && GetLastError() == ERROR_PROC_NOT_FOUND);
    char path[MAX_PATH];
    GetModuleFileNameA(plugin, path, sizeof path);
    CHECK(std::strstr(path, "\\vxplugin.dll") != nullptr);
    CHECK(GetModuleHandleA("kernel32.dll") != nullptr);
    CHECK(LoadLibraryA("no_such.dll") == nullptr && GetLastError() == ERROR_MOD_NOT_FOUND);
    FreeLibrary(plugin);
    std::printf("dlls: %d failed checks, plugin at %s -> %s\n", failures, path, failures ? "FAILED" : "ok");
    return failures;
}
