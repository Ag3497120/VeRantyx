// Finding another program by its image name, as SteamVR's client looks for vrserver.exe: the process list has the
// other programs vxrt runs, and their image names are those programs'. The child is this test started again; it
// waits for a made-up event before it ends.
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("check failed (line %d): %s\n", __LINE__, #cond); failures++; } } while (0)

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "-child")) {
        HANDLE done = OpenEventA(SYNCHRONIZE, FALSE, "vxrt_process_list_done");
        return done && WaitForSingleObject(done, 20000) == WAIT_OBJECT_0 ? 0 : 1;
    }
    HANDLE done = CreateEventA(NULL, TRUE, FALSE, "vxrt_process_list_done");
    char self[MAX_PATH], cmd[MAX_PATH + 16];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    snprintf(cmd, sizeof cmd, "\"%s\" -child", self);
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi = { 0 };
    CHECK(CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi));
    Sleep(500);                                                   // (until the child runs its program)

    DWORD ids[4096], got = 0;
    CHECK(K32EnumProcesses(ids, sizeof ids, &got) && got >= 8 && got % 4 == 0);
    int self_seen = 0, child_seen = 0;
    for (DWORD i = 0; i < got / 4; i++) {
        if (ids[i] == GetCurrentProcessId()) self_seen = 1;
        if (ids[i] == pi.dwProcessId) child_seen = 1;
    }
    CHECK(self_seen && child_seen);

    // The child's name, opened by its ID as a program looking for it does; and this process's own.
    char name[MAX_PATH];
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pi.dwProcessId);
    CHECK(p != NULL);
    DWORD n = p ? K32GetProcessImageFileNameA(p, name, sizeof name) : 0;
    CHECK(n && !_stricmp(name, self));
    if (n && _stricmp(name, self)) printf("child named %s, not %s\n", name, self);
    DWORD size = sizeof name;
    CHECK(QueryFullProcessImageNameA(GetCurrentProcess(), 0, name, &size) && !_stricmp(name, self) && size == strlen(self));
    size = 4;
    SetLastError(0);
    CHECK(p && !QueryFullProcessImageNameA(p, 0, name, &size) && GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    if (p) CloseHandle(p);

    SetEvent(done);
    DWORD code = 1;
    CHECK(WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code) && code == 0);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(done);
    printf("other programs in the process list: %d failed checks -> %s\n", failures, failures ? "FAILED" : "ok");
    return failures != 0;
}
