#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dinput.h>
#include <stdio.h>

static DWORD g_count;
static GUID g_first;

static BOOL CALLBACK on_device(const DIDEVICEINSTANCEW *device, void *context) {
    (void)context;
    if (!g_count) g_first = device->guidInstance;
    g_count++;
    return DIENUM_CONTINUE;
}

int main(void) {
    LPDIRECTINPUT8W input = NULL;
    HRESULT hr = DirectInput8Create(GetModuleHandleW(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8W,
                                    (void **)&input, NULL);
    if (FAILED(hr) || !input) {
        fprintf(stderr, "dinput8: DirectInput8Create failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    hr = IDirectInput8_EnumDevices(input, DI8DEVCLASS_GAMECTRL, on_device, NULL, DIEDFL_ATTACHEDONLY);
    if (FAILED(hr)) {
        fprintf(stderr, "dinput8: EnumDevices failed: 0x%08lx\n", (unsigned long)hr);
        IDirectInput8_Release(input);
        return 2;
    }
    if (g_count) {
        LPDIRECTINPUTDEVICE8W device = NULL;
        DIDEVCAPS caps = { .dwSize = sizeof caps };
        hr = IDirectInput8_CreateDevice(input, &g_first, &device, NULL);
        if (FAILED(hr) || !device || FAILED(IDirectInputDevice8_GetCapabilities(device, &caps))) {
            fprintf(stderr, "dinput8: CreateDevice/GetCapabilities failed: 0x%08lx\n", (unsigned long)hr);
            if (device) IDirectInputDevice8_Release(device);
            IDirectInput8_Release(input);
            return 3;
        }
        DIPROPDWORD buffer_size = {
            .diph = { .dwSize = sizeof buffer_size, .dwHeaderSize = sizeof(DIPROPHEADER),
                      .dwObj = 0, .dwHow = DIPH_DEVICE },
            .dwData = 64
        };
        hr = IDirectInputDevice8_SetDataFormat(device, &c_dfDIJoystick2);
        if (FAILED(hr)) {
            fprintf(stderr, "dinput8: SetDataFormat failed: 0x%08lx\n", (unsigned long)hr);
            IDirectInputDevice8_Release(device);
            IDirectInput8_Release(input);
            return 4;
        }
        hr = IDirectInputDevice8_SetProperty(device, DIPROP_BUFFERSIZE, &buffer_size.diph);
        DIPROPDWORD actual_size = {
            .diph = { .dwSize = sizeof actual_size, .dwHeaderSize = sizeof(DIPROPHEADER),
                      .dwObj = 0, .dwHow = DIPH_DEVICE }
        };
        if (SUCCEEDED(hr)) hr = IDirectInputDevice8_GetProperty(device, DIPROP_BUFFERSIZE, &actual_size.diph);
        if (FAILED(hr) || actual_size.dwData != buffer_size.dwData) {
            fprintf(stderr, "dinput8: buffered size property failed: 0x%08lx (%lu)\n",
                    (unsigned long)hr, (unsigned long)actual_size.dwData);
            IDirectInputDevice8_Release(device);
            IDirectInput8_Release(input);
            return 5;
        }
        HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!event || FAILED(IDirectInputDevice8_SetEventNotification(device, event)) ||
            FAILED(IDirectInputDevice8_SetCooperativeLevel(device, GetDesktopWindow(),
                                                            DISCL_NONEXCLUSIVE | DISCL_BACKGROUND))) {
            fprintf(stderr, "dinput8: event/cooperative-level setup failed\n");
            if (event) CloseHandle(event);
            IDirectInputDevice8_Release(device);
            IDirectInput8_Release(input);
            return 6;
        }
        hr = IDirectInputDevice8_Acquire(device);
        if (SUCCEEDED(hr)) {
            DIJOYSTATE2 state;
            DIDEVICEOBJECTDATA events[64];
            DWORD event_count = 64;
            hr = IDirectInputDevice8_Poll(device);
            if (SUCCEEDED(hr)) hr = IDirectInputDevice8_GetDeviceState(device, sizeof state, &state);
            if (SUCCEEDED(hr)) hr = IDirectInputDevice8_GetDeviceData(device, sizeof events[0], events,
                                                                       &event_count, 0);
            if (FAILED(hr)) {
                fprintf(stderr, "dinput8: Poll/GetDeviceState/GetDeviceData failed: 0x%08lx\n",
                        (unsigned long)hr);
                IDirectInputDevice8_Unacquire(device);
                IDirectInputDevice8_SetEventNotification(device, NULL);
                CloseHandle(event);
                IDirectInputDevice8_Release(device);
                IDirectInput8_Release(input);
                return 7;
            }
            IDirectInputDevice8_Unacquire(device);
        } else {
            fprintf(stderr, "dinput8: Acquire failed: 0x%08lx\n", (unsigned long)hr);
            IDirectInputDevice8_SetEventNotification(device, NULL);
            CloseHandle(event);
            IDirectInputDevice8_Release(device);
            IDirectInput8_Release(input);
            return 8;
        }
        if (FAILED(IDirectInputDevice8_SetEventNotification(device, NULL))) {
            fprintf(stderr, "dinput8: failed to disable event notification\n");
            CloseHandle(event);
            IDirectInputDevice8_Release(device);
            IDirectInput8_Release(input);
            return 9;
        }
        CloseHandle(event);
        IDirectInputDevice8_Release(device);
    }
    IDirectInput8_Release(input);
    printf("dinput8: DirectInput8 factory and HID game-controller enumeration: ok (%lu attached)\n",
           (unsigned long)g_count);
    return 0;
}
