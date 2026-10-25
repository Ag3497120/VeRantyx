// <VXRT_ROOT>/program_env.txt (src/launchargs.c): the Makefile writes one with a line for this program (in other
// letter case), one for another program and a comment, then runs this program. Its own line's settings are in its
// environment, quoted spaces kept; the other program's are not; a child it starts inherits them. Made-up names only.
#include <windows.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)

static void expect_settings(void) {
    char v[256];
    DWORD n = GetEnvironmentVariableA("VXRT_TEST_PROGRAM_ENV", v, sizeof v);
    CHECK(n == 9 && !strcmp(v, "two words"));
    n = GetEnvironmentVariableA("VXRT_TEST_PROGRAM_ENV_B", v, sizeof v);
    CHECK(n == 1 && !strcmp(v, "1"));
    CHECK(GetEnvironmentVariableA("VXRT_TEST_PROGRAM_ENV_OTHER", v, sizeof v) == 0);
}

int main(int argc, char **argv) {
    expect_settings();
    if (argc > 1 && !strcmp(argv[1], "child")) return failures ? 1 : 0;
    char cmd[MAX_PATH + 16];
    snprintf(cmd, sizeof cmd, "\"%s\" child", argv[0]);
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    CHECK(CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi));
    DWORD code = 1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CHECK(code == 0);
    printf(failures ? "program_env: FAILED\n" : "program_env: ok\n");
    return failures ? 1 : 0;
}
