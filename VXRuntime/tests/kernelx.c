// kernel32 and ntdll functions the Windows Steam client imports (src/kernelx.c): serial ports, memory notifications
// and hints, threads started "remotely", thread and process information, locales, volumes and DOS devices, time zones,
// Windows Error Reporting, NtQueryObject, NtWriteFile, NTSTATUS codes, DLL directories. Made-up data only.
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)
static HMODULE kernel32, ntdll;
// Newer functions are looked up by name (what GetProcAddress finds is what an import binds to).
#define GET(module, type, name) __typeof__(type) name = (type)(void *)GetProcAddress(module, #name); CHECK(name != NULL); if (!name) return
static void group(const char *name, int before) { printf("kernelx: %-22s %s\n", name, failures == before ? "ok" : "FAILED"); }
static char g_temp[MAX_PATH];

static void test_comm(void) {
    int before = failures;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%skx_comm.bin", g_temp);
    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CHECK(f != INVALID_HANDLE_VALUE);
    DCB dcb = { sizeof dcb };
    COMMTIMEOUTS timeouts = { 0 };
    COMSTAT stat; DWORD errors, modem;
    SetLastError(0); CHECK(!GetCommState(f, &dcb) && GetLastError() == ERROR_INVALID_FUNCTION);   // a file: no serial driver
    SetLastError(0); CHECK(!SetCommState(f, &dcb) && GetLastError() == ERROR_INVALID_FUNCTION);
    SetLastError(0); CHECK(!SetCommTimeouts(f, &timeouts) && GetLastError() == ERROR_INVALID_FUNCTION);
    SetLastError(0); CHECK(!ClearCommError(f, &errors, &stat) && GetLastError() == ERROR_INVALID_FUNCTION);
    SetLastError(0); CHECK(!PurgeComm(f, PURGE_RXCLEAR) && GetLastError() == ERROR_INVALID_FUNCTION);
    SetLastError(0); CHECK(!EscapeCommFunction(f, SETDTR) && GetLastError() == ERROR_INVALID_FUNCTION);
    SetLastError(0); CHECK(!GetCommModemStatus(f, &modem) && GetLastError() == ERROR_INVALID_FUNCTION);
    HANDLE e = CreateEventA(NULL, TRUE, FALSE, NULL);
    SetLastError(0); CHECK(!GetCommState(e, &dcb) && GetLastError() == ERROR_INVALID_HANDLE);   // not a file at all
    CloseHandle(e); CloseHandle(f); DeleteFileA(path);
    group("serial ports", before);
}

typedef struct { PVOID address; ULONG_PTR attributes; } working_set_entry;
typedef struct { PVOID address; SIZE_T bytes; } range_entry;
typedef struct { DWORD cb; SIZE_T commit_total, commit_limit, commit_peak, physical_total, physical_available, system_cache,
                 kernel_total, kernel_paged, kernel_nonpaged, page_size; DWORD handles, processes, threads; } perf_info;
static void test_memory(void) {
    int before = failures;
    HANDLE low = CreateMemoryResourceNotification(LowMemoryResourceNotification);
    HANDLE high = CreateMemoryResourceNotification(HighMemoryResourceNotification);
    CHECK(low && high);
    BOOL is_low = 2, is_high = 2;
    CHECK(QueryMemoryResourceNotification(low, &is_low) && QueryMemoryResourceNotification(high, &is_high));
    CHECK((is_low == 0 || is_low == 1) && (is_high == 0 || is_high == 1) && !(is_low && is_high));
    CHECK(WaitForSingleObject(high, 0) == (is_high ? WAIT_OBJECT_0 : WAIT_TIMEOUT));   // the event follows the state
    HANDLE again = CreateMemoryResourceNotification(LowMemoryResourceNotification);
    BOOL also = 2;
    CHECK(again && again != low && QueryMemoryResourceNotification(again, &also) && also == is_low);
    SetLastError(0); CHECK(!CreateMemoryResourceNotification((MEMORY_RESOURCE_NOTIFICATION_TYPE)7) && GetLastError() == ERROR_INVALID_PARAMETER);
    HANDLE e = CreateEventA(NULL, TRUE, FALSE, NULL);
    SetLastError(0); CHECK(!QueryMemoryResourceNotification(e, &is_low) && GetLastError() == ERROR_INVALID_HANDLE);
    CloseHandle(e); CloseHandle(again); CloseHandle(low); CloseHandle(high);

    GET(kernel32, DWORD (WINAPI *)(PVOID, SIZE_T), DiscardVirtualMemory);
    GET(kernel32, BOOL (WINAPI *)(HANDLE, ULONG_PTR, range_entry *, ULONG), PrefetchVirtualMemory);
    GET(kernel32, BOOL (WINAPI *)(HANDLE, PVOID, DWORD), K32QueryWorkingSetEx);
    GET(kernel32, BOOL (WINAPI *)(perf_info *, DWORD), K32GetPerformanceInfo);
    BYTE *p = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    BYTE *ro = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READONLY);
    BYTE *gone = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(p && ro && gone && VirtualFree(gone, 0, MEM_RELEASE));
    memset(p, 0x5a, 65536);
    CHECK(DiscardVirtualMemory(p, 65536) == ERROR_SUCCESS);
    CHECK(DiscardVirtualMemory(p + 1, 4096) == ERROR_INVALID_PARAMETER);          // not page-aligned
    CHECK(DiscardVirtualMemory(p, 100) == ERROR_INVALID_PARAMETER);               // not whole pages
    CHECK(DiscardVirtualMemory(ro, 4096) != ERROR_SUCCESS);                       // only read-write pages
    p[0] = 1; p[65535] = 2;
    CHECK(p[0] == 1 && p[65535] == 2);                                            // still committed
    // Views of file mappings are not private memory: refused, and their contents stay (another process may share
    // them; a file's pages are the file's)
    HANDLE shared = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 65536, NULL);
    BYTE *view = shared ? MapViewOfFile(shared, FILE_MAP_ALL_ACCESS, 0, 0, 65536) : NULL;
    CHECK(view != NULL);
    if (view) {
        memset(view, 0x6b, 65536);
        CHECK(DiscardVirtualMemory(view, 65536) == ERROR_INVALID_PARAMETER);
        CHECK(DiscardVirtualMemory(view + 4096, 4096) == ERROR_INVALID_PARAMETER);
        CHECK(view[0] == 0x6b && view[4096] == 0x6b && view[65535] == 0x6b);
        CHECK(UnmapViewOfFile(view));
    }
    if (shared) CloseHandle(shared);
    char backing[MAX_PATH];
    snprintf(backing, sizeof backing, "%skx_view.bin", g_temp);
    HANDLE file = CreateFileA(backing, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    HANDLE on_file = file != INVALID_HANDLE_VALUE ? CreateFileMappingA(file, NULL, PAGE_READWRITE, 0, 8192, NULL) : NULL;
    BYTE *file_view = on_file ? MapViewOfFile(on_file, FILE_MAP_WRITE, 0, 0, 8192) : NULL;
    CHECK(file_view != NULL);
    if (file_view) {
        memset(file_view, 0x3c, 8192);
        CHECK(DiscardVirtualMemory(file_view, 8192) == ERROR_INVALID_PARAMETER);
        CHECK(file_view[0] == 0x3c && file_view[8191] == 0x3c);
        CHECK(UnmapViewOfFile(file_view));
    }
    if (on_file) CloseHandle(on_file);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    DeleteFileA(backing);
    CHECK(DiscardVirtualMemory(p, 65536) == ERROR_SUCCESS);                       // (private memory still is)
    p[0] = 1; p[65535] = 2;
    range_entry r = { p, 65536 };
    CHECK(PrefetchVirtualMemory(GetCurrentProcess(), 1, &r, 0));
    SetLastError(0); CHECK(!PrefetchVirtualMemory(GetCurrentProcess(), 1, &r, 1) && GetLastError() == ERROR_INVALID_PARAMETER);
    working_set_entry ws[2] = { { p, 0 }, { gone, ~(ULONG_PTR)0 } };
    CHECK(K32QueryWorkingSetEx(GetCurrentProcess(), ws, sizeof ws));
    CHECK((ws[0].attributes & 1) && ((ws[0].attributes >> 4) & 0x7ff) == PAGE_READWRITE);   // Valid, Win32Protection
    CHECK(ws[1].attributes == 0);                                                // released: nothing there
    SetLastError(0); CHECK(!K32QueryWorkingSetEx(GetCurrentProcess(), ws, 8) && GetLastError() == ERROR_BAD_LENGTH);
    CHECK(VirtualFreeEx(GetCurrentProcess(), ro, 0, MEM_RELEASE));
    CHECK(VirtualFree(p, 0, MEM_RELEASE));
    perf_info pi;
    SetLastError(0); CHECK(!K32GetPerformanceInfo(&pi, 8) && GetLastError() == ERROR_BAD_LENGTH);
    CHECK(K32GetPerformanceInfo(&pi, sizeof pi));
    MEMORYSTATUSEX ms = { sizeof ms };
    CHECK(GlobalMemoryStatusEx(&ms) && ms.ullAvailPhys <= ms.ullTotalPhys && ms.ullAvailPhys > 0);
    CHECK(pi.page_size == 4096 && pi.physical_total == ms.ullTotalPhys / 4096 && pi.physical_available <= pi.physical_total);
    CHECK(pi.commit_total <= pi.commit_limit && pi.commit_peak >= pi.commit_total);
    CHECK(pi.processes > 0 && pi.threads >= pi.processes && pi.handles > 0 && pi.kernel_total > 0);
    group("memory", before);
}

static DWORD WINAPI remote_main(void *arg) { *(volatile LONG *)arg = 42; return 7; }
typedef struct { ULONG version, control, state; } throttling;
static void test_threads(void) {
    int before = failures;
    GET(kernel32, HANDLE (WINAPI *)(HANDLE, void *, SIZE_T, LPTHREAD_START_ROUTINE, void *, DWORD, void *, DWORD *), CreateRemoteThreadEx);
    GET(kernel32, BOOL (WINAPI *)(HANDLE, int, void *, DWORD), SetThreadInformation);
    GET(kernel32, BOOL (WINAPI *)(HANDLE, int, void *, DWORD), GetThreadInformation);
    GET(kernel32, BOOL (WINAPI *)(HANDLE, void *), Wow64GetThreadContext);
    volatile LONG value = 0;
    DWORD tid = 0, code = 0;
    HANDLE t = CreateRemoteThread(GetCurrentProcess(), NULL, 0, remote_main, (void *)&value, 0, &tid);
    CHECK(t && tid);
    CHECK(WaitForSingleObject(t, 10000) == WAIT_OBJECT_0 && GetExitCodeThread(t, &code) && code == 7 && value == 42);
    value = 0;
    HANDLE t2 = CreateRemoteThreadEx(GetCurrentProcess(), NULL, 0, remote_main, (void *)&value, 0, NULL, NULL);
    CHECK(t2 && WaitForSingleObject(t2, 10000) == WAIT_OBJECT_0 && value == 42);
    HANDLE e = CreateEventA(NULL, TRUE, FALSE, NULL);
    SetLastError(0); CHECK(!CreateRemoteThread(e, NULL, 0, remote_main, (void *)&value, 0, NULL) && GetLastError() == ERROR_INVALID_HANDLE);

    BOOL off = 2;
    CHECK(GetThreadPriorityBoost(GetCurrentThread(), &off) && off == FALSE);
    CHECK(SetThreadPriorityBoost(GetCurrentThread(), TRUE) && GetThreadPriorityBoost(GetCurrentThread(), &off) && off == TRUE);
    CHECK(SetThreadPriorityBoost(GetCurrentThread(), FALSE) && GetThreadPriorityBoost(GetCurrentThread(), &off) && off == FALSE);
    CHECK(SetThreadPriorityBoost(t, TRUE) && GetThreadPriorityBoost(t, &off) && off == TRUE);   // another thread's
    SetLastError(0); CHECK(!GetThreadPriorityBoost(e, &off) && GetLastError() == ERROR_INVALID_HANDLE);

    ULONG priority = 2;                                                       // MEMORY_PRIORITY_LOW
    CHECK(SetThreadInformation(GetCurrentThread(), 0, &priority, sizeof priority));
    priority = 0; CHECK(GetThreadInformation(GetCurrentThread(), 0, &priority, sizeof priority) && priority == 2);
    priority = 9; SetLastError(0); CHECK(!SetThreadInformation(GetCurrentThread(), 0, &priority, sizeof priority) && GetLastError() == ERROR_INVALID_PARAMETER);
    ULONGLONG wide = 3; SetLastError(0); CHECK(!SetThreadInformation(GetCurrentThread(), 0, &wide, sizeof wide) && GetLastError() == ERROR_BAD_LENGTH);
    throttling eco = { 1, 1, 1 };                                             // EcoQoS on, then left to the system
    CHECK(SetThreadInformation(GetCurrentThread(), 3, &eco, sizeof eco));
    throttling read = { 0, 0, 0 };
    CHECK(GetThreadInformation(GetCurrentThread(), 3, &read, sizeof read) && read.version == 1 && read.control == 1 && read.state == 1);
    eco.state = 0; CHECK(SetThreadInformation(GetCurrentThread(), 3, &eco, sizeof eco));
    eco.version = 2; SetLastError(0); CHECK(!SetThreadInformation(GetCurrentThread(), 3, &eco, sizeof eco) && GetLastError() == ERROR_INVALID_PARAMETER);
    throttling stray = { 1, 0, 1 }; SetLastError(0); CHECK(!SetThreadInformation(GetCurrentThread(), 3, &stray, sizeof stray) && GetLastError() == ERROR_INVALID_PARAMETER);
    LONG absolute = 0; SetLastError(0); CHECK(!SetThreadInformation(GetCurrentThread(), 1, &absolute, sizeof absolute) && GetLastError() == ERROR_INVALID_PARAMETER);
    SetLastError(0); CHECK(!SetThreadInformation(e, 0, &priority, sizeof priority) && GetLastError() == ERROR_INVALID_HANDLE);

    BYTE context[1024] = { 0 };
    SetLastError(0); CHECK(!Wow64GetThreadContext(GetCurrentThread(), context) && GetLastError() == ERROR_INVALID_FUNCTION);   // no WOW64
    SetLastError(0); CHECK(!Wow64GetThreadContext(e, context) && GetLastError() == ERROR_INVALID_HANDLE);
    CloseHandle(e); CloseHandle(t); CloseHandle(t2);
    group("threads", before);
}

typedef HRESULT (WINAPI *wer_fn)(PCWSTR, PVOID);
static void test_process(void) {
    int before = failures;
    GET(kernel32, BOOL (WINAPI *)(HANDLE, PDWORD), GetProcessHandleCount);
    GET(kernel32, BOOL (WINAPI *)(HANDLE, PULONG64), QueryProcessCycleTime);
    GET(kernel32, BOOL (WINAPI *)(HANDLE, int, void *, DWORD), SetProcessInformation);
    GET(kernel32, BOOL (WINAPI *)(int *), GetFirmwareType);
    GET(kernel32, BOOL (WINAPI *)(DWORD *), GetConsoleDisplayMode);
    GET(kernel32, wer_fn, WerRegisterRuntimeExceptionModule);
    GET(kernel32, wer_fn, WerUnregisterRuntimeExceptionModule);
    DWORD n1 = 0, n2 = 0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &n1) && n1 > 0);
    HANDLE e = CreateEventA(NULL, TRUE, FALSE, NULL);
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &n2) && n2 == n1 + 1);
    ULONG64 c1 = 0, c2 = 0;
    CHECK(QueryProcessCycleTime(GetCurrentProcess(), &c1) && c1 > 0);
    volatile double x = 0; for (int i = 0; i < 2000000; i++) x += i * 0.5;
    CHECK(QueryProcessCycleTime(GetCurrentProcess(), &c2) && c2 > c1);
    SetLastError(0); CHECK(!QueryProcessCycleTime(e, &c1) && GetLastError() == ERROR_INVALID_HANDLE);

    ULONG priority = 3;                                                       // ProcessMemoryPriority: MEMORY_PRIORITY_MEDIUM
    CHECK(SetProcessInformation(GetCurrentProcess(), 0, &priority, sizeof priority));
    throttling eco = { 1, 1, 1 };                                             // ProcessPowerThrottling: EXECUTION_SPEED
    CHECK(SetProcessInformation(GetCurrentProcess(), 4, &eco, sizeof eco));
    eco.version = 0; SetLastError(0); CHECK(!SetProcessInformation(GetCurrentProcess(), 4, &eco, sizeof eco) && GetLastError() == ERROR_INVALID_PARAMETER);
    SetLastError(0); CHECK(!SetProcessInformation(GetCurrentProcess(), 4, &eco, 8) && GetLastError() == ERROR_BAD_LENGTH);
    struct { USHORT version, reserved; int type; ULONG_PTR value; } exhaustion = { 1, 0, 0, 1 };
    SetLastError(0); CHECK(!SetProcessInformation(GetCurrentProcess(), 1, &exhaustion, sizeof exhaustion) && GetLastError() == ERROR_INVALID_PARAMETER);
    SetLastError(0); CHECK(!SetProcessInformation(e, 0, &priority, sizeof priority) && GetLastError() == ERROR_INVALID_HANDLE);

    int firmware = 99;
    CHECK(GetFirmwareType(&firmware) && firmware >= 0 && firmware <= 2);
    DWORD mode = 99;
    SetLastError(0);
    BOOL console = GetConsoleDisplayMode(&mode);
    CHECK(console ? mode == 0 : GetLastError() == ERROR_INVALID_HANDLE);

    static int contexts[16];
    CHECK(WerRegisterRuntimeExceptionModule(L"C:\\made\\up\\kx_wer_handler.dll", &contexts[0]) == S_OK);
    for (int i = 1; i < 16; i++) CHECK(WerRegisterRuntimeExceptionModule(L"C:\\made\\up\\kx_other.dll", &contexts[i]) == S_OK);
    CHECK(WerRegisterRuntimeExceptionModule(L"C:\\made\\up\\kx_one_more.dll", NULL) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
    CHECK(WerUnregisterRuntimeExceptionModule(L"C:\\MADE\\up\\KX_WER_HANDLER.dll", &contexts[0]) == S_OK);
    CHECK(WerUnregisterRuntimeExceptionModule(L"C:\\made\\up\\kx_wer_handler.dll", &contexts[0]) == HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    for (int i = 1; i < 16; i++) CHECK(WerUnregisterRuntimeExceptionModule(L"C:\\made\\up\\kx_other.dll", &contexts[i]) == S_OK);
    CloseHandle(e);
    group("processes", before);
}

typedef struct { int count, stop_after, en_us, ja_jp, unknown; DWORD flags; } locale_walk;
static BOOL CALLBACK each_locale(LPWSTR name, DWORD flags, LPARAM param) {
    locale_walk *w = (locale_walk *)param;
    w->count++; w->flags |= flags;
    if (!wcscmp(name, L"en-US")) w->en_us = 1;
    if (!wcscmp(name, L"ja-JP")) w->ja_jp = 1;
    if (!LocaleNameToLCID(name, 0)) w->unknown++;                             // every one is a locale the runtime knows
    return w->count < w->stop_after;
}
static void test_locales(void) {
    int before = failures;
    locale_walk all = { 0, 1000, 0, 0, 0, 0 };
    CHECK(EnumSystemLocalesEx(each_locale, LOCALE_ALL, (LPARAM)&all, NULL));
    CHECK(all.count >= 2 && all.en_us && all.ja_jp && !all.unknown && all.flags == (LOCALE_WINDOWS | LOCALE_SPECIFICDATA));
    locale_walk specific = { 0, 1000, 0, 0, 0, 0 };
    CHECK(EnumSystemLocalesEx(each_locale, LOCALE_SPECIFICDATA, (LPARAM)&specific, NULL) && specific.count == all.count);
    locale_walk neutral = { 0, 1000, 0, 0, 0, 0 };
    CHECK(EnumSystemLocalesEx(each_locale, LOCALE_NEUTRALDATA, (LPARAM)&neutral, NULL) && neutral.count == 0);
    locale_walk one = { 0, 1, 0, 0, 0, 0 };
    CHECK(EnumSystemLocalesEx(each_locale, 0, (LPARAM)&one, NULL) && one.count == 1);   // the callback stops it
    SetLastError(0); CHECK(!EnumSystemLocalesEx(each_locale, LOCALE_REPLACEMENT, (LPARAM)&one, NULL) && GetLastError() == ERROR_INVALID_FLAGS);
    SetLastError(0); CHECK(!EnumSystemLocalesEx(each_locale, 0, (LPARAM)&one, (void *)1) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK(IsValidLocaleName(L"en-GB") && !IsValidLocaleName(L"xx-ZZ"));
    group("locales", before);
}

static void test_volumes(void) {
    int before = failures;
    WCHAR volume[MAX_PATH], device[MAX_PATH], other[MAX_PATH], paths[64];
    HANDLE find = FindFirstVolumeW(volume, MAX_PATH);
    CHECK(find != INVALID_HANDLE_VALUE);
    if (find == INVALID_HANDLE_VALUE) return;
    int n = 0;
    DWORD drives = 0;
    do {
        n++;
        size_t len = wcslen(volume);
        CHECK(len > 12 && !wcsncmp(volume, L"\\\\?\\Volume{", 11) && volume[len - 1] == L'\\');
        volume[len - 1] = 0;                                                   // QueryDosDevice takes "Volume{...}"
        DWORD k = QueryDosDeviceW(volume + 4, device, MAX_PATH);
        volume[len - 1] = L'\\';
        CHECK(k == wcslen(device) + 2 && !wcsncmp(device, L"\\Device\\HarddiskVolume", 22));
        DWORD need = 0;
        CHECK(GetVolumePathNamesForVolumeNameW(volume, paths, 64, &need) && paths[1] == L':' && paths[2] == L'\\' && !paths[3] && !paths[4]);
        WCHAR drive[3] = { paths[0], L':', 0 };
        CHECK(QueryDosDeviceW(drive, other, MAX_PATH) && !wcscmp(device, other));   // the drive and its volume: one device
        drives |= 1u << (paths[0] - L'A');
        SetLastError(0); CHECK(!GetVolumePathNamesForVolumeNameW(volume, paths, 2, &need) && GetLastError() == ERROR_MORE_DATA && need == 5);
    } while (FindNextVolumeW(find, volume, MAX_PATH));
    CHECK(GetLastError() == ERROR_NO_MORE_FILES);
    CHECK(FindVolumeClose(find));
    CHECK(drives == GetLogicalDrives() && n == 2);                             // C: and Z:
    SetLastError(0); CHECK(FindFirstVolumeW(volume, 10) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILENAME_EXCED_RANGE);
    WCHAR list[512];
    DWORD k = QueryDosDeviceW(NULL, list, 512);
    int c = 0, z = 0;
    for (WCHAR *s = list; k && *s; s += wcslen(s) + 1) { c |= !wcscmp(s, L"C:"); z |= !wcscmp(s, L"Z:"); }
    CHECK(k > 0 && c && z);
    SetLastError(0); CHECK(!QueryDosDeviceW(L"C:", device, 5) && GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    SetLastError(0); CHECK(!QueryDosDeviceW(L"Q:", device, MAX_PATH) && GetLastError() == ERROR_FILE_NOT_FOUND);
    char narrow[64];
    CHECK(QueryDosDeviceA("c:", narrow, sizeof narrow) && !strcmp(narrow, "\\Device\\HarddiskVolume1"));
    group("volumes", before);
}

static SYSTEMTIME st(int year, int month, int day, int hour, int minute) { SYSTEMTIME s = { (WORD)year, (WORD)month, 0, (WORD)day, (WORD)hour, (WORD)minute, 0, 0 }; return s; }
static int same(const SYSTEMTIME *s, int year, int month, int day, int hour, int minute) {
    return s->wYear == year && s->wMonth == month && s->wDay == day && s->wHour == hour && s->wMinute == minute;
}
static int to_utc(TIME_ZONE_INFORMATION *tz, SYSTEMTIME local, int year, int month, int day, int hour, int minute) {
    SYSTEMTIME utc, back;
    return TzSpecificLocalTimeToSystemTime(tz, &local, &utc) && same(&utc, year, month, day, hour, minute) &&
           SystemTimeToTzSpecificLocalTime(tz, &utc, &back) && same(&back, local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
}
static void test_time_zones(void) {
    int before = failures;
    TIME_ZONE_INFORMATION us = { 0 };                     // UTC-5, daylight from the 2nd Sunday of March to the 1st of November
    us.Bias = 300; us.DaylightBias = -60;
    us.StandardDate = st(0, 11, 1, 2, 0); us.DaylightDate = st(0, 3, 2, 2, 0);
    CHECK(to_utc(&us, st(2026, 7, 1, 12, 0), 2026, 7, 1, 16, 0));
    CHECK(to_utc(&us, st(2026, 1, 15, 12, 0), 2026, 1, 15, 17, 0));
    CHECK(to_utc(&us, st(2026, 3, 8, 1, 30), 2026, 3, 8, 6, 30));            // (2026-03-08 is the 2nd Sunday)
    CHECK(to_utc(&us, st(2026, 3, 8, 3, 30), 2026, 3, 8, 7, 30));
    TIME_ZONE_INFORMATION eu = { 0 };                     // UTC+0, daylight from the last Sunday of March to the last of October
    eu.DaylightBias = -60;
    eu.StandardDate = st(0, 10, 5, 2, 0); eu.DaylightDate = st(0, 3, 5, 1, 0);
    CHECK(to_utc(&eu, st(2026, 3, 29, 0, 30), 2026, 3, 29, 0, 30));
    CHECK(to_utc(&eu, st(2026, 3, 29, 3, 0), 2026, 3, 29, 2, 0));
    CHECK(to_utc(&eu, st(2026, 10, 24, 12, 0), 2026, 10, 24, 11, 0));
    CHECK(to_utc(&eu, st(2026, 10, 26, 12, 0), 2026, 10, 26, 12, 0));
    TIME_ZONE_INFORMATION south = { 0 };                  // UTC+10, daylight from the 1st Sunday of October to the 1st of April
    south.Bias = -600; south.DaylightBias = -60;
    south.StandardDate = st(0, 4, 1, 3, 0); south.DaylightDate = st(0, 10, 1, 2, 0);
    CHECK(to_utc(&south, st(2026, 1, 15, 12, 0), 2026, 1, 15, 1, 0));
    CHECK(to_utc(&south, st(2026, 7, 15, 12, 0), 2026, 7, 15, 2, 0));
    TIME_ZONE_INFORMATION east = { 0 };                   // UTC+9, no daylight time
    east.Bias = -540;
    SYSTEMTIME local = st(2026, 1, 1, 5, 0), utc;
    CHECK(TzSpecificLocalTimeToSystemTime(&east, &local, &utc) && same(&utc, 2025, 12, 31, 20, 0) && utc.wDayOfWeek == 3);
    local.wMonth = 13;
    SetLastError(0); CHECK(!TzSpecificLocalTimeToSystemTime(&east, &local, &utc) && GetLastError() == ERROR_INVALID_PARAMETER);
    // the Mac's own zone: no zone, or the one GetTimeZoneInformation describes, give the same answer
    TIME_ZONE_INFORMATION mine;
    GetTimeZoneInformation(&mine);
    SYSTEMTIME noon = st(2026, 7, 1, 12, 0), a, b, back;
    CHECK(TzSpecificLocalTimeToSystemTime(NULL, &noon, &a) && TzSpecificLocalTimeToSystemTime(&mine, &noon, &b) && same(&a, b.wYear, b.wMonth, b.wDay, b.wHour, b.wMinute));
    CHECK(SystemTimeToTzSpecificLocalTime(NULL, &a, &back) && same(&back, 2026, 7, 1, 12, 0));
    group("time zones", before);
}

typedef LONG (NTAPI *query_object_fn)(HANDLE, int, void *, ULONG, ULONG *);
typedef struct { USHORT length, max; PWSTR buffer; } ustring;
static int type_is(query_object_fn query, HANDLE h, const WCHAR *type) {
    static BYTE buf[1024];
    ULONG need = 0;
    if (query(h, 2, buf, sizeof buf, &need)) return 0;
    ustring *u = (ustring *)buf;
    return u->buffer && u->length == wcslen(type) * 2 && !wcsncmp(u->buffer, type, wcslen(type)) && need >= 104 + u->length;
}
static void test_ntdll(void) {
    int before = failures;
    GET(ntdll, query_object_fn, NtQueryObject);
    GET(ntdll, LONG (NTAPI *)(HANDLE, HANDLE, void *, void *, ULONG_PTR *, const void *, ULONG, LARGE_INTEGER *, ULONG *), NtWriteFile);
    GET(ntdll, ULONG (NTAPI *)(LONG), RtlNtStatusToDosError);
    GET(ntdll, LONG (NTAPI *)(void), RtlGetLastNtStatus);
    static BYTE buf[2048];
    ULONG need = 0;
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, L"Local\\kx_made_up_event");
    CHECK(NtQueryObject(ev, 2, buf, 8, &need) == (LONG)0xC0000004 && need > 104);   // STATUS_INFO_LENGTH_MISMATCH
    CHECK(type_is(NtQueryObject, ev, L"Event"));
    ustring *u = (ustring *)buf;
    CHECK(!NtQueryObject(ev, 1, buf, sizeof buf, NULL) && u->buffer && !wcscmp(u->buffer, L"\\Sessions\\1\\BaseNamedObjects\\kx_made_up_event"));
    ULONG basic[14];
    CHECK(!NtQueryObject(ev, 0, basic, sizeof basic, NULL) && basic[1] == 0x1f0003 && basic[2] == 1);   // EVENT_ALL_ACCESS, one handle
    HANDLE dup = NULL;
    CHECK(DuplicateHandle(GetCurrentProcess(), ev, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS));
    CHECK(!NtQueryObject(ev, 0, basic, sizeof basic, NULL) && basic[2] == 2);
    HANDLE unnamed = CreateEventA(NULL, TRUE, FALSE, NULL);
    CHECK(!NtQueryObject(unnamed, 1, buf, sizeof buf, &need) && u->length == 0 && need == 16);
    HANDLE mutex = CreateMutexA(NULL, FALSE, NULL), sem = CreateSemaphoreA(NULL, 0, 2, NULL);
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    HANDLE section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, NULL);
    CHECK(type_is(NtQueryObject, mutex, L"Mutant") && type_is(NtQueryObject, sem, L"Semaphore") && type_is(NtQueryObject, port, L"IoCompletion"));
    CHECK(type_is(NtQueryObject, section, L"Section") && type_is(NtQueryObject, GetCurrentThread(), L"Thread") && type_is(NtQueryObject, GetCurrentProcess(), L"Process"));
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%skx_ntdll.bin", g_temp);
    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CHECK(type_is(NtQueryObject, f, L"File"));
    CHECK(!NtQueryObject(f, 1, buf, sizeof buf, NULL) && u->buffer && !wcsncmp(u->buffer, L"\\Device\\HarddiskVolume", 22) &&
          wcslen(u->buffer) > 12 && !wcscmp(u->buffer + wcslen(u->buffer) - 13, L"\\kx_ntdll.bin"));
    CHECK(NtQueryObject((HANDLE)(ULONG_PTR)0x1234560, 2, buf, sizeof buf, NULL) == (LONG)0xC0000008);   // STATUS_INVALID_HANDLE
    CHECK(NtQueryObject(ev, 3, buf, sizeof buf, NULL) == (LONG)0xC0000003);                                // STATUS_INVALID_INFO_CLASS

    DWORD n = 0;
    CHECK(WriteFile(f, "0123456789", 10, &n, NULL) && n == 10);
    ULONG_PTR iosb[2] = { 99, 99 };
    LARGE_INTEGER at = { .QuadPart = 4 };
    HANDLE done = CreateEventA(NULL, TRUE, FALSE, NULL);
    CHECK(NtWriteFile(f, done, NULL, NULL, iosb, "ab", 2, &at, NULL) == 0 && iosb[0] == 0 && iosb[1] == 2);
    CHECK(SetFilePointer(f, 0, NULL, FILE_CURRENT) == 6 && WaitForSingleObject(done, 0) == WAIT_OBJECT_0);   // the position follows
    at.QuadPart = -1;                                                           // FILE_WRITE_TO_END_OF_FILE
    CHECK(NtWriteFile(f, NULL, NULL, NULL, iosb, "Z", 1, &at, NULL) == 0 && iosb[1] == 1);
    CHECK(NtWriteFile(f, NULL, NULL, NULL, iosb, "Q", 1, NULL, NULL) == 0);    // at the position
    char back[32] = { 0 };
    SetFilePointer(f, 0, NULL, FILE_BEGIN);
    CHECK(ReadFile(f, back, sizeof back - 1, &n, NULL) && n == 12 && !strcmp(back, "0123ab6789ZQ"));
    SetLastError(1234);
    CHECK(NtWriteFile(ev, NULL, NULL, NULL, iosb, "x", 1, NULL, NULL) == (LONG)0xC0000008 && GetLastError() == 1234);   // last error kept
    CHECK(NtWriteFile(f, NULL, (void *)1, NULL, iosb, "x", 1, NULL, NULL) == (LONG)0xC00000BB);   // APC routines: STATUS_NOT_SUPPORTED

    CHECK(RtlNtStatusToDosError(0) == 0 && RtlNtStatusToDosError((LONG)0xC0000034) == ERROR_FILE_NOT_FOUND);
    CHECK(RtlNtStatusToDosError((LONG)0xC000003A) == ERROR_PATH_NOT_FOUND && RtlNtStatusToDosError((LONG)0xC0000022) == ERROR_ACCESS_DENIED);
    CHECK(RtlNtStatusToDosError((LONG)0xC0000008) == ERROR_INVALID_HANDLE && RtlNtStatusToDosError((LONG)0x80000005) == ERROR_MORE_DATA);
    CHECK(RtlNtStatusToDosError((LONG)0xC00000BB) == ERROR_NOT_SUPPORTED && RtlNtStatusToDosError(0x103) == ERROR_IO_PENDING);
    CHECK(RtlNtStatusToDosError((LONG)0xC0070057) == ERROR_INVALID_PARAMETER);        // a Win32 error carried as a status
    CHECK(RtlNtStatusToDosError((LONG)0xC0DEDEAD) == ERROR_MR_MID_NOT_FOUND);          // made up: no Win32 code
    SetLastError(RtlNtStatusToDosError((LONG)0xC000003A));
    CHECK(RtlGetLastNtStatus() == (LONG)0xC000003A);
    SetLastError(ERROR_ACCESS_DENIED);
    CHECK(RtlGetLastNtStatus() == (LONG)0xC0000022);
    CHECK(CreateFileA("C:\\kx_made_up_missing.bin", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE);
    CHECK(GetLastError() == ERROR_FILE_NOT_FOUND && RtlGetLastNtStatus() == (LONG)0xC0000034);   // STATUS_OBJECT_NAME_NOT_FOUND
    SetLastError(12345);
    CHECK(RtlGetLastNtStatus() == (LONG)0xC0073039 && RtlNtStatusToDosError((LONG)0xC0073039) == 12345);
    HANDLE all[] = { ev, dup, unnamed, mutex, sem, port, section, f, done };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) CloseHandle(all[i]);
    DeleteFileA(path);
    group("ntdll", before);
}

// Last: SetDefaultDllDirectories changes how this process finds DLLs from then on. kx_a/b/c/d.dll are copies of the
// test plugin (build/vxplugin.dll, beside this program) in a directory of their own, which only AddDllDirectory or
// the current directory puts in the search.
static void test_dll_directories(void) {
    int before = failures;
    GET(kernel32, BOOL (WINAPI *)(DWORD), SetDefaultDllDirectories);
    GET(kernel32, void *(WINAPI *)(PCWSTR), AddDllDirectory);
    GET(kernel32, BOOL (WINAPI *)(void *), RemoveDllDirectory);
    char exe[MAX_PATH], plugin[MAX_PATH], dir[MAX_PATH], copy[MAX_PATH], cwd[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    strcpy(plugin, exe); strcpy(strrchr(plugin, '\\') + 1, "vxplugin.dll");
    snprintf(dir, sizeof dir, "%skx_dlls", g_temp);
    CreateDirectoryA(dir, NULL);
    const char *names[] = { "kx_a.dll", "kx_b.dll", "kx_c.dll", "kx_d.dll" };
    for (int i = 0; i < 4; i++) { snprintf(copy, sizeof copy, "%s\\%s", dir, names[i]); CHECK(CopyFileA(plugin, copy, FALSE)); }
    WCHAR wdir[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, dir, -1, wdir, MAX_PATH);

    // system32 only (LOAD_LIBRARY_SEARCH_SYSTEM32, or _SYSTEM32_NO_FORWARDER): neither the program's directory (where
    // vxplugin.dll is) nor the current one (kx_d.dll), which the standard search has
    GetCurrentDirectoryA(sizeof cwd, cwd);
    CHECK(SetCurrentDirectoryA(dir));
    const DWORD system32_only[] = { LOAD_LIBRARY_SEARCH_SYSTEM32, 0x4000 /* LOAD_LIBRARY_SEARCH_SYSTEM32_NO_FORWARDER */ };
    for (int i = 0; i < 2; i++) {
        SetLastError(0); CHECK(!LoadLibraryExA("vxplugin.dll", NULL, system32_only[i]) && GetLastError() == ERROR_MOD_NOT_FOUND);
        SetLastError(0); CHECK(!LoadLibraryExA("kx_d.dll", NULL, system32_only[i]) && GetLastError() == ERROR_MOD_NOT_FOUND);
        CHECK(LoadLibraryExA("kernel32.dll", NULL, system32_only[i]) != NULL);    // (the system's own DLLs are there)
    }
    CHECK(LoadLibraryExA("kx_d.dll", NULL, 0) != NULL);                          // the standard search looks here
    SetCurrentDirectoryA(cwd);
    // LOAD_WITH_ALTERED_SEARCH_PATH goes with no LOAD_LIBRARY_SEARCH_* flag; LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR needs a full path
    snprintf(copy, sizeof copy, "%s\\kx_d.dll", dir);
    SetLastError(0); CHECK(!LoadLibraryExA(copy, NULL, LOAD_WITH_ALTERED_SEARCH_PATH | LOAD_LIBRARY_SEARCH_SYSTEM32) && GetLastError() == ERROR_INVALID_PARAMETER);
    SetLastError(0); CHECK(!LoadLibraryExA(copy, NULL, LOAD_WITH_ALTERED_SEARCH_PATH | 0x4000) && GetLastError() == ERROR_INVALID_PARAMETER);
    SetLastError(0); CHECK(!LoadLibraryExA("kx_d.dll", NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR) && GetLastError() == ERROR_INVALID_PARAMETER);
    SetLastError(0); CHECK(!LoadLibraryExA("kx_dlls\\kx_d.dll", NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK(LoadLibraryExA(copy, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) != NULL);
    CHECK(LoadLibraryExA(copy, NULL, LOAD_WITH_ALTERED_SEARCH_PATH) != NULL);

    SetLastError(0); CHECK(!SetDefaultDllDirectories(0) && GetLastError() == ERROR_INVALID_PARAMETER);
    SetLastError(0); CHECK(!SetDefaultDllDirectories(0x8) && GetLastError() == ERROR_INVALID_PARAMETER);
    SetLastError(0); CHECK(!AddDllDirectory(L"relative\\dir") && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK(!LoadLibraryExA("kx_a.dll", NULL, LOAD_LIBRARY_SEARCH_USER_DIRS));    // no user directories yet
    void *cookie = AddDllDirectory(wdir);
    CHECK(cookie != NULL);
    HMODULE a = LoadLibraryExA("kx_a.dll", NULL, LOAD_LIBRARY_SEARCH_USER_DIRS);
    CHECK(a && GetProcAddress(a, "plugin_value") && ((int (*)(void))(void *)GetProcAddress(a, "plugin_value"))() == 7);
    CHECK(!LoadLibraryA("kx_b.dll"));                                            // the standard search does not use them
    CHECK(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
    CHECK(LoadLibraryA("kx_b.dll") != NULL);                                     // now the process's search does
    CHECK(LoadLibraryA("kernel32.dll") != NULL);                                 // (the system's DLLs as before)
    CHECK(RemoveDllDirectory(cookie));
    SetLastError(0); CHECK(!RemoveDllDirectory(cookie) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK(SetCurrentDirectoryA(dir));
    CHECK(!LoadLibraryA("kx_c.dll"));                                            // nor the current directory, with search flags
    SetCurrentDirectoryA(cwd);
    snprintf(copy, sizeof copy, "%s\\kx_c.dll", dir);
    CHECK(LoadLibraryA(copy) != NULL);                                           // a full path still loads
    group("DLL directories", before);
}

int main(void) {
    kernel32 = GetModuleHandleA("kernel32.dll");
    ntdll = GetModuleHandleA("ntdll.dll");
    GetTempPathA(sizeof g_temp, g_temp);
    test_comm();
    test_memory();
    test_threads();
    test_process();
    test_locales();
    test_volumes();
    test_time_zones();
    test_ntdll();
    test_dll_directories();
    printf("kernelx: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
