// A process that ends while it holds the lock of a named object's shared state (src/shlock.c). On Windows the kernel
// keeps named objects, so a process ending at any moment never leaves a name unusable; here their state is in shared
// memory, and a process that ended between lock and unlock (TerminateProcess on itself, killed, crashed) left the lock
// taken for good: every later process creating or opening that name hung (Steam's web helper did, 2026-10-25).
//
// "shlock.exe" starts itself as "shlock.exe die" with VXRT_TEST_SHLOCK_DIE naming a made-up event: the child ends
// (exit code 3) inside CreateEventA, holding the lock of that name's holder registry. Then "shlock.exe create" must
// create the same event within 10 s (it spun forever before, so it is a process of its own that can be ended),
// signal it, and start "shlock.exe open", which must open it and see it signaled.
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define NAME "vxrt_shlock_test_event_7c41"
static int failures;
#define CHECK(c) do { if (!(c)) { printf("shlock: FAILED %s (line %d, last error %lu)\n", #c, __LINE__, GetLastError()); failures++; } } while (0)

// Runs this program with `arg`; its exit code, 0xFFFFFFFE when it did not end within `ms` (then it is ended).
static DWORD run_self(const char *arg, DWORD ms) {
    char self[MAX_PATH], cmd[MAX_PATH + 16];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    snprintf(cmd, sizeof cmd, "\"%s\" %s", self, arg);
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 0xFFFFFFFF;
    DWORD code = 0xFFFFFFFE;
    if (WaitForSingleObject(pi.hProcess, ms) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 9);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "die")) {        // ends inside, holding the registry's lock (exit code 3)
        HANDLE e = CreateEventA(NULL, TRUE, FALSE, NAME);
        printf("shlock die: CreateEventA returned %p (the test hook did not end it)\n", e);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "create")) {
        HANDLE e = CreateEventA(NULL, TRUE, FALSE, NAME);
        if (!e) return 20;
        if (!SetEvent(e)) return 21;
        DWORD opened = run_self("open", 10000);
        CloseHandle(e);
        return opened == 0 ? 0 : 22;
    }
    if (argc > 1 && !strcmp(argv[1], "open")) {
        HANDLE e = OpenEventA(SYNCHRONIZE, FALSE, NAME);
        if (!e) return 10;
        DWORD w = WaitForSingleObject(e, 0);
        CloseHandle(e);
        return w == WAIT_OBJECT_0 ? 0 : 11;
    }

    SetEnvironmentVariableA("VXRT_TEST_SHLOCK_DIE", NAME);
    DWORD code = run_self("die", 20000);
    SetEnvironmentVariableA("VXRT_TEST_SHLOCK_DIE", NULL);
    CHECK(code == 3);                                  // (else the lock was never left taken: the test shows nothing)
    DWORD t0 = GetTickCount();
    code = run_self("create", 10000);
    DWORD ms = GetTickCount() - t0;
    if (code == 0xFFFFFFFE) printf("shlock: CreateEventA still waiting for the lock after 10 s\n");
    CHECK(code == 0);
    printf("shlock: a name whose lock a process ended holding: created again, signaled and opened by another process"
           " (%lu ms) -> %s\n", ms, failures ? "FAILED" : "ok");
    return failures != 0;
}
