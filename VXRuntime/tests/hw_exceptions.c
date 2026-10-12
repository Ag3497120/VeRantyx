// Milestone 2c: CPU exceptions become Windows exceptions.
//  - a vectored handler sees an access violation and an integer divide by zero, skips the faulting
//    instruction (2 bytes each, registers pinned) and continues;
//  - a software exception nobody handles reaches the unhandled-exception filter.
#include <windows.h>
#include <stdio.h>
static volatile int seen_av, seen_div;
static volatile ULONG_PTR av_address;
static LONG WINAPI vectored(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        seen_av = 1; av_address = ep->ExceptionRecord->ExceptionInformation[1];
        ep->ContextRecord->Rip += 2; return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_INT_DIVIDE_BY_ZERO) { seen_div = 1; ep->ContextRecord->Rip += 2; return EXCEPTION_CONTINUE_EXECUTION; }
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI unhandled(EXCEPTION_POINTERS *ep) {
    printf("hw exceptions: access violation=%d (at %#llx) divide by zero=%d unhandled code=%#lx params=%lu -> %s\n",
           seen_av, (unsigned long long)av_address, seen_div, ep->ExceptionRecord->ExceptionCode,
           ep->ExceptionRecord->NumberParameters,
           seen_av && av_address == 16 && seen_div && ep->ExceptionRecord->ExceptionCode == 0xE0001234 ? "ok" : "FAILED");
    fflush(stdout);
    ExitProcess(0);
    return EXCEPTION_EXECUTE_HANDLER;
}
int main(void) {
    AddVectoredExceptionHandler(1, vectored);
    SetUnhandledExceptionFilter(unhandled);
    __asm__ volatile("movl (%%rcx), %%eax" :: "c"(16ull) : "eax", "memory");            // 8B 01
    __asm__ volatile("movl $7, %%eax\n\tcltd\n\tidivl %%ecx" :: "c"(0) : "eax", "edx");  // F7 F9
    ULONG_PTR args[2] = { 1, 2 };
    RaiseException(0xE0001234, 0, 2, args);
    printf("hw exceptions: returned from an unhandled exception -> FAILED\n");
    return 1;
}
