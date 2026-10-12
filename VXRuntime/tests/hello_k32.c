// The smallest Windows program: kernel32 only, no C runtime.
#include <windows.h>
void __stdcall start(void) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    static const char msg[] = "hello from a Windows program, kernel32 only\n";
    DWORD written = 0;
    WriteFile(out, msg, sizeof msg - 1, &written, NULL);
    ExitProcess(written == sizeof msg - 1 ? 0 : 1);
}
