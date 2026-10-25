// The clipboard (src/clipboard.m), against Microsoft's documentation of the clipboard functions, with made-up text, on
// a pasteboard of the test's own (VXRT_PASTEBOARD, set by make check: never the user's): one opener at a time,
// EmptyClipboard makes the owner, text set as CF_UNICODETEXT reads back in all three text forms (and CF_TEXT set
// reads back as Unicode), a registered format keeps its number and its data in this process, the formats enumerate,
// the sequence number moves with each change.
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("check failed (line %d): ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static HGLOBAL global_of(const void *p, size_t n) {
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    memcpy(GlobalLock(g), p, n);
    GlobalUnlock(g);
    return g;
}
static DWORD WINAPI other_thread(void *arg) {
    (void)arg;
    BOOL opened = OpenClipboard(NULL);
    DWORD e = GetLastError();
    if (opened) CloseClipboard();
    return !opened && e == ERROR_ACCESS_DENIED;
}

int main(void) {
    if (!getenv("VXRT_PASTEBOARD")) { printf("clipboard: VXRT_PASTEBOARD is not set: not touching the user's clipboard\n"); return 1; }
    HWND w = CreateWindowW(L"STATIC", L"clipboard owner", 0, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    UINT fmt = RegisterClipboardFormatA("VXRT made-up format");
    CHECK(fmt >= 0xC000 && RegisterClipboardFormatW(L"vxrt MADE-UP format") == fmt, "the same name, the same number: %u", fmt);
    char name[64];
    CHECK(GetClipboardFormatNameA(fmt, name, sizeof name) && !strcmp(name, "VXRT made-up format"), "its name: %s", name);
    CHECK(!CloseClipboard() && GetLastError() == ERROR_CLIPBOARD_NOT_OPEN, "close before open");
    CHECK(!EmptyClipboard() && GetLastError() == ERROR_CLIPBOARD_NOT_OPEN, "empty before open");
    DWORD seq0 = GetClipboardSequenceNumber();
    CHECK(OpenClipboard(w), "open: %lu", GetLastError());
    HANDLE t = CreateThread(NULL, 0, other_thread, NULL, 0, NULL);
    DWORD code = 0;
    WaitForSingleObject(t, 5000); GetExitCodeThread(t, &code);
    CHECK(code == 1, "another thread cannot open it meanwhile");
    CHECK(GetOpenClipboardWindow() == w, "the window that has it open");
    CHECK(EmptyClipboard(), "empty");
    static const WCHAR text[] = L"made-up clipboard text \x00e9\x30c6";
    CHECK(SetClipboardData(CF_UNICODETEXT, global_of(text, sizeof text)) != NULL, "set text: %lu", GetLastError());
    static const char blob[] = "made-up private bytes";
    HGLOBAL mine = global_of(blob, sizeof blob);
    CHECK(SetClipboardData(fmt, mine) == mine, "set a private format");
    CHECK(CloseClipboard(), "close");
    CHECK(GetClipboardOwner() == w, "the owner");
    CHECK(GetClipboardSequenceNumber() != seq0, "the sequence number moved");
    CHECK(IsClipboardFormatAvailable(CF_UNICODETEXT) && IsClipboardFormatAvailable(CF_TEXT) && IsClipboardFormatAvailable(fmt), "available");
    CHECK(!IsClipboardFormatAvailable(CF_BITMAP), "no bitmap");
    CHECK(CountClipboardFormats() == 4, "four formats: %d", CountClipboardFormats());
    UINT order[] = { CF_BITMAP, fmt, CF_UNICODETEXT };
    CHECK(GetPriorityClipboardFormat(order, 3) == (int)fmt, "the first available of a list");
    CHECK(!GetClipboardData(CF_UNICODETEXT) && GetLastError() == ERROR_CLIPBOARD_NOT_OPEN, "read before open");
    CHECK(OpenClipboard(NULL), "open again");
    HANDLE u = GetClipboardData(CF_UNICODETEXT), a = GetClipboardData(CF_TEXT), p = GetClipboardData(fmt);
    const WCHAR *ut = u ? GlobalLock(u) : NULL;
    CHECK(ut && !wcscmp(ut, text), "the text back");
    CHECK(u && GlobalSize(u) >= sizeof text, "its size: %zu", u ? (size_t)GlobalSize(u) : 0);
    const char *at = a ? GlobalLock(a) : NULL;
    CHECK(at && !strncmp(at, "made-up clipboard text ", 23), "as CF_TEXT: %s", at ? at : "(none)");
    CHECK(p && !strcmp(GlobalLock(p), blob), "the private format back");
    UINT seen[8]; int n = 0;
    for (UINT f = EnumClipboardFormats(0); f && n < 8; f = EnumClipboardFormats(f)) seen[n++] = f;
    CHECK(n == 4 && seen[0] == CF_UNICODETEXT && seen[3] == fmt && GetLastError() == ERROR_SUCCESS, "enumerated: %d", n);
    CHECK(EmptyClipboard(), "empty again");
    static const char ansi[] = "made-up ANSI text";
    CHECK(SetClipboardData(CF_TEXT, global_of(ansi, sizeof ansi)) != NULL, "set CF_TEXT");
    HANDLE u2 = GetClipboardData(CF_UNICODETEXT);
    const WCHAR *ut2 = u2 ? GlobalLock(u2) : NULL;
    CHECK(ut2 && !wcscmp(ut2, L"made-up ANSI text"), "CF_TEXT read as Unicode");
    CHECK(!IsClipboardFormatAvailable(fmt), "the private format went with the emptying");
    CHECK(GetClipboardOwner() == NULL, "opened without a window: no owner");
    CHECK(CloseClipboard(), "close again");
    DestroyWindow(w);
    printf("clipboard: one opener at a time, owner, text in three forms, a registered format, enumeration, sequence number: "
           "%d failed checks -> %s\n", failures, failures ? "FAILED" : "ok");
    return failures != 0;
}
