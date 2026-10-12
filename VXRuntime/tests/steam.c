// The Steam bridge (src/steambridge.c): steamclient64.dll's CreateInterface and the ISteamClient calls
// steam_api64.dll makes first, through the proxies, against the Steam client running on the Mac. Needs
// VXRT_STEAM_BRIDGE=1 and Steam running; not part of make check.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
typedef uint64_t (*method)(void *, uint64_t, uint64_t, uint64_t);
int main(void) {
    HMODULE m = LoadLibraryA("steamclient64.dll");
    void *(*create)(const char *, int *) = m ? (void *(*)(const char *, int *))GetProcAddress(m, "CreateInterface") : NULL;
    void *c = create ? create("SteamClient023", NULL) : NULL;
    if (!c) { printf("steam: no SteamClient023 -> FAILED\n"); return 1; }
    void **vt = *(void ***)c;
    uint64_t pipe = ((method)vt[0])(c, 0, 0, 0), user = ((method)vt[2])(c, pipe, 0, 0);
    void *utils = (void *)((method)vt[12])(c, 0, pipe, (uint64_t)"SteamUtils011");
    void *sockets = (void *)((method)vt[12])(c, user, pipe, (uint64_t)"SteamNetworkingSockets013");
    uint32_t app = utils ? (uint32_t)((method)(*(void ***)utils)[9])(utils, 0, 0, 0) : 0;   // ISteamUtils::GetAppID
    printf("steam: pipe %u user %u, utils %p (app %u), sockets %p -> %s\n", (unsigned)pipe, (unsigned)user, utils, app, sockets, utils && sockets ? "ok" : "FAILED");
    return !(utils && sockets);
}
