// Code that changes after it ran (what JIT compilers do; the ARM64 build's translations must notice): an immediate
// rewritten in place many times, a function rewritten while data next to it on the same page keeps being written,
// code that rewrites the instruction right after its own store (one translated block), and memory freed and
// allocated again at the same address with other code in it.
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef int (*fn)(void);
static int failed;
#define CHECK(c) do { if (!(c)) { failed++; printf("  failed: %s (line %d)\n", #c, __LINE__); } } while (0)

static void put_mov_ret(unsigned char *p, int v) { p[0] = 0xB8; memcpy(p + 1, &v, 4); p[5] = 0xC3; }
static unsigned char *g_p6;
static int g_remapped6;
static LONG WINAPI remap6(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || g_remapped6) return EXCEPTION_CONTINUE_SEARCH;
    g_remapped6++;
    return VirtualAlloc(g_p6, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) == g_p6 ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

int main(void) {
    unsigned char *p = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    CHECK(p != NULL);
    // 1. mov eax, i; ret -- rewritten and called 200 times
    int ok1 = 0;
    for (int i = 0; i < 200; i++) { put_mov_ret(p, i * 7); if (((fn)p)() == i * 7) ok1++; }
    CHECK(ok1 == 200);

    // 2. data on the code's page, written between calls; the code itself rewritten now and then
    volatile int *data = (volatile int *)(p + 2048);
    int ok2 = 0;
    for (int i = 0; i < 2000; i++) {
        data[i & 63] = i;
        if (i % 100 == 0) put_mov_ret(p + 64, i);
        if (((fn)(p + 64))() == i / 100 * 100) ok2++;
    }
    CHECK(ok2 == 2000);

    // 3. self-modifying: the store patches the immediate of the instruction that follows it
    //    mov byte [rip + 1], 0x42 ; mov eax, 0x11 ; ret   ->  returns 0x42
    unsigned char sm[] = { 0xC6, 0x05, 0x01, 0x00, 0x00, 0x00, 0x42, 0xB8, 0x11, 0x00, 0x00, 0x00, 0xC3 };
    // (the displacement is from the next instruction (p+7) to the immediate (p+8): 1)
    int ok3 = 0;
    unsigned char *s3 = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    for (int i = 0; i < 3; i++) { memcpy(s3, sm, sizeof sm); if (((fn)s3)() == 0x42) ok3++; }
    CHECK(ok3 == 3);

    // 4. freed, then allocated again (often at the same address) with other code
    int ok4 = 0;
    for (int i = 0; i < 20; i++) {
        unsigned char *q = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        put_mov_ret(q, 1000 + i);
        if (((fn)q)() == 1000 + i) ok4++;
        VirtualFree(q, 0, MEM_RELEASE);
    }
    CHECK(ok4 == 20);

    // 5. a jump reaching its translated target through the lookup (not linked yet), then a block found changed: the
    //    engine went back to C from that block's check while it still had the jump's site as the one to link, and
    //    linked the jump to the changed block. On a page written often (its blocks check their bytes: Mono's JIT).
    //    A: mov eax, 1; test ecx, ecx; jnz B; ret    B: add eax, 2; jmp C    C: add eax, 4; ret  (then add eax, 8)
    unsigned char *p5 = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    unsigned char *a5 = p5, *b5 = p5 + 64, *c5 = p5 + 128;
    volatile int *data5 = (volatile int *)(p5 + 8192);
    unsigned char ca[] = { 0xB8, 1, 0, 0, 0, 0x85, 0xC9, 0x0F, 0x85, 0, 0, 0, 0, 0xC3 };
    int rel = (int)(b5 - (a5 + 13)); memcpy(ca + 9, &rel, 4);
    unsigned char cb[] = { 0x83, 0xC0, 0x02, 0xE9, 0, 0, 0, 0 };
    rel = (int)(c5 - (b5 + 8)); memcpy(cb + 4, &rel, 4);
    unsigned char cc[] = { 0x83, 0xC0, 0x04, 0xC3 };
    memcpy(a5, ca, sizeof ca); memcpy(b5, cb, sizeof cb); memcpy(c5, cc, sizeof cc);
    typedef int (*fn1)(int);
    for (int i = 0; i < 40; i++) { ((fn1)a5)(0); data5[i] = i; }        // (the page written often: checked blocks)
    int r5a = ((fn1)a5)(0);                                               // A, its JNZ not taken yet
    ((fn)b5)();                                                           // B and C translated
    c5[2] = 0x08;                                                         // C changed
    int r5c = ((fn1)a5)(1), r5d = ((fn1)a5)(1);                           // A -> B -> C, twice
    CHECK(r5a == 1); CHECK(r5c == 11); CHECK(r5d == 11);

    // 6. a write to memory that held translated code and was freed: an access violation for the program (here its
    //    handler maps the memory again), not a write to code to be retried (the engine had retried it for ever)
    unsigned char *p6 = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    put_mov_ret(p6, 66);
    for (int i = 0; i < 20; i++) { ((fn)p6)(); p6[2048 + i] = (unsigned char)i; }   // (translated, page written often)
    int r6a = ((fn)p6)();
    VirtualFree(p6, 0, MEM_RELEASE);
    g_p6 = p6;
    PVOID h6 = AddVectoredExceptionHandler(1, remap6);
    ((volatile unsigned char *)p6)[100] = 7;
    RemoveVectoredExceptionHandler(h6);
    CHECK(r6a == 66); CHECK(g_remapped6 == 1); CHECK(p6[100] == 7);

    printf("smc: in place %d/200, with data on the page %d/2000, self-modifying %d/3, reallocated %d/20, linked past a changed block %d %d, written after freed %d: %d failed checks -> %s\n",
           ok1, ok2, ok3, ok4, r5c, r5d, g_remapped6, failed, failed ? "FAILED" : "ok");
    return failed != 0;
}
