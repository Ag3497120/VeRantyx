// A named event whose name a process that ended still holds, under a process ID now in use by another process
// (tests/name_reuse_check.py leaves such a hold): it is new here, as on Windows (no ERROR_ALREADY_EXISTS).
#include <windows.h>
#include <stdio.h>
int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "vxrt-test made-up stale event";
    HANDLE h = CreateEventA(NULL, TRUE, FALSE, name);
    DWORD e = GetLastError();
    printf("name_reuse: CreateEvent %s, last error %lu\n", h ? "made" : "failed", e);
    if (h) CloseHandle(h);
    return h && e == 0 ? 0 : 1;
}
