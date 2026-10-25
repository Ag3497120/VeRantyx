// A self-updating program's file calls (Steam's bootstrapper is one): the documented behaviour it relies on, with
// made-up files in C:\vxupdate.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("check failed (line %d): %s\n", __LINE__, #cond); failures++; } } while (0)

static LONGLONG pointer(HANDLE h) { LARGE_INTEGER zero = { 0 }, at; return SetFilePointerEx(h, zero, &at, FILE_CURRENT) ? at.QuadPart : -1; }

// SetFilePointer(Ex) to before the start of a file: fails with ERROR_NEGATIVE_SEEK, the pointer stays. A C runtime
// opening a file for reading and writing in text mode seeks to 1 byte before the end to look for a CTRL+Z; on an
// empty file only this error tells it the file is just empty.
static void negative_seek(void) {
    HANDLE f = CreateFileA("C:\\vxupdate\\manifest", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, 0, NULL);
    CHECK(f != INVALID_HANDLE_VALUE);
    LARGE_INTEGER back = { .QuadPart = -1 }, at = { .QuadPart = 77 };
    SetLastError(0);
    CHECK(!SetFilePointerEx(f, back, &at, FILE_END) && GetLastError() == ERROR_NEGATIVE_SEEK && at.QuadPart == 77);
    CHECK(pointer(f) == 0);
    DWORD n;
    CHECK(WriteFile(f, "\"win64\"\n{\n}\n", 12, &n, NULL) && n == 12);
    CHECK(SetFilePointerEx(f, back, &at, FILE_END) && at.QuadPart == 11);         // the last byte of a file that has one
    LARGE_INTEGER far_back = { .QuadPart = -100 };
    SetLastError(0);
    CHECK(!SetFilePointerEx(f, far_back, NULL, FILE_CURRENT) && GetLastError() == ERROR_NEGATIVE_SEEK && pointer(f) == 11);
    SetLastError(0);
    CHECK(!SetFilePointerEx(f, far_back, NULL, FILE_BEGIN) && GetLastError() == ERROR_NEGATIVE_SEEK && pointer(f) == 11);
    LARGE_INTEGER zero = { 0 };
    SetLastError(0);
    CHECK(!SetFilePointerEx(f, zero, NULL, 7) && GetLastError() == ERROR_INVALID_PARAMETER);   // no such move method
    // SetFilePointer: INVALID_SET_FILE_POINTER and the same error; a success sets NO_ERROR.
    SetLastError(0);
    CHECK(SetFilePointer(f, -20, NULL, FILE_END) == INVALID_SET_FILE_POINTER && GetLastError() == ERROR_NEGATIVE_SEEK && pointer(f) == 11);
    LONG high = -1;
    SetLastError(ERROR_INVALID_FUNCTION);
    CHECK(SetFilePointer(f, -2, &high, FILE_END) == 10 && high == 0 && GetLastError() == NO_ERROR);
    // Past the end is not an error, and the size does not change until a write.
    CHECK(SetFilePointer(f, 100, NULL, FILE_END) == 112 && GetFileSize(f, NULL) == 12);
    CloseHandle(f);
    CHECK(DeleteFileA("C:\\vxupdate\\manifest"));
}

// Starting itself again (as an updater does once it has replaced its files): the new process's command line is the
// one given to CreateProcess word for word, its own name as a Windows path included, quotes and backslashes as they
// were. The child checks it and ends with 42; the arguments are made up.
static const wchar_t *const relaunch_tail = L" -relaunched -opt=\"two words\" \\\"quoted\\\" C:\\dir\\ last";
static int relaunched(void) {
    wchar_t self[MAX_PATH], want[MAX_PATH + 128];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    swprintf(want, sizeof want / sizeof *want, L"\"%ls\"%ls", self, relaunch_tail);
    const wchar_t *got = GetCommandLineW();
    if (wcscmp(got, want)) { printf("relaunched with %ls\n           not %ls\n", got, want); return 1; }
    char *env = GetEnvironmentStringsA(), *e;
    for (e = env; *e; e += strlen(e) + 1) if (!strncmp(e, "VXRT_COMMAND_LINE=", 18)) { printf("relaunched: %s in the environment\n", e); return 1; }
    FreeEnvironmentStringsA(env);
    return 42;
}
static void relaunch(void) {
    wchar_t self[MAX_PATH], cmd[MAX_PATH + 128];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    CHECK(self[0] && self[1] == L':' && self[2] == L'\\');                     // (a Windows path: what is passed on)
    swprintf(cmd, sizeof cmd / sizeof *cmd, L"\"%ls\"%ls", self, relaunch_tail);
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi = { 0 };
    DWORD code = 0;
    CHECK(CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi));
    if (!pi.hProcess) return;
    CHECK(WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0);
    CHECK(GetExitCodeProcess(pi.hProcess, &code) && code == 42);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "-relaunched")) return relaunched();
    CreateDirectoryA("C:\\vxupdate", NULL);
    negative_seek();
    relaunch();
    RemoveDirectoryA("C:\\vxupdate");
    printf("a self-updating program's file calls: %d failed checks -> %s\n", failures, failures ? "FAILED" : "ok");
    return failures != 0;
}
