// xros_input.c: what reaches a Windows program from the keyboard, the mouse and game controllers (VISIONOS_PORT.md,
// "input"). A window for 25 s with raw input registered for the mouse and the keyboard; it counts WM_KEYDOWN, WM_CHAR,
// WM_INPUT (mouse moves, buttons, keys), button messages and XInput packet changes, and prints them. Someone has to
// type, move the mouse or press a controller's buttons while it runs; nothing touched is "0 events", not a failure.
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
static unsigned keydown, chars, raw_moves, raw_buttons, raw_keys, buttons, moves;
static long raw_dx, raw_dy;
static char typed[64]; static int ntyped;
static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: if (keydown++ < 6) printf("xros input: WM_KEYDOWN vk %#llx\n", (unsigned long long)w); return 0;
    case WM_CHAR: chars++; if (ntyped < 63) typed[ntyped++] = (char)w; return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: buttons++; return 0;
    case WM_MOUSEMOVE: moves++; return 0;
    case WM_INPUT: {
        RAWINPUT ri; UINT n = sizeof ri;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &n, sizeof ri.header) != (UINT)-1) {
            if (ri.header.dwType == RIM_TYPEMOUSE) {
                if (ri.data.mouse.lLastX || ri.data.mouse.lLastY) { raw_moves++; raw_dx += ri.data.mouse.lLastX; raw_dy += ri.data.mouse.lLastY; }
                if (ri.data.mouse.usButtonFlags) raw_buttons++;
            } else if (ri.header.dwType == RIM_TYPEKEYBOARD) raw_keys++;
        }
        break;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
int main(void) {
    WNDCLASSA wc = { 0 }; wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "xros_input";
    RegisterClassA(&wc);
    HWND h = CreateWindowA("xros_input", "xros input", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 1280, 720, NULL, NULL, wc.hInstance, NULL);
    RAWINPUTDEVICE rd[2] = { { 1, 2, 0, h }, { 1, 6, 0, h } };          // (generic desktop: mouse, keyboard)
    BOOL raw = RegisterRawInputDevices(rd, 2, sizeof rd[0]);
    DWORD last_packet[4] = { 0 }; unsigned pad_changes = 0, pads = 0;
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < 25000) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        for (DWORD u = 0; u < 4; u++) {
            XINPUT_STATE s;
            if (XInputGetState(u, &s) == ERROR_SUCCESS) {
                pads |= 1u << u;
                if (s.dwPacketNumber != last_packet[u]) { if (last_packet[u]) pad_changes++; last_packet[u] = s.dwPacketNumber; }
            }
        }
        Sleep(5);
    }
    typed[ntyped] = 0;
    printf("xros input: raw input registered %d; %u WM_KEYDOWN, %u WM_CHAR (\"%s\"), %u raw keys; %u raw mouse moves (%ld, %ld), "
           "%u raw button changes, %u button messages, %u WM_MOUSEMOVE; controllers %#x, %u XInput changes -> ok\n",
           raw, keydown, chars, typed, raw_keys, raw_moves, raw_dx, raw_dy, raw_buttons, buttons, moves, pads, pad_changes);
    DestroyWindow(h);
    return 0;
}
