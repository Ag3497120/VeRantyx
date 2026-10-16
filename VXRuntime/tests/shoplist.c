// A shopping list for Command Clips (CLIPS.md) to be operated through: the kind of small program an AI would generate
// for VXRuntime, drawn with GDI as it has no standard controls. A name field (click it to type into it), "[ Add ]"
// (or Enter), one row per item with its own "[ x ]" that asks in a second window ("[ Yes ]" / "[ No ]") before it
// deletes, "[ Save ]" that writes the list to shoplist.txt, and a status line. On WM_CLOSE it prints the list and
// whether it was saved, which is how a task done through it is checked (tests/shoplist_check.py, the M2 demo).
// --hard (the harder comparison, demos/m3-hard): 18 items to start with, six to a page with "[ < ]" "Page 1 of 3"
// "[ > ]", names alike (milk, oat milk, soy milk ...), a small font, and a question that reads "Remove ... from the
// list?" with "[ Cancel ]" first and "[ Remove ]" second.
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define MAX_ITEMS 32
static char items[MAX_ITEMS][32];
static int nitems, field_focus, saved, asking = -1, hard, page;
static const char *start_items[] = { "milk", "bread", "oat milk", "rice", "milk chocolate", "apples", "soy milk", "rice crackers",
                                     "butter", "tea", "almond milk", "rice flour", "coffee", "brown rice", "milk powder", "honey",
                                     "pasta", "oats" };
static int per_page(void) { return hard ? 6 : MAX_ITEMS; }
static int pages(void) { int n = (nitems + per_page() - 1) / per_page(); return n ? n : 1; }
static char field[32], status[96] = "Ready";
static HWND g_main, g_ask;
enum { ROW_Y = 110, ROW_H = 24, WM_ANSWER = WM_APP + 1 };

static void text(HDC dc, int x, int y, const char *s) { TextOutA(dc, x, y, s, (int)strlen(s)); }
static void button(HDC dc, RECT r, const char *label) {
    HBRUSH b = CreateSolidBrush(RGB(205, 214, 232));
    FillRect(dc, &r, b); DeleteObject(b);
    DrawTextA(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
static RECT add_r = { 250, 50, 330, 76 }, save_r = { 340, 50, 420, 76 }, field_r = { 20, 50, 240, 76 };
static RECT prev_r = { 20, 270, 60, 292 }, next_r = { 200, 270, 240, 292 };
static RECT delete_r(int i) { RECT r = { 340, ROW_Y + i * ROW_H, 380, ROW_Y + i * ROW_H + 20 }; return r; }
static int in(RECT r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

static void paint_main(HWND h) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
    RECT all; GetClientRect(h, &all);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, all.right, all.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(RGB(246, 247, 250)), fb = CreateSolidBrush(field_focus ? RGB(255, 255, 255) : RGB(232, 234, 240));
    FillRect(mem, &all, bg);
    SetBkMode(mem, TRANSPARENT);
    HFONT small = hard ? CreateFontA(11, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, 0, 0, "Helvetica") : NULL;
    HGDIOBJ old_font = small ? SelectObject(mem, small) : NULL;
    text(mem, 20, 16, "Shopping list");
    FillRect(mem, &field_r, fb);
    char line[96];
    snprintf(line, sizeof line, "Name: %s%s", field, field_focus ? "|" : "");
    text(mem, 26, 55, line);
    button(mem, add_r, "[ Add ]");
    button(mem, save_r, "[ Save ]");
    if (!nitems) text(mem, 20, ROW_Y, "(no items)");
    for (int r = 0; r < per_page() && page * per_page() + r < nitems; r++) {
        int i = page * per_page() + r;
        snprintf(line, sizeof line, "%d. %s", i + 1, items[i]);
        text(mem, 20, ROW_Y + r * ROW_H, line);
        button(mem, delete_r(r), "[ x ]");
    }
    if (hard) {
        button(mem, prev_r, "[ < ]"); button(mem, next_r, "[ > ]");
        snprintf(line, sizeof line, "Page %d of %d", page + 1, pages());
        text(mem, 90, 274, line);
    }
    if (small) { SelectObject(mem, old_font); DeleteObject(small); }
    snprintf(line, sizeof line, "Status: %s", status);
    text(mem, 20, all.bottom - 30, line);
    BitBlt(dc, 0, 0, all.right, all.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp); DeleteObject(bg); DeleteObject(fb); DeleteDC(mem);
    EndPaint(h, &ps);
}

static void add_item(void) {
    if (!field[0]) { snprintf(status, sizeof status, "Type a name first"); return; }
    if (nitems == MAX_ITEMS) { snprintf(status, sizeof status, "The list is full"); return; }
    snprintf(items[nitems++], sizeof items[0], "%s", field);
    page = pages() - 1;                                          // (the page the new item is on)
    snprintf(status, sizeof status, "Added %s", field);
    field[0] = 0; saved = 0;
}
static void save(void) {
    FILE *f = fopen("shoplist.txt", "w");
    if (!f) { snprintf(status, sizeof status, "Could not save"); return; }
    for (int i = 0; i < nitems; i++) fprintf(f, "%s\n", items[i]);
    fclose(f);
    saved = 1;
    snprintf(status, sizeof status, "Saved %d item%s to shoplist.txt", nitems, nitems == 1 ? "" : "s");
}

static LRESULT CALLBACK ask_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    static RECT left = { 30, 60, 110, 86 }, right = { 130, 60, 210, 86 };
    RECT yes = hard ? right : left, no = hard ? left : right;
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT all; GetClientRect(h, &all);
        HBRUSH bg = CreateSolidBrush(RGB(255, 250, 235)); FillRect(dc, &all, bg); DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        char q[80];
        if (hard) snprintf(q, sizeof q, "Remove %s from the list?", asking >= 0 ? items[asking] : "");
        else snprintf(q, sizeof q, "Delete %s?", asking >= 0 ? items[asking] : "");
        HFONT small = hard ? CreateFontA(11, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, 0, 0, "Helvetica") : NULL;
        HGDIOBJ old_font = small ? SelectObject(dc, small) : NULL;
        text(dc, 30, 20, q);
        button(dc, yes, hard ? "[ Remove ]" : "[ Yes ]"); button(dc, no, hard ? "[ Cancel ]" : "[ No ]");
        if (small) { SelectObject(dc, old_font); DeleteObject(small); }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (in(yes, x, y) || in(no, x, y)) { PostMessageA(g_main, WM_ANSWER, in(yes, x, y), 0); DestroyWindow(h); }
        return 0;
    }
    case WM_DESTROY: g_ask = NULL; return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: paint_main(h); return 0;
    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (g_ask) { snprintf(status, sizeof status, "Answer the question first"); InvalidateRect(h, NULL, FALSE); return 0; }
        field_focus = in(field_r, x, y);
        if (in(add_r, x, y)) add_item();
        else if (in(save_r, x, y)) save();
        else if (hard && in(prev_r, x, y)) { if (page > 0) page--; }
        else if (hard && in(next_r, x, y)) { if (page + 1 < pages()) page++; }
        else for (int r = 0; r < per_page() && page * per_page() + r < nitems; r++)
            if (in(delete_r(r), x, y)) {
                asking = page * per_page() + r;
                g_ask = CreateWindowA("ShopAsk", "Confirm", WS_POPUP | WS_CAPTION | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 280, 140, h, NULL, NULL, NULL);
                break;
            }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_ANSWER:
        if (wp && asking >= 0 && asking < nitems) {
            snprintf(status, sizeof status, "Deleted %s", items[asking]);
            for (int i = asking; i + 1 < nitems; i++) memcpy(items[i], items[i + 1], sizeof items[i]);
            nitems--; saved = 0;
            if (page >= pages()) page = pages() - 1;
        } else snprintf(status, sizeof status, "Kept it");
        asking = -1;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_RETURN && field_focus) { add_item(); InvalidateRect(h, NULL, FALSE); }
        else if (wp == VK_BACK && field_focus && field[0]) { field[strlen(field) - 1] = 0; InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_CHAR: {
        size_t n = strlen(field);
        if (field_focus && wp >= 0x20 && wp < 0x7F && n + 1 < sizeof field) { field[n] = (char)wp; field[n + 1] = 0; InvalidateRect(h, NULL, FALSE); }
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

int main(int argc, char **argv) {
    hard = argc > 1 && !strcmp(argv[1], "--hard");
    if (hard) for (int i = 0; i < 18; i++) snprintf(items[nitems++], sizeof items[0], "%s", start_items[i]);
    WNDCLASSA wc = { 0 };
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpfnWndProc = main_proc; wc.lpszClassName = "ShopList"; RegisterClassA(&wc);
    wc.lpfnWndProc = ask_proc; wc.lpszClassName = "ShopAsk"; RegisterClassA(&wc);
    g_main = CreateWindowA("ShopList", "Shopping list", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 460, 460, NULL, NULL, wc.hInstance, NULL);
    if (!g_main) { printf("shoplist: no window\n"); return 1; }
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    printf("shoplist: items=");
    for (int i = 0; i < nitems; i++) printf("%s%s", i ? "," : "", items[i]);
    printf(" saved=%d\n", saved);
    return 0;
}
