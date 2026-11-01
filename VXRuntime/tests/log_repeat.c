// The runtime's log keeps a line that repeats word for word to 20 copies, then one every 10 s (src/loader.c):
// 100000 asks for an export SetupAPI does not have (Windows Steam asked 4 million times, a 494 MB log).
#include <windows.h>
#include <stdio.h>

int main(void) {
    HMODULE m = LoadLibraryA("SetupAPI.dll");
    if (!m) { printf("FAIL no SetupAPI.dll\n"); return 1; }
    for (int i = 0; i < 100000; i++) GetProcAddress(m, "VxrtNoSuchExportForTheLogTest");
    printf("log_repeat: asked 100000 times\n");
    return 0;
}
