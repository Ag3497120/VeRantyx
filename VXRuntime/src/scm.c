// The service control manager (advapi32; sechost and api-ms-win-service-* on Windows 8 and later) of a machine where
// no service is installed, as an ordinary user sees it: the local manager opens with the rights an authenticated user
// has (connect, enumerate, query the lock status, read its security), every service name is ERROR_SERVICE_DOES_NOT_EXIST,
// so no service handle ever exists and every call on one is ERROR_INVALID_HANDLE. Nothing is ever started: a program
// that needs its service (Steam's Steam Client Service) sees a missing service, as on a Windows without it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "vx.h"

#define ERROR_ACCESS_DENIED                     5
#define ERROR_INVALID_HANDLE                    6
#define ERROR_INVALID_PARAMETER                 87
#define ERROR_INVALID_NAME                      123
#define ERROR_INVALID_LEVEL                     124
#define ERROR_SERVICE_DOES_NOT_EXIST            1060
#define ERROR_FAILED_SERVICE_CONTROLLER_CONNECT 1063
#define ERROR_DATABASE_DOES_NOT_EXIST           1065
#define ERROR_INVALID_SERVICE_LOCK              1071
#define ERROR_SERVICE_NOT_IN_EXE                1083
#define RPC_S_SERVER_UNAVAILABLE                1722

// What an authenticated user may ask of the manager: SC_MANAGER_CONNECT, SC_MANAGER_ENUMERATE_SERVICE,
// SC_MANAGER_QUERY_LOCK_STATUS, READ_CONTROL (STANDARD_RIGHTS_READ), and GENERIC_READ, which maps onto them.
// MAXIMUM_ALLOWED gives those. Not GENERIC_EXECUTE: it maps to SC_MANAGER_CONNECT and SC_MANAGER_LOCK, and locking
// the database is not an authenticated user's right (ERROR_ACCESS_DENIED, as for SC_MANAGER_LOCK itself).
#define SCM_ALLOWED (0x0001u | 0x0004u | 0x0010u | 0x00020000u | 0x80000000u | 0x02000000u)

typedef struct { DWORD access; } scm;
static scm *manager(HANDLE h) { return vx_handle_data(h, VX_HANDLE_SC_MANAGER); }

// The local machine: no name, or this computer's (with or without the leading backslashes).
static int local_machine(const char *m) {
    if (!m || !*m) return 1;
    while (*m == '\\') m++;
    char name[256]; DWORD n = sizeof name;
    BOOL (WINAPI *computer)(char *, DWORD *) = (BOOL (WINAPI *)(char *, DWORD *))vx_find_export("kernel32.dll", "GetComputerNameA");
    return !strcasecmp(m, "localhost") || !strcmp(m, ".") || (computer && computer(name, &n) && !strcasecmp(m, name));
}
static WINAPI HANDLE OpenSCManagerA(const char *machine, const char *database, DWORD access) {
    if (!local_machine(machine)) { vx_set_last_error(RPC_S_SERVER_UNAVAILABLE); return NULL; }
    if (database && strcasecmp(database, "ServicesActive")) { vx_set_last_error(ERROR_DATABASE_DOES_NOT_EXIST); return NULL; }
    if (access & ~SCM_ALLOWED) { vx_set_last_error(ERROR_ACCESS_DENIED); return NULL; }
    scm *s = calloc(1, sizeof *s);
    s->access = access;
    HANDLE h = vx_handle_new(VX_HANDLE_SC_MANAGER, s, free);
    if (!h) { free(s); vx_set_last_error(8); }
    return h;
}
static WINAPI HANDLE OpenSCManagerW(const WCHAR *machine, const WCHAR *database, DWORD access) {
    char *m = vx_utf8_dup(machine), *d = vx_utf8_dup(database);
    HANDLE h = OpenSCManagerA(m, d, access);
    free(m); free(d);
    return h;
}
static WINAPI BOOL CloseServiceHandle(HANDLE h) {
    if (!manager(h)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    return vx_handle_close(h);
}
// A service name (key name or display name) on a valid manager: never installed.
static int no_such_service(HANDLE h, int empty) {
    vx_set_last_error(!manager(h) ? ERROR_INVALID_HANDLE : empty ? ERROR_INVALID_NAME : ERROR_SERVICE_DOES_NOT_EXIST);
    return 0;
}
static WINAPI HANDLE OpenServiceA(HANDLE h, const char *name, DWORD access) { (void)access; no_such_service(h, !name || !*name); return NULL; }
static WINAPI HANDLE OpenServiceW(HANDLE h, const WCHAR *name, DWORD access) { (void)access; no_such_service(h, !name || !*name); return NULL; }
static WINAPI BOOL GetServiceKeyNameA(HANDLE h, const char *display, char *out, DWORD *n) { (void)out; (void)n; return no_such_service(h, !display || !*display); }
static WINAPI BOOL GetServiceKeyNameW(HANDLE h, const WCHAR *display, WCHAR *out, DWORD *n) { (void)out; (void)n; return no_such_service(h, !display || !*display); }
static WINAPI BOOL GetServiceDisplayNameA(HANDLE h, const char *key, char *out, DWORD *n) { (void)out; (void)n; return no_such_service(h, !key || !*key); }
static WINAPI BOOL GetServiceDisplayNameW(HANDLE h, const WCHAR *key, WCHAR *out, DWORD *n) { (void)out; (void)n; return no_such_service(h, !key || !*key); }
// Creating a service needs SC_MANAGER_CREATE_SERVICE, which no manager handle here has.
static WINAPI HANDLE CreateServiceA(HANDLE h, const char *name, const char *display, DWORD access, DWORD type, DWORD start, DWORD error,
                                   const char *path, const char *group, DWORD *tag, const char *deps, const char *user, const char *password) {
    (void)name; (void)display; (void)access; (void)type; (void)start; (void)error; (void)path; (void)group; (void)tag; (void)deps; (void)user; (void)password;
    vx_set_last_error(manager(h) ? ERROR_ACCESS_DENIED : ERROR_INVALID_HANDLE);
    return NULL;
}
static WINAPI HANDLE CreateServiceW(HANDLE h, const WCHAR *name, const WCHAR *display, DWORD access, DWORD type, DWORD start, DWORD error,
                                   const WCHAR *path, const WCHAR *group, DWORD *tag, const WCHAR *deps, const WCHAR *user, const WCHAR *password) {
    (void)name; (void)display; (void)access; (void)type; (void)start; (void)error; (void)path; (void)group; (void)tag; (void)deps; (void)user; (void)password;
    vx_set_last_error(manager(h) ? ERROR_ACCESS_DENIED : ERROR_INVALID_HANDLE);
    return NULL;
}
// So is locking the database (SC_MANAGER_LOCK).
static WINAPI void *LockServiceDatabase(HANDLE h) { vx_set_last_error(manager(h) ? ERROR_ACCESS_DENIED : ERROR_INVALID_HANDLE); return NULL; }
static WINAPI BOOL UnlockServiceDatabase(void *lock) { (void)lock; vx_set_last_error(ERROR_INVALID_SERVICE_LOCK); return 0; }

// Enumerating: no service. EnumServicesStatusEx takes SC_ENUM_PROCESS_INFO (0) only.
static BOOL enum_none(HANDLE h, DWORD *need, DWORD *count, DWORD *resume) {
    if (!manager(h)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    if (!need || !count) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    *need = 0; *count = 0;
    if (resume) *resume = 0;
    return 1;
}
static WINAPI BOOL EnumServicesStatusA(HANDLE h, DWORD type, DWORD state, void *buf, DWORD size, DWORD *need, DWORD *count, DWORD *resume) {
    (void)type; (void)state; (void)buf; (void)size;
    return enum_none(h, need, count, resume);
}
static WINAPI BOOL EnumServicesStatusW(HANDLE h, DWORD type, DWORD state, void *buf, DWORD size, DWORD *need, DWORD *count, DWORD *resume) {
    (void)type; (void)state; (void)buf; (void)size;
    return enum_none(h, need, count, resume);
}
static WINAPI BOOL EnumServicesStatusExA(HANDLE h, int level, DWORD type, DWORD state, void *buf, DWORD size, DWORD *need, DWORD *count, DWORD *resume, const char *group) {
    (void)type; (void)state; (void)buf; (void)size; (void)group;
    if (manager(h) && level != 0) { vx_set_last_error(ERROR_INVALID_LEVEL); return 0; }
    return enum_none(h, need, count, resume);
}
static WINAPI BOOL EnumServicesStatusExW(HANDLE h, int level, DWORD type, DWORD state, void *buf, DWORD size, DWORD *need, DWORD *count, DWORD *resume, const WCHAR *group) {
    (void)type; (void)state; (void)buf; (void)size; (void)group;
    if (manager(h) && level != 0) { vx_set_last_error(ERROR_INVALID_LEVEL); return 0; }
    return enum_none(h, need, count, resume);
}

// Calls on a service handle: there is none.
static BOOL no_service(void) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
static WINAPI BOOL StartServiceA(HANDLE s, DWORD argc, const char **argv) { (void)s; (void)argc; (void)argv; return no_service(); }
static WINAPI BOOL StartServiceW(HANDLE s, DWORD argc, const WCHAR **argv) { (void)s; (void)argc; (void)argv; return no_service(); }
static WINAPI BOOL ControlService(HANDLE s, DWORD control, void *status) { (void)s; (void)control; (void)status; return no_service(); }
static WINAPI BOOL ControlServiceExA(HANDLE s, DWORD control, DWORD level, void *params) { (void)s; (void)control; (void)level; (void)params; return no_service(); }
static WINAPI BOOL ControlServiceExW(HANDLE s, DWORD control, DWORD level, void *params) { (void)s; (void)control; (void)level; (void)params; return no_service(); }
static WINAPI BOOL QueryServiceStatus(HANDLE s, void *status) { (void)s; (void)status; return no_service(); }
static WINAPI BOOL QueryServiceStatusEx(HANDLE s, int level, void *buf, DWORD size, DWORD *need) { (void)s; (void)level; (void)buf; (void)size; (void)need; return no_service(); }
static WINAPI BOOL QueryServiceConfigA(HANDLE s, void *buf, DWORD size, DWORD *need) { (void)s; (void)buf; (void)size; (void)need; return no_service(); }
static WINAPI BOOL QueryServiceConfigW(HANDLE s, void *buf, DWORD size, DWORD *need) { (void)s; (void)buf; (void)size; (void)need; return no_service(); }
static WINAPI BOOL QueryServiceConfig2A(HANDLE s, DWORD level, void *buf, DWORD size, DWORD *need) { (void)s; (void)level; (void)buf; (void)size; (void)need; return no_service(); }
static WINAPI BOOL QueryServiceConfig2W(HANDLE s, DWORD level, void *buf, DWORD size, DWORD *need) { (void)s; (void)level; (void)buf; (void)size; (void)need; return no_service(); }
static WINAPI BOOL ChangeServiceConfigA(HANDLE s, DWORD type, DWORD start, DWORD error, const char *path, const char *group, DWORD *tag,
                                        const char *deps, const char *user, const char *password, const char *display) {
    (void)s; (void)type; (void)start; (void)error; (void)path; (void)group; (void)tag; (void)deps; (void)user; (void)password; (void)display;
    return no_service();
}
static WINAPI BOOL ChangeServiceConfigW(HANDLE s, DWORD type, DWORD start, DWORD error, const WCHAR *path, const WCHAR *group, DWORD *tag,
                                        const WCHAR *deps, const WCHAR *user, const WCHAR *password, const WCHAR *display) {
    (void)s; (void)type; (void)start; (void)error; (void)path; (void)group; (void)tag; (void)deps; (void)user; (void)password; (void)display;
    return no_service();
}
static WINAPI BOOL ChangeServiceConfig2A(HANDLE s, DWORD level, void *info) { (void)s; (void)level; (void)info; return no_service(); }
static WINAPI BOOL ChangeServiceConfig2W(HANDLE s, DWORD level, void *info) { (void)s; (void)level; (void)info; return no_service(); }
static WINAPI BOOL DeleteService(HANDLE s) { (void)s; return no_service(); }
static WINAPI BOOL EnumDependentServicesA(HANDLE s, DWORD state, void *buf, DWORD size, DWORD *need, DWORD *count) { (void)s; (void)state; (void)buf; (void)size; (void)need; (void)count; return no_service(); }
static WINAPI BOOL EnumDependentServicesW(HANDLE s, DWORD state, void *buf, DWORD size, DWORD *need, DWORD *count) { (void)s; (void)state; (void)buf; (void)size; (void)need; (void)count; return no_service(); }
// NotifyServiceStatusChange returns the error. On the manager (services created or deleted) it is registered and,
// with no service ever coming or going, never completes.
static WINAPI DWORD NotifyServiceStatusChangeW(HANDLE h, DWORD mask, void *notify) { (void)mask; return !notify ? ERROR_INVALID_PARAMETER : manager(h) ? 0 : ERROR_INVALID_HANDLE; }
static WINAPI DWORD NotifyServiceStatusChangeA(HANDLE h, DWORD mask, void *notify) { return NotifyServiceStatusChangeW(h, mask, notify); }

// The service side: this process was not started by the manager.
static WINAPI BOOL StartServiceCtrlDispatcherA(const void *table) { (void)table; vx_set_last_error(ERROR_FAILED_SERVICE_CONTROLLER_CONNECT); return 0; }
static WINAPI BOOL StartServiceCtrlDispatcherW(const void *table) { (void)table; vx_set_last_error(ERROR_FAILED_SERVICE_CONTROLLER_CONNECT); return 0; }
static WINAPI HANDLE RegisterServiceCtrlHandlerA(const char *name, void *fn) { (void)name; (void)fn; vx_set_last_error(ERROR_SERVICE_NOT_IN_EXE); return NULL; }
static WINAPI HANDLE RegisterServiceCtrlHandlerW(const WCHAR *name, void *fn) { (void)name; (void)fn; vx_set_last_error(ERROR_SERVICE_NOT_IN_EXE); return NULL; }
static WINAPI HANDLE RegisterServiceCtrlHandlerExA(const char *name, void *fn, void *ctx) { (void)name; (void)fn; (void)ctx; vx_set_last_error(ERROR_SERVICE_NOT_IN_EXE); return NULL; }
static WINAPI HANDLE RegisterServiceCtrlHandlerExW(const WCHAR *name, void *fn, void *ctx) { (void)name; (void)fn; (void)ctx; vx_set_last_error(ERROR_SERVICE_NOT_IN_EXE); return NULL; }
static WINAPI BOOL SetServiceStatus(HANDLE h, const void *status) { (void)h; (void)status; return no_service(); }

static const vx_export scm_exports[] = {
    VX_EXPORT(OpenSCManagerA), VX_EXPORT(OpenSCManagerW), VX_EXPORT(CloseServiceHandle), VX_EXPORT(OpenServiceA), VX_EXPORT(OpenServiceW),
    VX_EXPORT(GetServiceKeyNameA), VX_EXPORT(GetServiceKeyNameW), VX_EXPORT(GetServiceDisplayNameA), VX_EXPORT(GetServiceDisplayNameW),
    VX_EXPORT(CreateServiceA), VX_EXPORT(CreateServiceW), VX_EXPORT(LockServiceDatabase), VX_EXPORT(UnlockServiceDatabase),
    VX_EXPORT(EnumServicesStatusA), VX_EXPORT(EnumServicesStatusW), VX_EXPORT(EnumServicesStatusExA), VX_EXPORT(EnumServicesStatusExW),
    VX_EXPORT(StartServiceA), VX_EXPORT(StartServiceW), VX_EXPORT(ControlService), VX_EXPORT(ControlServiceExA), VX_EXPORT(ControlServiceExW),
    VX_EXPORT(QueryServiceStatus), VX_EXPORT(QueryServiceStatusEx), VX_EXPORT(QueryServiceConfigA), VX_EXPORT(QueryServiceConfigW),
    VX_EXPORT(QueryServiceConfig2A), VX_EXPORT(QueryServiceConfig2W), VX_EXPORT(ChangeServiceConfigA), VX_EXPORT(ChangeServiceConfigW),
    VX_EXPORT(ChangeServiceConfig2A), VX_EXPORT(ChangeServiceConfig2W), VX_EXPORT(DeleteService), VX_EXPORT(EnumDependentServicesA),
    VX_EXPORT(EnumDependentServicesW), VX_EXPORT(NotifyServiceStatusChangeA), VX_EXPORT(NotifyServiceStatusChangeW),
    VX_EXPORT(StartServiceCtrlDispatcherA), VX_EXPORT(StartServiceCtrlDispatcherW), VX_EXPORT(RegisterServiceCtrlHandlerA),
    VX_EXPORT(RegisterServiceCtrlHandlerW), VX_EXPORT(RegisterServiceCtrlHandlerExA), VX_EXPORT(RegisterServiceCtrlHandlerExW),
    VX_EXPORT(SetServiceStatus),
};
void vx_scm_init(void) {
    vx_register_module("advapi32.dll", scm_exports, sizeof scm_exports / sizeof scm_exports[0]);
    vx_register_module("sechost.dll", scm_exports, sizeof scm_exports / sizeof scm_exports[0]);
}

#ifdef __aarch64__
#include "thunks/scm.c.inc"
#endif
