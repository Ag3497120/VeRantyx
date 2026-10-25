// ShellExecute(Ex) as Microsoft documents it (src/kernel32.c), with made-up classes registered per user
// (HKCU\Software\Classes): a URL protocol (<scheme>\shell\open\command with %1), a document by its extension's ProgID
// (%1 and %*), a class whose default verb is named in its shell key, SEE_MASK_NOCLOSEPROCESS, and the documented
// failures (SE_ERR_NOASSOC / ERROR_NO_ASSOCIATION for nothing registered, SE_ERR_FNF for a document not there).
// The started program reports what it got through the registry, which also checks that one process sees what another
// saved there (src/registry.c): a game asks the running Steam to start it with steam://rungameid/<id>, and Steam's
// second steam.exe hands its command line to the first in the registry.
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("shellexec: FAILED %s (line %d, last error %lu)\n", #c, __LINE__, GetLastError()); failures++; } } while (0)
#define KEY "Software\\VXRTTest\\ShellExec"

static void set_sz(const char *key, const char *name, const char *value) {
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL)) { failures++; return; }
    RegSetValueExA(k, name, 0, REG_SZ, (const BYTE *)value, (DWORD)strlen(value) + 1);
    RegCloseKey(k);
}
static int got(char *out, DWORD cap) {
    DWORD type = 0, n = cap;
    out[0] = 0;
    return RegGetValueA(HKEY_CURRENT_USER, KEY, "got", RRF_RT_REG_SZ, &type, out, &n) == ERROR_SUCCESS;
}
// Runs it with SEE_MASK_NOCLOSEPROCESS, waits for the program, and returns its exit code (-1: not started).
static int run(const char *verb, const char *file, const char *params) {
    SHELLEXECUTEINFOA sei = { sizeof sei };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = verb; sei.lpFile = file; sei.lpParameters = params; sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExA(&sei)) return -1;
    CHECK((INT_PTR)sei.hInstApp > 32);
    CHECK(sei.hProcess != NULL);
    if (!sei.hProcess) return -1;
    CHECK(WaitForSingleObject(sei.hProcess, 20000) == WAIT_OBJECT_0);
    DWORD code = 0;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return (int)code;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "child")) {               // the registered program: what it was given
        char all[2048] = "";
        for (int i = 2; i < argc; i++) { if (i > 2) strcat(all, "|"); strncat(all, argv[i], sizeof all - strlen(all) - 2); }
        set_sz(KEY, "got", all);
        return 7;
    }
    char self[MAX_PATH], cmd[MAX_PATH * 2 + 64], buf[2048];
    GetModuleFileNameA(NULL, self, sizeof self);

    // 1. A URL protocol: %1 is the whole URL; the program's process handle; what it saved is seen here.
    set_sz("Software\\Classes\\vxrt-test", "", "URL:VXRT test protocol");
    set_sz("Software\\Classes\\vxrt-test", "URL Protocol", "");
    snprintf(cmd, sizeof cmd, "\"%s\" child \"%%1\"", self);
    set_sz("Software\\Classes\\vxrt-test\\shell\\open\\command", "", cmd);
    set_sz(KEY, "got", "");
    CHECK(run(NULL, "vxrt-test://rungameid/275850", NULL) == 7);
    CHECK(got(buf, sizeof buf) && !strcmp(buf, "vxrt-test://rungameid/275850"));
    set_sz(KEY, "got", "");
    CHECK(run("open", "VXRT-Test:other", NULL) == 7);           // (schemes compare without case)
    CHECK(got(buf, sizeof buf) && !strcmp(buf, "VXRT-Test:other"));
    // ShellExecute: > 32 when started.
    set_sz(KEY, "got", "");
    CHECK((INT_PTR)ShellExecuteA(NULL, "open", "vxrt-test://plain", NULL, NULL, SW_SHOWNORMAL) > 32);
    for (int i = 0; i < 200 && (!got(buf, sizeof buf) || !*buf); i++) Sleep(50);
    CHECK(!strcmp(buf, "vxrt-test://plain"));

    // 2. Nothing registered: SE_ERR_NOASSOC; ShellExecuteEx FALSE with ERROR_NO_ASSOCIATION and hInstApp SE_ERR_NOASSOC.
    CHECK((INT_PTR)ShellExecuteA(NULL, "open", "vxrt-unregistered-zz://x", NULL, NULL, SW_SHOWNORMAL) == SE_ERR_NOASSOC);
    CHECK((INT_PTR)ShellExecuteA(NULL, NULL, "https://example.invalid/", NULL, NULL, SW_SHOWNORMAL) == SE_ERR_NOASSOC);
    SHELLEXECUTEINFOA sei = { sizeof sei };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS; sei.lpFile = "vxrt-unregistered-zz://x"; sei.nShow = SW_SHOWNORMAL;
    SetLastError(0);
    CHECK(!ShellExecuteExA(&sei));
    CHECK(GetLastError() == ERROR_NO_ASSOCIATION);
    CHECK((INT_PTR)sei.hInstApp == SE_ERR_NOASSOC);
    CHECK(sei.hProcess == NULL);
    // A verb the class does not have.
    CHECK((INT_PTR)ShellExecuteA(NULL, "print", "vxrt-test://x", NULL, NULL, SW_SHOWNORMAL) == SE_ERR_NOASSOC);

    // 3. A document: its extension's ProgID, %1 the file and %* the parameters.
    char tmp[MAX_PATH], doc[MAX_PATH + 32], other[MAX_PATH + 32];
    GetTempPathA(sizeof tmp, tmp);
    snprintf(doc, sizeof doc, "%svxrt_shellexec.vxrtdoc", tmp);
    snprintf(other, sizeof other, "%svxrt_shellexec.vxrtnone", tmp);
    HANDLE f = CreateFileA(doc, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); CHECK(f != INVALID_HANDLE_VALUE); CloseHandle(f);
    f = CreateFileA(other, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); CHECK(f != INVALID_HANDLE_VALUE); CloseHandle(f);
    set_sz("Software\\Classes\\.vxrtdoc", "", "VXRT.TestDoc");
    snprintf(cmd, sizeof cmd, "\"%s\" child \"%%1\" %%*", self);
    set_sz("Software\\Classes\\VXRT.TestDoc\\shell\\open\\command", "", cmd);
    set_sz(KEY, "got", "");
    CHECK(run(NULL, doc, "extra") == 7);
    snprintf(cmd, sizeof cmd, "%s|extra", doc);
    CHECK(got(buf, sizeof buf) && !strcmp(buf, cmd));
    // Not there: SE_ERR_FNF; there but nothing registered for its extension: SE_ERR_NOASSOC.
    snprintf(cmd, sizeof cmd, "%svxrt_shellexec_missing.vxrtdoc", tmp);
    CHECK((INT_PTR)ShellExecuteA(NULL, "open", cmd, NULL, NULL, SW_SHOWNORMAL) == SE_ERR_FNF);
    CHECK((INT_PTR)ShellExecuteA(NULL, "open", other, NULL, NULL, SW_SHOWNORMAL) == SE_ERR_NOASSOC);

    // 4. The default verb: the shell key's default value names it.
    set_sz("Software\\Classes\\vxrt-verb\\shell", "", "go");
    snprintf(cmd, sizeof cmd, "\"%s\" child went \"%%1\"", self);
    set_sz("Software\\Classes\\vxrt-verb\\shell\\go\\command", "", cmd);
    set_sz(KEY, "got", "");
    CHECK(run(NULL, "vxrt-verb:1", NULL) == 7);
    CHECK(got(buf, sizeof buf) && !strcmp(buf, "went|vxrt-verb:1"));

    // 5. A program: started with its parameters.
    set_sz(KEY, "got", "");
    CHECK(run(NULL, self, "child a b") == 7);
    CHECK(got(buf, sizeof buf) && !strcmp(buf, "a|b"));

    DeleteFileA(doc); DeleteFileA(other);
    if (!failures) printf("shellexec: ok\n");
    return failures != 0;
}
