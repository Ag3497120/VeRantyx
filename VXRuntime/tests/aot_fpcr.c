// A module's enter/exit stubs write FPCR only when it differs and leave FPSR's flags in the hardware (RECOMP.md "FPCR").
// The guest sets MXCSR (rounding, FTZ/DAZ, flags), calls a native import that does float work (ucrtbase's pow, the
// runtime's own C), returns and checks both sides: the guest's rounding, denormal mode and sticky flags are as it left
// them, the import ran with the host's FPCR (round to nearest, denormals kept), LDMXCSR leaves no stale flag from the
// import, and the flags the guest raised before the call are still there afterwards. Run twice by aot_fpcr_check.py:
// as it is, and with VXRT_AOT_FPCR_ALWAYS=1 (the previous stubs); both must print "aot_fpcr: 0 failures".
// The import's own flags (PE from pow) may or may not reach MXCSR (they do with lazy flags, as on Windows where the
// import shares MXCSR), so they are only checked where the guest cleared them.
#include <windows.h>
#include <stdint.h>

double __cdecl pow(double, double);

static uint32_t get_mxcsr(void) { uint32_t v; __asm__ volatile("stmxcsr %0" : "=m"(v)); return v; }
static void set_mxcsr(uint32_t v) { __asm__ volatile("ldmxcsr %0" : : "m"(v) : "memory"); }
static float divss(float a, float b) { __asm__ volatile("divss %1, %0" : "+x"(a) : "x"(b)); return a; }
static float mulss(float a, float b) { __asm__ volatile("mulss %1, %0" : "+x"(a) : "x"(b)); return a; }
static uint32_t fbits(float f) { union { float f; uint32_t u; } x = { f }; return x.u; }
static uint64_t dbits(double d) { union { double d; uint64_t u; } x = { d }; return x.u; }
static volatile double two = 2.0, half = 0.5, tiny = -1074.0;
static volatile float one = 1.0f, zero = 0.0f, three = 3.0f, denorm = 1e-40f;

static int failures;
static void out(const char *s) { DWORD n; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)lstrlenA(s), &n, NULL); }
static void hex(uint64_t v) { char b[19] = "0x"; int n = 2; for (int s = 60; s >= 0; s -= 4) { int d = (int)(v >> s & 15); if (d || n > 2 || s == 0) b[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); } b[n] = 0; out(b); }
static void expect(const char *what, uint64_t got, uint64_t want) {
    if (got != want) { failures++; out("aot_fpcr: FAIL "); out(what); out(" got "); hex(got); out(" want "); hex(want); out("\n"); }
}

void mainCRTStartup(void) {
    static const uint32_t third[4] = { 0x3eaaaaab, 0x3eaaaaaa, 0x3eaaaaab, 0x3eaaaaaa };   // 1/3 by RC: nearest, down, up, zero
    // rounding: the guest's mode survives the native call, which itself rounds to nearest
    for (uint32_t rc = 0; rc < 4; rc++) {
        set_mxcsr(0x1f80 | rc << 13);
        expect("rounding before the call", fbits(divss(one, three)), third[rc]);
        uint64_t p = dbits(pow(two, half));
        expect("native pow(2, .5) rounded to nearest", p, 0x3ff6a09e667f3bcdull);
        expect("MXCSR control bits after the call", get_mxcsr() & 0xe000, rc << 13);
        expect("rounding after the call", fbits(divss(one, three)), third[rc]);
    }
    // FTZ+DAZ: the guest keeps them, the native call keeps its denormals
    set_mxcsr(0x9fc0);
    expect("FTZ/DAZ flushes before the call", fbits(mulss(denorm, one)), 0);
    expect("native pow(2, -1074) is the denormal", dbits(pow(two, tiny)), 1);
    expect("FTZ/DAZ after the call", fbits(mulss(denorm, one)), 0);
    expect("MXCSR FTZ/DAZ bits after the call", get_mxcsr() & 0x8040, 0x8040);
    set_mxcsr(0x1f80);
    expect("denormals kept again", fbits(mulss(denorm, one)) != 0, 1);
    // flags: raised by the guest, kept across calls
    set_mxcsr(0x1f80);
    (void)divss(one, zero);                                   // ZE
    (void)divss(zero, zero);                                  // IE
    for (int i = 0; i < 3; i++) (void)pow(two, half);
    expect("IE and ZE after native calls", get_mxcsr() & 5, 5);
    // LDMXCSR clears whatever the import left; a flag raised after the call is seen
    (void)pow(two, half);
    set_mxcsr(0x1f80);
    expect("no stale flag after LDMXCSR", get_mxcsr() & 0x3f, 0);
    (void)pow(two, half);
    (void)divss(one, zero);
    expect("ZE after the call", get_mxcsr() & 4, 4);
    // STMXCSR twice: the first read does not lose what the second must see
    expect("flags are sticky across reads", get_mxcsr() & 4, 4);
    { char d[2] = { (char)('0' + (failures > 9 ? 9 : failures)), 0 }; out("aot_fpcr: "); out(d); out(" failures\n"); }
    ExitProcess(failures != 0);
}
