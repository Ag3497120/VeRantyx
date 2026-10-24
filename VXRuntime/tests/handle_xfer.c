// DuplicateHandle between processes in both directions, as Steam's process pipes use it (src/handlexfer.c): the
// child (this program run with "child <parent pid> <pipe write end> <event>") takes the parent's pipe end and event
// out of the parent (source process: the parent), and gives the parent the read end of a pipe of its own (target
// process: the parent). Then the parent reads what the child wrote into both pipes and sees the event set.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s (last error %lu)\n", __LINE__, #c, GetLastError()); failures++; } } while (0)

static int child(DWORD parent_pid, HANDLE parent_wr, HANDLE parent_event) {
    HANDLE parent = OpenProcess(PROCESS_DUP_HANDLE, FALSE, parent_pid);
    if (!parent) { printf("child: OpenProcess failed\n"); return 2; }
    HANDLE wr = NULL, ev = NULL, bogus = NULL;
    if (!DuplicateHandle(parent, parent_wr, GetCurrentProcess(), &wr, 0, FALSE, DUPLICATE_SAME_ACCESS)) { printf("child: pipe end not taken (%lu)\n", GetLastError()); return 3; }
    if (!DuplicateHandle(parent, parent_event, GetCurrentProcess(), &ev, 0, FALSE, DUPLICATE_SAME_ACCESS)) { printf("child: event not taken (%lu)\n", GetLastError()); return 4; }
    if (DuplicateHandle(parent, (HANDLE)(ULONG_PTR)0x7ff8, GetCurrentProcess(), &bogus, 0, FALSE, DUPLICATE_SAME_ACCESS)) { printf("child: a handle the parent does not have was taken\n"); return 5; }
    HANDLE rd2, wr2;
    if (!CreatePipe(&rd2, &wr2, NULL, 0)) return 6;
    HANDLE rd2_there = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), rd2, parent, &rd2_there, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) { printf("child: read end not given (%lu)\n", GetLastError()); return 7; }
    char line[64];
    int n = snprintf(line, sizeof line, "%llx\n", (unsigned long long)(ULONG_PTR)rd2_there);
    DWORD done;
    if (!WriteFile(wr, line, (DWORD)n, &done, NULL) || done != (DWORD)n) return 8;
    if (!WriteFile(wr2, "pong", 4, &done, NULL) || done != 4) return 9;
    CloseHandle(wr2);
    CloseHandle(wr);
    if (!SetEvent(ev)) return 10;
    CloseHandle(ev);
    CloseHandle(parent);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[1], "child"))
        return child((DWORD)strtoul(argv[2], NULL, 10), (HANDLE)(ULONG_PTR)strtoull(argv[3], NULL, 16), (HANDLE)(ULONG_PTR)strtoull(argv[4], NULL, 16));
    HANDLE rd, wr;
    CHECK(CreatePipe(&rd, &wr, NULL, 0));
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    CHECK(ev != NULL);
    char path[MAX_PATH], cmd[MAX_PATH + 96];
    GetModuleFileNameA(NULL, path, sizeof path);
    snprintf(cmd, sizeof cmd, "\"%s\" child %lu %llx %llx", path, GetCurrentProcessId(), (unsigned long long)(ULONG_PTR)wr, (unsigned long long)(ULONG_PTR)ev);
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    CHECK(CreateProcessA(path, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi));
    DWORD code = 99;
    CHECK(WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code));
    if (code) printf("child exit code %lu\n", code);
    CHECK(code == 0);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0);             // set by the child through its copy
    CloseHandle(wr);
    char buf[64] = { 0 };
    DWORD n = 0, total = 0;
    while (total < sizeof buf - 1 && ReadFile(rd, buf + total, sizeof buf - 1 - total, &n, NULL) && n) total += n;
    HANDLE rd2 = (HANDLE)(ULONG_PTR)strtoull(buf, NULL, 16);
    CHECK(total > 1 && rd2 != NULL);
    char pong[8] = { 0 };
    total = 0;
    while (rd2 && total < 4 && ReadFile(rd2, pong + total, 4 - total, &n, NULL) && n) total += n;
    CHECK(total == 4 && !memcmp(pong, "pong", 4));
    if (rd2) CHECK(CloseHandle(rd2));
    CloseHandle(rd); CloseHandle(ev); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    printf("handle_xfer: handles taken from and given to another process: %d failed checks -> %s\n", failures, failures ? "FAILED" : "ok");
    return failures != 0;
}
