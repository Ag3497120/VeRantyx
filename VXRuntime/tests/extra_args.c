// VXRT_EXTRA_ARGS (src/launchargs.c), as the Windows Steam launcher uses it for steamwebhelper.exe: the Makefile runs
// this program with  VXRT_EXTRA_ARGS='other.exe=--not-me;EXTRA_ARGS.EXE=--first "two words"'  and the arguments
// "self tail". The entry naming this program (case ignored) puts its arguments right after the program's name, in
// argv and in GetCommandLineW; the other entry does nothing. A child this program starts with CreateProcess, as
// steam.exe starts the web helper, gets them the same way. All arguments are made up for the test.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)

static void expect(int argc, char **argv, const char *mode, const char *last) {
    CHECK(argc == 5);
    if (argc != 5) return;
    CHECK(!strcmp(argv[1], "--first"));
    CHECK(!strcmp(argv[2], "two words"));
    CHECK(!strcmp(argv[3], mode));
    CHECK(!strcmp(argv[4], last));
    const wchar_t *cmd = GetCommandLineW();
    CHECK(wcsstr(cmd, L" --first \"two words\" ") != NULL);
    CHECK(wcsstr(cmd, L"--not-me") == NULL);
    // (CommandLineToArgvW splits it the same way: what Steam's web helper logs as its command line)
    int n = 0;
    wchar_t **args = CommandLineToArgvW(cmd, &n);
    CHECK(args != NULL && n == 5 && !wcscmp(args[2], L"two words"));
    LocalFree(args);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "child")) { expect(argc, argv, "child", "last"); return failures ? 1 : 0; }
    expect(argc, argv, "self", "tail");
    if (failures) { printf("VXRT_EXTRA_ARGS: not applied to this process -> FAILED\n"); return 1; }   // (and no child)
    char path[MAX_PATH], cmd[MAX_PATH + 32];
    GetModuleFileNameA(NULL, path, sizeof path);
    snprintf(cmd, sizeof cmd, "\"%s\" child last", path);
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi = { 0 };
    DWORD code = 99;
    CHECK(CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi));
    if (pi.hProcess) {
        CHECK(WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    CHECK(code == 0);
    printf("VXRT_EXTRA_ARGS: arguments after the program's name, in this process and its child -> %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
