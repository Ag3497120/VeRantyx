// Job objects (src/jobs.c): processes assigned to a job, the ones they start afterwards, a completion port's messages,
// limits (what cannot be applied is refused), TerminateJobObject and JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, also when the
// process holding the job ends without closing it. The processes are this program run as "sleep" (waits), "exit"
// (ends at once), "tree" (waits a moment, starts a "sleep" of its own, waits) and "owner <how> <file>" (puts a "sleep"
// in a kill-on-close job, writes its id to the file and, once the file is gone (the test has the process open), ends:
// "exit" by ExitProcess, "crash" by an unhandled exception, "wait" waiting to be ended by TerminateProcess).
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)
static char g_exe[MAX_PATH];
static int failures_before;
static void group(const char *name) { printf("jobs: %s -> %s\n", name, failures == failures_before ? "ok" : "FAILED"); failures_before = failures; }

static HANDLE start(const char *mode, DWORD *pid) {
    char cmd[3 * MAX_PATH];
    snprintf(cmd, sizeof cmd, "\"%s\" %s", g_exe, mode);
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(g_exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return NULL;
    CloseHandle(pi.hThread);
    if (pid) *pid = pi.dwProcessId;
    return pi.hProcess;
}
static DWORD active_ids(HANDLE job, ULONG_PTR *ids, DWORD cap) {
    struct { DWORD assigned, listed; ULONG_PTR ids[16]; } list;
    memset(&list, 0, sizeof list);
    if (!QueryInformationJobObject(job, JobObjectBasicProcessIdList, &list, sizeof list, NULL)) return 0;
    for (DWORD i = 0; i < list.listed && i < cap; i++) ids[i] = list.ids[i];
    return list.listed;
}
// The next message the port has for the job (key 77) within the time, or 0.
static DWORD next_message(HANDLE port, ULONG_PTR *value, DWORD ms) {
    DWORD msg = 0; ULONG_PTR key = 0; OVERLAPPED *ov = NULL;
    if (!GetQueuedCompletionStatus(port, &msg, &key, &ov, ms) || key != 77) return 0;
    if (value) *value = (ULONG_PTR)ov;
    return msg;
}

// "owner <how> <file>": a kill-on-close job with a "sleep" in it, never closed; then this process ends as <how> says.
static int owner(const char *how, const char *file) {
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    memset(&limits, 0, sizeof limits);
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits)) return 10;
    DWORD pid = 0;
    HANDLE p = start("sleep", &pid);
    if (!p || !AssignProcessToJobObject(job, p)) return 11;
    char text[32];
    int n = snprintf(text, sizeof text, "%lu\n", (unsigned long)pid);
    HANDLE f = CreateFileA(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD wrote = 0;
    if (f == INVALID_HANDLE_VALUE || !WriteFile(f, text, (DWORD)n, &wrote, NULL)) return 12;
    CloseHandle(f);
    for (int i = 0; i < 1500 && GetFileAttributesA(file) != INVALID_FILE_ATTRIBUTES; i++) Sleep(20);
    if (!strcmp(how, "exit")) ExitProcess(0);
    if (!strcmp(how, "crash")) RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, NULL);
    Sleep(60000);                                      // ("wait": until TerminateProcess)
    return 13;
}
static DWORD pid_in(const char *file) {
    char text[32] = { 0 };
    DWORD got = 0;
    HANDLE f = CreateFileA(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    ReadFile(f, text, sizeof text - 1, &got, NULL);
    CloseHandle(f);
    return got && text[got - 1] == '\n' ? (DWORD)strtoul(text, NULL, 10) : 0;   // (all of it written)
}
// The process holding a kill-on-close job ends as <how> says, the job's handle still open: its member ends too.
static void member_ends_with_owner(const char *how) {
    char temp[MAX_PATH], file[MAX_PATH], mode[2 * MAX_PATH];
    GetTempPathA(sizeof temp, temp);
    snprintf(file, sizeof file, "%sjobs_member_%s.txt", temp, how);
    DeleteFileA(file);
    snprintf(mode, sizeof mode, "owner %s \"%s\"", how, file);
    HANDLE o = start(mode, NULL);
    CHECK(o != NULL);
    DWORD member = 0;
    for (int i = 0; o && i < 300 && !(member = pid_in(file)); i++) Sleep(50);
    CHECK(member != 0);
    HANDLE m = member ? OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, member) : NULL;
    CHECK(m != NULL);
    DeleteFileA(file);                                 // (the owner ends now)
    if (!strcmp(how, "wait")) CHECK(o && TerminateProcess(o, 1));
    CHECK(o && WaitForSingleObject(o, 15000) == WAIT_OBJECT_0);
    CHECK(m && WaitForSingleObject(m, 10000) == WAIT_OBJECT_0);
    if (m && WaitForSingleObject(m, 0) == WAIT_TIMEOUT) TerminateProcess(m, 1);   // (not left running when this fails)
    if (m) CloseHandle(m);
    if (o) CloseHandle(o);
    DeleteFileA(file);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "sleep")) { Sleep(60000); return 3; }
    if (argc > 1 && !strcmp(argv[1], "exit")) return 0;
    if (argc > 3 && !strcmp(argv[1], "owner")) { GetModuleFileNameA(NULL, g_exe, sizeof g_exe); return owner(argv[2], argv[3]); }
    if (argc > 1 && !strcmp(argv[1], "tree")) {
        GetModuleFileNameA(NULL, g_exe, sizeof g_exe);
        Sleep(1500);                                   // (by then the parent has put this process in its job)
        HANDLE child = start("sleep", NULL);
        Sleep(60000);
        if (child) CloseHandle(child);
        return 4;
    }
    GetModuleFileNameA(NULL, g_exe, sizeof g_exe);

    // a job, a member, its messages and its end
    HANDLE job = CreateJobObjectW(NULL, NULL);
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    CHECK(job && port);
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT assoc = { (PVOID)77, port };
    CHECK(SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation, &assoc, sizeof assoc));
    SetLastError(0); CHECK(!SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation, &assoc, sizeof assoc) && GetLastError() == ERROR_INVALID_PARAMETER);
    DWORD pid = 0;
    HANDLE p = start("sleep", &pid);
    CHECK(p != NULL);
    BOOL in = TRUE;
    CHECK(IsProcessInJob(p, job, &in) && !in);
    CHECK(AssignProcessToJobObject(job, p));
    CHECK(IsProcessInJob(p, job, &in) && in && IsProcessInJob(p, NULL, &in) && in);
    CHECK(IsProcessInJob(GetCurrentProcess(), job, &in) && !in);
    ULONG_PTR ids[16], value = 0;
    CHECK(active_ids(job, ids, 16) == 1 && ids[0] == pid);
    CHECK(next_message(port, &value, 5000) == JOB_OBJECT_MSG_NEW_PROCESS && value == pid);
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct;
    CHECK(QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &acct, sizeof acct, NULL) && acct.TotalProcesses == 1 && acct.ActiveProcesses == 1);
    // limits: what can be applied (or always holds here) is kept and read back; scheduling hints are kept
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits, read;
    memset(&limits, 0, sizeof limits);
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION | JOB_OBJECT_LIMIT_PRIORITY_CLASS;
    limits.BasicLimitInformation.PriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
    CHECK(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits));
    DWORD got = 0;
    CHECK(QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &read, sizeof read, &got) && got == sizeof read &&
          read.BasicLimitInformation.LimitFlags == limits.BasicLimitInformation.LimitFlags && read.BasicLimitInformation.PriorityClass == BELOW_NORMAL_PRIORITY_CLASS);
    // what cannot be applied here (a sandbox's limits) is refused, and the job's limits stay as they were
    const DWORD refused[] = { JOB_OBJECT_LIMIT_ACTIVE_PROCESS, JOB_OBJECT_LIMIT_PROCESS_MEMORY, JOB_OBJECT_LIMIT_JOB_MEMORY,
                              JOB_OBJECT_LIMIT_PROCESS_TIME, JOB_OBJECT_LIMIT_JOB_TIME };
    for (int i = 0; i < 5; i++) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION more = limits;
        more.BasicLimitInformation.LimitFlags |= refused[i];
        more.BasicLimitInformation.ActiveProcessLimit = 1;
        more.ProcessMemoryLimit = more.JobMemoryLimit = 1u << 30;
        more.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart = more.BasicLimitInformation.PerJobUserTimeLimit.QuadPart = 10000000;
        SetLastError(0); CHECK(!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &more, sizeof more) && GetLastError() == ERROR_NOT_SUPPORTED);
        JOBOBJECT_BASIC_LIMIT_INFORMATION basic = more.BasicLimitInformation;
        basic.LimitFlags = refused[i];                 // (the memory limits are extended ones: not valid here at all)
        SetLastError(0); CHECK(!SetInformationJobObject(job, JobObjectBasicLimitInformation, &basic, sizeof basic) &&
                               GetLastError() == (refused[i] & 0xff ? ERROR_NOT_SUPPORTED : ERROR_INVALID_PARAMETER));
    }
    CHECK(QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &read, sizeof read, NULL) &&
          read.BasicLimitInformation.LimitFlags == limits.BasicLimitInformation.LimitFlags && read.BasicLimitInformation.ActiveProcessLimit == 0 && read.ProcessMemoryLimit == 0);
    SetLastError(0); CHECK(!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, 16) && GetLastError() == ERROR_BAD_LENGTH);
    limits.BasicLimitInformation.LimitFlags = 0x80000000u;
    SetLastError(0); CHECK(!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) && GetLastError() == ERROR_INVALID_PARAMETER);
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui = { JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_DESKTOP }, ui_read = { 0xff };
    SetLastError(0); CHECK(!SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof ui) && GetLastError() == ERROR_NOT_SUPPORTED);
    CHECK(QueryInformationJobObject(job, JobObjectBasicUIRestrictions, &ui_read, sizeof ui_read, NULL) && ui_read.UIRestrictionsClass == JOB_OBJECT_UILIMIT_NONE);
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_NONE;
    CHECK(SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof ui));
    CHECK(TerminateJobObject(job, 9));
    CHECK(WaitForSingleObject(p, 10000) == WAIT_OBJECT_0);
    DWORD msg = next_message(port, &value, 5000);
    CHECK(msg == JOB_OBJECT_MSG_EXIT_PROCESS && value == pid);
    CHECK(next_message(port, NULL, 5000) == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO);
    CHECK(QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &acct, sizeof acct, NULL) && acct.ActiveProcesses == 0 && acct.TotalTerminatedProcesses == 1);
    CloseHandle(p);
    group("member, messages, limits, TerminateJobObject");

    // processes that end at once (before the port's watcher has seen them run): their messages, then the job's
    // ACTIVE_PROCESS_ZERO, each time
    HANDLE quick = CreateJobObjectW(NULL, NULL), quick_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT quick_assoc = { (PVOID)77, quick_port };
    CHECK(quick && quick_port && SetInformationJobObject(quick, JobObjectAssociateCompletionPortInformation, &quick_assoc, sizeof quick_assoc));
    for (int round = 0; round < 4; round++) {
        HANDLE q = start("exit", &pid);
        CHECK(q && AssignProcessToJobObject(quick, q));
        CHECK(q && WaitForSingleObject(q, 10000) == WAIT_OBJECT_0);
        CHECK(next_message(quick_port, &value, 5000) == JOB_OBJECT_MSG_NEW_PROCESS && value == pid);
        CHECK(next_message(quick_port, &value, 5000) == JOB_OBJECT_MSG_EXIT_PROCESS && value == pid);
        CHECK(next_message(quick_port, NULL, 5000) == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO);
        if (q) CloseHandle(q);
    }
    CloseHandle(quick); CloseHandle(quick_port);
    group("processes that end at once, ACTIVE_PROCESS_ZERO");

    // what a member starts afterwards is in the job too, and ends with it
    HANDLE tree = start("tree", &pid);
    CHECK(tree && AssignProcessToJobObject(job, tree));
    DWORD n = 0;
    for (int i = 0; i < 100 && (n = active_ids(job, ids, 16)) < 2; i++) Sleep(100);
    CHECK(n == 2);
    DWORD grandchild = n == 2 ? (DWORD)(ids[0] == pid ? ids[1] : ids[0]) : 0;
    HANDLE g = grandchild ? OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, grandchild) : NULL;
    CHECK(g != NULL);
    CHECK(TerminateJobObject(job, 5));
    CHECK(WaitForSingleObject(tree, 10000) == WAIT_OBJECT_0);
    CHECK(g && WaitForSingleObject(g, 10000) == WAIT_OBJECT_0);
    if (g) CloseHandle(g);
    CloseHandle(tree);
    group("processes started by a member");

    // a named job, opened again; the last handle of a kill-on-close job ends its processes
    HANDLE named = CreateJobObjectA(NULL, "kx_made_up_job");
    CHECK(named && GetLastError() == 0);
    HANDLE again = CreateJobObjectA(NULL, "kx_made_up_job");
    CHECK(again && GetLastError() == ERROR_ALREADY_EXISTS);
    HANDLE opened = OpenJobObjectA(JOB_OBJECT_ALL_ACCESS, FALSE, "kx_made_up_job");
    CHECK(opened != NULL);
    SetLastError(0); CHECK(!OpenJobObjectA(JOB_OBJECT_ALL_ACCESS, FALSE, "kx_no_such_job") && GetLastError() == ERROR_FILE_NOT_FOUND);
    memset(&limits, 0, sizeof limits);
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    CHECK(SetInformationJobObject(named, JobObjectExtendedLimitInformation, &limits, sizeof limits));
    p = start("sleep", NULL);
    CHECK(p && AssignProcessToJobObject(opened, p));
    CloseHandle(named); CloseHandle(again);
    CHECK(WaitForSingleObject(p, 300) == WAIT_TIMEOUT);                          // a handle is still open
    CloseHandle(opened);
    CHECK(WaitForSingleObject(p, 10000) == WAIT_OBJECT_0);
    CloseHandle(p);
    SetLastError(0); CHECK(!TerminateJobObject(port, 1) && GetLastError() == ERROR_INVALID_HANDLE);
    SetLastError(0); CHECK(!AssignProcessToJobObject(job, port) && GetLastError() == ERROR_INVALID_HANDLE);
    CloseHandle(job); CloseHandle(port);
    group("named jobs, KILL_ON_JOB_CLOSE");

    // the process holding a kill-on-close job ends without closing it: by ExitProcess, by an unhandled exception, by
    // TerminateProcess (as Windows closes the handles of a process that ends, however it ends)
    member_ends_with_owner("exit");
    member_ends_with_owner("crash");
    member_ends_with_owner("wait");
    group("KILL_ON_JOB_CLOSE when the holder ends");
    return failures ? 1 : 0;
}
