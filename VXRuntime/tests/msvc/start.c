// Program start for the tests built for the MSVC ABI (clang -target x86_64-pc-windows-msvc, linked without a
// C runtime): calls main and exits with its result.
__declspec(dllimport) void __stdcall ExitProcess(unsigned);
// The vtable every type_info refers to; only its address matters for these tests.
const void *type_info_vtable[2] __asm__("??_7type_info@@6B@") = { 0, 0 };
int main(void);
void mainCRTStartup(void) { ExitProcess((unsigned)main()); }
