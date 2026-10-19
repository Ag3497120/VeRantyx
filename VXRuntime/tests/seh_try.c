#include <windows.h>
#include <stdio.h>
// a __try/__except whose filter executes the handler: the C runtime's handler unwinds to the __except block
static int filter(unsigned code) { return code == 0x406D1388 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH; }
int main(void) {
    int reached = 0;
    ULONG_PTR info[4] = { 0x1000, (ULONG_PTR)"worker", (ULONG_PTR)-1, 0 };
    __try {
        RaiseException(0x406D1388, 0, 4, info);
        printf("seh try: returned from RaiseException -> FAILED\n");
    } __except (filter(GetExceptionCode())) {
        reached = 1;
    }
    printf("seh try: __except %s -> %s\n", reached ? "reached" : "not reached", reached ? "ok" : "FAILED");
    return reached ? 0 : 1;
}
