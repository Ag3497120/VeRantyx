// Functions the Windows Steam client's steamwebhelper (Chromium) stopped at, each as Microsoft documents it:
// shlwapi's IsOS by ordinal 437 and by name, consistent with GetVersionEx and GetSystemMetrics (src/osinfo.c);
// the downlevel API sets resolving to the DLLs they are named after (src/loader.c); touch and pointer input on a Mac
// with no touch screen or pen (src/pointer.c); RegNotifyChangeKeyValue, synchronous and asynchronous (src/regnotify.c);
// NtQuerySection on file mappings (src/section.c). Made-up key names and data only.
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s (last error %lu)\n", __LINE__, #c, GetLastError()); failures++; } } while (0)

// ---- IsOS ------------------------------------------------------------------------------------------------------------
enum { OS_WINDOWS = 0, OS_NT = 1, OS_WIN95ORGREATER = 2, OS_NT4ORGREATER = 3, OS_WIN2000ORGREATER = 7, OS_WIN2000PRO = 8,
       OS_TERMINALCLIENT = 14, OS_XPORGREATER = 18, OS_HOME = 19, OS_PROFESSIONAL = 20, OS_SERVER = 23,
       OS_PERSONALTERMINALSERVER = 25, OS_DOMAINMEMBER = 28, OS_ANYSERVER = 29, OS_WOW6432 = 30, OS_TABLETPC = 33,
       OS_MEDIACENTER = 35 };
static void test_isos(void) {
    HMODULE shlwapi = LoadLibraryA("shlwapi.dll");
    CHECK(shlwapi != NULL);
    BOOL (WINAPI *is_os)(DWORD) = (BOOL (WINAPI *)(DWORD))GetProcAddress(shlwapi, MAKEINTRESOURCEA(437));
    BOOL (WINAPI *by_name)(DWORD) = (BOOL (WINAPI *)(DWORD))GetProcAddress(shlwapi, "IsOS");
    CHECK(is_os != NULL && by_name != NULL);
    if (!is_os || !by_name) return;
    CHECK(is_os(OS_NT) && is_os(OS_NT4ORGREATER) && is_os(OS_WIN2000ORGREATER) && is_os(OS_XPORGREATER));
    CHECK(!is_os(OS_WINDOWS) && !is_os(OS_WIN95ORGREATER) && !is_os(OS_WIN2000PRO));
    CHECK(by_name(OS_NT) && !by_name(OS_WINDOWS));
    CHECK(!is_os(4) && !is_os(37) && !is_os(1000));                    // values the documentation does not list
    CHECK(!is_os(OS_DOMAINMEMBER));
    // The same machine GetVersionEx describes: a workstation, the Pro edition, one interactive session.
    OSVERSIONINFOEXW v = { sizeof v };
    CHECK(GetVersionExW((OSVERSIONINFOW *)&v));
    CHECK(v.dwMajorVersion == 10 && v.dwBuildNumber == 19045 && v.wProductType == VER_NT_WORKSTATION);
    CHECK(!is_os(OS_ANYSERVER) && !is_os(OS_SERVER) && is_os(OS_PROFESSIONAL) && !is_os(OS_HOME));
    CHECK(!!(v.wSuiteMask & VER_SUITE_SINGLEUSERTS) == !!is_os(OS_PERSONALTERMINALSERVER));
    OSVERSIONINFOEXA va = { sizeof va };
    CHECK(GetVersionExA((OSVERSIONINFOA *)&va) && va.wProductType == v.wProductType && va.wSuiteMask == v.wSuiteMask);
    OSVERSIONINFOEXW want = { sizeof want };
    want.wProductType = VER_NT_WORKSTATION;
    CHECK(VerifyVersionInfoW(&want, VER_PRODUCT_TYPE, VerSetConditionMask(0, VER_PRODUCT_TYPE, VER_EQUAL)));
    DWORD product = 0;
    CHECK(GetProductInfo(10, 0, 0, 0, &product) && product == PRODUCT_PROFESSIONAL);
    // and the one GetSystemMetrics and IsWow64Process describe
    CHECK(!!is_os(OS_TABLETPC) == !!GetSystemMetrics(SM_TABLETPC));
    CHECK(!!is_os(OS_MEDIACENTER) == !!GetSystemMetrics(SM_MEDIACENTER));
    CHECK(!!is_os(OS_TERMINALCLIENT) == !!GetSystemMetrics(SM_REMOTESESSION));
    BOOL wow = TRUE;
    CHECK(IsWow64Process(GetCurrentProcess(), &wow) && !!wow == !!is_os(OS_WOW6432));
}

// ---- downlevel API sets --------------------------------------------------------------------------------------------------
static void test_downlevel(void) {
    HMODULE set = LoadLibraryA("api-ms-win-downlevel-shell32-l1-1-0.dll"), shell32 = LoadLibraryA("shell32.dll");
    CHECK(set != NULL && shell32 != NULL);
    FARPROC a = GetProcAddress(set, "CommandLineToArgvW"), b = GetProcAddress(shell32, "CommandLineToArgvW");
    CHECK(a != NULL && a == b);
    if (a) {
        int argc = 0;
        LPWSTR *argv = ((LPWSTR *(WINAPI *)(LPCWSTR, int *))a)(L"made-up.exe \"two words\" three", &argc);
        CHECK(argv && argc == 3 && !wcscmp(argv[1], L"two words") && !wcscmp(argv[2], L"three"));
        if (argv) LocalFree(argv);
    }
    HMODULE adv = LoadLibraryA("api-ms-win-downlevel-advapi32-l1-1-0.dll");
    CHECK(adv && GetProcAddress(adv, "RegOpenKeyExW") == GetProcAddress(LoadLibraryA("advapi32.dll"), "RegOpenKeyExW"));
    HMODULE shl = LoadLibraryA("api-ms-win-downlevel-shlwapi-l1-1-0.dll");
    CHECK(shl && GetProcAddress(shl, "IsOS") != NULL);
}

// ---- touch and pointer input ------------------------------------------------------------------------------------------------
static void test_pointer(void) {
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"VxrtPointerTest";
    CHECK(RegisterClassW(&wc));
    HWND w = CreateWindowExW(0, L"VxrtPointerTest", L"pointer test", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, NULL, NULL, wc.hInstance, NULL);
    CHECK(w != NULL);
    HWND bogus = (HWND)(ULONG_PTR)0x7ff0;

    UINT32 n = 99;
    CHECK(GetPointerDevices(&n, NULL) && n == 0);                      // no touch screen, no pen
    POINTER_DEVICE_INFO devices[2];
    n = 2;
    CHECK(GetPointerDevices(&n, devices) && n == 0);
    SetLastError(0);
    CHECK(!GetPointerDevices(NULL, NULL) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK(GetSystemMetrics(SM_DIGITIZER) == 0 && GetSystemMetrics(SM_MAXIMUMTOUCHES) == 0);
    POINTER_DEVICE_INFO info;
    RECT r1, r2;
    UINT32 count = 4;
    SetLastError(0);
    CHECK(!GetPointerDevice((HANDLE)(ULONG_PTR)0x1234, &info) && GetLastError() == ERROR_INVALID_HANDLE);
    CHECK(!GetPointerDeviceRects((HANDLE)(ULONG_PTR)0x1234, &r1, &r2) && GetLastError() == ERROR_INVALID_HANDLE);
    CHECK(!GetPointerDeviceProperties((HANDLE)(ULONG_PTR)0x1234, &count, NULL) && GetLastError() == ERROR_INVALID_HANDLE);
    CHECK(RegisterPointerDeviceNotifications(w, FALSE) && RegisterPointerDeviceNotifications(w, TRUE));
    SetLastError(0);
    CHECK(!RegisterPointerDeviceNotifications(bogus, FALSE) && GetLastError() == ERROR_INVALID_WINDOW_HANDLE);

    POINTER_INPUT_TYPE type = 0;
    POINTER_INFO pi;
    POINTER_PEN_INFO pen;
    POINTER_TOUCH_INFO touch;
    SetLastError(0);
    CHECK(!GetPointerType(1, &type) && GetLastError() == ERROR_INVALID_PARAMETER);   // no WM_POINTER, no pointer ids
    CHECK(!GetPointerInfo(1, &pi) && !GetPointerPenInfo(2, &pen) && !GetPointerTouchInfo(3, &touch));
    count = 1;
    CHECK(!GetPointerFrameInfo(1, &count, &pi) && GetLastError() == ERROR_INVALID_PARAMETER);
    SetLastError(0);
    CHECK(!EnableMouseInPointer(TRUE) && GetLastError() == ERROR_NOT_SUPPORTED);     // not delivered, so not pretended
    CHECK(EnableMouseInPointer(FALSE) && !IsMouseInPointerEnabled());

    ULONG flags = 99;
    CHECK(!IsTouchWindow(w, &flags) && flags == 0);
    CHECK(RegisterTouchWindow(w, TWF_WANTPALM));
    CHECK(IsTouchWindow(w, &flags) && flags == TWF_WANTPALM);
    CHECK(RegisterTouchWindow(w, TWF_FINETOUCH) && IsTouchWindow(w, &flags) && flags == TWF_FINETOUCH);
    CHECK(UnregisterTouchWindow(w) && !IsTouchWindow(w, NULL));
    SetLastError(0);
    CHECK(!RegisterTouchWindow(bogus, 0) && GetLastError() == ERROR_INVALID_WINDOW_HANDLE);
    CHECK(RegisterTouchWindow(w, 0));
    TOUCHINPUT ti;
    SetLastError(0);
    CHECK(!GetTouchInputInfo((HTOUCHINPUT)(ULONG_PTR)0x1234, 1, &ti, sizeof ti) && GetLastError() == ERROR_INVALID_HANDLE);
    CHECK(!CloseTouchInputHandle((HTOUCHINPUT)(ULONG_PTR)0x1234) && GetLastError() == ERROR_INVALID_HANDLE);
    DestroyWindow(w);
    CHECK(!IsTouchWindow(w, NULL));                                     // gone with the window
}

// ---- RegNotifyChangeKeyValue -------------------------------------------------------------------------------------------------
#define BASE L"Software\\VXRT Test\\Notify"
static const DWORD LAST_SET = REG_NOTIFY_CHANGE_LAST_SET, NAME = REG_NOTIFY_CHANGE_NAME;
static int signalled(HANDLE e, DWORD ms) { return WaitForSingleObject(e, ms) == WAIT_OBJECT_0; }
static void set_dword(HKEY k, const WCHAR *name, DWORD v) { CHECK(RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, 4) == ERROR_SUCCESS); }

typedef struct { HKEY key; DWORD filter; HANDLE event; LONG result; volatile LONG returned; } notify_args;
static DWORD WINAPI ask_and_end(void *p) {                      // asks asynchronously, then the thread ends
    notify_args *a = p;
    a->result = RegNotifyChangeKeyValue(a->key, FALSE, a->filter, a->event, TRUE);
    return 0;
}
static DWORD WINAPI ask_and_wait(void *p) {                     // asks synchronously: returns after the change
    notify_args *a = p;
    a->result = RegNotifyChangeKeyValue(a->key, FALSE, a->filter, NULL, FALSE);
    InterlockedExchange(&a->returned, 1);
    return 0;
}

static void test_regnotify(void) {
    HKEY base, child;
    RegDeleteKeyW(HKEY_CURRENT_USER, BASE L"\\Child\\Grandchild");
    RegDeleteKeyW(HKEY_CURRENT_USER, BASE L"\\Child");
    CHECK(RegCreateKeyExW(HKEY_CURRENT_USER, BASE, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &base, NULL) == ERROR_SUCCESS);
    CHECK(RegCreateKeyExW(base, L"Child", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &child, NULL) == ERROR_SUCCESS);
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);

    // A value set: signalled once, then the caller has to ask again.
    CHECK(RegNotifyChangeKeyValue(base, FALSE, LAST_SET, ev, TRUE) == ERROR_SUCCESS);
    CHECK(!signalled(ev, 0));
    set_dword(base, L"made-up", 1);
    CHECK(signalled(ev, 2000));
    ResetEvent(ev);
    set_dword(base, L"made-up", 2);
    CHECK(!signalled(ev, 50));
    // A deleted value counts as a change of the values too.
    CHECK(RegNotifyChangeKeyValue(base, FALSE, LAST_SET, ev, TRUE) == ERROR_SUCCESS);
    CHECK(RegDeleteValueW(base, L"made-up") == ERROR_SUCCESS && signalled(ev, 2000));
    ResetEvent(ev);

    // The filter: values do not wake a REG_NOTIFY_CHANGE_NAME watch; a new subkey does.
    CHECK(RegNotifyChangeKeyValue(base, FALSE, NAME, ev, TRUE) == ERROR_SUCCESS);
    set_dword(base, L"other", 3);
    CHECK(!signalled(ev, 50));
    HKEY sib;
    CHECK(RegCreateKeyExW(base, L"Sibling", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &sib, NULL) == ERROR_SUCCESS);
    CHECK(signalled(ev, 2000));
    ResetEvent(ev);
    RegCloseKey(sib);
    // A deleted subkey is a REG_NOTIFY_CHANGE_NAME change of its parent.
    CHECK(RegNotifyChangeKeyValue(base, FALSE, NAME, ev, TRUE) == ERROR_SUCCESS);
    CHECK(RegDeleteKeyW(base, L"Sibling") == ERROR_SUCCESS && signalled(ev, 2000));
    ResetEvent(ev);

    // Below the key: only with bWatchSubtree.
    CHECK(RegNotifyChangeKeyValue(base, FALSE, LAST_SET, ev, TRUE) == ERROR_SUCCESS);
    set_dword(child, L"deep", 1);
    CHECK(!signalled(ev, 50));
    set_dword(base, L"other", 4);                                  // (takes that watch)
    CHECK(signalled(ev, 2000));
    ResetEvent(ev);
    CHECK(RegNotifyChangeKeyValue(base, TRUE, LAST_SET, ev, TRUE) == ERROR_SUCCESS);
    set_dword(child, L"deep", 2);
    CHECK(signalled(ev, 2000));
    ResetEvent(ev);
    HKEY grand;                                                    // a new key two levels down, through the parent
    CHECK(RegNotifyChangeKeyValue(base, TRUE, NAME, ev, TRUE) == ERROR_SUCCESS);
    CHECK(RegCreateKeyExW(base, L"Child\\Grandchild", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &grand, NULL) == ERROR_SUCCESS);
    CHECK(signalled(ev, 2000));
    ResetEvent(ev);
    RegCloseKey(grand);
    RegDeleteKeyW(child, L"Grandchild");

    // Several watches at once, each told; a watch through a predefined key.
    HANDLE ev2 = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(RegNotifyChangeKeyValue(base, FALSE, LAST_SET, ev, TRUE) == ERROR_SUCCESS);
    CHECK(RegNotifyChangeKeyValue(HKEY_CURRENT_USER, TRUE, LAST_SET, ev2, TRUE) == ERROR_SUCCESS);
    set_dword(base, L"other", 5);
    CHECK(signalled(ev, 2000) && signalled(ev2, 2000));
    ResetEvent(ev); ResetEvent(ev2);

    // The program's event handle may be closed meanwhile: the notification still reaches the event.
    HANDLE keep;
    CHECK(DuplicateHandle(GetCurrentProcess(), ev2, GetCurrentProcess(), &keep, 0, FALSE, DUPLICATE_SAME_ACCESS));
    CHECK(RegNotifyChangeKeyValue(base, FALSE, LAST_SET, ev2, TRUE) == ERROR_SUCCESS);
    CloseHandle(ev2);
    set_dword(base, L"other", 6);
    CHECK(signalled(keep, 2000));
    CloseHandle(keep);

    // Closing the key's handle signals; so does deleting the key itself, after which asking fails.
    HKEY other;
    CHECK(RegOpenKeyExW(HKEY_CURRENT_USER, BASE, 0, KEY_READ, &other) == ERROR_SUCCESS);
    CHECK(RegNotifyChangeKeyValue(other, FALSE, LAST_SET, ev, TRUE) == ERROR_SUCCESS);
    CHECK(!signalled(ev, 0));
    RegCloseKey(other);
    CHECK(signalled(ev, 2000));
    ResetEvent(ev);
    CHECK(RegNotifyChangeKeyValue(child, FALSE, LAST_SET, ev, TRUE) == ERROR_SUCCESS);
    CHECK(RegDeleteKeyW(base, L"Child") == ERROR_SUCCESS);
    CHECK(signalled(ev, 2000));
    ResetEvent(ev);
    CHECK(RegNotifyChangeKeyValue(child, FALSE, LAST_SET, ev, TRUE) == ERROR_KEY_DELETED);
    RegCloseKey(child);

    // The thread that asked ends: signalled, unless it asked with REG_NOTIFY_THREAD_AGNOSTIC.
    notify_args a = { base, LAST_SET, ev };
    HANDLE t = CreateThread(NULL, 0, ask_and_end, &a, 0, NULL);
    CHECK(t && WaitForSingleObject(t, 5000) == WAIT_OBJECT_0);
    CloseHandle(t);
    CHECK(a.result == ERROR_SUCCESS && signalled(ev, 2000));
    ResetEvent(ev);
    a.filter = LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC;
    t = CreateThread(NULL, 0, ask_and_end, &a, 0, NULL);
    CHECK(t && WaitForSingleObject(t, 5000) == WAIT_OBJECT_0);
    CloseHandle(t);
    CHECK(a.result == ERROR_SUCCESS && !signalled(ev, 100));
    set_dword(base, L"other", 7);
    CHECK(signalled(ev, 2000));
    ResetEvent(ev);

    // Synchronous: the call returns once the change has happened.
    notify_args s = { base, LAST_SET, NULL, -1, 0 };
    t = CreateThread(NULL, 0, ask_and_wait, &s, 0, NULL);
    CHECK(t != NULL);
    Sleep(150);
    CHECK(!s.returned);
    set_dword(base, L"other", 8);
    CHECK(WaitForSingleObject(t, 5000) == WAIT_OBJECT_0 && s.returned && s.result == ERROR_SUCCESS);
    CloseHandle(t);

    // Refusals.
    HANDLE mutex = CreateMutexW(NULL, FALSE, NULL);
    CHECK(RegNotifyChangeKeyValue(base, FALSE, 0x100, ev, TRUE) == ERROR_INVALID_PARAMETER);
    CHECK(RegNotifyChangeKeyValue((HKEY)(ULONG_PTR)0x1234, FALSE, LAST_SET, ev, TRUE) == ERROR_INVALID_HANDLE);
    CHECK(RegNotifyChangeKeyValue(base, FALSE, LAST_SET, NULL, TRUE) == ERROR_INVALID_PARAMETER);
    CHECK(RegNotifyChangeKeyValue(base, FALSE, LAST_SET, mutex, TRUE) == ERROR_INVALID_HANDLE);
    CloseHandle(mutex);

    RegCloseKey(base);
    CHECK(RegDeleteKeyW(HKEY_CURRENT_USER, BASE) == ERROR_SUCCESS);
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\VXRT Test");
    CloseHandle(ev);
}

// ---- NtQuerySection -----------------------------------------------------------------------------------------------------------
typedef struct { void *base; ULONG attributes; LARGE_INTEGER size; } SECTION_BASIC;
typedef LONG (NTAPI *query_section_fn)(HANDLE, int, void *, SIZE_T, SIZE_T *);
#define SEC_FILE_ 0x800000
static void test_section(void) {
    query_section_fn query = (query_section_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySection");
    CHECK(query != NULL);
    if (!query) return;
    HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 65536, NULL);
    CHECK(m != NULL);
    SECTION_BASIC b;
    memset(&b, 0x55, sizeof b);
    SIZE_T got = 0;
    CHECK(sizeof b == 24 && query(m, 0, &b, sizeof b, &got) == 0 && got == 24);
    CHECK(b.base == NULL && b.attributes == SEC_COMMIT && b.size.QuadPart == 65536);
    CHECK(query(m, 0, &b, 16, &got) == (LONG)0xC0000004);            // STATUS_INFO_LENGTH_MISMATCH
    CHECK(query(m, 1, &b, 64, &got) == (LONG)0xC0000049);            // STATUS_SECTION_NOT_IMAGE
    CHECK(query(m, 7, &b, sizeof b, &got) == (LONG)0xC0000003);      // STATUS_INVALID_INFO_CLASS
    HANDLE dup;                                                        // the same section through another handle
    CHECK(DuplicateHandle(GetCurrentProcess(), m, GetCurrentProcess(), &dup, FILE_MAP_READ, FALSE, 0));
    CHECK(query(dup, 0, &b, sizeof b, NULL) == 0 && b.size.QuadPart == 65536);
    CloseHandle(dup);
    CloseHandle(m);

    m = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_RESERVE, 0, 1 << 20, NULL);
    CHECK(m && query(m, 0, &b, sizeof b, NULL) == 0 && b.attributes == SEC_RESERVE && b.size.QuadPart == 1 << 20);
    CloseHandle(m);

    char path[MAX_PATH];
    GetTempPathA(sizeof path, path);
    strcat(path, "vxrt_section_test.bin");
    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(f != INVALID_HANDLE_VALUE);
    char data[4096];
    memset(data, 'x', sizeof data);
    DWORD wrote = 0;
    CHECK(WriteFile(f, data, sizeof data, &wrote, NULL) && wrote == sizeof data);
    m = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
    CHECK(m && query(m, 0, &b, sizeof b, NULL) == 0 && b.attributes == SEC_FILE_ && b.size.QuadPart == 4096);
    CHECK(query(m, 1, &b, 64, NULL) == (LONG)0xC0000049);
    CloseHandle(m);
    CloseHandle(f);
    DeleteFileA(path);

    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    CHECK(query(ev, 0, &b, sizeof b, NULL) == (LONG)0xC0000024);     // STATUS_OBJECT_TYPE_MISMATCH
    CloseHandle(ev);
    CHECK(query((HANDLE)(ULONG_PTR)0x7ff4, 0, &b, sizeof b, NULL) == (LONG)0xC0000008);   // STATUS_INVALID_HANDLE
}

int main(void) {
    test_isos();
    test_downlevel();
    test_pointer();
    test_regnotify();
    test_section();
    if (failures) { printf("webhelper_apis: %d checks failed\n", failures); return 1; }
    printf("webhelper_apis: all checks passed\n");
    return 0;
}
