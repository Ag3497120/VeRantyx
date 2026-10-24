// Job objects (src/jobs.c): processes assigned to a job, the ones they start afterwards, a completion port's messages,
// TerminateJobObject and JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE. The processes are this program run as "sleep" (waits) and
// "tree" (waits a moment, starts a "sleep" of its own, waits).
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)
static char g_exe[MAX_PATH];

static HANDLE start(const char *mode, DWORD *pid) {
    char cmd[MAX_PATH + 32];
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

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "sleep")) { Sleep(60000); return 3; }
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
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    memset(&limits, 0, sizeof limits);
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    limits.BasicLimitInformation.ActiveProcessLimit = 4;
    limits.ProcessMemoryLimit = 1u << 30;
    CHECK(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION read;
    DWORD got = 0;
    CHECK(QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &read, sizeof read, &got) && got == sizeof read &&
          read.BasicLimitInformation.ActiveProcessLimit == 4 && read.ProcessMemoryLimit == (1u << 30));
    SetLastError(0); CHECK(!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, 16) && GetLastError() == ERROR_BAD_LENGTH);
    limits.BasicLimitInformation.LimitFlags = 0x80000000u;
    SetLastError(0); CHECK(!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) && GetLastError() == ERROR_INVALID_PARAMETER);
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui = { JOB_OBJECT_UILIMIT_HANDLES }, ui_read = { 0 };
    CHECK(SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof ui) &&
          QueryInformationJobObject(job, JobObjectBasicUIRestrictions, &ui_read, sizeof ui_read, NULL) && ui_read.UIRestrictionsClass == JOB_OBJECT_UILIMIT_HANDLES);
    CHECK(TerminateJobObject(job, 9));
    CHECK(WaitForSingleObject(p, 10000) == WAIT_OBJECT_0);
    DWORD msg = next_message(port, &value, 5000);
    CHECK(msg == JOB_OBJECT_MSG_EXIT_PROCESS && value == pid);
    CHECK(next_message(port, NULL, 5000) == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO);
    CHECK(QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &acct, sizeof acct, NULL) && acct.ActiveProcesses == 0 && acct.TotalTerminatedProcesses == 1);
    CloseHandle(p);
    printf("jobs: member, messages, limits, TerminateJobObject -> %s\n", failures ? "FAILED" : "ok");
    int before = failures;

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
    printf("jobs: processes started by a member -> %s\n", failures == before ? "ok" : "FAILED");
    before = failures;

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
    printf("jobs: named jobs, KILL_ON_JOB_CLOSE -> %s\n", failures == before ? "ok" : "FAILED");
    return failures ? 1 : 0;
}
