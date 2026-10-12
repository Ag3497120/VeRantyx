// Milestone 7b: input. Raw input registration and WM_INPUT records (mouse movement, buttons, wheel, keys),
// SendInput delivering key and button messages, the raw device list, and XInput (a controller if one is paired).
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)
static LONG mdx, mdy; static int downs, ups, wheel, raw_keys, key_downs, chars;
static USHORT last_make, last_vkey, break_seen;

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_INPUT: {
        UINT size = 0;
        GetRawInputData((HRAWINPUT)l, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER));
        BYTE buf[64];
        if (size > sizeof buf || GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != size) { failures++; return 0; }
        RAWINPUT *ri = (RAWINPUT *)buf;
        if (ri->header.dwType == RIM_TYPEMOUSE) {
            mdx += ri->data.mouse.lLastX; mdy += ri->data.mouse.lLastY;
            if (ri->data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) downs++;
            if (ri->data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) ups++;
            if (ri->data.mouse.usButtonFlags & RI_MOUSE_WHEEL) wheel += (SHORT)ri->data.mouse.usButtonData;
        } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
            raw_keys++;
            last_make = ri->data.keyboard.MakeCode; last_vkey = ri->data.keyboard.VKey;
            if (ri->data.keyboard.Flags & RI_KEY_BREAK) break_seen = 1;
        }
        return DefWindowProcA(h, m, w, l);
    }
    case WM_KEYDOWN: if (w == 'A') key_downs++; return 0;
    case WM_CHAR: if (w == 'a') chars++; return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int main(void) {
    WNDCLASSA wc = { 0, proc, 0, 0, GetModuleHandleA(NULL), NULL, NULL, NULL, NULL, "VXInput" };
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("VXInput", "VXRuntime input test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 320, 240, NULL, NULL, wc.hInstance, NULL);
    RAWINPUTDEVICE rid[2] = { { 1, 2, RIDEV_INPUTSINK, hwnd }, { 1, 6, RIDEV_INPUTSINK, hwnd } };
    CHECK(RegisterRawInputDevices(rid, 2, sizeof rid[0]));
    RAWINPUTDEVICE got[4]; UINT n = 4;
    CHECK(GetRegisteredRawInputDevices(got, &n, sizeof got[0]) == 2);
    UINT ndev = 0;
    GetRawInputDeviceList(NULL, &ndev, sizeof(RAWINPUTDEVICELIST));
    CHECK(ndev == 2);

    INPUT in[6];
    memset(in, 0, sizeof in);
    in[0].type = INPUT_MOUSE; in[0].mi.dx = 12; in[0].mi.dy = -7; in[0].mi.dwFlags = MOUSEEVENTF_MOVE;
    in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[2].type = INPUT_MOUSE; in[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    in[3].type = INPUT_MOUSE; in[3].mi.dwFlags = MOUSEEVENTF_WHEEL; in[3].mi.mouseData = (DWORD)-120;
    in[4].type = INPUT_KEYBOARD; in[4].ki.wVk = 'A';
    in[5].type = INPUT_KEYBOARD; in[5].ki.wVk = 'A'; in[5].ki.dwFlags = KEYEVENTF_KEYUP;
    CHECK(SendInput(6, in, sizeof(INPUT)) == 6);
    MSG msg;
    DWORD until = GetTickCount() + 500;
    while (GetTickCount() < until) { while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); } Sleep(5); }
    CHECK(mdx == 12 && mdy == -7);
    CHECK(downs == 1 && ups == 1 && wheel == -120);
    CHECK(raw_keys == 2 && last_vkey == 'A' && last_make == 0x1E && break_seen);
    CHECK(key_downs == 1 && chars == 1);
    CHECK(!(GetAsyncKeyState('A') & 0x8000));

    XINPUT_STATE xs;
    DWORD xr = XInputGetState(0, &xs);
    CHECK(xr == ERROR_SUCCESS || xr == ERROR_DEVICE_NOT_CONNECTED);
    DestroyWindow(hwnd);
    printf("input: raw mouse (%ld, %ld) buttons %d/%d wheel %d, raw keys %d, messages %d/%d, XInput user 0 %s: %d failed checks -> %s\n",
           mdx, mdy, downs, ups, wheel, raw_keys, key_downs, chars, xr == ERROR_SUCCESS ? "connected" : "not connected", failures, failures ? "FAILED" : "ok");
    return failures;
}
