// userenv, winspool.drv, dhcpcsvc, secur32 and the oleaut32 functions Chromium imports by ordinal (src/sysdlls.c):
// environment blocks, AppContainer SIDs and profiles (and no launch inside one), policy sections, a machine without
// printers, DHCP options, account names, no LSA and no security packages, variant conversions. Made-up names and
// values only.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <userenv.h>
#include <winspool.h>
#include <dhcpcsdk.h>
#define SECURITY_WIN32
#include <security.h>
#include <ntsecapi.h>
#include <oleauto.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s (last error %lu)\n", __LINE__, #c, GetLastError()); failures++; } } while (0)

// ---- userenv -------------------------------------------------------------------------------------------------------
static const WCHAR *env_value(const WCHAR *block, const WCHAR *name) {
    size_t n = wcslen(name);
    for (const WCHAR *p = block; *p; p += wcslen(p) + 1)
        if (!_wcsnicmp(p, name, n) && p[n] == '=') return p + n + 1;
    return NULL;
}
// Names in order without case, as Windows compares them (upper-cased: "_" sorts after the letters).
static size_t name_length(const WCHAR *s) { size_t n = 1; while (s[n] && s[n] != '=') n++; return n; }
static WCHAR upper(WCHAR c) { return c >= 'a' && c <= 'z' ? (WCHAR)(c - 32) : c; }
static int env_sorted(const WCHAR *block) {
    const WCHAR *prev = NULL;
    for (const WCHAR *p = block; *p; p += wcslen(p) + 1) {
        if (prev) {
            size_t a = name_length(prev), b = name_length(p), i = 0;
            while (i < a && i < b && upper(prev[i]) == upper(p[i])) i++;
            if (i < a && i < b ? upper(prev[i]) > upper(p[i]) : a > b) return 0;
        }
        prev = p;
    }
    return 1;
}
static void test_environment(void) {
    void *env = NULL;
    CHECK(SetEnvironmentVariableW(L"VXRT_SYSDLLS_TEST", L"made-up value"));
    CHECK(CreateEnvironmentBlock(&env, NULL, TRUE) && env);
    if (env) {
        const WCHAR *v = env_value(env, L"VXRT_SYSDLLS_TEST");
        CHECK(v && !wcscmp(v, L"made-up value"));               // inherited
        v = env_value(env, L"SystemRoot");
        CHECK(v && !_wcsicmp(v, L"C:\\windows"));                // system variables
        CHECK(env_value(env, L"windir") && env_value(env, L"ProgramFiles") && env_value(env, L"COMPUTERNAME"));
        CHECK(env_sorted(env));
        CHECK(DestroyEnvironmentBlock(env));
    }
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) token = (HANDLE)(LONG_PTR)-4;   // GetCurrentProcessToken()
    env = NULL;
    CHECK(CreateEnvironmentBlock(&env, token, FALSE) && env);
    if (env) {
        CHECK(!env_value(env, L"VXRT_SYSDLLS_TEST"));            // not inherited
        const WCHAR *profile = env_value(env, L"USERPROFILE"), *appdata = env_value(env, L"APPDATA"), *user = env_value(env, L"USERNAME");
        CHECK(profile && !_wcsicmp(profile, L"C:\\users\\vxrt"));
        CHECK(appdata && !_wcsnicmp(appdata, profile ? profile : L"?", wcslen(profile ? profile : L"?")));
        WCHAR name[256]; DWORD n = 256;
        CHECK(user && GetUserNameW(name, &n) && !wcscmp(user, name));
        CHECK(env_value(env, L"Path") && env_value(env, L"TEMP"));
        CHECK(env_sorted(env));
        CHECK(DestroyEnvironmentBlock(env));
    }
    if (token && token != (HANDLE)(LONG_PTR)-4) CloseHandle(token);
    SetLastError(0);
    CHECK(!CreateEnvironmentBlock(NULL, NULL, TRUE) && GetLastError() == ERROR_INVALID_PARAMETER);
    SetEnvironmentVariableW(L"VXRT_SYSDLLS_TEST", NULL);
}

static void test_app_container(void) {
    static const WCHAR *name = L"VXRT.Test.Container_1";
    // S-1-15-2 and seven RIDs: SHA-256 of "vxrt.test.container_1" in UTF-16LE, first 28 bytes (computed for this test)
    static const DWORD rids[8] = { 2, 0x7ed3d612, 0x04d1db46, 0xbf1db855, 0xafd63926, 0x07fa02b9, 0x5ab4f904, 0xa08226d5 };
    DeleteAppContainerProfile(name);                       // (left over from an interrupted run)
    PSID sid = NULL, lower = NULL, other = NULL, made = NULL, again = NULL;
    CHECK(DeriveAppContainerSidFromAppContainerName(name, &sid) == S_OK && sid);
    if (sid) {
        const BYTE *b = sid;
        static const BYTE head[8] = { 1, 8, 0, 0, 0, 0, 0, 15 };
        CHECK(!memcmp(b, head, 8) && !memcmp(b + 8, rids, sizeof rids));
    }
    CHECK(DeriveAppContainerSidFromAppContainerName(L"vxrt.test.container_1", &lower) == S_OK && sid && lower && !memcmp(sid, lower, 40));
    CHECK(DeriveAppContainerSidFromAppContainerName(L"VXRT.Test.Container_2", &other) == S_OK && sid && other && memcmp(sid, other, 40));
    CHECK(DeriveAppContainerSidFromAppContainerName(L"bad/name", &other) == E_INVALIDARG);
    CHECK(DeriveAppContainerSidFromAppContainerName(L"", &other) == E_INVALIDARG);
    WCHAR longer[80];
    for (int i = 0; i < 65; i++) longer[i] = 'a';
    longer[65] = 0;
    CHECK(DeriveAppContainerSidFromAppContainerName(longer, &other) == E_INVALIDARG);
    longer[64] = 0;
    PSID sixty_four = NULL;
    CHECK(DeriveAppContainerSidFromAppContainerName(longer, &sixty_four) == S_OK);
    CHECK(CreateAppContainerProfile(name, L"Made-up container", L"A profile for the runtime's tests", NULL, 0, &made) == S_OK);
    CHECK(made && sid && !memcmp(made, sid, 40));
    CHECK(CreateAppContainerProfile(name, L"Made-up container", L"Again", NULL, 0, &again) == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) && !again);
    CHECK(DeleteAppContainerProfile(name) == S_OK);
    CHECK(CreateAppContainerProfile(name, L"Made-up container", L"Once more", NULL, 0, &again) == S_OK && again);
    CHECK(DeleteAppContainerProfile(name) == S_OK);
    CHECK(DeleteAppContainerProfile(name) != S_OK);        // (gone)
    CHECK(CreateAppContainerProfile(L"bad\\name", L"x", L"y", NULL, 0, &other) == E_INVALIDARG);
    for (PSID *p = (PSID[]){ sid, lower, made, again, sixty_four }, *end = p + 5; p < end; p++) if (*p) FreeSid(*p);
    if (other && other != sid) FreeSid(other);
}

// A profile exists, but no process can be started inside an AppContainer here: asking for one fails when the attribute
// is set (ERROR_NOT_SUPPORTED), so a sandbox learns it before any launch instead of starting a child with all of the
// user's rights while believing it isolated.
static void test_app_container_launch(void) {
    PSID sid = NULL;
    CHECK(DeriveAppContainerSidFromAppContainerName(L"VXRT.Test.Container_3", &sid) == S_OK && sid);
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(NULL, 3, 0, &size);
    LPPROC_THREAD_ATTRIBUTE_LIST list = size ? HeapAlloc(GetProcessHeap(), 0, size) : NULL;
    CHECK(list && InitializeProcThreadAttributeList(list, 3, 0, &size));
    if (list) {
        SECURITY_CAPABILITIES caps = { sid, NULL, 0, 0 };
        SetLastError(0);
        CHECK(!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &caps, sizeof caps, NULL, NULL));
        CHECK(GetLastError() == ERROR_NOT_SUPPORTED);
        DWORD less_privileged = PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT;     // the LPAC form
        SetLastError(0);
        CHECK(!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY, &less_privileged, sizeof less_privileged, NULL, NULL));
        CHECK(GetLastError() == ERROR_NOT_SUPPORTED);
        HANDLE parent = GetCurrentProcess();             // an attribute that confines nothing is still taken
        CHECK(UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &parent, sizeof parent, NULL, NULL));
        DeleteProcThreadAttributeList(list);
        HeapFree(GetProcessHeap(), 0, list);
    }
    if (sid) FreeSid(sid);
}

static volatile LONG g_first_left, g_second_saw_left;
static DWORD WINAPI second_section(void *unused) {
    (void)unused;
    HANDLE h = EnterCriticalPolicySection(TRUE);
    g_second_saw_left = g_first_left;
    if (h) LeaveCriticalPolicySection(h);
    return h != NULL;
}
static void test_policy(void) {
    HANDLE machine = EnterCriticalPolicySection(TRUE), user = EnterCriticalPolicySection(FALSE);
    CHECK(machine && user);
    HANDLE t = CreateThread(NULL, 0, second_section, NULL, 0, NULL);
    Sleep(300);                                            // the other thread waits for the machine section meanwhile
    g_first_left = 1;
    CHECK(LeaveCriticalPolicySection(machine));
    CHECK(WaitForSingleObject(t, 10000) == WAIT_OBJECT_0);
    DWORD code = 0;
    CHECK(GetExitCodeThread(t, &code) && code == 1 && g_second_saw_left == 1);
    CloseHandle(t);
    CHECK(LeaveCriticalPolicySection(user));
    SetLastError(0);
    CHECK(!LeaveCriticalPolicySection(user) && GetLastError() == ERROR_INVALID_PARAMETER);   // already left
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(RegisterGPNotification(event, FALSE));
    CHECK(WaitForSingleObject(event, 0) == WAIT_TIMEOUT); // policy never changes here
    CHECK(UnregisterGPNotification(event));
    CHECK(!UnregisterGPNotification(event));
    CloseHandle(event);
}

// ---- winspool.drv --------------------------------------------------------------------------------------------------
static void test_printers(void) {
    DWORD needed = 99, returned = 99;
    CHECK(EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2, NULL, 0, &needed, &returned) && !needed && !returned);
    BYTE buf[512];
    CHECK(EnumPrintersW(PRINTER_ENUM_LOCAL, NULL, 4, buf, sizeof buf, &needed, &returned) && !returned);
    SetLastError(0);
    CHECK(!EnumPrintersW(PRINTER_ENUM_LOCAL, NULL, 3, buf, sizeof buf, &needed, &returned) && GetLastError() == ERROR_INVALID_LEVEL);
    HANDLE printer = (HANDLE)1;
    SetLastError(0);
    CHECK(!OpenPrinterW(L"Made-up Printer", &printer, NULL) && GetLastError() == ERROR_INVALID_PRINTER_NAME && !printer);
    SetLastError(0);
    CHECK(!GetPrinterW((HANDLE)0x1234, 2, buf, sizeof buf, &needed) && GetLastError() == ERROR_INVALID_HANDLE);
    SetLastError(0);
    CHECK(!GetPrinterDriverW((HANDLE)0x1234, NULL, 2, buf, sizeof buf, &needed) && GetLastError() == ERROR_INVALID_HANDLE);
    CHECK(DocumentPropertiesW(NULL, (HANDLE)0x1234, L"Made-up Printer", NULL, NULL, 0) < 0);
    CHECK(DeviceCapabilitiesW(L"Made-up Printer", NULL, DC_PAPERS, NULL, NULL) == -1);
    SetLastError(0);
    CHECK(!ClosePrinter((HANDLE)0x1234) && GetLastError() == ERROR_INVALID_HANDLE);
    WCHAR name[260]; DWORD size = 260;
    SetLastError(0);
    CHECK(!GetDefaultPrinterW(name, &size) && GetLastError() == ERROR_FILE_NOT_FOUND);
    // libcef.dll imports GetDefaultPrinterW by ordinal 203
    HMODULE spool = LoadLibraryA("winspool.drv");
    FARPROC by_number = spool ? GetProcAddress(spool, MAKEINTRESOURCEA(203)) : NULL;
    CHECK(by_number && by_number == GetProcAddress(spool, "GetDefaultPrinterW"));
    if (by_number) {
        SetLastError(0);
        CHECK(!((BOOL (WINAPI *)(WCHAR *, DWORD *))by_number)(name, &size) && GetLastError() == ERROR_FILE_NOT_FOUND);
    }
}

// ---- dhcpcsvc ----------------------------------------------------------------------------------------------------------
static void test_dhcp(void) {
    DWORD version = 0;
    CHECK(DhcpCApiInitialize(&version) == 0 && version == 2);
    DHCPCAPI_PARAMS params[2] = { { 0, 1 /* subnet mask */, FALSE, NULL, 0 }, { 0, 252 /* WPAD */, FALSE, NULL, 0 } };
    DHCPCAPI_PARAMS_ARRAY send = { 0, NULL }, recd = { 2, params };
    BYTE buffer[1024]; DWORD size = sizeof buffer;
    CHECK(DhcpRequestParams(DHCPCAPI_REQUEST_SYNCHRONOUS, NULL, L"{00000000-1111-2222-3333-444444444444}", NULL, send, recd, buffer, &size, NULL) == ERROR_INVALID_PARAMETER);
    // the adapter iphlpapi reports: either its DHCP options, or (not configured by DHCP) ERROR_INVALID_PARAMETER
    ULONG need = 0;
    if (GetAdaptersAddresses(AF_INET, 0, NULL, NULL, &need) != ERROR_BUFFER_OVERFLOW || !need) { printf("dhcp: no adapter, skipped\n"); DhcpCApiCleanup(); return; }
    IP_ADAPTER_ADDRESSES *a = malloc(need);
    if (GetAdaptersAddresses(AF_INET, 0, NULL, a, &need) == 0) {
        WCHAR adapter[64];
        MultiByteToWideChar(CP_UTF8, 0, a->AdapterName, -1, adapter, 64);
        size = sizeof buffer;
        DWORD r = DhcpRequestParams(DHCPCAPI_REQUEST_SYNCHRONOUS, NULL, adapter, NULL, send, recd, buffer, &size, NULL);
        CHECK(r == 0 || r == ERROR_INVALID_PARAMETER);
        if (r == 0) {
            CHECK(params[0].nBytesData == 0 || (params[0].nBytesData == 4 && params[0].Data >= buffer && params[0].Data + 4 <= buffer + size));
            CHECK(params[1].nBytesData || !params[1].Data);
            CHECK(size == params[0].nBytesData + params[1].nBytesData);
            if (size) {                                        // too small a buffer: the size needed
                DWORD total = size; size = 1;
                CHECK(DhcpRequestParams(DHCPCAPI_REQUEST_SYNCHRONOUS, NULL, adapter, NULL, send, recd, buffer, &size, NULL) == ERROR_MORE_DATA && size == total);
            }
            printf("dhcp: options from the Mac's DHCP client (subnet mask %s, WPAD %s)\n", params[0].nBytesData ? "present" : "absent", params[1].nBytesData ? "present" : "absent");
        } else printf("dhcp: the adapter is not configured by DHCP\n");
    }
    free(a);
    DhcpCApiCleanup();
}

// ---- secur32 -------------------------------------------------------------------------------------------------------------
static void test_secur32(void) {
    WCHAR name[512]; ULONG size = 512;
    CHECK(GetUserNameExW(NameSamCompatible, name, &size) && size == wcslen(name));
    WCHAR user[256], computer[256], expected[600]; DWORD un = 256, cn = 256;
    CHECK(GetUserNameW(user, &un) && GetComputerNameW(computer, &cn));
    swprintf(expected, 600, L"%ls\\%ls", computer, user);
    CHECK(!wcscmp(name, expected));
    ULONG small = 3;
    SetLastError(0);
    CHECK(!GetUserNameExW(NameSamCompatible, name, &small) && GetLastError() == ERROR_MORE_DATA && small == wcslen(expected) + 1);
    size = 512; SetLastError(0);
    CHECK(!GetUserNameExW(NameUserPrincipal, name, &size) && GetLastError() == ERROR_NONE_MAPPED);
    size = 512; SetLastError(0);
    CHECK(!GetUserNameExW(NameDisplay, name, &size) && GetLastError() == ERROR_NONE_MAPPED);   // (the Mac's real name stays there)
    HANDLE lsa = (HANDLE)1;
    CHECK(LsaConnectUntrusted(&lsa) < 0 && !lsa);              // no LSA: an NTSTATUS error
    LSA_STRING origin = { 4, 5, (char *)"test" };
    TOKEN_SOURCE source = { "vxrt", { 0, 0 } };
    void *profile = (void *)1; ULONG profile_size = 7; LUID id; HANDLE token = (HANDLE)1; QUOTA_LIMITS quotas; NTSTATUS sub = 1;
    CHECK(LsaLogonUser(NULL, &origin, Network, 0, NULL, 0, NULL, &source, &profile, &profile_size, &id, &token, &quotas, &sub) < 0);
    CHECK(!profile && !profile_size && !token);
    CHECK(LsaDeregisterLogonProcess(NULL) < 0);
    CHECK(LsaFreeReturnBuffer(NULL) == 0);
    static const WCHAR *packages[] = { L"Negotiate", L"NTLM", L"Kerberos" };
    for (int i = 0; i < 3; i++) {
        PSecPkgInfoW info = (PSecPkgInfoW)1;
        CHECK(QuerySecurityPackageInfoW((WCHAR *)packages[i], &info) == SEC_E_SECPKG_NOT_FOUND && !info);
    }
}

// ---- oleaut32 --------------------------------------------------------------------------------------------------------------
static void test_oleaut32(void) {
    HMODULE ole = LoadLibraryA("oleaut32.dll");
    static const struct { int ordinal; const char *name; } ordinals[] = {
        { 77, "SafeArrayGetVartype" }, { 161, "LoadTypeLib" }, { 162, "LoadRegTypeLib" }, { 185, "VariantTimeToSystemTime" },
        { 277, "VarUI4FromStr" }, { 314, "VarBstrCmp" },
    };
    for (int i = 0; i < 6; i++) {
        FARPROC a = ole ? GetProcAddress(ole, MAKEINTRESOURCEA(ordinals[i].ordinal)) : NULL;
        if (!a || a != GetProcAddress(ole, ordinals[i].name)) { printf("ordinal %d is not %s\n", ordinals[i].ordinal, ordinals[i].name); failures++; }
    }
    SAFEARRAY *sa = SafeArrayCreateVector(VT_I4, 0, 3);
    VARTYPE vt = 0;
    HRESULT (WINAPI *get_vartype)(SAFEARRAY *, VARTYPE *) = ole ? (HRESULT (WINAPI *)(SAFEARRAY *, VARTYPE *))GetProcAddress(ole, MAKEINTRESOURCEA(77)) : NULL;
    CHECK(sa && get_vartype && get_vartype(sa, &vt) == S_OK && vt == VT_I4);
    if (sa) SafeArrayDestroy(sa);

    BSTR apple = SysAllocString(L"apple"), Apple = SysAllocString(L"Apple"), banana = SysAllocString(L"banana"), empty = SysAllocString(L"");
    CHECK(VarBstrCmp(apple, banana, LOCALE_USER_DEFAULT, 0) == VARCMP_LT);
    CHECK(VarBstrCmp(banana, apple, LOCALE_USER_DEFAULT, 0) == VARCMP_GT);
    CHECK(VarBstrCmp(apple, apple, LOCALE_USER_DEFAULT, 0) == VARCMP_EQ);
    CHECK(VarBstrCmp(Apple, apple, LOCALE_USER_DEFAULT, NORM_IGNORECASE) == VARCMP_EQ);
    CHECK(VarBstrCmp(NULL, empty, LOCALE_USER_DEFAULT, 0) == VARCMP_EQ);   // a NULL BSTR is an empty string
    CHECK(VarBstrCmp(NULL, apple, LOCALE_USER_DEFAULT, 0) == VARCMP_LT);
    SysFreeString(apple); SysFreeString(Apple); SysFreeString(banana); SysFreeString(empty);

    static const struct { const WCHAR *text; HRESULT hr; ULONG value; } numbers[] = {
        { L"42", S_OK, 42 }, { L"  1,234 ", S_OK, 1234 }, { L"2.5", S_OK, 2 }, { L"3.5", S_OK, 4 }, { L"0.5", S_OK, 0 },
        { L"2.50001", S_OK, 3 }, { L"0.4999", S_OK, 0 }, { L"1e3", S_OK, 1000 }, { L"12.5e-1", S_OK, 1 }, { L"&HFF", S_OK, 255 },
        { L"&O17", S_OK, 15 }, { L"4294967295", S_OK, 4294967295u }, { L"+7", S_OK, 7 }, { L"-0", S_OK, 0 }, { L"007", S_OK, 7 },
        { L"4294967296", DISP_E_OVERFLOW, 0 }, { L"-1", DISP_E_OVERFLOW, 0 }, { L"99999999999", DISP_E_OVERFLOW, 0 },
        { L"abc", DISP_E_TYPEMISMATCH, 0 }, { L"", DISP_E_TYPEMISMATCH, 0 }, { L"4 2", DISP_E_TYPEMISMATCH, 0 }, { L"&H", DISP_E_TYPEMISMATCH, 0 },
    };
    for (size_t i = 0; i < sizeof numbers / sizeof numbers[0]; i++) {
        ULONG v = 12345;
        HRESULT hr = VarUI4FromStr(numbers[i].text, LOCALE_USER_DEFAULT, 0, &v);
        if (hr != numbers[i].hr || (hr == S_OK && v != numbers[i].value)) { printf("VarUI4FromStr(\"%ls\"): %#lx %lu\n", numbers[i].text, (unsigned long)hr, v); failures++; }
    }

    static const struct { double v; WORD y, mo, dow, d, h, mi, s; } dates[] = {
        { 2.5, 1900, 1, 1, 1, 12, 0, 0 }, { 36526.75, 2000, 1, 6, 1, 18, 0, 0 }, { 0.0, 1899, 12, 6, 30, 0, 0, 0 },
        { -1.25, 1899, 12, 5, 29, 6, 0, 0 }, { 45000.0 + 1.0 / 86400 * 3661, 2023, 3, 3, 15, 1, 1, 1 },
    };
    for (size_t i = 0; i < sizeof dates / sizeof dates[0]; i++) {
        SYSTEMTIME st; memset(&st, 0xff, sizeof st);
        int ok = VariantTimeToSystemTime(dates[i].v, &st);
        if (!ok || st.wYear != dates[i].y || st.wMonth != dates[i].mo || st.wDayOfWeek != dates[i].dow || st.wDay != dates[i].d ||
            st.wHour != dates[i].h || st.wMinute != dates[i].mi || st.wSecond != dates[i].s || st.wMilliseconds) {
            printf("VariantTimeToSystemTime(%f): %d %u-%u-%u (%u) %u:%u:%u\n", dates[i].v, ok, st.wYear, st.wMonth, st.wDay, st.wDayOfWeek, st.wHour, st.wMinute, st.wSecond);
            failures++;
        }
        double back = 0;
        CHECK(SystemTimeToVariantTime(&st, &back) && back > dates[i].v - 1e-6 && back < dates[i].v + 1e-6);
    }
    SYSTEMTIME st;
    CHECK(!VariantTimeToSystemTime(3000000.0, &st) && !VariantTimeToSystemTime(-700000.0, &st));

    ITypeLib *lib = (ITypeLib *)1;
    CHECK(FAILED(LoadTypeLib(L"C:\\made-up\\library.tlb", &lib)) && !lib);
    static const GUID made_up = { 0x12345678, 0x1234, 0x5678, { 1, 2, 3, 4, 5, 6, 7, 8 } };
    lib = (ITypeLib *)1;
    CHECK(LoadRegTypeLib(&made_up, 1, 0, 0, &lib) == TYPE_E_LIBNOTREGISTERED && !lib);
}

int main(void) {
    test_environment();
    test_app_container();
    test_app_container_launch();
    test_policy();
    test_printers();
    test_dhcp();
    test_secur32();
    test_oleaut32();
    printf("sysdlls: userenv, winspool, dhcpcsvc, secur32, oleaut32 ordinals: %d failed checks -> %s\n", failures, failures ? "FAILED" : "ok");
    return failures;
}
