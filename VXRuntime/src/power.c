// powrprof.dll (and the api-ms-win-power-* sets naming it): power information and power plans, which Chromium (the
// Windows Steam client's steamwebhelper) reads through delay-loaded imports and stops at a CHECK without. The machine
// they describe is the one GetSystemPowerStatus (src/desktop.c) reports: on AC power with no system battery, so a
// desktop. From Microsoft's documentation of each function:
//   CallNtPowerInformation  SystemBatteryState (AC on line, no battery) and ProcessorInformation (one entry per
//                           logical processor: the frequency in the brand string programs read from CPUID, which
//                           Rosetta gives as "VirtualApple @ 2.50GHz", as maximum, current and limit; no idle states);
//                           other levels are not provided (STATUS_NOT_IMPLEMENTED, and a log line names the level)
//   PowerDeterminePlatformRoleEx   PlatformRoleDesktop
//   PowerGetActiveScheme    the Balanced plan, the one Windows starts with
//   PowerReadACValue/DCValue  no power setting values are kept here: ERROR_FILE_NOT_FOUND
#include <string.h>
#include <unistd.h>
#include "vx.h"

typedef struct { uint32_t a; uint16_t b, c; uint8_t d[8]; } GUID;
enum { ERROR_FILE_NOT_FOUND = 2, ERROR_INVALID_PARAMETER = 87 };
#define STATUS_SUCCESS 0
#define STATUS_NOT_IMPLEMENTED 0xC0000002u
#define STATUS_BUFFER_TOO_SMALL 0xC0000023u
enum { SystemBatteryState = 5, ProcessorInformation = 11 };
enum { PlatformRoleUnspecified = 0, PlatformRoleDesktop = 1 };
#define PROCESSOR_MHZ 2500                                // (see above)

static void *(WINAPI *rt_LocalAlloc)(DWORD, size_t);

static WINAPI DWORD CallNtPowerInformation(int level, void *in, DWORD in_size, void *out, DWORD out_size) {
    (void)in; (void)in_size;
    switch (level) {
    case SystemBatteryState: {                             // SYSTEM_BATTERY_STATE, 32 bytes
        if (!out || out_size < 32) return STATUS_BUFFER_TOO_SMALL;
        uint8_t *b = out;
        memset(b, 0, 32);
        b[0] = 1;                                          // AcOnLine; BatteryPresent, Charging, Discharging: no
        return STATUS_SUCCESS;
    }
    case ProcessorInformation: {                           // PROCESSOR_POWER_INFORMATION (6 ULONGs) per processor
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n < 1) n = 1;
        if (!out || out_size < (DWORD)n * 24) return STATUS_BUFFER_TOO_SMALL;
        uint32_t *p = out;
        for (long i = 0; i < n; i++, p += 6) {
            p[0] = (uint32_t)i;                            // Number
            p[1] = p[2] = p[3] = PROCESSOR_MHZ;            // MaxMhz, CurrentMhz, MhzLimit
            p[4] = p[5] = 0;                               // MaxIdleState, CurrentIdleState
        }
        return STATUS_SUCCESS;
    }
    default:
        vx_log("CallNtPowerInformation: information level %d is not provided", level);
        return STATUS_NOT_IMPLEMENTED;
    }
}
static WINAPI int PowerDeterminePlatformRoleEx(DWORD version) {
    return version == 1 || version == 2 ? PlatformRoleDesktop : PlatformRoleUnspecified;   // POWER_PLATFORM_ROLE_V1, _V2
}
static WINAPI int PowerDeterminePlatformRole(void) { return PlatformRoleDesktop; }
static WINAPI DWORD PowerGetActiveScheme(void *root, GUID **active) {
    (void)root;
    if (!active) return ERROR_INVALID_PARAMETER;
    static const GUID balanced = { 0x381b4222, 0xf694, 0x41f0, { 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e } };
    GUID *g = rt_LocalAlloc(0, sizeof *g);                 // the caller frees it with LocalFree
    if (!g) return 8;                                      // ERROR_NOT_ENOUGH_MEMORY
    *g = balanced;
    *active = g;
    return 0;
}
static WINAPI DWORD PowerReadACValue(void *root, const GUID *scheme, const GUID *group, const GUID *setting, DWORD *type, uint8_t *buf, DWORD *size) {
    (void)root; (void)scheme; (void)group; (void)setting; (void)type; (void)buf; (void)size;
    return ERROR_FILE_NOT_FOUND;
}
static WINAPI DWORD PowerReadDCValue(void *root, const GUID *scheme, const GUID *group, const GUID *setting, DWORD *type, uint8_t *buf, DWORD *size) {
    (void)root; (void)scheme; (void)group; (void)setting; (void)type; (void)buf; (void)size;
    return ERROR_FILE_NOT_FOUND;
}

static const vx_export powrprof[] = {
    VX_EXPORT(CallNtPowerInformation), VX_EXPORT(PowerDeterminePlatformRoleEx), VX_EXPORT(PowerDeterminePlatformRole),
    VX_EXPORT(PowerGetActiveScheme), VX_EXPORT(PowerReadACValue), VX_EXPORT(PowerReadDCValue),
};

void vx_power_init(void) {
    rt_LocalAlloc = (void *(WINAPI *)(DWORD, size_t))vx_find_export("kernel32.dll", "LocalAlloc");
    vx_register_module("powrprof.dll", powrprof, sizeof powrprof / sizeof powrprof[0]);
}

#ifdef __aarch64__
#include "thunks/power.c.inc"
#endif
