// user32's touch and pointer input (Windows 7's WM_TOUCH, Windows 8's WM_POINTER) on a Mac, which has no touch
// screen and no pen: no pointer device is attached (GetSystemMetrics(SM_DIGITIZER) and SM_MAXIMUMTOUCHES are 0 as
// well) and the mouse is not routed as a pointer, so no WM_TOUCH or WM_POINTER* message is ever sent and no pointer
// id, pointer device handle or touch input handle is ever valid. Chromium (the Windows Steam client's
// steamwebhelper) enumerates the devices and registers its windows for touch and for device changes; those calls
// work as on a Windows PC without a touch screen: the registrations are kept, there is just never anything to report.
// From Microsoft's documentation of each function.
#include <pthread.h>
#include "ui.h"

enum { ERROR_INVALID_HANDLE = 6, ERROR_NOT_SUPPORTED = 50, ERROR_INVALID_PARAMETER = 87, ERROR_INVALID_WINDOW_HANDLE = 1400 };

// ---- devices -------------------------------------------------------------------------------------------------------
// GetPointerDevices: with no devices, the count is 0 whether or not an array was given.
static WINAPI BOOL GetPointerDevices(uint32_t *count, void *devices) {
    (void)devices;
    if (!count) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    *count = 0;
    return 1;
}
// Every function about one device: a device handle that names no device (there are none).
static BOOL no_device(void) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
static WINAPI BOOL GetPointerDevice(HANDLE device, void *info) { (void)device; (void)info; return no_device(); }
static WINAPI BOOL GetPointerDeviceProperties(HANDLE device, uint32_t *count, void *properties) { (void)device; (void)count; (void)properties; return no_device(); }
static WINAPI BOOL GetPointerDeviceRects(HANDLE device, RECT *device_rect, RECT *display_rect) { (void)device; (void)device_rect; (void)display_rect; return no_device(); }
static WINAPI BOOL GetPointerDeviceCursors(HANDLE device, uint32_t *count, void *cursors) { (void)device; (void)count; (void)cursors; return no_device(); }
// Device arrival, removal and (notifyRange) pen range: WM_POINTERDEVICECHANGE and friends to a live window.
static WINAPI BOOL RegisterPointerDeviceNotifications(HANDLE window, BOOL notify_range) {
    (void)notify_range;
    if (!vx_window_get(window)) { vx_set_last_error(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    return 1;
}

// ---- pointers ----------------------------------------------------------------------------------------------------------
// A pointer id comes from a WM_POINTER* message, and none is ever sent here.
static BOOL no_pointer(void) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
static WINAPI BOOL GetPointerType(uint32_t id, DWORD *type) { (void)id; (void)type; return no_pointer(); }
static WINAPI BOOL GetPointerCursorId(uint32_t id, uint32_t *cursor) { (void)id; (void)cursor; return no_pointer(); }
static WINAPI BOOL GetPointerInfo(uint32_t id, void *info) { (void)id; (void)info; return no_pointer(); }
static WINAPI BOOL GetPointerInfoHistory(uint32_t id, uint32_t *entries, void *info) { (void)id; (void)entries; (void)info; return no_pointer(); }
static WINAPI BOOL GetPointerFrameInfo(uint32_t id, uint32_t *count, void *info) { (void)id; (void)count; (void)info; return no_pointer(); }
static WINAPI BOOL GetPointerFrameInfoHistory(uint32_t id, uint32_t *entries, uint32_t *count, void *info) { (void)id; (void)entries; (void)count; (void)info; return no_pointer(); }
static WINAPI BOOL GetPointerTouchInfo(uint32_t id, void *info) { (void)id; (void)info; return no_pointer(); }
static WINAPI BOOL GetPointerTouchInfoHistory(uint32_t id, uint32_t *entries, void *info) { (void)id; (void)entries; (void)info; return no_pointer(); }
static WINAPI BOOL GetPointerFrameTouchInfo(uint32_t id, uint32_t *count, void *info) { (void)id; (void)count; (void)info; return no_pointer(); }
static WINAPI BOOL GetPointerFrameTouchInfoHistory(uint32_t id, uint32_t *entries, uint32_t *count, void *info) { (void)id; (void)entries; (void)count; (void)info; return no_pointer(); }
static WINAPI BOOL GetPointerPenInfo(uint32_t id, void *info) { (void)id; (void)info; return no_pointer(); }
static WINAPI BOOL GetPointerPenInfoHistory(uint32_t id, uint32_t *entries, void *info) { (void)id; (void)entries; (void)info; return no_pointer(); }
static WINAPI BOOL GetPointerFramePenInfo(uint32_t id, uint32_t *count, void *info) { (void)id; (void)count; (void)info; return no_pointer(); }
static WINAPI BOOL GetPointerFramePenInfoHistory(uint32_t id, uint32_t *entries, uint32_t *count, void *info) { (void)id; (void)entries; (void)count; (void)info; return no_pointer(); }
static WINAPI BOOL SkipPointerFrameMessages(uint32_t id) { (void)id; return no_pointer(); }
static WINAPI BOOL GetRawPointerDeviceData(uint32_t id, uint32_t history, uint32_t count, void *properties, LONG *values) {
    (void)id; (void)history; (void)count; (void)properties; (void)values; return no_pointer();
}
// The mouse as a pointer (WM_POINTER for mouse input): not delivered by this runtime, so it cannot be turned on;
// asking for the state it is already in succeeds.
static WINAPI BOOL EnableMouseInPointer(BOOL enable) {
    if (enable) { vx_set_last_error(ERROR_NOT_SUPPORTED); return 0; }
    return 1;
}
static WINAPI BOOL IsMouseInPointerEnabled(void) { return 0; }

// ---- WM_TOUCH ------------------------------------------------------------------------------------------------------------
// The windows registered as touch-capable, with their TWF_* flags (IsTouchWindow reports them).
typedef struct { HANDLE hwnd; vx_window *w; DWORD flags; } touch_window;
static touch_window *g_touch; static int g_touch_count, g_touch_cap;
static pthread_mutex_t g_touch_lock = PTHREAD_MUTEX_INITIALIZER;
static int touch_find(HANDLE hwnd, vx_window *w) {                // (lock held) its index, -1 if not registered
    for (int i = 0; i < g_touch_count; i++) if (g_touch[i].hwnd == hwnd && g_touch[i].w == w) return i;
    return -1;
}
static WINAPI BOOL RegisterTouchWindow(HANDLE hwnd, DWORD flags) {
    vx_window *w = vx_window_get(hwnd);
    if (!w) { vx_set_last_error(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    pthread_mutex_lock(&g_touch_lock);
    for (int i = 0; i < g_touch_count; i++)                         // forget windows destroyed since
        if (!vx_window_get(g_touch[i].hwnd) || vx_window_get(g_touch[i].hwnd) != g_touch[i].w) g_touch[i--] = g_touch[--g_touch_count];
    int i = touch_find(hwnd, w);
    if (i < 0) {
        if (g_touch_count == g_touch_cap) { g_touch_cap = g_touch_cap ? g_touch_cap * 2 : 8; g_touch = realloc(g_touch, (size_t)g_touch_cap * sizeof *g_touch); }
        i = g_touch_count++;
        g_touch[i].hwnd = hwnd; g_touch[i].w = w;
    }
    g_touch[i].flags = flags;
    pthread_mutex_unlock(&g_touch_lock);
    return 1;
}
static WINAPI BOOL UnregisterTouchWindow(HANDLE hwnd) {
    vx_window *w = vx_window_get(hwnd);
    if (!w) { vx_set_last_error(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    pthread_mutex_lock(&g_touch_lock);
    int i = touch_find(hwnd, w);
    if (i >= 0) g_touch[i] = g_touch[--g_touch_count];
    pthread_mutex_unlock(&g_touch_lock);
    return 1;
}
static WINAPI BOOL IsTouchWindow(HANDLE hwnd, DWORD *flags) {
    vx_window *w = vx_window_get(hwnd);
    pthread_mutex_lock(&g_touch_lock);
    int i = w ? touch_find(hwnd, w) : -1;
    DWORD f = i >= 0 ? g_touch[i].flags : 0;
    pthread_mutex_unlock(&g_touch_lock);
    if (flags) *flags = f;
    return i >= 0;
}
// A touch input handle comes with a WM_TOUCH message, and none is ever sent here.
static WINAPI BOOL GetTouchInputInfo(HANDLE input, uint32_t count, void *inputs, int size) {
    (void)input; (void)count; (void)inputs; (void)size;
    vx_set_last_error(ERROR_INVALID_HANDLE);
    return 0;
}
static WINAPI BOOL CloseTouchInputHandle(HANDLE input) { (void)input; vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }

static const vx_export user32[] = {
    VX_EXPORT(GetPointerDevices), VX_EXPORT(GetPointerDevice), VX_EXPORT(GetPointerDeviceProperties), VX_EXPORT(GetPointerDeviceRects),
    VX_EXPORT(GetPointerDeviceCursors), VX_EXPORT(RegisterPointerDeviceNotifications),
    VX_EXPORT(GetPointerType), VX_EXPORT(GetPointerCursorId), VX_EXPORT(GetPointerInfo), VX_EXPORT(GetPointerInfoHistory),
    VX_EXPORT(GetPointerFrameInfo), VX_EXPORT(GetPointerFrameInfoHistory), VX_EXPORT(GetPointerTouchInfo),
    VX_EXPORT(GetPointerTouchInfoHistory), VX_EXPORT(GetPointerFrameTouchInfo), VX_EXPORT(GetPointerFrameTouchInfoHistory),
    VX_EXPORT(GetPointerPenInfo), VX_EXPORT(GetPointerPenInfoHistory), VX_EXPORT(GetPointerFramePenInfo),
    VX_EXPORT(GetPointerFramePenInfoHistory), VX_EXPORT(SkipPointerFrameMessages), VX_EXPORT(GetRawPointerDeviceData),
    VX_EXPORT(EnableMouseInPointer), VX_EXPORT(IsMouseInPointerEnabled),
    VX_EXPORT(RegisterTouchWindow), VX_EXPORT(UnregisterTouchWindow), VX_EXPORT(IsTouchWindow), VX_EXPORT(GetTouchInputInfo),
    VX_EXPORT(CloseTouchInputHandle),
};

void vx_pointer_init(void) {
    vx_register_module("user32.dll", user32, sizeof user32 / sizeof user32[0]);
}

#ifdef __aarch64__
#include "thunks/pointer.c.inc"
#endif
