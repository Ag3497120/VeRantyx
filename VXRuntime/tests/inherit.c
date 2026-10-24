// Handle inheritance for pipe ends (src/inherit.c), the way a crash handler is started: CreatePipe with inheritable
// ends, the read end made non-inheritable with SetHandleInformation, a child started with the write end's value on its
// command line, which writes a made-up name there; the parent reads it, then the end of the pipe once the child has
// exited. Also: GetHandleInformation's answers, a pipe made without inheritance, and an unknown descriptor.
// The child is this program run with "child <handle value>".
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;
#define CHECK(c, ...) do { if (!(c)) { failed++; printf("inherit: " __VA_ARGS__); printf(" (line %d)\n", __LINE__); } } while (0)

static const char NAME[] = "made-up pipe name 42";

static DWORD flags_of(HANDLE h) { DWORD f = 0xdead; return GetHandleInformation(h, &f) ? f : 0xffff; }

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "child")) {
        HANDLE h = (HANDLE)(intptr_t)strtoull(argv[2], NULL, 16);
        DWORD n = 0;
        if (!WriteFile(h, NAME, sizeof NAME - 1, &n, NULL) || n != sizeof NAME - 1) return 3;
        CloseHandle(h);
        return 5;
    }

    // Without SECURITY_ATTRIBUTES (or with bInheritHandle FALSE): neither end is inheritable.
    HANDLE r0, w0;
    CHECK(CreatePipe(&r0, &w0, NULL, 0), "CreatePipe without attributes failed");
    CHECK(flags_of(r0) == 0 && flags_of(w0) == 0, "a pipe made without inheritance: flags %lu, %lu", flags_of(r0), flags_of(w0));
    SECURITY_ATTRIBUTES no = { sizeof no, NULL, FALSE };
    HANDLE r1, w1;
    CHECK(CreatePipe(&r1, &w1, &no, 0), "CreatePipe with bInheritHandle FALSE failed");
    CHECK(flags_of(r1) == 0 && flags_of(w1) == 0, "bInheritHandle FALSE: flags %lu, %lu", flags_of(r1), flags_of(w1));
    // SetHandleInformation turns it on and off; a mask without HANDLE_FLAG_INHERIT leaves it.
    CHECK(SetHandleInformation(w1, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) && flags_of(w1) == HANDLE_FLAG_INHERIT, "not made inheritable");
    CHECK(SetHandleInformation(w1, HANDLE_FLAG_PROTECT_FROM_CLOSE, 0) && flags_of(w1) == HANDLE_FLAG_INHERIT, "a mask without INHERIT changed it");
    CHECK(SetHandleInformation(w1, HANDLE_FLAG_INHERIT, 0) && flags_of(w1) == 0, "not made non-inheritable");
    CloseHandle(r0); CloseHandle(w0); CloseHandle(r1); CloseHandle(w1);

    // A descriptor that is not open.
    HANDLE bogus = (HANDLE)(intptr_t)(0x10000 + 4000);
    DWORD f = 0;
    SetLastError(0);
    CHECK(!GetHandleInformation(bogus, &f) && GetLastError() == ERROR_INVALID_HANDLE, "an unknown descriptor: %lu", GetLastError());
    SetLastError(0);
    CHECK(!SetHandleInformation(bogus, HANDLE_FLAG_INHERIT, 0) && GetLastError() == ERROR_INVALID_HANDLE, "SetHandleInformation on an unknown descriptor: %lu", GetLastError());

    // The crash handler's way.
    SECURITY_ATTRIBUTES yes = { sizeof yes, NULL, TRUE };
    HANDLE rd, wr;
    CHECK(CreatePipe(&rd, &wr, &yes, 0), "CreatePipe with bInheritHandle TRUE failed");
    CHECK(flags_of(rd) == HANDLE_FLAG_INHERIT && flags_of(wr) == HANDLE_FLAG_INHERIT, "bInheritHandle TRUE: flags %lu, %lu", flags_of(rd), flags_of(wr));
    CHECK(SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0) && flags_of(rd) == 0, "the read end stayed inheritable");
    char path[MAX_PATH], cmd[MAX_PATH + 64];
    GetModuleFileNameA(NULL, path, sizeof path);
    snprintf(cmd, sizeof cmd, "\"%s\" child %llx", path, (unsigned long long)(intptr_t)wr);
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(path, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) { printf("inherit: CreateProcess failed -> FAILED\n"); return 1; }
    CloseHandle(wr);                                   // (the child's copy is the only writer left)
    char buf[128];
    DWORD n, total = 0;
    while (total < sizeof buf - 1 && ReadFile(rd, buf + total, (DWORD)(sizeof buf - 1 - total), &n, NULL) && n) total += n;
    buf[total] = 0;
    CHECK(!strcmp(buf, NAME), "read \"%s\" (%lu bytes) from the child", buf, (unsigned long)total);
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CHECK(code == 5, "the child's exit code %lu (3: its WriteFile failed)", (unsigned long)code);
    SetLastError(0);
    n = 1;
    BOOL more = ReadFile(rd, buf, sizeof buf, &n, NULL);     // (the end: FALSE with ERROR_BROKEN_PIPE on Windows; no bytes)
    CHECK(more ? n == 0 : GetLastError() == ERROR_BROKEN_PIPE, "after the child: not the end of the pipe (%lu bytes, error %lu)", (unsigned long)n, GetLastError());
    CloseHandle(rd); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    printf("inherit: pipe ends inherited by a child as asked, Get/SetHandleInformation: %d failed checks -> %s\n", failed, failed ? "FAILED" : "ok");
    return failed ? 1 : 0;
}
