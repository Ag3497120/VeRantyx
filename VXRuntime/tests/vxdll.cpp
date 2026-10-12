// A DLL for milestone 3b: exports, DllMain notifications, and a C++ exception thrown across the DLL boundary.
#include <windows.h>
#include <stdexcept>
#include <string>
static int process_attaches, thread_attaches;
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, void *) {
    if (reason == DLL_PROCESS_ATTACH) process_attaches++;
    if (reason == DLL_THREAD_ATTACH) thread_attaches++;
    return TRUE;
}
extern "C" __declspec(dllexport) int vxdll_add(int a, int b) { return a + b; }
extern "C" __declspec(dllexport) int vxdll_attaches(void) { return process_attaches * 100 + thread_attaches; }
extern "C" __declspec(dllexport) void vxdll_throw(int n) { throw std::runtime_error("from the dll " + std::to_string(n)); }
