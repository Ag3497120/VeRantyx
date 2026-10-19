// A child program's standard output through a pipe: CreateProcess with STARTF_USESTDHANDLES hands the child the
// pipe's write end as its stdout, and the parent reads what it wrote. The child is this program run with "child".
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "child")) { printf("hello from the child\n"); fflush(stdout); return 7; }
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, sizeof path);
    HANDLE rd, wr;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    if (!CreatePipe(&rd, &wr, &sa, 0)) { printf("child_stdout: CreatePipe failed -> FAILED\n"); return 1; }
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    char cmd[MAX_PATH + 16];
    snprintf(cmd, sizeof cmd, "\"%s\" child", path);
    if (!CreateProcessA(path, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) { printf("child_stdout: CreateProcess failed -> FAILED\n"); return 1; }
    CloseHandle(wr);                                    // (the parent's copy: the reader sees the end only when the child's is closed too)
    char buf[256];
    DWORD n, total = 0;
    while (ReadFile(rd, buf + total, (DWORD)(sizeof buf - 1 - total), &n, NULL) && n) total += n;
    buf[total] = 0;
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    int ok = !strcmp(buf, "hello from the child\r\n") || !strcmp(buf, "hello from the child\n");
    printf("child_stdout: read %lu bytes \"%s\", exit code %lu -> %s\n", (unsigned long)total, ok ? "hello from the child" : buf, (unsigned long)code, ok && code == 7 ? "ok" : "FAILED");
    return ok && code == 7 ? 0 : 1;
}
