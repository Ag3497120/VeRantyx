// Functions games look up by name (GetProcAddress) before their own code runs: the Steam DRM stub of No Man's Sky
// takes msvcrt.dll!vsprintf, and its libraries register classic (WPP) event-trace providers. No Man's Sky's own imports:
// VkKeyScanA, ToAscii, the shlwapi path calls, SHCreateDirectoryExW, IsTextUnicode, GetProcessIdOfThread,
// _set_purecall_handler, comctl32 ordinal 345 (TaskDialogIndirect); the Steam overlay's GetSystemWindowsDirectoryW,
// VkKeyScanExW and cfgmgr32 (a device tree with no nodes); API sets for version.dll and cfgmgr32. A built-in DLL's
// handle is a PE image whose export directory a program can walk itself (NVIDIA Streamline does, for dxgi.dll).
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static int failures;
// What a program walking a module's export directory finds for a name (Microsoft's PE format), NULL if absent.
static void *walk_exports(HMODULE m, const char *want) {
    const BYTE *base = (const BYTE *)m;
    if (base[0] != 'M' || base[1] != 'Z') return NULL;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return NULL;
    const IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd->VirtualAddress) return NULL;
    const IMAGE_EXPORT_DIRECTORY *ed = (const IMAGE_EXPORT_DIRECTORY *)(base + dd->VirtualAddress);
    const DWORD *names = (const DWORD *)(base + ed->AddressOfNames), *funcs = (const DWORD *)(base + ed->AddressOfFunctions);
    const WORD *ords = (const WORD *)(base + ed->AddressOfNameOrdinals);
    for (DWORD lo = 0, hi = ed->NumberOfNames; lo < hi;) {         // names are sorted: a binary search, as loaders do
        DWORD mid = (lo + hi) / 2;
        int c = strcmp((const char *)base + names[mid], want);
        if (!c) return (void *)(base + funcs[ords[mid]]);
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)

typedef int (__cdecl *vsprintf_fn)(char *, const char *, va_list);
static vsprintf_fn msvcrt_vsprintf;
static int call_vsprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = msvcrt_vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

typedef struct { const GUID *guid; HANDLE handle; } guid_registration;
typedef ULONG (WINAPI *register_fn)(void *, void *, const GUID *, ULONG, guid_registration *, const WCHAR *, const WCHAR *, ULONG64 *);
typedef ULONG (WINAPI *unregister_fn)(ULONG64);
typedef UCHAR (WINAPI *level_fn)(ULONG64);

int main(void) {
    msvcrt_vsprintf = (vsprintf_fn)GetProcAddress(LoadLibraryA("msvcrt.dll"), "vsprintf");
    CHECK(msvcrt_vsprintf != NULL);
    if (msvcrt_vsprintf) {
        char buf[64];
        CHECK(call_vsprintf(buf, "%s:%08X:%d", "load", 0x10016u, -7) == 16 && !strcmp(buf, "load:00010016:-7"));
    }

    HMODULE advapi = LoadLibraryA("advapi32.dll");
    register_fn reg = (register_fn)GetProcAddress(advapi, "RegisterTraceGuidsW");
    unregister_fn unreg = (unregister_fn)GetProcAddress(advapi, "UnregisterTraceGuids");
    level_fn level = (level_fn)GetProcAddress(advapi, "GetTraceEnableLevel");
    CHECK(reg && unreg && level);
    if (reg && unreg && level) {
        static const GUID control = { 0x12345678, 0x1234, 0x5678, { 1, 2, 3, 4, 5, 6, 7, 8 } };
        guid_registration regs[2] = { { &control, NULL }, { &control, NULL } };
        ULONG64 handle = 0;
        CHECK(reg(NULL, NULL, &control, 2, regs, NULL, NULL, &handle) == ERROR_SUCCESS);
        CHECK(handle != 0 && regs[0].handle != NULL && regs[1].handle != NULL);
        CHECK(level(handle) == 0);                                   // no session enables the provider
        CHECK(unreg(handle) == ERROR_SUCCESS);
    }
    HMODULE user32 = LoadLibraryA("user32.dll"), shlwapi = LoadLibraryA("shlwapi.dll"), shell32 = LoadLibraryA("shell32.dll");
    SHORT (WINAPI *vkscan)(CHAR) = (void *)GetProcAddress(user32, "VkKeyScanA");
    int (WINAPI *to_ascii)(UINT, UINT, const BYTE *, WORD *, UINT) = (void *)GetProcAddress(user32, "ToAscii");
    CHECK(vkscan && to_ascii);
    if (vkscan && to_ascii) {
        CHECK(vkscan('q') == 'Q' && vkscan('Q') == 0x100 + 'Q' && vkscan('4') == '4' && vkscan('$') == 0x100 + '4');
        CHECK(vkscan(';') == 0xBA && vkscan('?') == 0x1BF && vkscan(' ') == ' ');
        BYTE keys[256] = { 0 }; WORD ch = 0;
        CHECK(to_ascii('K', 0, keys, &ch, 0) == 1 && ch == 'k');
        keys[VK_SHIFT] = 0x80;
        CHECK(to_ascii('K', 0, keys, &ch, 0) == 1 && ch == 'K');
    }
    BOOL (WINAPI *append)(WCHAR *, const WCHAR *) = (void *)GetProcAddress(shlwapi, "PathAppendW");
    BOOL (WINAPI *root)(WCHAR *) = (void *)GetProcAddress(shlwapi, "PathStripToRootW");
    BOOL (WINAPI *pretty)(WCHAR *) = (void *)GetProcAddress(shlwapi, "PathMakePrettyW");
    CHECK(append && root && pretty);
    if (append && root && pretty) {
        WCHAR p[MAX_PATH] = L"C:\\Games\\Atlas";
        CHECK(append(p, L"\\Saves\\slot3.hg") && !wcscmp(p, L"C:\\Games\\Atlas\\Saves\\slot3.hg"));
        CHECK(root(p) && !wcscmp(p, L"C:\\"));
        WCHAR unc[] = L"\\\\host7\\share\\dir\\f.txt", rel[] = L"dir\\f";
        CHECK(root(unc) && !wcscmp(unc, L"\\\\host7\\share") && !root(rel));
        WCHAR caps[] = L"C:\\SPACE\\ANOMALY.TXT", mixed[] = L"C:\\Space";
        CHECK(pretty(caps) && !wcscmp(caps, L"c:\\space\\anomaly.txt") && !pretty(mixed) && !wcscmp(mixed, L"C:\\Space"));
    }
    int (WINAPI *mkdirs)(HWND, const WCHAR *, void *) = (void *)GetProcAddress(shell32, "SHCreateDirectoryExW");
    CHECK(mkdirs != NULL);
    if (mkdirs) {
        WCHAR base[MAX_PATH], deep[MAX_PATH];
        GetTempPathW(MAX_PATH, base);
        swprintf(deep, MAX_PATH, L"%lsvx_nms_%lu\\a\\b\\c", base, GetCurrentProcessId());
        CHECK(mkdirs(NULL, deep, NULL) == ERROR_SUCCESS && (GetFileAttributesW(deep) & FILE_ATTRIBUTE_DIRECTORY));
        CHECK(mkdirs(NULL, deep, NULL) == ERROR_ALREADY_EXISTS && mkdirs(NULL, L"rel\\dir", NULL) == ERROR_BAD_PATHNAME);
        for (int i = 0; i < 4; i++) { RemoveDirectoryW(deep); *wcsrchr(deep, L'\\') = 0; }
    }
    BOOL (WINAPI *textuni)(const void *, int, int *) = (void *)GetProcAddress(LoadLibraryA("advapi32.dll"), "IsTextUnicode");
    CHECK(textuni != NULL);
    if (textuni) {
        int r = IS_TEXT_UNICODE_SIGNATURE;
        CHECK(textuni("\xff\xfeh\0i\0", 6, &r) && r == IS_TEXT_UNICODE_SIGNATURE);
        r = IS_TEXT_UNICODE_ASCII16;
        CHECK(textuni(L"warp", 8, &r) && r == IS_TEXT_UNICODE_ASCII16);
        r = IS_TEXT_UNICODE_ASCII16;
        CHECK(!textuni("plain bytes", 11, &r) && r == 0);
    }
    DWORD (WINAPI *pidof)(HANDLE) = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetProcessIdOfThread");
    CHECK(pidof && pidof(GetCurrentThread()) == GetCurrentProcessId() && pidof((HANDLE)(ULONG_PTR)0x7777770) == 0);
    void *(__cdecl *purecall)(void *) = (void *)GetProcAddress(LoadLibraryA("vcruntime140.dll"), "_set_purecall_handler");
    CHECK(purecall && purecall((void *)main) == NULL && purecall(NULL) == (void *)main);
    CHECK(GetProcAddress(LoadLibraryA("comctl32.dll"), (LPCSTR)345) == GetProcAddress(LoadLibraryA("comctl32.dll"), "TaskDialogIndirect"));
    UINT (WINAPI *windir)(WCHAR *, UINT) = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetSystemWindowsDirectoryW");
    SHORT (WINAPI *vkscanex)(WCHAR, HKL) = (void *)GetProcAddress(user32, "VkKeyScanExW");
    CHECK(windir && vkscanex);
    if (windir && vkscanex) {
        WCHAR wd[MAX_PATH], gw[MAX_PATH];
        UINT n = windir(wd, MAX_PATH);
        CHECK(n && n == GetWindowsDirectoryW(gw, MAX_PATH) && !wcscmp(wd, gw) && windir(NULL, 0) == n + 1);
        CHECK(vkscanex(L'z', NULL) == 'Z' && vkscanex(L'"', NULL) == 0x1DE && vkscanex(0x263A, NULL) == -1);
    }
    HMODULE cfg = LoadLibraryA("cfgmgr32.dll");
    DWORD (WINAPI *locate)(DWORD *, const WCHAR *, ULONG) = (void *)GetProcAddress(cfg, "CM_Locate_DevNodeW");
    DWORD (WINAPI *list_size)(ULONG *, const WCHAR *, ULONG) = (void *)GetProcAddress(cfg, "CM_Get_Device_ID_List_SizeW");
    DWORD (WINAPI *list)(const WCHAR *, WCHAR *, ULONG, ULONG) = (void *)GetProcAddress(cfg, "CM_Get_Device_ID_ListW");
    CHECK(cfg && locate && list_size && list);
    if (locate && list_size && list) {
        DWORD node = 99; ULONG len = 0; WCHAR ids[8] = { 'x', 'x' };
        CHECK(locate(&node, L"PCI\\VEN_1234&DEV_5678", 0) == 0x0D /* CR_NO_SUCH_DEVNODE */ && node == 0);
        CHECK(list_size(&len, NULL, 0) == 0 && len >= 1 && list(NULL, ids, len, 0) == 0 && ids[0] == 0);
    }
    CHECK(GetProcAddress(LoadLibraryA("api-ms-win-core-version-l1-1-0.dll"), "VerQueryValueW") != NULL);
    CHECK(GetProcAddress(LoadLibraryA("api-ms-win-devices-config-l1-1-1.dll"), "CM_MapCrToWin32Err") != NULL);
    HMODULE dxgi = LoadLibraryA("dxgi.dll"), k32 = GetModuleHandleA("kernel32.dll");
    void *walked = walk_exports(dxgi, "CreateDXGIFactory1");
    CHECK(walked != NULL && walk_exports(dxgi, "NoSuchExport") == NULL);
    if (walked) {
        const BYTE *stub = walked;                                   // jmp qword [rip+0] to the function itself
        void *target; memcpy(&target, stub + 6, 8);
        CHECK(stub[0] == 0xff && stub[1] == 0x25 && target == (void *)GetProcAddress(dxgi, "CreateDXGIFactory1"));
    }
    DWORD (WINAPI *pid)(void) = (DWORD (WINAPI *)(void))walk_exports(k32, "GetCurrentProcessId");
    CHECK(pid && pid() == GetCurrentProcessId());
    char modname[MAX_PATH];
    CHECK(GetModuleFileNameA(dxgi, modname, MAX_PATH) && strstr(modname, "dxgi") && GetModuleHandleA("dxgi.dll") == dxgi);
    DWORD was = 0;                                                   // a hooking library's VirtualProtect on runtime code
    CHECK(!VirtualProtect((void *)GetProcAddress(k32, "GetTickCount"), 1, PAGE_EXECUTE_READWRITE, &was) && GetLastError() == ERROR_INVALID_ADDRESS);
    BYTE *own = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(own && VirtualProtect(own, 16, PAGE_EXECUTE_READWRITE, &was) && was == PAGE_READWRITE);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    LONG (WINAPI *rtl_version)(OSVERSIONINFOEXW *) = (void *)GetProcAddress(ntdll, "RtlGetVersion");
    LONG (WINAPI *rtl_verify)(OSVERSIONINFOEXW *, DWORD, DWORDLONG) = (void *)GetProcAddress(ntdll, "RtlVerifyVersionInfo");
    CHECK(rtl_version && rtl_verify);
    if (rtl_version && rtl_verify) {
        OSVERSIONINFOEXW ov = { sizeof ov };
        CHECK(rtl_version(&ov) == 0 && ov.dwMajorVersion == 10 && ov.dwBuildNumber >= 10240);
        OSVERSIONINFOEXW want = { sizeof want, 10, 0 };
        DWORDLONG cond = VerSetConditionMask(VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL), VER_MINORVERSION, VER_GREATER_EQUAL);
        CHECK(rtl_verify(&want, VER_MAJORVERSION | VER_MINORVERSION, cond) == 0);
        want.dwMajorVersion = 11;
        CHECK(rtl_verify(&want, VER_MAJORVERSION | VER_MINORVERSION, cond) == (LONG)0xC0000059);   // STATUS_REVISION_MISMATCH
    }
    LONG (WINAPI *timer_res)(ULONG, BOOLEAN, ULONG *) = (void *)GetProcAddress(ntdll, "NtSetTimerResolution");
    ULONG cur = 0;
    CHECK(timer_res && timer_res(10000, TRUE, &cur) == 0 && cur == 10000);
    // InitSecurityInterfaceA, as libcurl's Schannel backend uses it: the table's AcquireCredentialsHandleA (offset 0x18)
    void **(WINAPI *init_sspi)(void) = (void *)GetProcAddress(LoadLibraryA("secur32.dll"), "InitSecurityInterfaceA");
    CHECK(init_sspi != NULL);
    if (init_sspi) {
        void **table = init_sspi();
        LONG (WINAPI *enum_packages)(ULONG *, void **) = (void *)table[1];
        LONG (WINAPI *acquire)(char *, char *, ULONG, void *, void *, void *, void *, void *, void *) = (void *)table[3];
        ULONG count = 7; void *packages = (void *)1; char unisp[] = "Microsoft Unified Security Protocol Provider";
        BYTE cred[16], expiry[8];
        CHECK(table && *(ULONG *)table >= 1 && enum_packages && acquire);
        CHECK(enum_packages(&count, &packages) == 0 && count == 0);
        CHECK(acquire(NULL, unisp, 2 /* SECPKG_CRED_OUTBOUND */, NULL, NULL, NULL, NULL, cred, expiry) < 0);
    }
    printf("game imports: msvcrt vsprintf, classic event tracing, No Man's Sky's -> %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
