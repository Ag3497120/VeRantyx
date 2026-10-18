// A breakpoint (int3, the 0xCC byte) raises STATUS_BREAKPOINT (0x80000003). Windows delivers it to the program's
// handlers like any other exception, and it is a trap: the saved Rip points to the instruction after the int3, so a
// handler that returns EXCEPTION_CONTINUE_EXECUTION without touching Rip resumes past the breakpoint. The reported
// ExceptionAddress is the int3's own address. (Anti-debug stubs, CRT assertions and __debugbreak() all rely on this;
// a program that handles the breakpoint itself must not be killed as if it were unhandled.)
#include <windows.h>
#include <stdio.h>

static volatile int veh_seen;
static volatile unsigned char *bp_byte;       // what ExceptionAddress pointed at (should be the int3, 0xCC)
static LONG WINAPI veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
        veh_seen++;
        bp_byte = (unsigned char *)ep->ExceptionRecord->ExceptionAddress;
        return EXCEPTION_CONTINUE_EXECUTION;   // no Rip adjustment: Windows resumes after the int3
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(void) {
    void *h = AddVectoredExceptionHandler(1, veh);
    __asm__ volatile("int3");                  // the handler must resume right here, after the int3
    volatile int reached = 1;
    __asm__ volatile("int3");
    RemoveVectoredExceptionHandler(h);
    int addr_ok = bp_byte && *bp_byte == 0xCC;
    int ok = veh_seen == 2 && reached && addr_ok;
    printf("breakpoint: VEH saw int3 x%d, resumed past it without adjusting Rip=%d, ExceptionAddress at the int3=%d -> %s\n",
           veh_seen, reached, addr_ok, ok ? "ok" : "FAILED");
    fflush(stdout);
    return ok ? 0 : 1;
}
