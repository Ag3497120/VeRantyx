// shlwapi's IsOS: which kind of Windows a program runs on, by the OS_* values Microsoft documents for the function.
// Chromium (the Windows Steam client's steamwebhelper) imports it by ordinal 437, the only way it was exported
// before Windows XP SP2; it has a name as well since then.
//
// The answers describe the machine the rest of the runtime reports: a 64-bit Windows 10 22H2 workstation
// (GetVersionEx, VerifyVersionInfo and the PEB say 10.0.19045, VER_NT_WORKSTATION and VER_SUITE_SINGLEUSERTS;
// GetProductInfo PRODUCT_PROFESSIONAL; the registry's ProductName "Windows 10 Pro"), in its console session (no
// Remote Desktop client: GetSystemMetrics(SM_REMOTESESSION) is 0), joined to no domain, neither a Tablet PC nor a
// Media Center edition (SM_TABLETPC and SM_MEDIACENTER are 0), with a 64-bit program in it (IsWow64Process: no).
#include "vx.h"

enum {
    OS_WINDOWS = 0, OS_NT = 1, OS_WIN95ORGREATER = 2, OS_NT4ORGREATER = 3, OS_WIN98ORGREATER = 5, OS_WIN98_GOLD = 6,
    OS_WIN2000ORGREATER = 7, OS_WIN2000PRO = 8, OS_WIN2000SERVER = 9, OS_WIN2000ADVSERVER = 10, OS_WIN2000DATACENTER = 11,
    OS_WIN2000TERMINAL = 12, OS_EMBEDDED = 13, OS_TERMINALCLIENT = 14, OS_TERMINALREMOTEADMIN = 15, OS_WIN95_GOLD = 16,
    OS_MEORGREATER = 17, OS_XPORGREATER = 18, OS_HOME = 19, OS_PROFESSIONAL = 20, OS_DATACENTER = 21, OS_ADVSERVER = 22,
    OS_SERVER = 23, OS_TERMINALSERVER = 24, OS_PERSONALTERMINALSERVER = 25, OS_FASTUSERSWITCHING = 26,
    OS_WELCOMELOGONUI = 27, OS_DOMAINMEMBER = 28, OS_ANYSERVER = 29, OS_WOW6432 = 30, OS_WEBSERVER = 31,
    OS_SMALLBUSINESSSERVER = 32, OS_TABLETPC = 33, OS_SERVERADMINUI = 34, OS_MEDIACENTER = 35, OS_APPLIANCE = 36,
};

static WINAPI BOOL IsOS(DWORD os) {
    switch (os) {
    case OS_NT: case OS_NT4ORGREATER: case OS_WIN2000ORGREATER: case OS_XPORGREATER:
        return 1;                                    // an NT-based Windows, newer than all of these
    case OS_PROFESSIONAL:
        return 1;                                    // a workstation (VER_NT_WORKSTATION), the Pro edition
    case OS_PERSONALTERMINALSERVER:
        return 1;                                    // one interactive session (the VER_SUITE_SINGLEUSERTS GetVersionEx sets)
    case OS_FASTUSERSWITCHING: case OS_WELCOMELOGONUI:
        return 1;                                    // the defaults of a workstation outside a domain
    default:
        // The Windows 9x/Me family (OS_WINDOWS, OS_WIN95ORGREATER, OS_WIN98*, OS_WIN95_GOLD, OS_MEORGREATER: false on
        // every supported Windows), the Windows 2000 editions, Home, every server kind, Embedded, Terminal Server and
        // its client (this is the console session), a domain member, WOW64 (a 64-bit program), Tablet PC, Media
        // Center, the server administration UI, the appliance server; and values the documentation does not list.
        return 0;
    }
}

static const vx_export shlwapi[] = { VX_EXPORT(IsOS), { "#437", (void *)IsOS } };

void vx_osinfo_init(void) {
    vx_register_module("shlwapi.dll", shlwapi, sizeof shlwapi / sizeof shlwapi[0]);
}

#ifdef __aarch64__
#include "thunks/osinfo.c.inc"
#endif
