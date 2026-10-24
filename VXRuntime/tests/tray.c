// The notification area on VXRuntime (src/tray.m, menus.m, icons.c), with made-up data only:
//   icons from bitmaps (CreateIconIndirect), from icon resource bits (the program's buffers, refilled, and its own
//   RT_ICON resource, shared), from an .ico file and from the program's own resources (tests/tray_ico.py,
//   tests/tray.rc), drawn back with DrawIconEx and taken apart with GetIconInfo; ExtractIconEx of made-up DLLs whose
//   resources are in the headers or in no section (tests/tray_pe.py, next to the .ico file);
//   the menu functions; Shell_NotifyIcon adding, changing and deleting icons of both structure forms (W and A);
//   the callback messages of clicks in the forms of version 4 and version 0, and TrackPopupMenu from them.
// The clicks and the menu choices come from tests/tray_check.py through Command Clips (VXRT_CONTROL); this program
// prints what it received and checks the rest itself.
//
// usage: tray.exe <the .ico file as a Windows path>
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("check failed (line %d): ", __LINE__); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

#define WM_TRAY (WM_APP + 1)
#define WM_TRAY_A (WM_APP + 2)
static const GUID icon_guid = { 0x12345678, 0x9abc, 0xdef0, { 1, 2, 3, 4, 5, 6, 7, 8 } };   // made up
static HWND g_main, g_second;
static HMENU g_menu, g_sub;
static HICON g_file16;
static int g_enter, g_exit, g_init, g_uninit;

// ---- icons ---------------------------------------------------------------------------------------------------------------
static int icon_width(HICON icon) {
    ICONINFO ii;
    if (!GetIconInfo(icon, &ii)) return -1;
    BITMAP bm = { 0 }, mm = { 0 };
    GetObjectW(ii.hbmColor, sizeof bm, &bm);
    GetObjectW(ii.hbmMask, sizeof mm, &mm);
    if (mm.bmBitsPixel != 1 || mm.bmWidth != bm.bmWidth) { printf("mask %ldx%ld %d bpp\n", mm.bmWidth, mm.bmHeight, mm.bmBitsPixel); return -2; }
    DeleteObject(ii.hbmColor); DeleteObject(ii.hbmMask);
    return bm.bmWidth;
}
// The icon drawn over white into a 32-bpp DIB section of size x size: the pixel at (x, y) as 0xRRGGBB.
static DWORD drawn(HICON icon, int size, int x, int y) {
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB } };
    void *bits;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(dc, bm);
    RECT r = { 0, 0, size, size };
    FillRect(dc, &r, GetStockObject(WHITE_BRUSH));
    CHECK(DrawIconEx(dc, 0, 0, icon, size, size, 0, NULL, DI_NORMAL), "DrawIconEx");
    DWORD p = ((DWORD *)bits)[y * size + x] & 0xffffff;
    SelectObject(dc, old); DeleteObject(bm); DeleteDC(dc);
    return p;
}
static int close_to(DWORD p, DWORD want) {
    for (int s = 0; s < 24; s += 8) { int a = (p >> s) & 0xff, b = (want >> s) & 0xff; if (a - b > 16 || b - a > 16) return 0; }
    return 1;
}
#define WHITE 0xffffff

// ExtractIconEx of the made-up DLLs of tests/tray_pe.py (next to the .ico file), whose resources are where a file may
// have them: the one icon group's picture (16x16, blue, its outer ring clear) or no icon, and nothing read outside the file.
static void pe_file(const WCHAR *ico_path, const WCHAR *name, WCHAR *out) {
    wcscpy(out, ico_path);
    WCHAR *slash = wcsrchr(out, L'\\');
    wcscpy(slash ? slash + 1 : out, name);
}
static void check_extract(const WCHAR *ico_path) {
    WCHAR path[MAX_PATH];
    HICON large = NULL, small = NULL;
    pe_file(ico_path, L"tray_pe_hdr.dll", path);                  // in the header area, in no section: there
    UINT count = ExtractIconExW(path, -1, NULL, NULL, 0), got = ExtractIconExW(path, 0, &large, &small, 1);
    CHECK(count == 1 && got == 1 && icon_width(large) == 32 && icon_width(small) == 16, "ExtractIconEx of resources in the headers: %u %u %p %p", count, got, (void *)large, (void *)small);
    if (large && small)
        CHECK(close_to(drawn(large, 32, 16, 16), 0x0000ff) && drawn(large, 32, 0, 0) == WHITE && close_to(drawn(small, 16, 8, 8), 0x0000ff) && drawn(small, 16, 0, 0) == WHITE,
              "the icon in the headers drawn: %06lx %06lx %06lx %06lx", drawn(large, 32, 16, 16), drawn(large, 32, 0, 0), drawn(small, 16, 8, 8), drawn(small, 16, 0, 0));
    DestroyIcon(large); DestroyIcon(small);
    static const struct { const WCHAR *name; UINT groups; } none[] = {
        { L"tray_pe_nosec.dll", 0 },                              // the directory in no section, past the headers
        { L"tray_pe_nodata.dll", 1 },                             // the group's data in no section
        { L"tray_pe_zero.dll", 1 },                               // the group's name and data entry at offset 0
    };
    for (int i = 0; i < 3; i++) {
        pe_file(ico_path, none[i].name, path);
        large = small = NULL;
        count = ExtractIconExW(path, -1, NULL, NULL, 0);
        got = ExtractIconExW(path, 0, &large, &small, 1);
        CHECK(count == none[i].groups && got == 0 && !large && !small, "ExtractIconEx of made-up DLL %d: %u groups, %u extracted", i, count, got);
    }
}

static void check_icons(const WCHAR *ico_path) {
    // From bitmaps: an orange disc in a 32-bpp DIB with alpha, a 1-bpp mask of zeros.
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), 32, -32, 1, 32, BI_RGB } };
    DWORD *px;
    HBITMAP color = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) px[y * 32 + x] = (x - 16) * (x - 16) + (y - 16) * (y - 16) < 144 ? 0xffff8000 : 0;
    HBITMAP mask = CreateBitmap(32, 32, 1, 1, NULL);
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON disc = CreateIconIndirect(&ii);
    CHECK(disc != NULL, "CreateIconIndirect");
    DeleteObject(color); DeleteObject(mask);                     // (the icon has its own copy)
    CHECK(icon_width(disc) == 32, "the disc's size: %d", icon_width(disc));
    CHECK(close_to(drawn(disc, 32, 16, 16), 0xff8000) && drawn(disc, 32, 1, 1) == WHITE, "the disc drawn: %06lx %06lx", drawn(disc, 32, 16, 16), drawn(disc, 32, 1, 1));
    ICONINFO got;
    CHECK(GetIconInfo(disc, &got) && got.fIcon && got.xHotspot == 16, "GetIconInfo of the disc");

    // From an icon resource's bits: 16x16, 4 bpp, every pixel yellow, the left half masked out.
    BYTE res[40 + 16 * 4 + 16 * 8 + 16 * 4];
    memset(res, 0, sizeof res);
    BITMAPINFOHEADER *h = (BITMAPINFOHEADER *)res;
    h->biSize = 40; h->biWidth = 16; h->biHeight = 32; h->biPlanes = 1; h->biBitCount = 4;
    DWORD *pal = (DWORD *)(res + 40);
    pal[1] = 0x00ffff00;                                         // yellow (an RGBQUAD: blue, green, red, 0)
    memset(res + 40 + 64, 0x11, 16 * 8);
    for (int y = 0; y < 16; y++) { res[40 + 64 + 128 + y * 4] = 0xff; }   // mask: x 0-7 transparent
    HICON yellow = CreateIconFromResourceEx(res, sizeof res, TRUE, 0x00030000, 0, 0, 0);
    CHECK(yellow && icon_width(yellow) == 16, "CreateIconFromResourceEx: %p %d", (void *)yellow, yellow ? icon_width(yellow) : 0);
    CHECK(close_to(drawn(yellow, 16, 12, 8), 0xffff00) && drawn(yellow, 16, 2, 8) == WHITE, "the yellow icon drawn: %06lx %06lx", drawn(yellow, 16, 12, 8), drawn(yellow, 16, 2, 8));
    HICON yellow32 = CreateIconFromResourceEx(res, sizeof res, TRUE, 0x00030000, 32, 32, 0);
    CHECK(icon_width(yellow32) == 32, "CreateIconFromResourceEx scaled: %d", icon_width(yellow32));
    CHECK(!CreateIconFromResourceEx(res, sizeof res, TRUE, 0x00010000, 0, 0, 0), "a version below 2.0 is refused");

    // CreateIconFromResource (LR_SHARED) of bits in the program's own buffers, filled anew between the calls (here
    // the stack, and a static buffer in the program's image): each call is an icon of what the buffer holds then,
    // which DestroyIcon frees.
    static BYTE static_res[sizeof res];
    BYTE *buffers[2] = { res, static_res };
    for (int b = 0; b < 2; b++) {
        BYTE *buf = buffers[b];
        if (buf != res) memcpy(buf, res, sizeof res);
        DWORD *bpal = (DWORD *)(buf + 40);
        bpal[1] = 0x00ffff00;                                    // yellow
        HICON first = CreateIconFromResource(buf, sizeof res, TRUE, 0x00030000);
        bpal[1] = 0x000000ff;                                    // blue
        HICON second = CreateIconFromResource(buf, sizeof res, TRUE, 0x00030000);
        CHECK(first && second && first != second, "CreateIconFromResource of a refilled %s buffer: %p %p", b ? "static" : "stack", (void *)first, (void *)second);
        CHECK(icon_width(first) == 32 && close_to(drawn(first, 32, 24, 16), 0xffff00) && close_to(drawn(second, 32, 24, 16), 0x0000ff),
              "the pictures of a refilled %s buffer: %06lx %06lx", b ? "static" : "stack", drawn(first, 32, 24, 16), drawn(second, 32, 24, 16));
        CHECK(DestroyIcon(first) && !DestroyIcon(first) && DestroyIcon(second) && !DestroyIcon(second), "icons of the program's bits are freed");
    }
    pal[1] = 0x00ffff00;

    // CreateIconFromResource of a module's RT_ICON resource (tray.rc's icon 1, the file's 16x16) is shared: the same
    // handle each time, kept by DestroyIcon. Without LR_SHARED a new icon each time.
    HMODULE self_module = GetModuleHandleW(NULL);
    HRSRC one = FindResourceW(self_module, MAKEINTRESOURCEW(1), (LPCWSTR)RT_ICON);
    BYTE *one_bits = one ? LockResource(LoadResource(self_module, one)) : NULL;
    DWORD one_size = one ? SizeofResource(self_module, one) : 0;
    HICON s1 = one_bits ? CreateIconFromResource(one_bits, one_size, TRUE, 0x00030000) : NULL;
    HICON s2 = one_bits ? CreateIconFromResource(one_bits, one_size, TRUE, 0x00030000) : NULL;
    CHECK(s1 && s1 == s2 && icon_width(s1) == 32 && close_to(drawn(s1, 32, 16, 16), 0x0000ff), "CreateIconFromResource of a resource: %p %p", (void *)s1, (void *)s2);
    CHECK(DestroyIcon(s1) && CreateIconFromResource(one_bits, one_size, TRUE, 0x00030000) == s1 && icon_width(s1) == 32, "a shared icon of a resource stays");
    HICON e1 = CreateIconFromResourceEx(one_bits, one_size, TRUE, 0x00030000, 0, 0, 0), e2 = CreateIconFromResourceEx(one_bits, one_size, TRUE, 0x00030000, 0, 0, 0);
    CHECK(e1 && e2 && e1 != e2 && e1 != s1 && icon_width(e1) == 16 && DestroyIcon(e1) && !DestroyIcon(e1) && DestroyIcon(e2), "CreateIconFromResourceEx of a resource without LR_SHARED");

    // From the .ico file: the entry for the size asked for, or the nearest larger one scaled.
    HICON f16 = LoadImageW(NULL, ico_path, IMAGE_ICON, 16, 16, LR_LOADFROMFILE);
    HICON f32 = LoadImageW(NULL, ico_path, IMAGE_ICON, 32, 32, LR_LOADFROMFILE);
    HICON f48 = LoadImageW(NULL, ico_path, IMAGE_ICON, 48, 48, LR_LOADFROMFILE);
    HICON f24 = LoadImageW(NULL, ico_path, IMAGE_ICON, 24, 24, LR_LOADFROMFILE);
    HICON f0 = LoadImageW(NULL, ico_path, IMAGE_ICON, 0, 0, LR_LOADFROMFILE);
    CHECK(f16 && f32 && f48 && f24 && f0, "LoadImage from the file: %p %p %p %p %p", (void *)f16, (void *)f32, (void *)f48, (void *)f24, (void *)f0);
    CHECK(icon_width(f16) == 16 && icon_width(f32) == 32 && icon_width(f48) == 48 && icon_width(f24) == 24 && icon_width(f0) == 32,
          "sizes from the file: %d %d %d %d %d", icon_width(f16), icon_width(f32), icon_width(f48), icon_width(f24), icon_width(f0));
    CHECK(close_to(drawn(f16, 16, 8, 8), 0x0000ff) && drawn(f16, 16, 0, 0) == WHITE, "16 (32 bpp, alpha): %06lx %06lx", drawn(f16, 16, 8, 8), drawn(f16, 16, 0, 0));
    CHECK(close_to(drawn(f32, 32, 16, 16), 0xff0000) && drawn(f32, 32, 2, 2) == WHITE, "32 (8 bpp, mask): %06lx %06lx", drawn(f32, 32, 16, 16), drawn(f32, 32, 2, 2));
    CHECK(close_to(drawn(f48, 48, 40, 24), 0x00ff00) && drawn(f48, 48, 5, 24) == WHITE, "48 (PNG): %06lx %06lx", drawn(f48, 48, 40, 24), drawn(f48, 48, 5, 24));
    CHECK(!LoadImageW(NULL, L"C:\\no such file.ico", IMAGE_ICON, 16, 16, LR_LOADFROMFILE), "LoadImage of a missing file");
    g_file16 = f16;

    // From the program's resources (tray.rc: icon group 1 of the same three pictures).
    HMODULE self = GetModuleHandleW(NULL);
    HICON r1 = LoadIconW(self, MAKEINTRESOURCEW(1)), r1b = LoadIconW(self, MAKEINTRESOURCEW(1));
    CHECK(r1 && r1 == r1b && icon_width(r1) == 32, "LoadIcon: %p %p %d", (void *)r1, (void *)r1b, r1 ? icon_width(r1) : 0);
    HICON r16 = LoadImageW(self, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
    CHECK(r16 && icon_width(r16) == 16 && close_to(drawn(r16, 16, 8, 8), 0x0000ff), "LoadImage from resources at 16");
    SetLastError(0);
    CHECK(!LoadIconW(self, MAKEINTRESOURCEW(99)) && GetLastError() == ERROR_RESOURCE_NAME_NOT_FOUND, "LoadIcon of a missing id: %lu", GetLastError());
    CHECK(LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION) != NULL, "LoadIcon(NULL, IDI_APPLICATION)");
    HRSRC group = FindResourceW(self, MAKEINTRESOURCEW(1), (LPCWSTR)RT_GROUP_ICON);
    BYTE *dir = group ? LockResource(LoadResource(self, group)) : NULL;
    CHECK(dir && LookupIconIdFromDirectoryEx(dir, TRUE, 16, 16, 0) == 1 && LookupIconIdFromDirectoryEx(dir, TRUE, 48, 48, 0) == 3,
          "LookupIconIdFromDirectoryEx: %d %d", dir ? LookupIconIdFromDirectoryEx(dir, TRUE, 16, 16, 0) : -1, dir ? LookupIconIdFromDirectoryEx(dir, TRUE, 48, 48, 0) : -1);
    WCHAR exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    HICON large = NULL, small = NULL;
    CHECK(ExtractIconExW(exe, -1, NULL, NULL, 0) == 1 && ExtractIconExW(ico_path, -1, NULL, NULL, 0) == 1, "ExtractIconEx counts");
    CHECK(ExtractIconExW(exe, 0, &large, &small, 1) == 1 && icon_width(large) == 32 && icon_width(small) == 16, "ExtractIconEx of the program");
    CHECK(ExtractIconExW(L"C:\\no such file.exe", -1, NULL, NULL, 0) == 0, "ExtractIconEx of a missing file");
    check_extract(ico_path);

    // Copies and destruction.
    HICON copy = CopyIcon(f48);
    CHECK(copy && copy != f48 && icon_width(copy) == 48, "CopyIcon");
    CHECK(DestroyIcon(copy) && !DestroyIcon(copy), "DestroyIcon twice");
    HICON small_copy = CopyImage(f48, IMAGE_ICON, 16, 16, 0);
    CHECK(icon_width(small_copy) == 16, "CopyImage to 16: %d", icon_width(small_copy));
    CHECK(DestroyIcon(r1) && LoadIconW(self, MAKEINTRESOURCEW(1)) == r1, "a shared icon stays");
    DestroyIcon(small_copy); DestroyIcon(large); DestroyIcon(small); DestroyIcon(yellow32); DestroyIcon(f24); DestroyIcon(f0);
}

// ---- menus ---------------------------------------------------------------------------------------------------------------
static void check_menus(void) {
    HMENU m = CreatePopupMenu(), sub = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 101, L"&Open");
    AppendMenuW(m, MF_STRING | MF_CHECKED, 102, L"Check me");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 103, L"Disabled");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sub, MF_STRING, 201, L"Deep item");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"More");
    AppendMenuW(m, MF_STRING, 104, L"E&xit");
    CHECK(GetMenuItemCount(m) == 6 && GetMenuItemCount(sub) == 1, "counts: %d %d", GetMenuItemCount(m), GetMenuItemCount(sub));
    CHECK(GetMenuItemID(m, 0) == 101 && GetMenuItemID(m, 4) == (UINT)-1 && GetSubMenu(m, 4) == sub, "ids and the submenu");
    CHECK(GetMenuState(m, 102, MF_BYCOMMAND) & MF_CHECKED, "GetMenuState: checked");
    CHECK(GetMenuState(m, 4, MF_BYPOSITION) == (1u << 8 | MF_POPUP), "GetMenuState of a submenu: %x", GetMenuState(m, 4, MF_BYPOSITION));
    CHECK(GetMenuState(m, 3, MF_BYPOSITION) & MF_SEPARATOR, "GetMenuState: separator");
    CHECK(CheckMenuItem(m, 102, MF_UNCHECKED) == MF_CHECKED && CheckMenuItem(m, 102, MF_CHECKED) == MF_UNCHECKED, "CheckMenuItem");
    CHECK(CheckMenuItem(m, 999, MF_CHECKED) == (DWORD)-1, "CheckMenuItem of nothing");
    CHECK(EnableMenuItem(m, 103, MF_ENABLED) == MF_GRAYED && EnableMenuItem(m, 103, MF_GRAYED) == MF_ENABLED, "EnableMenuItem");
    CHECK(EnableMenuItem(m, 201, MF_GRAYED) == MF_ENABLED && (GetMenuState(sub, 201, MF_BYCOMMAND) & MF_GRAYED), "by command in a submenu");
    EnableMenuItem(m, 201, MF_ENABLED);
    WCHAR buf[64];
    CHECK(GetMenuStringW(m, 104, buf, 64, MF_BYCOMMAND) == 5 && !wcscmp(buf, L"E&xit"), "GetMenuString");
    CHECK(GetMenuStringW(m, 104, NULL, 0, MF_BYCOMMAND) == 5 && GetMenuStringW(m, 104, buf, 3, MF_BYCOMMAND) == 2 && !wcscmp(buf, L"E&"), "GetMenuString lengths");
    MENUITEMINFOW mi = { sizeof mi, MIIM_STRING };
    CHECK(GetMenuItemInfoW(m, 101, FALSE, &mi) && mi.cch == 5, "GetMenuItemInfo length: %u", mi.cch);
    mi.dwTypeData = buf; mi.cch = 64;
    CHECK(GetMenuItemInfoW(m, 101, FALSE, &mi) && mi.cch == 5 && !wcscmp(buf, L"&Open"), "GetMenuItemInfo string");
    MENUITEMINFOW add = { sizeof add, MIIM_ID | MIIM_STRING | MIIM_DATA };
    add.wID = 105; add.dwTypeData = L"Top"; add.dwItemData = 0x1234;
    CHECK(InsertMenuItemW(m, 0, TRUE, &add) && GetMenuItemCount(m) == 7 && GetMenuItemID(m, 0) == 105, "InsertMenuItem");
    MENUITEMINFOW data = { sizeof data, MIIM_DATA | MIIM_ID | MIIM_STATE | MIIM_FTYPE };
    CHECK(GetMenuItemInfoW(m, 0, TRUE, &data) && data.dwItemData == 0x1234 && data.wID == 105 && data.fType == MFT_STRING, "GetMenuItemInfo data");
    MENUITEMINFOW st = { sizeof st, MIIM_STATE };
    st.fState = MFS_CHECKED | MFS_GRAYED;
    CHECK(SetMenuItemInfoW(m, 105, FALSE, &st) && (GetMenuState(m, 105, MF_BYCOMMAND) & (MF_CHECKED | MF_GRAYED)) == (MF_CHECKED | MF_GRAYED), "SetMenuItemInfo state");
    CHECK(DeleteMenu(m, 105, MF_BYCOMMAND) && GetMenuItemCount(m) == 6 && !DeleteMenu(m, 105, MF_BYCOMMAND), "DeleteMenu");
    CHECK(InsertMenuW(m, 104, MF_BYCOMMAND | MF_STRING, 106, L"Before exit") && GetMenuItemID(m, 5) == 106 && GetMenuItemID(m, 6) == 104, "InsertMenu before a command");
    CHECK(RemoveMenu(m, 106, MF_BYCOMMAND) && GetMenuItemCount(m) == 6, "RemoveMenu");
    CHECK(ModifyMenuW(m, 101, MF_BYCOMMAND | MF_STRING, 101, L"&Open window") && GetMenuStringW(m, 101, buf, 64, MF_BYCOMMAND) == 12, "ModifyMenu");
    CHECK(SetMenuDefaultItem(m, 104, FALSE) && GetMenuDefaultItem(m, FALSE, 0) == 104 && GetMenuDefaultItem(m, TRUE, 0) == 5, "the default item");
    CHECK(CheckMenuRadioItem(m, 101, 102, 101, MF_BYCOMMAND) && (GetMenuState(m, 101, 0) & MF_CHECKED) && !(GetMenuState(m, 102, 0) & MF_CHECKED), "CheckMenuRadioItem");
    CheckMenuItem(m, 101, MF_UNCHECKED); CheckMenuItem(m, 102, MF_CHECKED);
    MENUINFO info = { sizeof info, MIM_MENUDATA };
    info.dwMenuData = 77;
    MENUINFO back = { sizeof back, MIM_MENUDATA };
    CHECK(SetMenuInfo(m, &info) && GetMenuInfo(m, &back) && back.dwMenuData == 77, "Set/GetMenuInfo");
    HMENU other = CreatePopupMenu(), kept = CreatePopupMenu();
    AppendMenuW(other, MF_POPUP, (UINT_PTR)kept, L"Kept");
    CHECK(RemoveMenu(other, 0, MF_BYPOSITION) && IsMenu(kept), "RemoveMenu keeps the submenu");
    AppendMenuW(other, MF_POPUP, (UINT_PTR)kept, L"Kept");
    CHECK(DestroyMenu(other) && !IsMenu(other) && !IsMenu(kept), "DestroyMenu takes its submenus");
    SetLastError(0);
    CHECK(GetMenuItemCount((HMENU)(UINT_PTR)0x1234) == -1 && GetLastError() == ERROR_INVALID_MENU_HANDLE, "an invalid menu: %lu", GetLastError());
    g_menu = m; g_sub = sub;
}

// ---- the notification area -------------------------------------------------------------------------------------------------
static NOTIFYICONDATAW icon_w(UINT flags) {
    NOTIFYICONDATAW n = { sizeof n };
    n.hWnd = g_main; n.uID = 7; n.uFlags = flags; n.uCallbackMessage = WM_TRAY;
    return n;
}
static NOTIFYICONDATAA icon_a(UINT flags) {
    NOTIFYICONDATAA n = { sizeof n };
    n.hWnd = g_second; n.uID = 8; n.uFlags = flags | NIF_GUID; n.uCallbackMessage = WM_TRAY_A; n.guidItem = icon_guid;
    return n;
}
static void icon_rect(UINT id, HRESULT want) {
    NOTIFYICONIDENTIFIER ident = { sizeof ident, g_main, id };
    RECT r = { 0 };
    HRESULT hr = Shell_NotifyIconGetRect(&ident, &r);
    if (want == S_OK && hr == E_FAIL) { printf("rect=none (the menu bar has no room for it?)\n"); return; }
    CHECK(hr == want, "Shell_NotifyIconGetRect(%u): %lx", id, hr);
    if (hr == S_OK) {
        // (in the menu bar of whichever display has it: the main display's, or one above it at negative y)
        CHECK(r.right > r.left && r.bottom > r.top && r.bottom - r.top < 100, "the icon's place: %ld %ld %ld %ld", r.left, r.top, r.right, r.bottom);
        printf("rect=%ld,%ld,%ld,%ld\n", r.left, r.top, r.right, r.bottom);
    }
}
static void add_icons(HICON first, HICON second) {
    NOTIFYICONDATAW n = icon_w(NIF_MESSAGE | NIF_ICON | NIF_TIP);
    n.hIcon = first;
    wcscpy(n.szTip, L"Tray test");
    CHECK(Shell_NotifyIconW(NIM_ADD, &n), "NIM_ADD");
    CHECK(!Shell_NotifyIconW(NIM_ADD, &n), "NIM_ADD of an icon that is there");
    n.uVersion = 9;
    CHECK(!Shell_NotifyIconW(NIM_SETVERSION, &n), "NIM_SETVERSION 9");
    n.uVersion = NOTIFYICON_VERSION_4;
    CHECK(Shell_NotifyIconW(NIM_SETVERSION, &n), "NIM_SETVERSION 4");
    NOTIFYICONDATAW m = icon_w(NIF_ICON | NIF_TIP | NIF_SHOWTIP);
    m.hIcon = g_file16;                                           // (the file's icon: 16 with its 32 and 48 kept)
    wcscpy(m.szTip, L"Tray test: ready");
    CHECK(Shell_NotifyIconW(NIM_MODIFY, &m), "NIM_MODIFY");
    NOTIFYICONDATAW b = icon_w(NIF_INFO);
    wcscpy(b.szInfoTitle, L"Made-up title"); wcscpy(b.szInfo, L"Made-up balloon");
    CHECK(Shell_NotifyIconW(NIM_MODIFY, &b), "NIM_MODIFY with a balloon");
    CHECK(Shell_NotifyIconW(NIM_SETFOCUS, &n), "NIM_SETFOCUS");
    icon_rect(7, S_OK);
    icon_rect(55, E_FAIL);

    NOTIFYICONDATAA a = icon_a(NIF_MESSAGE | NIF_ICON | NIF_TIP);
    a.hIcon = second;
    strcpy(a.szTip, "Second (A)");
    CHECK(Shell_NotifyIconA(NIM_ADD, &a), "NIM_ADD (A)");
    NOTIFYICONDATAA by_guid = icon_a(NIF_TIP);
    by_guid.uID = 999;                                            // (the GUID names it)
    strcpy(by_guid.szTip, "Second icon");
    CHECK(Shell_NotifyIconA(NIM_MODIFY, &by_guid), "NIM_MODIFY by GUID");
    NOTIFYICONDATAW none = icon_w(NIF_TIP);
    none.uID = 55;
    CHECK(!Shell_NotifyIconW(NIM_MODIFY, &none) && !Shell_NotifyIconW(NIM_DELETE, &none), "an icon that is not there");
    NOTIFYICONDATAW bad = icon_w(NIF_TIP);
    bad.hWnd = (HWND)(UINT_PTR)0x7777; bad.uID = 9;
    CHECK(!Shell_NotifyIconW(NIM_ADD, &bad), "NIM_ADD for no window");
}

static void show_menu(HWND hwnd, int x, int y, UINT flags) {
    SetForegroundWindow(hwnd);
    UINT r = TrackPopupMenu(g_menu, flags | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN, x, y, 0, hwnd, NULL);
    printf("%s=%u\n", flags & TPM_RETURNCMD ? "picked" : "track", r);
    fflush(stdout);
    if (r == 104 && (flags & TPM_RETURNCMD)) {                   // Exit: the version 4 icon goes
        NOTIFYICONDATAW d = icon_w(0);
        CHECK(Shell_NotifyIconW(NIM_DELETE, &d) && !Shell_NotifyIconW(NIM_DELETE, &d), "NIM_DELETE");
        icon_rect(7, E_FAIL);
    }
}

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY: {                                               // version 4: the event and id in lParam, the point in wParam
        UINT event = LOWORD(lp), id = HIWORD(lp);
        int x = GET_X_LPARAM(wp), y = GET_Y_LPARAM(wp);
        printf("event icon=%u msg=0x%x at=%d,%d\n", id, event, x, y);
        fflush(stdout);
        if (event == WM_CONTEXTMENU) show_menu(hwnd, x, y, TPM_RETURNCMD);
        return 0;
    }
    case WM_TRAY_A: {                                             // version 0: the id in wParam, the mouse message in lParam
        printf("event icon=%u msg=0x%x\n", (UINT)wp, (UINT)lp);
        fflush(stdout);
        if (lp == WM_LBUTTONUP) { POINT p; GetCursorPos(&p); show_menu(hwnd, p.x, p.y, 0); }
        return 0;
    }
    case WM_COMMAND:
        printf("command=%u window=%s\n", LOWORD(wp), hwnd == g_second ? "second" : "main");
        fflush(stdout);
        if (hwnd == g_second && LOWORD(wp) == 101) {             // the second window goes, and its icon with it
            DestroyWindow(g_second);
            NOTIFYICONDATAA a = icon_a(NIF_TIP);
            a.hWnd = NULL;
            CHECK(!Shell_NotifyIconA(NIM_MODIFY, &a), "the icon went with its window");
        }
        return 0;
    case WM_ENTERMENULOOP: g_enter++; return 0;
    case WM_EXITMENULOOP: g_exit++; return 0;
    case WM_INITMENUPOPUP: g_init++; return 0;
    case WM_UNINITMENUPOPUP: g_uninit++; return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: if (hwnd == g_main) PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: tray.exe <icon file>\n"); return 2; }
    WCHAR ico[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, ico, MAX_PATH);
    check_icons(ico);
    check_menus();
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"TrayTest";
    RegisterClassW(&wc);
    g_main = CreateWindowW(L"TrayTest", L"Tray test", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, NULL, NULL, wc.hInstance, NULL);
    g_second = CreateWindowW(L"TrayTest", L"Tray test 2", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, NULL, NULL, wc.hInstance, NULL);
    add_icons(LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1)), LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1)));
    printf("ready main=%llu\n", (unsigned long long)(UINT_PTR)g_main);
    fflush(stdout);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    printf("menu loops: enter=%d exit=%d init=%d uninit=%d\n", g_enter, g_exit, g_init, g_uninit);
    CHECK(g_enter == 3 && g_exit == 3 && g_init == 6 && g_uninit == 6, "menu notifications");
    DestroyMenu(g_menu);
    CHECK(!IsMenu(g_sub), "the submenu went with its menu");
    printf("tray: failures=%d\n", failures);
    return failures ? 1 : 0;
}
