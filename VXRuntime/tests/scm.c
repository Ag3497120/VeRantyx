// The service control manager with no service installed, as an ordinary user sees it on Windows: the local manager
// opens with a user's rights only, a service is ERROR_SERVICE_DOES_NOT_EXIST, every call on a service handle is
// ERROR_INVALID_HANDLE, enumeration finds nothing, and nothing blocks or spins. The service names are made up.
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)
#define FAILS_WITH(call, error) do { SetLastError(0); CHECK(!(call) && GetLastError() == (error)); } while (0)

static void WINAPI service_main(DWORD argc, LPWSTR *argv) { (void)argc; (void)argv; }

int main(void) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    CHECK(scm != NULL);
    SC_HANDLE active = OpenSCManagerW(NULL, SERVICES_ACTIVE_DATABASEW, SC_MANAGER_ENUMERATE_SERVICE | GENERIC_READ);
    CHECK(active != NULL);
    wchar_t computer[258] = L"\\\\";
    DWORD n = 256;
    CHECK(GetComputerNameW(computer + 2, &n));
    SC_HANDLE here = OpenSCManagerW(computer, NULL, SC_MANAGER_CONNECT);
    CHECK(here != NULL && CloseServiceHandle(here));
    // What an ordinary user may not do, and what is not there.
    FAILS_WITH(OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS), ERROR_ACCESS_DENIED);
    FAILS_WITH(OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE), ERROR_ACCESS_DENIED);
    FAILS_WITH(OpenSCManagerA(NULL, "VxNoSuchDatabase", SC_MANAGER_CONNECT), ERROR_DATABASE_DOES_NOT_EXIST);
    FAILS_WITH(OpenSCManagerW(L"\\\\vx-no-such-machine", NULL, SC_MANAGER_CONNECT), RPC_S_SERVER_UNAVAILABLE);

    // No service exists.
    FAILS_WITH(OpenServiceW(scm, L"VxExampleService", SERVICE_QUERY_STATUS), ERROR_SERVICE_DOES_NOT_EXIST);
    FAILS_WITH(OpenServiceA(scm, "VxExampleService", SERVICE_START | SERVICE_QUERY_STATUS), ERROR_SERVICE_DOES_NOT_EXIST);
    FAILS_WITH(OpenServiceW(scm, L"", SERVICE_QUERY_STATUS), ERROR_INVALID_NAME);
    FAILS_WITH(OpenServiceW(NULL, L"VxExampleService", SERVICE_QUERY_STATUS), ERROR_INVALID_HANDLE);
    wchar_t name[64]; DWORD cch = 64;
    FAILS_WITH(GetServiceDisplayNameW(scm, L"VxExampleService", name, &cch), ERROR_SERVICE_DOES_NOT_EXIST);
    cch = 64;
    FAILS_WITH(GetServiceKeyNameW(scm, L"Vx Example Service", name, &cch), ERROR_SERVICE_DOES_NOT_EXIST);
    DWORD need = 1, count = 1, resume = 7;
    CHECK(EnumServicesStatusExW(active, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, NULL, 0, &need, &count, &resume, NULL));
    CHECK(need == 0 && count == 0 && resume == 0);
    FAILS_WITH(EnumServicesStatusExW(active, (SC_ENUM_TYPE)5, SERVICE_WIN32, SERVICE_STATE_ALL, NULL, 0, &need, &count, NULL, NULL), ERROR_INVALID_LEVEL);
    need = count = 1;
    CHECK(EnumServicesStatusW(active, SERVICE_WIN32, SERVICE_STATE_ALL, NULL, 0, &need, &count, NULL) && need == 0 && count == 0);

    // A manager handle is not a service handle; there is no other.
    SERVICE_STATUS status;
    SERVICE_STATUS_PROCESS process_status;
    FAILS_WITH(QueryServiceStatus(scm, &status), ERROR_INVALID_HANDLE);
    FAILS_WITH(QueryServiceStatusEx(scm, SC_STATUS_PROCESS_INFO, (BYTE *)&process_status, sizeof process_status, &need), ERROR_INVALID_HANDLE);
    FAILS_WITH(QueryServiceStatusEx(NULL, SC_STATUS_PROCESS_INFO, (BYTE *)&process_status, sizeof process_status, &need), ERROR_INVALID_HANDLE);
    FAILS_WITH(StartServiceW(scm, 0, NULL), ERROR_INVALID_HANDLE);
    FAILS_WITH(ControlService(scm, SERVICE_CONTROL_STOP, &status), ERROR_INVALID_HANDLE);
    FAILS_WITH(QueryServiceConfigW(scm, NULL, 0, &need), ERROR_INVALID_HANDLE);
    FAILS_WITH(ChangeServiceConfig2W(scm, SERVICE_CONFIG_DESCRIPTION, NULL), ERROR_INVALID_HANDLE);
    FAILS_WITH(DeleteService(scm), ERROR_INVALID_HANDLE);
    // Creating a service or locking the database needs rights the manager handle does not have.
    FAILS_WITH(CreateServiceW(scm, L"VxExampleService", L"Vx Example Service", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                              SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, L"C:\\vx\\example.exe", NULL, NULL, NULL, NULL, NULL), ERROR_ACCESS_DENIED);
    FAILS_WITH(LockServiceDatabase(scm), ERROR_ACCESS_DENIED);
    // This program was not started by the manager.
    SERVICE_TABLE_ENTRYW table[] = { { L"VxExampleService", service_main }, { NULL, NULL } };
    FAILS_WITH(StartServiceCtrlDispatcherW(table), ERROR_FAILED_SERVICE_CONTROLLER_CONNECT);

    // The functions under their Windows 8 names (sechost, API sets), and handles that close once.
    HMODULE set = LoadLibraryA("api-ms-win-service-management-l1-1-0.dll");
    CHECK(set && GetProcAddress(set, "OpenSCManagerW") && GetProcAddress(LoadLibraryA("sechost.dll"), "OpenServiceW"));
    CHECK(CloseServiceHandle(active) && CloseServiceHandle(scm));
    FAILS_WITH(CloseServiceHandle(scm), ERROR_INVALID_HANDLE);
    FAILS_WITH(CloseServiceHandle(NULL), ERROR_INVALID_HANDLE);

    // A program that polls for its service (Steam looks for its service again and again) only gets answers.
    DWORD start = GetTickCount();
    for (int i = 0; i < 2000; i++) {
        SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
        SC_HANDLE s = m ? OpenServiceW(m, L"VxExampleService", SERVICE_QUERY_STATUS | SERVICE_START) : NULL;
        if (!m || s || GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST || !CloseServiceHandle(m)) { CHECK(!"service poll"); break; }
    }
    CHECK(GetTickCount() - start < 5000);
    printf("service control manager without services -> %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
