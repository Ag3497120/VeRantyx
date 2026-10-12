// Loads a DLL with LoadLibrary (running its initialization) and looks up exports, to see how far a real DLL
// gets on this runtime. Run with VXRT_TRACE=1 to list the imports that are not implemented yet.
//   vxrt build/dllprobe.exe 'Z:\path\to\some.dll' [export ...]
#include <windows.h>
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: dllprobe <dll path> [export ...]\n"); return 2; }
    HMODULE m = LoadLibraryExA(argv[1], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);   // dependencies from the DLL's directory
    if (!m) { printf("LoadLibrary failed: error %lu\n", GetLastError()); return 1; }
    printf("loaded %s at %p\n", argv[1], (void *)m);
    int missing = 0;
    for (int i = 2; i < argc; i++) {
        FARPROC p = GetProcAddress(m, argv[i]);
        printf("  %-40s %p\n", argv[i], (void *)p);
        missing += !p;
    }
    return missing;
}
