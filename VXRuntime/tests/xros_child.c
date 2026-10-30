// xros_child.c: CreateProcess from a program that must go on whatever it answers (VISIONOS_PORT.md, "child
// processes"). On the Mac the child (this program with "child") runs and its exit code comes back; on visionOS an app
// may start no processes, so CreateProcess fails with an error and the program carries on. Then a program that does
// not exist (as Cyberpunk's REDEngineErrorReporter.exe would be in a copy without it): ERROR_FILE_NOT_FOUND.
#include <windows.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "child")) return 42;
    char self[MAX_PATH], cmd[MAX_PATH + 16];
    GetModuleFileNameA(NULL, self, sizeof self);
    snprintf(cmd, sizeof cmd, "\"%s\" child", self);
    STARTUPINFOA si = { sizeof si }; PROCESS_INFORMATION pi = { 0 };
    int ok = 1;
    BOOL r = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    DWORD err = r ? 0 : GetLastError(), code = 0;
    if (r) {
        WaitForSingleObject(pi.hProcess, 20000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        printf("xros child: started itself, exit code %lu\n", code);
        ok &= code == 42;
    } else {
        printf("xros child: CreateProcess failed, error %lu (no child processes here)\n", err);
        ok &= err != 0;
    }
    r = CreateProcessA(NULL, "REDEngineErrorReporter_missing.exe -x", NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    err = r ? 0 : GetLastError();
    printf("xros child: missing program: %s, error %lu\n", r ? "started?" : "failed", err);
    ok &= !r && err == ERROR_FILE_NOT_FOUND;
    printf("xros child: went on after both -> %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
