// The session around the windows, as Chromium's sandbox and Steam's web helper use it: window stations and desktops
// (the process's WinSta0\Default, a window station made without a name, a desktop made in it, their names, flags and
// lifetimes, the rights each handle was opened with, the windows each desktop lists), power notifications, DPI
// awareness (user32 and shcore through its API set) and SetCoalescableTimer.
// Newer functions are looked up by name, as the programs do. All names are made up for the test.
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)

static int name_is(HANDLE h, const wchar_t *expected) {
    wchar_t buf[128]; DWORD need = 0;
    return GetUserObjectInformationW(h, UOI_NAME, buf, sizeof buf, &need) && !wcscmp(buf, expected) &&
           need == (wcslen(expected) + 1) * sizeof(wchar_t);
}

static HANDLE g_moved, g_done;
static DWORD WINAPI on_alternate_desktop(void *desk) {
    // A thread without windows may move to another desktop, and says so; it goes back before it ends.
    HDESK home = GetThreadDesktop(GetCurrentThreadId());
    if (!SetThreadDesktop((HDESK)desk)) return 1;
    if (GetThreadDesktop(GetCurrentThreadId()) != (HDESK)desk) return 2;
    SetEvent(g_moved);
    WaitForSingleObject(g_done, 5000);
    return SetThreadDesktop(home) ? 0 : 3;
}

static void stations_and_desktops(void) {
    // The process's: WinSta0 (visible) and its Default desktop, the input desktop.
    HWINSTA ws = GetProcessWindowStation();
    HDESK desk = GetThreadDesktop(GetCurrentThreadId());
    CHECK(ws != NULL && desk != NULL);
    CHECK(name_is(ws, L"WinSta0") && name_is(desk, L"Default"));
    char type[32]; DWORD need = 0;
    CHECK(GetUserObjectInformationA(ws, UOI_TYPE, type, sizeof type, &need) && !strcmp(type, "WindowStation") && need == 14);
    CHECK(GetUserObjectInformationA(desk, UOI_TYPE, type, sizeof type, &need) && !strcmp(type, "Desktop"));
    USEROBJECTFLAGS flags = { 0 };
    CHECK(GetUserObjectInformationW(ws, UOI_FLAGS, &flags, sizeof flags, &need) && (flags.dwFlags & WSF_VISIBLE) && need == sizeof flags);
    wchar_t small[3];
    SetLastError(0);
    CHECK(!GetUserObjectInformationW(ws, UOI_NAME, small, sizeof small, &need) && GetLastError() == ERROR_INSUFFICIENT_BUFFER && need == 16);
    BOOL io = FALSE; DWORD heap = 0;
    CHECK(GetUserObjectInformationW(desk, 6 /* UOI_IO */, &io, sizeof io, &need) && io);
    CHECK(GetUserObjectInformationW(desk, 5 /* UOI_HEAPSIZE */, &heap, sizeof heap, &need) && heap > 0);
    SetLastError(0);
    CHECK(!GetUserObjectInformationW(ws, 5, &heap, sizeof heap, &need) && GetLastError() == ERROR_INVALID_PARAMETER);
    SetLastError(0);
    CHECK(!GetUserObjectInformationW((HANDLE)(INT_PTR)0x1234, UOI_NAME, small, sizeof small, &need) && GetLastError() == ERROR_INVALID_HANDLE);
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    CHECK(input && input != desk && name_is(input, L"Default") && CloseDesktop(input));
    // What belongs to the process stays open.
    SetLastError(0);
    CHECK(!CloseDesktop(desk) && GetLastError() == ERROR_BUSY);
    SetLastError(0);
    CHECK(!CloseWindowStation(ws) && GetLastError() == ERROR_BUSY);
    SetLastError(0);
    CHECK(!CloseDesktop((HDESK)ws) && GetLastError() == ERROR_INVALID_HANDLE);

    // Only an administrator names a window station; without a name it is the logon session's.
    SetLastError(0);
    CHECK(!CreateWindowStationW(L"VxTestStation", 0, WINSTA_ALL_ACCESS, NULL) && GetLastError() == ERROR_ACCESS_DENIED);
    HWINSTA alt = CreateWindowStationW(NULL, 0, WINSTA_ALL_ACCESS, NULL);
    CHECK(alt != NULL);
    wchar_t alt_name[128] = { 0 };
    CHECK(GetUserObjectInformationW(alt, UOI_NAME, alt_name, sizeof alt_name, &need) && !wcsncmp(alt_name, L"Service-0x", 10));
    CHECK(GetUserObjectInformationW(alt, UOI_FLAGS, &flags, sizeof flags, &need) && !(flags.dwFlags & WSF_VISIBLE));
    SetLastError(0);
    CHECK(!CreateWindowStationW(NULL, CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, NULL) && GetLastError() == ERROR_ALREADY_EXISTS);
    HWINSTA again = OpenWindowStationW(alt_name, FALSE, WINSTA_ENUMDESKTOPS);
    CHECK(again && again != alt && name_is(again, alt_name) && CloseWindowStation(again));
    SetLastError(0);
    CHECK(!OpenWindowStationW(L"VxNoSuchStation", FALSE, WINSTA_ENUMDESKTOPS) && GetLastError() == ERROR_FILE_NOT_FOUND);

    // A desktop is made in the process's window station of the moment (Chromium's sandbox: switch, create, switch back).
    CHECK(SetProcessWindowStation(alt) && GetProcessWindowStation() == alt);
    SetLastError(0);
    CHECK(!CreateDesktopW(L"vx\\bad", NULL, NULL, 0, GENERIC_ALL, NULL) && GetLastError() == ERROR_INVALID_NAME);
    SetLastError(0);
    CHECK(!CreateDesktopW(L"vx_test_desktop", L"DISPLAY9", NULL, 0, GENERIC_ALL, NULL) && GetLastError() == ERROR_INVALID_PARAMETER);
    HDESK alt_desk = CreateDesktopW(L"vx_test_desktop", NULL, NULL, 0, GENERIC_ALL, NULL);
    CHECK(alt_desk && name_is(alt_desk, L"vx_test_desktop"));
    HDESK same = CreateDesktopW(L"VX_TEST_DESKTOP", NULL, NULL, 0, GENERIC_ALL, NULL);     // (names: any case)
    CHECK(same && same != alt_desk && GetLastError() == ERROR_ALREADY_EXISTS && name_is(same, L"vx_test_desktop") && CloseDesktop(same));
    HDESK opened = OpenDesktopW(L"vx_test_desktop", 0, FALSE, DESKTOP_READOBJECTS);
    CHECK(opened && CloseDesktop(opened));
    CHECK(SetProcessWindowStation(ws) && GetProcessWindowStation() == ws);
    SetLastError(0);
    CHECK(!OpenDesktopW(L"vx_test_desktop", 0, FALSE, DESKTOP_READOBJECTS) && GetLastError() == ERROR_FILE_NOT_FOUND);
    opened = OpenDesktopW(L"default", 0, FALSE, DESKTOP_READOBJECTS);
    CHECK(opened && name_is(opened, L"Default") && CloseDesktop(opened));
    // Flags and the handle's inheritance can be changed.
    USEROBJECTFLAGS set = { TRUE, FALSE, DF_ALLOWOTHERACCOUNTHOOK };
    CHECK(SetUserObjectInformationW(alt_desk, UOI_FLAGS, &set, sizeof set));
    memset(&flags, 0, sizeof flags);
    CHECK(GetUserObjectInformationW(alt_desk, UOI_FLAGS, &flags, sizeof flags, &need) && flags.fInherit && flags.dwFlags == DF_ALLOWOTHERACCOUNTHOOK);
    BOOL suppress = FALSE;
    CHECK(SetUserObjectInformationW(GetCurrentProcess(), UOI_TIMERPROC_EXCEPTION_SUPPRESSION, &suppress, sizeof suppress));

    // Threads: one moves to the other desktop, which stays open while it is there; the one with a window cannot.
    g_moved = CreateEventA(NULL, TRUE, FALSE, NULL); g_done = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, on_alternate_desktop, alt_desk, 0, NULL);
    DWORD tid = GetThreadId(t), code = 9;
    CHECK(WaitForSingleObject(g_moved, 5000) == WAIT_OBJECT_0);
    CHECK(GetThreadDesktop(tid) == alt_desk && GetThreadDesktop(GetCurrentThreadId()) == desk);
    SetLastError(0);
    CHECK(!CloseDesktop(alt_desk) && GetLastError() == ERROR_BUSY);
    SetEvent(g_done);
    WaitForSingleObject(t, 5000);
    CHECK(GetExitCodeThread(t, &code) && code == 0);
    CloseHandle(t); CloseHandle(g_moved); CloseHandle(g_done);
    HWND w = CreateWindowExA(0, "STATIC", "vx", WS_POPUP, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    CHECK(w != NULL);
    HDESK other = OpenDesktopW(L"Default", 0, FALSE, DESKTOP_READOBJECTS);
    CHECK(other && SetThreadDesktop(other));                        // (the same desktop: allowed)
    SetProcessWindowStation(alt);
    HDESK alt_desk2 = OpenDesktopW(L"vx_test_desktop", 0, FALSE, DESKTOP_READOBJECTS);
    SetProcessWindowStation(ws);
    SetLastError(0);
    CHECK(alt_desk2 && !SetThreadDesktop(alt_desk2) && GetLastError() == ERROR_BUSY);
    CHECK(SetThreadDesktop(desk));
    DestroyWindow(w);
    CHECK(CloseDesktop(other) && CloseDesktop(alt_desk2));
    // A window station goes with its last handle and its desktops: then it cannot be opened any more.
    CHECK(CloseWindowStation(alt));
    again = OpenWindowStationW(alt_name, FALSE, WINSTA_ENUMDESKTOPS);           // (its desktop still holds it)
    CHECK(again && CloseWindowStation(again));
    CHECK(CloseDesktop(alt_desk));
    SetLastError(0);
    CHECK(!OpenWindowStationW(alt_name, FALSE, WINSTA_ENUMDESKTOPS) && GetLastError() == ERROR_FILE_NOT_FOUND);
}

// What EnumDesktopWindows showed.
static HWND g_seen[32];
static int g_seen_count;
static BOOL CALLBACK collect(HWND w, LPARAM lp) { (void)lp; if (g_seen_count < 32) g_seen[g_seen_count++] = w; return TRUE; }
static int seen(HWND w) { for (int i = 0; i < g_seen_count; i++) if (g_seen[i] == w) return 1; return 0; }
static int enumerate(HDESK desk) { g_seen_count = 0; return EnumDesktopWindows(desk, collect, 0); }

static HANDLE g_window_made, g_window_done;
static HWND g_alt_window;
static DWORD WINAPI window_on_alternate_desktop(void *desk) {
    // A thread on the other desktop makes a window there; it destroys it and goes back before it ends.
    HDESK home = GetThreadDesktop(GetCurrentThreadId());
    if (!SetThreadDesktop((HDESK)desk)) return 1;
    g_alt_window = CreateWindowExA(0, "STATIC", "vx alternate", WS_POPUP, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    SetEvent(g_window_made);
    WaitForSingleObject(g_window_done, 5000);
    if (!g_alt_window || !DestroyWindow(g_alt_window)) return 2;
    return SetThreadDesktop(home) ? 0 : 3;
}

// A handle can do only what it was opened for, and a desktop's windows are its own.
static void access_and_windows(void) {
    HWINSTA ws = GetProcessWindowStation();
    HDESK desk = GetThreadDesktop(GetCurrentThreadId());
    HWND w = CreateWindowExA(0, "STATIC", "vx enum", WS_POPUP, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    CHECK(w != NULL);

    // EnumDesktopWindows: NULL is the thread's desktop; a handle needs DESKTOP_READOBJECTS (GENERIC_READ has it).
    CHECK(enumerate(NULL) && seen(w));
    CHECK(enumerate(desk) && seen(w));
    HDESK reader = OpenDesktopW(L"Default", 0, FALSE, DESKTOP_READOBJECTS);
    CHECK(reader && enumerate(reader) && seen(w));
    HDESK generic_reader = OpenDesktopW(L"Default", 0, FALSE, GENERIC_READ);
    CHECK(generic_reader && enumerate(generic_reader) && seen(w) && CloseDesktop(generic_reader));
    HDESK switcher = OpenDesktopW(L"Default", 0, FALSE, DESKTOP_SWITCHDESKTOP);
    CHECK(switcher != NULL);
    SetLastError(0);
    CHECK(!enumerate(switcher) && GetLastError() == ERROR_ACCESS_DENIED && !g_seen_count);
    SetLastError(0);
    CHECK(!enumerate((HDESK)ws) && GetLastError() == ERROR_INVALID_HANDLE && !g_seen_count);
    SetLastError(0);
    CHECK(!enumerate((HDESK)(INT_PTR)0x1234) && GetLastError() == ERROR_INVALID_HANDLE && !g_seen_count);

    // SwitchDesktop needs DESKTOP_SWITCHDESKTOP (GENERIC_EXECUTE has it).
    SetLastError(0);
    CHECK(!SwitchDesktop(reader) && GetLastError() == ERROR_ACCESS_DENIED);
    CHECK(SwitchDesktop(switcher));
    HDESK executer = OpenDesktopW(L"Default", 0, FALSE, GENERIC_EXECUTE);
    CHECK(executer && SwitchDesktop(executer) && CloseDesktop(executer));
    CHECK(SwitchDesktop(desk));                                    // (the process's own handles have every right)

    // Flags are written only through a handle that may: DESKTOP_WRITEOBJECTS, WINSTA_WRITEATTRIBUTES (or GENERIC_WRITE).
    USEROBJECTFLAGS before = { 0 }, after = { 0 }, changed;
    DWORD need = 0;
    CHECK(GetUserObjectInformationW(desk, UOI_FLAGS, &before, sizeof before, &need));
    changed = before; changed.dwFlags ^= DF_ALLOWOTHERACCOUNTHOOK;
    SetLastError(0);
    CHECK(!SetUserObjectInformationW(reader, UOI_FLAGS, &changed, sizeof changed) && GetLastError() == ERROR_ACCESS_DENIED);
    CHECK(GetUserObjectInformationW(desk, UOI_FLAGS, &after, sizeof after, &need) && after.dwFlags == before.dwFlags);
    HDESK writer = OpenDesktopW(L"Default", 0, FALSE, GENERIC_WRITE);
    CHECK(writer && SetUserObjectInformationW(writer, UOI_FLAGS, &changed, sizeof changed));
    CHECK(GetUserObjectInformationW(desk, UOI_FLAGS, &after, sizeof after, &need) && after.dwFlags == changed.dwFlags);
    CHECK(SetUserObjectInformationW(writer, UOI_FLAGS, &before, sizeof before) && CloseDesktop(writer));
    HWINSTA ws_reader = OpenWindowStationW(L"WinSta0", FALSE, WINSTA_ENUMDESKTOPS | WINSTA_READATTRIBUTES);
    USEROBJECTFLAGS station = { 0 }, hidden;
    CHECK(ws_reader && GetUserObjectInformationW(ws_reader, UOI_FLAGS, &station, sizeof station, &need) && (station.dwFlags & WSF_VISIBLE));
    hidden = station; hidden.dwFlags = 0;
    SetLastError(0);
    CHECK(!SetUserObjectInformationW(ws_reader, UOI_FLAGS, &hidden, sizeof hidden) && GetLastError() == ERROR_ACCESS_DENIED);
    CHECK(GetUserObjectInformationW(ws, UOI_FLAGS, &station, sizeof station, &need) && (station.dwFlags & WSF_VISIBLE));
    CHECK(CloseWindowStation(ws_reader));
    HWINSTA ws_writer = OpenWindowStationW(L"WinSta0", FALSE, WINSTA_WRITEATTRIBUTES);
    CHECK(ws_writer && SetUserObjectInformationW(ws_writer, UOI_FLAGS, &station, sizeof station) && CloseWindowStation(ws_writer));

    // A desktop made in another window station: it shows the windows its threads make there, and only those.
    HWINSTA alt = CreateWindowStationW(NULL, 0, WINSTA_ALL_ACCESS, NULL);
    CHECK(alt && SetProcessWindowStation(alt));
    HDESK alt_desk = CreateDesktopW(L"vx_enum_desktop", NULL, NULL, 0, GENERIC_ALL, NULL);
    CHECK(SetProcessWindowStation(ws));
    CHECK(alt_desk && enumerate(alt_desk) && !g_seen_count);
    g_window_made = CreateEventA(NULL, TRUE, FALSE, NULL); g_window_done = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, window_on_alternate_desktop, alt_desk, 0, NULL);
    DWORD code = 9;
    CHECK(WaitForSingleObject(g_window_made, 5000) == WAIT_OBJECT_0 && g_alt_window != NULL);
    CHECK(enumerate(alt_desk) && g_seen_count == 1 && seen(g_alt_window));
    CHECK(enumerate(desk) && seen(w) && !seen(g_alt_window));
    CHECK(enumerate(NULL) && seen(w) && !seen(g_alt_window));
    SetEvent(g_window_done);
    WaitForSingleObject(t, 5000);
    CHECK(GetExitCodeThread(t, &code) && code == 0);
    CloseHandle(t); CloseHandle(g_window_made); CloseHandle(g_window_done);
    CHECK(enumerate(alt_desk) && !g_seen_count);
    CHECK(CloseDesktop(alt_desk) && CloseWindowStation(alt));
    CHECK(CloseDesktop(reader) && CloseDesktop(switcher));
    DestroyWindow(w);
}

static void power_notifications(HWND hwnd) {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    HPOWERNOTIFY (WINAPI *reg_suspend)(HANDLE, DWORD) = (void *)GetProcAddress(user32, "RegisterSuspendResumeNotification");
    BOOL (WINAPI *unreg_suspend)(HPOWERNOTIFY) = (void *)GetProcAddress(user32, "UnregisterSuspendResumeNotification");
    HPOWERNOTIFY (WINAPI *reg_setting)(HANDLE, LPCGUID, DWORD) = (void *)GetProcAddress(user32, "RegisterPowerSettingNotification");
    BOOL (WINAPI *unreg_setting)(HPOWERNOTIFY) = (void *)GetProcAddress(user32, "UnregisterPowerSettingNotification");
    CHECK(reg_suspend && unreg_suspend && reg_setting && unreg_setting);
    if (!(reg_suspend && unreg_suspend && reg_setting && unreg_setting)) return;
    HPOWERNOTIFY a = reg_suspend(hwnd, DEVICE_NOTIFY_WINDOW_HANDLE);
    struct { void *callback, *context; } params = { (void *)power_notifications, (void *)0x5678 };   // DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS
    HPOWERNOTIFY b = reg_suspend(&params, 2 /* DEVICE_NOTIFY_CALLBACK */);
    CHECK(a && b && a != b);
    SetLastError(0);
    CHECK(!reg_suspend(hwnd, 5) && GetLastError() == ERROR_INVALID_PARAMETER);
    static const GUID setting = { 0x0badc0de, 0x1234, 0x5678, { 9, 8, 7, 6, 5, 4, 3, 2 } };
    HPOWERNOTIFY c = reg_setting(hwnd, &setting, DEVICE_NOTIFY_WINDOW_HANDLE);
    CHECK(c != NULL);
    SetLastError(0);
    CHECK(!reg_setting(hwnd, NULL, DEVICE_NOTIFY_WINDOW_HANDLE) && GetLastError() == ERROR_INVALID_PARAMETER);
    SetLastError(0);
    CHECK(!unreg_setting(a) && GetLastError() == ERROR_INVALID_HANDLE);          // (not a power-setting registration)
    CHECK(unreg_suspend(a) && unreg_suspend(b) && unreg_setting(c));
    SetLastError(0);
    CHECK(!unreg_suspend(a) && GetLastError() == ERROR_INVALID_HANDLE);
}

typedef DPI_AWARENESS_CONTEXT (WINAPI *get_ctx_fn)(void);
typedef DPI_AWARENESS_CONTEXT (WINAPI *set_ctx_fn)(DPI_AWARENESS_CONTEXT);
static get_ctx_fn g_get_thread;
static DWORD WINAPI thread_awareness(void *unused) {
    (void)unused;
    int (WINAPI *awareness)(DPI_AWARENESS_CONTEXT) = (void *)GetProcAddress(GetModuleHandleA("user32.dll"), "GetAwarenessFromDpiAwarenessContext");
    return (DWORD)awareness(g_get_thread());
}
static void dpi(void) {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    get_ctx_fn get_thread = (get_ctx_fn)GetProcAddress(user32, "GetThreadDpiAwarenessContext");
    set_ctx_fn set_thread = (set_ctx_fn)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
    BOOL (WINAPI *set_process)(DPI_AWARENESS_CONTEXT) = (void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    BOOL (WINAPI *equal)(DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT) = (void *)GetProcAddress(user32, "AreDpiAwarenessContextsEqual");
    BOOL (WINAPI *valid)(DPI_AWARENESS_CONTEXT) = (void *)GetProcAddress(user32, "IsValidDpiAwarenessContext");
    int (WINAPI *awareness)(DPI_AWARENESS_CONTEXT) = (void *)GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext");
    UINT (WINAPI *dpi_of)(DPI_AWARENESS_CONTEXT) = (void *)GetProcAddress(user32, "GetDpiFromDpiAwarenessContext");
    BOOL (WINAPI *is_aware)(void) = (void *)GetProcAddress(user32, "IsProcessDPIAware");
    CHECK(get_thread && set_thread && set_process && equal && valid && awareness && dpi_of && is_aware);
    if (!(get_thread && set_thread && set_process && equal && valid && awareness && dpi_of && is_aware)) return;
    // Unaware until the program says otherwise; a thread's own awareness on top.
    CHECK(!is_aware() && equal(get_thread(), DPI_AWARENESS_CONTEXT_UNAWARE));
    DPI_AWARENESS_CONTEXT old = set_thread(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CHECK(equal(old, DPI_AWARENESS_CONTEXT_UNAWARE) && equal(get_thread(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    CHECK(awareness(get_thread()) == DPI_AWARENESS_PER_MONITOR_AWARE && !equal(get_thread(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE));
    SetLastError(0);
    CHECK(!set_thread((DPI_AWARENESS_CONTEXT)(INT_PTR)-9) && GetLastError() == ERROR_INVALID_PARAMETER);
    set_thread(old);
    CHECK(!valid((DPI_AWARENESS_CONTEXT)(INT_PTR)-7) && valid(DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED));
    CHECK(awareness((DPI_AWARENESS_CONTEXT)(INT_PTR)-7) == DPI_AWARENESS_INVALID && awareness(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE) == DPI_AWARENESS_SYSTEM_AWARE);
    CHECK(dpi_of(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE) == 96 && dpi_of(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE) == 0);

    // shcore through its API set, as Chromium loads it: the process's awareness is set once.
    HMODULE shcore = LoadLibraryA("api-ms-win-shcore-scaling-l1-1-1.dll");
    HRESULT (WINAPI *set_awareness)(int) = (void *)GetProcAddress(shcore, "SetProcessDpiAwareness");
    HRESULT (WINAPI *get_awareness)(HANDLE, int *) = (void *)GetProcAddress(shcore, "GetProcessDpiAwareness");
    HRESULT (WINAPI *monitor_dpi)(HMONITOR, int, UINT *, UINT *) = (void *)GetProcAddress(shcore, "GetDpiForMonitor");
    HRESULT (WINAPI *scale)(HMONITOR, int *) = (void *)GetProcAddress(shcore, "GetScaleFactorForMonitor");
    CHECK(shcore && set_awareness && get_awareness && monitor_dpi && scale);
    if (!(set_awareness && get_awareness && monitor_dpi && scale)) return;
    int value = -1;
    CHECK(get_awareness(NULL, &value) == S_OK && value == 0);
    CHECK(set_awareness(3) == E_INVALIDARG);
    CHECK(set_awareness(2 /* PROCESS_PER_MONITOR_DPI_AWARE */) == S_OK);
    CHECK(set_awareness(1) == E_ACCESSDENIED);
    CHECK(get_awareness(GetCurrentProcess(), &value) == S_OK && value == 2 && is_aware());
    SetLastError(0);
    CHECK(!set_process(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE) && GetLastError() == ERROR_ACCESS_DENIED);
    g_get_thread = get_thread;                                     // (a new thread follows the process)
    HANDLE t = CreateThread(NULL, 0, thread_awareness, NULL, 0, NULL);
    DWORD code = 0;
    WaitForSingleObject(t, 5000);
    CHECK(GetExitCodeThread(t, &code) && code == 2 && awareness(get_thread()) == DPI_AWARENESS_UNAWARE);   // (this one set its own)
    CloseHandle(t);
    POINT origin = { 0, 0 };
    HMONITOR m = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    UINT x = 0, y = 0;
    int percent = 0;
    CHECK(monitor_dpi(m, 0 /* MDT_EFFECTIVE_DPI */, &x, &y) == S_OK && x == 96 && y == 96);
    CHECK(monitor_dpi(m, 7, &x, &y) == E_INVALIDARG && monitor_dpi(NULL, 0, &x, &y) == E_INVALIDARG);
    CHECK(scale(m, &percent) == S_OK && percent == 100);
}

static void coalescable_timer(void) {
    UINT_PTR (WINAPI *set_timer)(HWND, UINT_PTR, UINT, TIMERPROC, ULONG) = (void *)GetProcAddress(GetModuleHandleA("user32.dll"), "SetCoalescableTimer");
    CHECK(set_timer != NULL);
    if (!set_timer) return;
    UINT_PTR id = set_timer(NULL, 0, 15, NULL, 0 /* TIMERV_DEFAULT_COALESCING */);
    CHECK(id != 0);
    MSG msg;
    int got = 0;
    for (DWORD start = GetTickCount(); !got && GetTickCount() - start < 2000;) {
        if (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) got = msg.message == WM_TIMER && msg.wParam == id;
        else Sleep(1);
    }
    CHECK(got);
    CHECK(KillTimer(NULL, id));
}

int main(void) {
    stations_and_desktops();
    access_and_windows();
    HWND w = CreateWindowExA(0, "STATIC", "vx power", WS_POPUP, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    power_notifications(w);
    DestroyWindow(w);
    dpi();
    coalescable_timer();
    printf("window stations, desktops, power notifications, DPI awareness -> %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
