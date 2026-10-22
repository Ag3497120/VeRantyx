// Named waitable timers between processes (system.c): A sets "Local\VxTestHeartbeat" due in 2 s, B opens it and sets
// it due in 4 s; A must see it fire at 4 s, as Elite Dangerous's WatchDog64.exe and the game share their heartbeat.
#include <windows.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "push")) {
        HANDLE t = OpenWaitableTimerA(TIMER_ALL_ACCESS, FALSE, "Local\\VxTestHeartbeat");
        if (!t) { printf("B: open failed %lu\n", GetLastError()); return 1; }
        LARGE_INTEGER due; due.QuadPart = -40000000LL;                   // 4 s
        printf("B: opened, set 4 s -> %d\n", SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE));
        return 0;
    }
    HANDLE t = CreateWaitableTimerA(NULL, TRUE, "Local\\VxTestHeartbeat");
    LARGE_INTEGER due; due.QuadPart = -20000000LL;                       // 2 s
    SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE);
    DWORD start = GetTickCount();
    char cmd[MAX_PATH + 16]; GetModuleFileNameA(NULL, cmd, MAX_PATH); strcat(cmd, " push");
    STARTUPINFOA si = { sizeof si }; PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { printf("A: CreateProcess failed\n"); return 1; }
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD r = WaitForSingleObject(t, 10000);
    DWORD took = GetTickCount() - start;
    printf("waitable timer: named, pushed back by another process: fired after %lu ms (wait %lu) -> %s\n", took, r, r == 0 && took >= 3500 && took < 6000 ? "ok" : "FAILED");
    return r == 0 && took >= 3500 && took < 6000 ? 0 : 1;
}
