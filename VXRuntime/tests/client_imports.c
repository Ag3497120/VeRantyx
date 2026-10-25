// The functions the Windows Steam client's sign-in path imports and the runtime lacked, each against Microsoft's
// documentation, with made-up data:
//   imagehlp      ImageEnumerateCertificates / ImageAddCertificate / ImageRemoveCertificate (and the header and data
//                 of one) on a copy of this program (no certificates), given two made-up ones
//   advapi32      RegDeleteTreeA (a whole tree; a key's contents only), OpenEventLogA / ReadEventLogW / CloseEventLog
//                 (no records here), SetLocalTime (no privilege to set the clock)
//   ws2_32        WSASendMsg to this process's own UDP socket: two buffers, a source address (IP_PKTINFO), a control
//                 message it cannot send
//   user32        ShutdownBlockReasonCreate/Query/Destroy (from the window's thread only), InitializeTouchInjection and
//                 InjectTouchInput (no touch input here), SetKeyboardState (the thread's own state)
//   shell32       SHOpenWithDialog (no "Open with" dialog here)
//   kernel32      Beep's argument check; CreateProcess of a 32-bit program fails with ERROR_BAD_EXE_FORMAT
//   ntdll         RtlCaptureStackBackTrace: this program's own frames, the hash their sum
//   ntdll         NtQueryObject: a file opened for reading has no write access granted
//   ole32         the Spell Checking API's factory: no languages
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <imagehlp.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("check failed (line %d): ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- imagehlp ------------------------------------------------------------------------------------------------------------
static void certificates(void) {
    char self[MAX_PATH], copy[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof self);
    GetTempPathA(sizeof copy, copy);
    strcat(copy, "client_imports_cert.exe");
    CHECK(CopyFileA(self, copy, FALSE), "CopyFile: %lu", GetLastError());
    HANDLE f = CreateFileA(copy, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(f != INVALID_HANDLE_VALUE, "open the copy: %lu", GetLastError());
    DWORD size0 = GetFileSize(f, NULL), count = 99, idx[4] = { 9, 9, 9, 9 };
    CHECK(ImageEnumerateCertificates(f, CERT_SECTION_TYPE_ANY, &count, idx, 4) && count == 0, "none at first: %lu", count);
    struct { WIN_CERTIFICATE h; BYTE rest[13]; } one = { { 8 + 13, WIN_CERT_REVISION_2_0, WIN_CERT_TYPE_PKCS_SIGNED_DATA, { 0 } }, { 0 } };
    memcpy(one.h.bCertificate, "made-up data!", 13);
    struct { WIN_CERTIFICATE h; BYTE rest[3]; } two = { { 8 + 4, WIN_CERT_REVISION_2_0, WIN_CERT_TYPE_X509, { 'm' } }, { 'a', 'd', 'e' } };
    DWORD at = 99;
    CHECK(ImageAddCertificate(f, &one.h, &at) && at == 0, "add the first: %lu at %lu", GetLastError(), at);
    CHECK(ImageAddCertificate(f, &two.h, &at) && at == 1, "add the second: %lu at %lu", GetLastError(), at);
    CHECK(ImageEnumerateCertificates(f, CERT_SECTION_TYPE_ANY, &count, idx, 4) && count == 2 && idx[0] == 0 && idx[1] == 1, "two: %lu", count);
    CHECK(ImageEnumerateCertificates(f, WIN_CERT_TYPE_X509, &count, idx, 4) && count == 1 && idx[0] == 1, "one X.509: %lu [%lu]", count, idx[0]);
    WIN_CERTIFICATE hdr;
    CHECK(ImageGetCertificateHeader(f, 1, &hdr) && hdr.dwLength == 12 && hdr.wCertificateType == WIN_CERT_TYPE_X509, "the second's header");
    BYTE data[64]; DWORD len = 4;
    CHECK(!ImageGetCertificateData(f, 0, (WIN_CERTIFICATE *)data, &len) && GetLastError() == ERROR_INSUFFICIENT_BUFFER && len == 21, "too small: %lu", len);
    len = sizeof data;
    CHECK(ImageGetCertificateData(f, 0, (WIN_CERTIFICATE *)data, &len) && len == 21 && !memcmp(data + 8, "made-up data!", 13), "the first's data");
    DWORD size2 = GetFileSize(f, NULL);
    CHECK(size2 == ((size0 + 7) & ~7u) + 24 + 16, "the table at the end, 8-byte entries: %lu -> %lu", size0, size2);
    CHECK(ImageRemoveCertificate(f, 0), "remove the first: %lu", GetLastError());
    CHECK(ImageEnumerateCertificates(f, CERT_SECTION_TYPE_ANY, &count, NULL, 0) && count == 1, "one left: %lu", count);
    CHECK(ImageGetCertificateHeader(f, 0, &hdr) && hdr.wCertificateType == WIN_CERT_TYPE_X509, "the X.509 one moved down");
    CHECK(!ImageRemoveCertificate(f, 5) && GetLastError() == ERROR_INVALID_PARAMETER, "no such index: %lu", GetLastError());
    CHECK(ImageRemoveCertificate(f, 0), "remove the last");
    CHECK(ImageEnumerateCertificates(f, CERT_SECTION_TYPE_ANY, &count, NULL, 0) && count == 0, "none again");
    CHECK(GetFileSize(f, NULL) <= ((size0 + 7) & ~7u), "the file is no longer than its table needed: %lu", GetFileSize(f, NULL));
    CloseHandle(f);
    HANDLE h = CreateFileA("C:\\windows\\win.ini", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        CHECK(!ImageEnumerateCertificates(h, CERT_SECTION_TYPE_ANY, &count, NULL, 0) && GetLastError() == ERROR_BAD_EXE_FORMAT, "not a PE file: %lu", GetLastError());
        CloseHandle(h);
    }
    DeleteFileA(copy);
}

// ---- advapi32 --------------------------------------------------------------------------------------------------------------
static void registry_tree(void) {
    HKEY k;
    CHECK(!RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\VXRT test\\tree\\a\\b", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL), "create");
    RegSetValueExA(k, "made-up", 0, REG_SZ, (const BYTE *)"x", 2);
    RegCloseKey(k);
    CHECK(!RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\VXRT test\\tree\\c", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL), "create c");
    RegCloseKey(k);
    CHECK(!RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\VXRT test\\tree", 0, KEY_ALL_ACCESS, &k), "open tree");
    RegSetValueExA(k, "top", 0, REG_SZ, (const BYTE *)"y", 2);
    CHECK(RegDeleteTreeA(k, NULL) == ERROR_SUCCESS, "the contents");
    DWORD subkeys = 9, values = 9;
    RegQueryInfoKeyA(k, NULL, NULL, NULL, &subkeys, NULL, NULL, &values, NULL, NULL, NULL, NULL);
    CHECK(subkeys == 0 && values == 0, "emptied, the key stays: %lu subkeys, %lu values", subkeys, values);
    RegCloseKey(k);
    CHECK(!RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\VXRT test\\tree\\d\\e", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL), "create d");
    RegCloseKey(k);
    CHECK(RegDeleteTreeA(HKEY_CURRENT_USER, "Software\\VXRT test\\tree") == ERROR_SUCCESS, "the whole tree");
    CHECK(RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\VXRT test\\tree", 0, KEY_READ, &k) == ERROR_FILE_NOT_FOUND, "gone");
    CHECK(RegDeleteTreeA(HKEY_CURRENT_USER, "Software\\VXRT test\\tree") == ERROR_FILE_NOT_FOUND, "not there");
    RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\VXRT test");
}
static void event_log(void) {
    HANDLE h = OpenEventLogA(NULL, "Application");
    CHECK(h != NULL, "Application: %lu", GetLastError());
    BYTE buf[256]; DWORD got = 9, need = 9;
    CHECK(!ReadEventLogW(h, EVENTLOG_SEQUENTIAL_READ | EVENTLOG_FORWARDS_READ, 0, buf, sizeof buf, &got, &need) &&
          GetLastError() == ERROR_HANDLE_EOF && got == 0 && need == 0, "no records: %lu", GetLastError());
    CHECK(!ReadEventLogW(h, EVENTLOG_SEQUENTIAL_READ, 0, buf, sizeof buf, &got, &need) && GetLastError() == ERROR_INVALID_PARAMETER, "no direction");
    DWORD n = 9;
    CHECK(GetNumberOfEventLogRecords(h, &n) && n == 0, "count");
    CHECK(CloseEventLog(h), "close");
    CHECK(!CloseEventLog(h) && GetLastError() == ERROR_INVALID_HANDLE, "closed already");
    HANDLE made_up = OpenEventLogA(NULL, "Made-up log");
    CHECK(made_up != NULL, "a name that is no log opens the Application log");
    CloseEventLog(made_up);
    CHECK(!OpenEventLogA("\\\\made-up-server", "Application") && GetLastError() == RPC_S_SERVER_UNAVAILABLE, "another computer: %lu", GetLastError());
    CHECK(!OpenEventLogA(NULL, "Security") && GetLastError() == ERROR_PRIVILEGE_NOT_HELD, "Security: %lu", GetLastError());
}
static void local_time(void) {
    SYSTEMTIME st = { 2026, 2, 0, 30, 12, 0, 0, 0 };
    CHECK(!SetLocalTime(&st) && GetLastError() == ERROR_INVALID_PARAMETER, "30 February: %lu", GetLastError());
    st.wDay = 28;
    CHECK(!SetLocalTime(&st) && GetLastError() == ERROR_PRIVILEGE_NOT_HELD, "no privilege: %lu", GetLastError());
    CHECK(!Beep(10, 1) && GetLastError() == ERROR_INVALID_PARAMETER, "Beep below 37 Hz");
}

// ---- ws2_32 -----------------------------------------------------------------------------------------------------------------
static void send_msg(void) {
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    int al = sizeof a;
    CHECK(!bind(s, (struct sockaddr *)&a, sizeof a) && !getsockname(s, (struct sockaddr *)&a, &al), "bind");
    int (WINAPI *send)(SOCKET, LPWSAMSG, DWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE) =
        (void *)GetProcAddress(GetModuleHandleA("ws2_32.dll"), "WSASendMsg");
    CHECK(send != NULL, "WSASendMsg");
    if (!send) return;
    char p1[] = "made-up ", p2[] = "datagram";
    WSABUF bufs[2] = { { 8, p1 }, { 8, p2 } };
    WSAMSG m = { (struct sockaddr *)&a, sizeof a, bufs, 2, { 0, NULL }, 0 };
    DWORD sent = 0;
    CHECK(send(s, &m, 0, &sent, NULL, NULL) == 0 && sent == 16, "two buffers: %d", WSAGetLastError());
    char got[64] = { 0 };
    CHECK(recv(s, got, sizeof got, 0) == 16 && !memcmp(got, "made-up datagram", 16), "received: %s", got);
    // a source address: WSACMSGHDR (length, level, type), then IN_PKTINFO
    union { WSACMSGHDR h; char b[WSA_CMSG_SPACE(sizeof(IN_PKTINFO))]; } ctl = { { 0 } };
    ctl.h.cmsg_len = WSA_CMSG_LEN(sizeof(IN_PKTINFO)); ctl.h.cmsg_level = IPPROTO_IP; ctl.h.cmsg_type = IP_PKTINFO;
    IN_PKTINFO pi = { .ipi_addr.s_addr = htonl(INADDR_LOOPBACK), .ipi_ifindex = 0 };
    memcpy(WSA_CMSG_DATA(&ctl.h), &pi, sizeof pi);
    m.Control.buf = ctl.b; m.Control.len = sizeof ctl.b;
    CHECK(send(s, &m, 0, &sent, NULL, NULL) == 0 && sent == 16, "with IP_PKTINFO: %d", WSAGetLastError());
    memset(got, 0, sizeof got);
    CHECK(recv(s, got, sizeof got, 0) == 16, "received again");
    ctl.h.cmsg_type = 99;                                          // (no such option)
    CHECK(send(s, &m, 0, &sent, NULL, NULL) == SOCKET_ERROR && WSAGetLastError() == WSAEINVAL, "an unknown control message: %d", WSAGetLastError());
    GUID id = WSAID_WSASENDMSG; void *fn = NULL; DWORD bytes;
    CHECK(!WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof id, &fn, sizeof fn, &bytes, NULL, NULL) && fn == (void *)send, "the extension pointer");
    closesocket(s);
}

// ---- user32, shell32 ----------------------------------------------------------------------------------------------------------
static HWND g_window;
static DWORD WINAPI other_thread(void *arg) {
    (void)arg;
    return !ShutdownBlockReasonCreate(g_window, L"made-up") && GetLastError() == ERROR_ACCESS_DENIED;
}
static void window_bits(void) {
    g_window = CreateWindowW(L"STATIC", L"made-up", 0, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    CHECK(ShutdownBlockReasonCreate(g_window, L"Saving made-up data"), "create a reason");
    WCHAR r[64]; DWORD n = 0;
    CHECK(ShutdownBlockReasonQuery(g_window, NULL, &n) && n == 20, "its size: %lu", n);
    n = 64;
    CHECK(ShutdownBlockReasonQuery(g_window, r, &n) && !wcscmp(r, L"Saving made-up data"), "the reason");
    HANDLE t = CreateThread(NULL, 0, other_thread, NULL, 0, NULL);
    DWORD code = 0;
    WaitForSingleObject(t, 5000); GetExitCodeThread(t, &code);
    CHECK(code == 1, "another thread may not");
    CHECK(ShutdownBlockReasonDestroy(g_window), "destroy");
    n = 64;
    CHECK(!ShutdownBlockReasonQuery(g_window, r, &n), "no reason now");
    DestroyWindow(g_window);
    CHECK(!InitializeTouchInjection(0, TOUCH_FEEDBACK_DEFAULT) && GetLastError() == ERROR_INVALID_PARAMETER, "0 contacts");
    CHECK(!InitializeTouchInjection(10, TOUCH_FEEDBACK_DEFAULT) && GetLastError() == ERROR_NOT_SUPPORTED, "no touch input: %lu", GetLastError());
    POINTER_TOUCH_INFO contact = { 0 };
    CHECK(!InjectTouchInput(1, &contact) && GetLastError() == ERROR_NOT_SUPPORTED, "inject");
    BYTE keys[256] = { 0 };
    keys[VK_SHIFT] = 0x80; keys[VK_CAPITAL] = 0x01;
    CHECK(SetKeyboardState(keys), "SetKeyboardState");
    CHECK(GetKeyState(VK_SHIFT) < 0 && (GetKeyState(VK_CAPITAL) & 1), "the thread's state: %x %x", GetKeyState(VK_SHIFT), GetKeyState(VK_CAPITAL));
    CHECK(!(GetAsyncKeyState(VK_SHIFT) & 0x8000), "not the keyboard's");
    memset(keys, 0, sizeof keys);
    SetKeyboardState(keys);
    OPENASINFO info = { L"C:\\made-up.txt", NULL, OAIF_EXEC };
    CHECK(SHOpenWithDialog(NULL, NULL) == E_INVALIDARG, "no info");
    CHECK(SHOpenWithDialog(NULL, &info) == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "no dialog here");
}

// ---- kernel32, ntdll ------------------------------------------------------------------------------------------------------------
static void create_32bit(const char *exe) {
    STARTUPINFOA si = { sizeof si }; PROCESS_INFORMATION pi;
    DWORD t0 = GetTickCount();
    CHECK(!CreateProcessA(exe, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi) && GetLastError() == ERROR_BAD_EXE_FORMAT,
          "a 32-bit program: %lu", GetLastError());
    CHECK(GetTickCount() - t0 < 1000, "at once");
}
typedef USHORT (WINAPI *capture_fn)(ULONG, ULONG, PVOID *, PULONG);
static capture_fn capture;
static void *frames[16]; static USHORT got; static ULONG hash;
__attribute__((noinline)) static void inner(void) { got = capture(0, 16, frames, &hash); __asm__ volatile(""); }
__attribute__((noinline)) static void middle(void) { inner(); __asm__ volatile(""); }
__attribute__((noinline)) static void outer(void) { middle(); __asm__ volatile(""); }
static void backtrace(void) {
    capture = (capture_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlCaptureStackBackTrace");
    CHECK(capture != NULL, "ntdll's RtlCaptureStackBackTrace");
    if (!capture) return;
    outer();
    CHECK(got >= 4, "frames: %u", got);
    char *f0 = frames[0], *f1 = frames[1], *f2 = frames[2], *f3 = frames[3];
    CHECK(f0 > (char *)inner && f0 < (char *)inner + 64, "the first in inner: %p (%p)", f0, (void *)inner);
    CHECK(f1 > (char *)middle && f1 < (char *)middle + 64, "then middle: %p (%p)", f1, (void *)middle);
    CHECK(f2 > (char *)outer && f2 < (char *)outer + 64, "then outer: %p (%p)", f2, (void *)outer);
    CHECK(f3 > (char *)backtrace && f3 < (char *)backtrace + 256, "then this function: %p", f3);
    ULONG sum = 0;
    for (USHORT i = 0; i < got; i++) sum += (ULONG)(ULONG_PTR)frames[i];
    CHECK(hash == sum, "the hash");
    void *again[4];
    USHORT n = capture(1, 2, again, NULL);                         // (skipping this function's own place)
    CHECK(n == 2 && got >= 5 && again[0] == frames[4], "skip 1: %u %p (%p): this function's caller first", n, again[0], frames[4]);
}
typedef LONG (WINAPI *query_object_fn)(HANDLE, int, void *, ULONG, ULONG *);
static void file_access(void) {
    query_object_fn query = (query_object_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryObject");
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, sizeof path);
    HANDLE r = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    struct { ULONG attributes; ACCESS_MASK granted; ULONG rest[12]; } info;
    CHECK(query && !query(r, 0, &info, sizeof info, NULL), "NtQueryObject");
    CHECK(!(info.granted & (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER)) &&
          (info.granted & FILE_READ_DATA), "read-only: %#lx", info.granted);
    CloseHandle(r);
}
static const GUID clsid_factory = { 0x7ab36653, 0x1796, 0x484b, { 0xbd, 0xfa, 0xe7, 0x4f, 0x1d, 0xb7, 0xc1, 0xdc } };
static const GUID iid_factory = { 0x8e018a9d, 0x2415, 0x4677, { 0xbf, 0x08, 0x79, 0x4e, 0xa6, 0x1f, 0x94, 0xbb } };
typedef struct factory { struct {
    HRESULT (WINAPI *qi)(struct factory *, const GUID *, void **); ULONG (WINAPI *addref)(struct factory *); ULONG (WINAPI *release)(struct factory *);
    HRESULT (WINAPI *languages)(struct factory *, IEnumString **); HRESULT (WINAPI *supported)(struct factory *, LPCWSTR, BOOL *);
    HRESULT (WINAPI *create)(struct factory *, LPCWSTR, void **);
} *v; } factory;
static void spelling(void) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    factory *f = NULL;
    CHECK(CoCreateInstance(&clsid_factory, NULL, CLSCTX_INPROC_SERVER, &iid_factory, (void **)&f) == S_OK && f, "the factory");
    if (!f) return;
    IEnumString *e = NULL; LPOLESTR s = NULL; ULONG n = 9;
    CHECK(f->v->languages(f, &e) == S_OK && e, "languages");
    CHECK(e && e->lpVtbl->Next(e, 1, &s, &n) == S_FALSE && n == 0, "none");
    if (e) e->lpVtbl->Release(e);
    BOOL yes = TRUE;
    CHECK(f->v->supported(f, L"en-US", &yes) == S_OK && !yes, "en-US is not supported");
    void *checker = (void *)1;
    CHECK(f->v->create(f, L"en-US", &checker) == E_INVALIDARG && !checker, "no checker");
    f->v->release(f);
}

int main(int argc, char **argv) {
    certificates();
    registry_tree();
    event_log();
    local_time();
    send_msg();
    window_bits();
    if (argc > 1) create_32bit(argv[1]);
    backtrace();
    file_access();
    spelling();
    printf("client_imports: certificates, RegDeleteTree, event log, WSASendMsg, shutdown reasons, touch injection, keyboard state, "
           "Open with, 32-bit CreateProcess, RtlCaptureStackBackTrace, file access, spell checking: %d failed checks -> %s\n",
           failures, failures ? "FAILED" : "ok");
    return failures != 0;
}
