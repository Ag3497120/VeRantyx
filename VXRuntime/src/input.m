// XInput on the Game Controller framework: the first four controllers with an extended gamepad profile (Xbox,
// PlayStation, Switch Pro, a pair of Joy-Cons and others macOS knows) are XInput users 0..3.
#import <GameController/GameController.h>
#include "vx.h"

#define ERROR_DEVICE_NOT_CONNECTED 1167
typedef struct { uint16_t buttons; uint8_t left_trigger, right_trigger; int16_t lx, ly, rx, ry; } GAMEPAD;
typedef struct { DWORD packet; GAMEPAD pad; } STATE;

static uint32_t g_packets[4];
static GAMEPAD g_last[4];

static GCExtendedGamepad *gamepad(DWORD user) {
    DWORD n = 0;
    for (GCController *c in GCController.controllers) {
        if (!c.extendedGamepad) continue;
        if (n++ == user) return c.extendedGamepad;
    }
    return nil;
}
static int16_t axis(float v) { return (int16_t)(v < -1 ? -32768 : v > 1 ? 32767 : v < 0 ? v * 32768 : v * 32767); }

static WINAPI DWORD XInputGetState(DWORD user, STATE *state) {
    if (user > 3) return 87;
    GCExtendedGamepad *g = gamepad(user);
    if (!g) return ERROR_DEVICE_NOT_CONNECTED;
    GAMEPAD p = { 0 };
    if (g.dpad.up.pressed) p.buttons |= 0x0001;
    if (g.dpad.down.pressed) p.buttons |= 0x0002;
    if (g.dpad.left.pressed) p.buttons |= 0x0004;
    if (g.dpad.right.pressed) p.buttons |= 0x0008;
    if (g.buttonMenu.pressed) p.buttons |= 0x0010;                      // START
    if (g.buttonOptions.pressed) p.buttons |= 0x0020;                   // BACK
    if (g.leftThumbstickButton.pressed) p.buttons |= 0x0040;
    if (g.rightThumbstickButton.pressed) p.buttons |= 0x0080;
    if (g.leftShoulder.pressed) p.buttons |= 0x0100;
    if (g.rightShoulder.pressed) p.buttons |= 0x0200;
    if (g.buttonA.pressed) p.buttons |= 0x1000;
    if (g.buttonB.pressed) p.buttons |= 0x2000;
    if (g.buttonX.pressed) p.buttons |= 0x4000;
    if (g.buttonY.pressed) p.buttons |= 0x8000;
    p.left_trigger = (uint8_t)(g.leftTrigger.value * 255.0f + 0.5f);
    p.right_trigger = (uint8_t)(g.rightTrigger.value * 255.0f + 0.5f);
    p.lx = axis(g.leftThumbstick.xAxis.value); p.ly = axis(g.leftThumbstick.yAxis.value);   // both y axes point up, as in XInput
    p.rx = axis(g.rightThumbstick.xAxis.value); p.ry = axis(g.rightThumbstick.yAxis.value);
    if (memcmp(&p, &g_last[user], sizeof p)) { g_last[user] = p; g_packets[user]++; }
    state->packet = g_packets[user];
    state->pad = p;
    return 0;
}
// Rumble needs the controller's haptics engine; accepted without effect for now.
static WINAPI DWORD XInputSetState(DWORD user, void *vibration) { (void)vibration; return gamepad(user) ? 0 : ERROR_DEVICE_NOT_CONNECTED; }
// XINPUT_CAPABILITIES: type, subtype, flags, the gamepad's ranges, vibration.
static WINAPI DWORD XInputGetCapabilities(DWORD user, DWORD flags, uint8_t *caps) {
    (void)flags;
    if (!gamepad(user)) return ERROR_DEVICE_NOT_CONNECTED;
    memset(caps, 0, 20);
    caps[0] = 1; caps[1] = 1;                                           // XINPUT_DEVTYPE_GAMEPAD, XINPUT_DEVSUBTYPE_GAMEPAD
    GAMEPAD *g = (GAMEPAD *)(caps + 4);
    g->buttons = 0xf3ff; g->left_trigger = g->right_trigger = 255;
    g->lx = g->ly = g->rx = g->ry = (int16_t)0xffc0;
    return 0;
}
static WINAPI void XInputEnable(int enable) { (void)enable; }
// XINPUT_BATTERY_INFORMATION: type, level.
static WINAPI DWORD XInputGetBatteryInformation(DWORD user, uint8_t type, uint8_t *info) {
    (void)type;
    GCExtendedGamepad *g = gamepad(user);
    if (!g) return ERROR_DEVICE_NOT_CONNECTED;
    GCDeviceBattery *b = g.controller.battery;
    info[0] = b ? 2 : 1;                                                // BATTERY_TYPE_ALKALINE-like / wired
    info[1] = !b ? 3 : b.batteryLevel > 0.7 ? 3 : b.batteryLevel > 0.3 ? 2 : b.batteryLevel > 0.05 ? 1 : 0;
    return 0;
}
static WINAPI DWORD XInputGetKeystroke(DWORD user, DWORD reserved, void *keystroke) { (void)reserved; (void)keystroke; return gamepad(user) ? 4306 /* ERROR_EMPTY */ : ERROR_DEVICE_NOT_CONNECTED; }

static const vx_export xinput[] = {
    VX_EXPORT(XInputGetState), VX_EXPORT(XInputSetState), VX_EXPORT(XInputGetCapabilities), VX_EXPORT(XInputEnable),
    VX_EXPORT(XInputGetBatteryInformation), VX_EXPORT(XInputGetKeystroke),
    { "#100", (void *)XInputGetState },                                // XInputGetStateEx (adds the guide button)
};
void vx_input_init(void) {
    vx_register_module("xinput1_4.dll", xinput, sizeof xinput / sizeof xinput[0]);
    vx_register_module("xinput1_3.dll", xinput, sizeof xinput / sizeof xinput[0]);
    vx_register_module("xinput9_1_0.dll", xinput, sizeof xinput / sizeof xinput[0]);
    vx_register_module("xinput1_2.dll", xinput, sizeof xinput / sizeof xinput[0]);
    vx_register_module("xinput1_1.dll", xinput, sizeof xinput / sizeof xinput[0]);
}
