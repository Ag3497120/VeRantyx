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
#include <math.h>
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

enum { PMSF_NORMAL = 0, PMSF_MULTIPLE = 1, PMSF_DONT_STRIP_SPACES = 0x10000 };
// ---- shlwapi path strings ---------------------------------------------------------------------------------------------
static void test_paths(void) {
    HMODULE shlwapi = LoadLibraryA("shlwapi.dll");
    LPWSTR (WINAPI *find_name)(LPCWSTR) = (LPWSTR (WINAPI *)(LPCWSTR))GetProcAddress(shlwapi, "PathFindFileNameW");
    LPWSTR (WINAPI *find_ext)(LPCWSTR) = (LPWSTR (WINAPI *)(LPCWSTR))GetProcAddress(shlwapi, "PathFindExtensionW");
    void (WINAPI *remove_ext)(LPWSTR) = (void (WINAPI *)(LPWSTR))GetProcAddress(shlwapi, "PathRemoveExtensionW");
    BOOL (WINAPI *match)(LPCWSTR, LPCWSTR) = (BOOL (WINAPI *)(LPCWSTR, LPCWSTR))GetProcAddress(shlwapi, "PathMatchSpecW");
    BOOL (WINAPI *match_a)(LPCSTR, LPCSTR) = (BOOL (WINAPI *)(LPCSTR, LPCSTR))GetProcAddress(shlwapi, "PathMatchSpecA");
    HRESULT (WINAPI *match_ex)(LPCWSTR, LPCWSTR, DWORD) = (HRESULT (WINAPI *)(LPCWSTR, LPCWSTR, DWORD))GetProcAddress(shlwapi, "PathMatchSpecExW");
    LPSTR (WINAPI *find_ext_a)(LPCSTR) = (LPSTR (WINAPI *)(LPCSTR))GetProcAddress(shlwapi, "PathFindExtensionA");
    CHECK(find_name && find_ext && remove_ext && match && match_a && match_ex && find_ext_a);
    if (!find_name || !find_ext || !remove_ext || !match || !match_a || !match_ex || !find_ext_a) return;
    const WCHAR *p = L"C:\\made up\\dir.d\\file.name.txt";
    CHECK(!wcscmp(find_name(p), L"file.name.txt") && !wcscmp(find_ext(p), L".txt"));
    CHECK(!wcscmp(find_name(L"C:\\dir\\sub\\"), L"sub\\") && !wcscmp(find_name(L"c:file"), L"file"));
    CHECK(!wcscmp(find_name(L"plain"), L"plain") && !wcscmp(find_name(L"a/b"), L"b"));
    const WCHAR *no_ext = L"C:\\dir.d\\file";
    CHECK(find_ext(no_ext) == no_ext + wcslen(no_ext));                   // the terminating NUL
    CHECK(!strcmp(find_ext_a("x\\y.tar.gz"), ".gz"));
    WCHAR buf[64];
    wcscpy(buf, L"C:\\dir\\file.txt");
    remove_ext(buf);
    CHECK(!wcscmp(buf, L"C:\\dir\\file"));
    wcscpy(buf, L"C:\\dir.d\\file");
    remove_ext(buf);
    CHECK(!wcscmp(buf, L"C:\\dir.d\\file"));
    CHECK(match(L"C:\\dir\\File.TXT", L"*.txt") && !match(L"file.txt", L"*.exe"));
    CHECK(match(L"file.txt", L"*.exe; *.txt") && match(L"file.txt", L"*.exe;*.TXT"));
    CHECK(match(L"noext", L"*.*") && match(L"a.b", L"?.?") && !match(L"ab.b", L"?.?"));
    CHECK(match(L"anything", L"*") && !match(L"anything", L"") && match_a("Setup.EXE", "*.exe"));
    CHECK(match_ex(L"file.txt", L"*.exe; *.txt", PMSF_MULTIPLE) == S_OK);
    CHECK(match_ex(L"file.txt", L"*.exe; *.txt", PMSF_NORMAL) == S_FALSE);   // one pattern: the ";" is a character
    CHECK(match_ex(L"file.txt", L" *.txt", PMSF_NORMAL) == S_OK && match_ex(L"file.txt", L" *.txt", PMSF_DONT_STRIP_SPACES) == S_FALSE);
    CHECK(match_ex(L"file.txt", L"*", 0x40) == E_INVALIDARG);
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

    // The keyboard layout (US English) has no IME, so no IME file name.
    UINT (WINAPI *ime_file)(HKL, LPWSTR, UINT) = (UINT (WINAPI *)(HKL, LPWSTR, UINT))GetProcAddress(LoadLibraryA("imm32.dll"), "ImmGetIMEFileNameW");
    WCHAR ime[64] = L"x";
    CHECK(ime_file && ime_file(GetKeyboardLayout(0), ime, 64) == 0 && ime[0] == 0 && ime_file(GetKeyboardLayout(0), NULL, 0) == 0);
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

// ---- wevtapi: no Windows event log here ---------------------------------------------------------------------------------
typedef HANDLE (WINAPI *evt_query_fn)(HANDLE, LPCWSTR, LPCWSTR, DWORD);
typedef BOOL (WINAPI *evt_next_fn)(HANDLE, DWORD, HANDLE *, DWORD, DWORD, DWORD *);
typedef HANDLE (WINAPI *evt_context_fn)(DWORD, LPCWSTR *, DWORD);
typedef BOOL (WINAPI *evt_render_fn)(HANDLE, HANDLE, DWORD, DWORD, void *, DWORD *, DWORD *);
typedef BOOL (WINAPI *evt_close_fn)(HANDLE);
static void test_wevtapi(void) {
    HMODULE m = LoadLibraryA("wevtapi.dll");
    CHECK(m != NULL);
    if (!m) return;
    evt_query_fn query = (evt_query_fn)GetProcAddress(m, "EvtQuery");
    evt_next_fn next = (evt_next_fn)GetProcAddress(m, "EvtNext");
    evt_context_fn context = (evt_context_fn)GetProcAddress(m, "EvtCreateRenderContext");
    evt_render_fn render = (evt_render_fn)GetProcAddress(m, "EvtRender");
    evt_close_fn close = (evt_close_fn)GetProcAddress(m, "EvtClose");
    CHECK(query && next && context && render && close);
    if (!query || !next || !context || !render || !close) return;
    HANDLE q = query(NULL, L"application", L"*[System[(Level=2)]]", 1 | 0x200);   // channel path, newest first
    CHECK(q != NULL);
    HANDLE events[4] = { 0 };
    DWORD got = 99;
    SetLastError(0);
    CHECK(!next(q, 4, events, INFINITE, 0, &got) && GetLastError() == ERROR_NO_MORE_ITEMS && got == 0);
    CHECK(close(q));
    SetLastError(0);
    CHECK(!close(q) && GetLastError() == ERROR_INVALID_HANDLE);                    // closed already
    CHECK(!next(q, 4, events, 0, 0, &got) && GetLastError() == ERROR_INVALID_HANDLE);
    SetLastError(0);
    CHECK(!query(NULL, L"Made-Up-Channel/Operational", NULL, 1) && GetLastError() == 15007);   // ERROR_EVT_CHANNEL_NOT_FOUND
    CHECK(!query(NULL, L"Security", NULL, 1) && GetLastError() == ERROR_ACCESS_DENIED);
    CHECK(!query(NULL, L"C:\\made-up.evtx", NULL, 2) && GetLastError() == ERROR_NOT_SUPPORTED);
    CHECK(!query((HANDLE)(ULONG_PTR)0x1234, L"System", NULL, 1) && GetLastError() == ERROR_INVALID_HANDLE);
    const WCHAR *xml = L"<QueryList><Query Id='0' Path='System'><Select Path='System'>*</Select>"
                       L"<Select Path=\"Made-Up/Debug\">*</Select></Query></QueryList>";
    SetLastError(0);
    CHECK(!query(NULL, NULL, xml, 1) && GetLastError() == 15007);
    q = query(NULL, NULL, xml, 1 | 0x1000);                                         // EvtQueryTolerateQueryErrors
    CHECK(q != NULL && close(q));
    LPCWSTR paths[] = { L"Event/System/TimeCreated/@SystemTime" };
    HANDLE c = context(1, paths, 0);
    HANDLE sys = context(0, NULL, 1);
    CHECK(c && sys && !context(0, NULL, 0) && !context(1, paths, 1));
    char buf[64];
    DWORD used = 9, count = 9;
    SetLastError(0);
    CHECK(!render(c, (HANDLE)(ULONG_PTR)0x1234, 0, sizeof buf, buf, &used, &count) && GetLastError() == ERROR_INVALID_HANDLE);
    CHECK(close(c) && close(sys));
}

// ---- interrupt time -------------------------------------------------------------------------------------------------------
static void test_interrupt_time(void) {
    HMODULE set = LoadLibraryA("api-ms-win-core-realtime-l1-1-1.dll");
    void (WINAPI *unbiased_precise)(PULONGLONG) = (void (WINAPI *)(PULONGLONG))GetProcAddress(set, "QueryUnbiasedInterruptTimePrecise");
    void (WINAPI *precise)(PULONGLONG) = (void (WINAPI *)(PULONGLONG))GetProcAddress(set, "QueryInterruptTimePrecise");
    void (WINAPI *coarse)(PULONGLONG) = (void (WINAPI *)(PULONGLONG))GetProcAddress(set, "QueryInterruptTime");
    BOOL (WINAPI *unbiased)(PULONGLONG) = (BOOL (WINAPI *)(PULONGLONG))GetProcAddress(LoadLibraryA("kernel32.dll"), "QueryUnbiasedInterruptTime");
    CHECK(unbiased_precise && precise && coarse && unbiased);
    if (!unbiased_precise || !precise || !coarse || !unbiased) return;
    ULONGLONG u0, u1, i0, i1, c, ub;
    LARGE_INTEGER qpc0, qpc1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&qpc0);
    unbiased_precise(&u0);
    precise(&i0);
    QueryPerformanceCounter(&qpc1);
    Sleep(30);
    unbiased_precise(&u1);
    precise(&i1);
    coarse(&c);
    CHECK(unbiased(&ub) && ub >= u1);
    CHECK(i0 >= u0 && i1 >= u1);                                       // interrupt time also counts time asleep
    CHECK(u1 - u0 >= 290000 && u1 - u0 < 50000000);                    // 30 ms in 100 ns units (generous above)
    CHECK(i1 - i0 >= 290000 && c >= i1);
    // the same clock as QueryPerformanceCounter: the counter in 100 ns units, within a millisecond
    ULONGLONG q = (ULONGLONG)((qpc0.QuadPart / 2 + qpc1.QuadPart / 2) / (freq.QuadPart / 10000000));
    CHECK(q > u0 ? q - u0 < 10000 : u0 - q < 10000);
    CHECK(!unbiased(NULL) && GetLastError() == ERROR_INVALID_PARAMETER);
}

// ---- power information ----------------------------------------------------------------------------------------------------
typedef struct { ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState; } PROCESSOR_POWER;
static void test_power(void) {
    HMODULE base = LoadLibraryA("api-ms-win-power-base-l1-1-0.dll"), setting = LoadLibraryA("api-ms-win-power-setting-l1-1-0.dll");
    LONG (WINAPI *info)(int, void *, ULONG, void *, ULONG) = (LONG (WINAPI *)(int, void *, ULONG, void *, ULONG))GetProcAddress(base, "CallNtPowerInformation");
    int (WINAPI *role)(ULONG) = (int (WINAPI *)(ULONG))GetProcAddress(base, "PowerDeterminePlatformRoleEx");
    DWORD (WINAPI *active)(HKEY, GUID **) = (DWORD (WINAPI *)(HKEY, GUID **))GetProcAddress(setting, "PowerGetActiveScheme");
    DWORD (WINAPI *read_ac)(HKEY, const GUID *, const GUID *, const GUID *, PULONG, LPBYTE, LPDWORD) =
        (DWORD (WINAPI *)(HKEY, const GUID *, const GUID *, const GUID *, PULONG, LPBYTE, LPDWORD))GetProcAddress(setting, "PowerReadACValue");
    CHECK(info && role && active && read_ac);
    CHECK(GetProcAddress(LoadLibraryA("powrprof.dll"), "CallNtPowerInformation") == (FARPROC)info);
    if (!info || !role || !active || !read_ac) return;
    // the same machine GetSystemPowerStatus describes: AC power, no battery
    SYSTEM_POWER_STATUS ps;
    CHECK(GetSystemPowerStatus(&ps) && ps.ACLineStatus == 1 && (ps.BatteryFlag & 128));
    SYSTEM_BATTERY_STATE bs;
    memset(&bs, 0x55, sizeof bs);
    CHECK(sizeof bs == 32 && info(SystemBatteryState, NULL, 0, &bs, sizeof bs) == 0);
    CHECK(bs.AcOnLine && !bs.BatteryPresent && !bs.Charging && !bs.Discharging && bs.MaxCapacity == 0);
    CHECK(info(SystemBatteryState, NULL, 0, &bs, 16) == (LONG)0xC0000023);   // STATUS_BUFFER_TOO_SMALL
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    PROCESSOR_POWER cpus[256];
    CHECK(si.dwNumberOfProcessors <= 256 && info(ProcessorInformation, NULL, 0, cpus, si.dwNumberOfProcessors * sizeof *cpus) == 0);
    CHECK(cpus[0].Number == 0 && cpus[0].MaxMhz > 0 && cpus[0].CurrentMhz <= cpus[0].MaxMhz);
    CHECK(cpus[si.dwNumberOfProcessors - 1].Number == si.dwNumberOfProcessors - 1);
    CHECK(info(ProcessorInformation, NULL, 0, cpus, (si.dwNumberOfProcessors - 1) * sizeof *cpus) == (LONG)0xC0000023);
    CHECK(info(SystemPowerCapabilities, NULL, 0, cpus, sizeof cpus) != 0);         // not provided
    CHECK(role(POWER_PLATFORM_ROLE_V2) == PlatformRoleDesktop && role(99) == PlatformRoleUnspecified);
    GUID *scheme = NULL;
    static const GUID balanced = { 0x381b4222, 0xf694, 0x41f0, { 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e } };
    CHECK(active(NULL, &scheme) == 0 && scheme && !memcmp(scheme, &balanced, sizeof balanced));
    ULONG type = 0; BYTE value[8]; DWORD size = sizeof value;
    CHECK(read_ac(NULL, scheme, &balanced, &balanced, &type, value, &size) == ERROR_FILE_NOT_FOUND);
    if (scheme) CHECK(LocalFree(scheme) == NULL);
}

// ---- GDI font heights: LOGFONT's height as Windows maps it, reported back by GetTextMetrics -------------------------------
static void test_font_heights(void) {
    HDC dc = CreateCompatibleDC(NULL);
    CHECK(dc != NULL);
    static const WCHAR *faces[] = { L"Arial", L"Segoe UI", L"" };
    for (int f = 0; f < 3; f++)
        for (int h = 8; h <= 40; h += 7) {
            LOGFONTW lf = { 0 };
            wcscpy(lf.lfFaceName, faces[f]);
            lf.lfHeight = -h;                                        // the character (em) height
            HFONT font = CreateFontIndirectW(&lf);
            HGDIOBJ old = SelectObject(dc, font);
            TEXTMETRICW tm;
            CHECK(GetTextMetricsW(dc, &tm));
            CHECK(tm.tmHeight - tm.tmInternalLeading == h && tm.tmAscent + tm.tmDescent == tm.tmHeight);
            LOGFONTW back;
            CHECK(GetObjectW(font, sizeof back, &back) == sizeof back && back.lfHeight == -h);
            SelectObject(dc, old);
            DeleteObject(font);
            lf.lfHeight = h;                                         // the cell height
            font = CreateFontIndirectW(&lf);
            old = SelectObject(dc, font);
            CHECK(GetTextMetricsW(dc, &tm) && tm.tmHeight == h && tm.tmInternalLeading >= 0 && tm.tmInternalLeading < h);
            SelectObject(dc, old);
            DeleteObject(font);
        }
    DeleteDC(dc);
}

// ---- SystemParametersInfo's UI fonts ------------------------------------------------------------------------------------------
static void test_ui_fonts(void) {
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0x55, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    CHECK(sizeof ncm == 504 && SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0));
    CHECK(ncm.cbSize == sizeof ncm && ncm.lfMessageFont.lfHeight == -12 && ncm.lfMessageFont.lfWeight == FW_NORMAL);
    CHECK(!wcscmp(ncm.lfMessageFont.lfFaceName, L"Arial") && !wcscmp(ncm.lfCaptionFont.lfFaceName, L"Arial"));
    CHECK(!wcscmp(ncm.lfMenuFont.lfFaceName, L"Arial") && !wcscmp(ncm.lfStatusFont.lfFaceName, L"Arial") && !wcscmp(ncm.lfSmCaptionFont.lfFaceName, L"Arial"));
    CHECK(ncm.iScrollWidth == GetSystemMetrics(SM_CXVSCROLL) && ncm.iCaptionHeight == GetSystemMetrics(SM_CYSIZE));
    CHECK(ncm.iPaddedBorderWidth == GetSystemMetrics(SM_CXPADDEDBORDER) && ncm.iMenuHeight == GetSystemMetrics(SM_CYMENUSIZE));
    ncm.cbSize = sizeof ncm - 4;                                       // without iPaddedBorderWidth (before Vista)
    ncm.iPaddedBorderWidth = 77;
    CHECK(SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0) && ncm.iPaddedBorderWidth == 77 && ncm.lfMessageFont.lfHeight == -12);
    ncm.cbSize = 100;
    SetLastError(0);
    CHECK(!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, 100, &ncm, 0) && GetLastError() == ERROR_INVALID_PARAMETER);
    NONCLIENTMETRICSA ncma = { sizeof ncma };
    CHECK(sizeof ncma == 344 && SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncma, &ncma, 0));
    CHECK(!strcmp(ncma.lfMessageFont.lfFaceName, "Arial") && ncma.lfMessageFont.lfHeight == -12);
    LOGFONTW icon;
    CHECK(SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof icon, &icon, 0) && !wcscmp(icon.lfFaceName, L"Arial"));
    BOOL (WINAPI *for_dpi)(UINT, UINT, PVOID, UINT, UINT) = (BOOL (WINAPI *)(UINT, UINT, PVOID, UINT, UINT))GetProcAddress(GetModuleHandleA("user32.dll"), "SystemParametersInfoForDpi");
    CHECK(for_dpi != NULL);
    if (for_dpi) {
        ncm.cbSize = sizeof ncm;
        CHECK(for_dpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, 192) && ncm.lfMessageFont.lfHeight == -24);
        CHECK(for_dpi(SPI_GETICONTITLELOGFONT, sizeof icon, &icon, 0, 144) && icon.lfHeight == -18);
        SetLastError(0);
        CHECK(!for_dpi(SPI_GETWORKAREA, 0, &icon, 0, 96) && GetLastError() == ERROR_INVALID_PARAMETER);   // only the three
    }
    // the message font is a usable GDI font of 12 pixels' character height
    HDC dc = CreateCompatibleDC(NULL);
    HFONT f = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    DeleteObject(f);
    f = CreateFontIndirectW(&ncm.lfMessageFont);
    HGDIOBJ old = SelectObject(dc, f);
    TEXTMETRICW tm;
    CHECK(GetTextMetricsW(dc, &tm) && tm.tmHeight - tm.tmInternalLeading == 12);
    SelectObject(dc, old);
    DeleteObject(f);
    DeleteDC(dc);
}

// ---- dwmapi ----------------------------------------------------------------------------------------------------------------
typedef struct { UINT32 num, den; } RATIO;
static void test_dwmapi(void) {
    HMODULE m = LoadLibraryA("dwmapi.dll");
    CHECK(m != NULL);
    if (!m) return;
    HRESULT (WINAPI *enabled)(BOOL *) = (HRESULT (WINAPI *)(BOOL *))GetProcAddress(m, "DwmIsCompositionEnabled");
    HRESULT (WINAPI *extend)(HWND, const void *) = (HRESULT (WINAPI *)(HWND, const void *))GetProcAddress(m, "DwmExtendFrameIntoClientArea");
    BOOL (WINAPI *def)(HWND, UINT, WPARAM, LPARAM, LRESULT *) = (BOOL (WINAPI *)(HWND, UINT, WPARAM, LPARAM, LRESULT *))GetProcAddress(m, "DwmDefWindowProc");
    HRESULT (WINAPI *get)(HWND, DWORD, void *, DWORD) = (HRESULT (WINAPI *)(HWND, DWORD, void *, DWORD))GetProcAddress(m, "DwmGetWindowAttribute");
    HRESULT (WINAPI *set)(HWND, DWORD, const void *, DWORD) = (HRESULT (WINAPI *)(HWND, DWORD, const void *, DWORD))GetProcAddress(m, "DwmSetWindowAttribute");
    HRESULT (WINAPI *timing)(HWND, void *) = (HRESULT (WINAPI *)(HWND, void *))GetProcAddress(m, "DwmGetCompositionTimingInfo");
    CHECK(enabled && extend && def && get && set && timing);
    if (!enabled || !extend || !def || !get || !set || !timing) return;
    BOOL on = FALSE;
    CHECK(enabled(&on) == S_OK && on);
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"VxrtDwmTest";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(0, L"VxrtDwmTest", L"dwm test", WS_OVERLAPPEDWINDOW, 10, 20, 300, 200, NULL, NULL, wc.hInstance, NULL);
    CHECK(w != NULL);
    int margins[4] = { -1, -1, -1, -1 };
    CHECK(extend(w, margins) == S_OK && extend((HWND)(ULONG_PTR)0x7ff0, margins) == (HRESULT)0x80070006);
    LRESULT r = 99;
    CHECK(!def(w, WM_NCHITTEST, 0, 0, &r) && r == 0);
    RECT frame, wr;
    CHECK(get(w, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, &frame, sizeof frame) == S_OK && GetWindowRect(w, &wr));
    CHECK(!memcmp(&frame, &wr, sizeof wr));
    DWORD cloaked = 7;
    CHECK(get(w, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof cloaked) == S_OK && cloaked == 0);
    CHECK(get(w, 14, &cloaked, 2) == E_INVALIDARG && get(w, 2 /* set-only */, &cloaked, 4) == E_INVALIDARG);
    BOOL dark = TRUE;
    CHECK(set(w, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark) == S_OK);
    DWORD corner = 2;
    CHECK(set(w, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE, Windows 11 */, &corner, sizeof corner) == E_INVALIDARG);
    DestroyWindow(w);
    unsigned char t[292];
    memset(t, 0, sizeof t);
    *(UINT32 *)t = sizeof t;
    CHECK(timing(NULL, t) == S_OK);
    RATIO rate; ULONGLONG period, vblank, count;
    memcpy(&rate, t + 4, 8); memcpy(&period, t + 12, 8); memcpy(&vblank, t + 28, 8); memcpy(&count, t + 36, 8);
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
    double hz = (double)rate.num / rate.den;
    CHECK(rate.den && hz >= 24 && hz <= 500);
    CHECK(period > 0 && fabs((double)period / freq.QuadPart - 1 / hz) < 1e-4);   // one refresh, on the QPC clock
    CHECK(vblank <= (ULONGLONG)now.QuadPart && (ULONGLONG)now.QuadPart - vblank < 2 * period + freq.QuadPart / 10 && count > 0);
    *(UINT32 *)t = 100;
    CHECK(timing(NULL, t) == E_INVALIDARG);
}

// ---- smaller user32 functions -----------------------------------------------------------------------------------------------
static void test_user32_more(void) {
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"VxrtMoreTest";
    wc.style = CS_HREDRAW | CS_VREDRAW; wc.cbWndExtra = 16; wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    ATOM atom = RegisterClassW(&wc);
    CHECK(atom != 0);
    HWND top = CreateWindowExW(0, L"VxrtMoreTest", L"top", WS_OVERLAPPEDWINDOW, 0, 0, 300, 200, NULL, NULL, wc.hInstance, NULL);
    HWND child = CreateWindowExW(0, L"VxrtMoreTest", L"child", WS_CHILD, 0, 0, 50, 50, top, NULL, wc.hInstance, NULL);
    HWND grandchild = CreateWindowExW(0, L"VxrtMoreTest", L"grandchild", WS_CHILD, 0, 0, 10, 10, child, NULL, wc.hInstance, NULL);
    HWND owned = CreateWindowExW(0, L"VxrtMoreTest", L"owned", WS_POPUP, 0, 0, 10, 10, top, NULL, wc.hInstance, NULL);
    CHECK(top && child && grandchild && owned);
    CHECK(IsChild(top, child) && IsChild(top, grandchild) && IsChild(child, grandchild));
    CHECK(!IsChild(child, top) && !IsChild(top, top) && !IsChild(top, owned));   // an owned pop-up is no child
    WCHAR name[64];
    CHECK(GetClassNameW(child, name, 64) == 12 && !wcscmp(name, L"VxrtMoreTest"));
    CHECK(GetClassNameW(child, name, 5) == 4 && !wcscmp(name, L"Vxrt"));          // cut to the buffer
    CHECK(GetClassNameW((HWND)(ULONG_PTR)0x7ff0, name, 64) == 0);
    CHECK(GetClassLongPtrW(top, GCW_ATOM) == atom && GetClassLongPtrW(top, GCL_STYLE) == (CS_HREDRAW | CS_VREDRAW));
    CHECK(GetClassLongPtrW(top, GCL_CBWNDEXTRA) == 16 && GetClassLongPtrW(top, GCLP_WNDPROC) == (ULONG_PTR)DefWindowProcW);
    CHECK(GetClassLongPtrW(top, GCLP_HBRBACKGROUND) == (ULONG_PTR)(COLOR_WINDOW + 1) && GetClassLongPtrW(top, GCLP_HMODULE) == (ULONG_PTR)wc.hInstance);
    SetLastError(0);
    CHECK(GetClassLongPtrW(top, 8) == 0 && GetLastError() == ERROR_INVALID_INDEX);
    HRGN rgn = CreateRectRgn(0, 0, 1, 1);
    RECT box;
    CHECK(GetWindowRgn(top, rgn) == ERROR && GetWindowRgnBox(top, &box) == ERROR);   // windows are their rectangles here
    DeleteObject(rgn);

    HDWP dwp = BeginDeferWindowPos(2);
    CHECK(dwp != NULL);
    dwp = DeferWindowPos(dwp, child, NULL, 5, 6, 40, 30, SWP_NOZORDER | SWP_NOACTIVATE);
    CHECK(dwp != NULL);
    dwp = DeferWindowPos(dwp, owned, NULL, 0, 0, 77, 33, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT before;
    GetWindowRect(owned, &before);
    CHECK(dwp && before.right - before.left == 10);                    // nothing moves before EndDeferWindowPos
    CHECK(EndDeferWindowPos(dwp));
    RECT cr, orr;
    GetClientRect(child, &cr); GetWindowRect(owned, &orr);
    CHECK(cr.right == 40 && cr.bottom == 30 && orr.right - orr.left == 77 && orr.bottom - orr.top == 33);
    dwp = BeginDeferWindowPos(1);
    CHECK(DeferWindowPos(dwp, (HWND)(ULONG_PTR)0x7ff0, NULL, 0, 0, 1, 1, 0) == NULL && GetLastError() == ERROR_INVALID_WINDOW_HANDLE);
    NotifyWinEvent(EVENT_OBJECT_FOCUS, top, OBJID_CLIENT, CHILDID_SELF);
    DestroyWindow(owned); DestroyWindow(grandchild); DestroyWindow(child); DestroyWindow(top);

    RECT r = { 10, 20, 30, 40 }, s = { 10, 20, 30, 40 };
    CHECK(EqualRect(&r, &s) && InflateRect(&r, 2, -3) && r.left == 8 && r.top == 23 && r.right == 32 && r.bottom == 37 && !EqualRect(&r, &s));
    WCHAR text[] = L"made-up \u00e9t\u00e9 x";
    CHECK(CharUpperW(text) == text && !wcscmp(text, L"MADE-UP \u00c9T\u00c9 X"));
    CHECK((WCHAR)(ULONG_PTR)CharUpperW((LPWSTR)(ULONG_PTR)L'q') == L'Q');
    const WCHAR pair[] = { 'a', 0xd83d, 0xde00, 'b', 0 };
    CHECK(CharNextW(pair) == pair + 1 && CharNextW(pair + 1) == pair + 3 && CharNextW(pair + 4) == pair + 4);
    LASTINPUTINFO li = { sizeof li };
    CHECK(GetLastInputInfo(&li) && li.dwTime <= GetTickCount());
    li.cbSize = 4;
    CHECK(!GetLastInputInfo(&li));
}

int main(void) {
    test_isos();
    test_paths();
    test_downlevel();
    test_pointer();
    test_regnotify();
    test_section();
    test_wevtapi();
    test_interrupt_time();
    test_power();
    test_font_heights();
    test_ui_fonts();
    test_dwmapi();
    test_user32_more();
    if (failures) { printf("webhelper_apis: %d checks failed\n", failures); return 1; }
    printf("webhelper_apis: all checks passed\n");
    return 0;
}
