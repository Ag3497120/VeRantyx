// Milestone 5a: MSVC-style C++ exceptions. Built for the MSVC ABI with clang, which emits the same FH3 tables
// (__CxxFrameHandler3) and catch funclets as MSVC; runs against this runtime's vcruntime140.
extern "C" __declspec(dllimport) int puts(const char *);
void operator delete(void *, decltype(sizeof 0)) noexcept {}
void operator delete(void *) noexcept {}

static int destroyed, order[16], norder;
struct Guard { int id; Guard(int i) : id(i) {} ~Guard() { destroyed += id; order[norder++] = id; } };
struct Base { int code; Base() : code(0) {} virtual ~Base() {} };
struct Derived : Base { int extra; Derived() : extra(0) {} };
struct Counted { static int live; Counted() { live++; } Counted(const Counted &) { live++; } ~Counted() { live--; } };
int Counted::live;

__declspec(noinline) static void thrower(int depth) {
    Guard g(depth);
    if (depth == 0) { Derived d; d.code = 42; d.extra = 7; throw d; }
    thrower(depth - 1);
}
__declspec(noinline) static int test_basic() {          // across 4 frames, caught as a base class by reference
    try { thrower(3); } catch (const Base &b) { return b.code; }
    return -1;
}
__declspec(noinline) static int test_value() { try { throw 5; } catch (long) { return -1; } catch (int v) { return v; } return -2; }
__declspec(noinline) static int test_rethrow() {
    try { try { throw Derived(); } catch (Derived &) { throw; } } catch (const Base &) { return 1; }
    return 0;
}
__declspec(noinline) static int test_nested() {
    int r = 0;
    try { throw 1; } catch (int) { try { throw 2.5; } catch (double d) { r = d == 2.5; } }
    return r;
}
__declspec(noinline) static int test_catch_all() { try { throw "text"; } catch (int) { return -1; } catch (...) { return 3; } return 0; }
__declspec(noinline) static int test_lifetime() { try { throw Counted(); } catch (Counted c) { (void)c; } return Counted::live; }
__declspec(noinline) static int test_loop() {          // many throws: nothing accumulates on the stack
    int caught = 0;
    for (int i = 0; i < 1000; i++) try { if (i >= 0) throw i; } catch (int v) { caught += v == i; }
    return caught;
}

static char *put_int(char *p, int v) {
    char tmp[12]; int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; }
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}
int main() {
    int basic = test_basic(), value = test_value(), rethrow = test_rethrow(), nested = test_nested();
    int all = test_catch_all(), live = test_lifetime(), loop = test_loop();
    bool ok = basic == 42 && destroyed == 6 && order[0] == 0 && order[3] == 3 && value == 5 && rethrow == 1 && nested == 1 &&
              all == 3 && live == 0 && loop == 1000;
    char line[160], *p = line;
    const char *head = "msvc exceptions: basic=";
    while (*head) *p++ = *head++;
    p = put_int(p, basic); *p++ = ' '; *p++ = 'd'; *p++ = '='; p = put_int(p, destroyed);
    *p++ = ' '; *p++ = 'v'; *p++ = '='; p = put_int(p, value); *p++ = ' '; *p++ = 'r'; *p++ = '='; p = put_int(p, rethrow);
    *p++ = ' '; *p++ = 'n'; *p++ = '='; p = put_int(p, nested); *p++ = ' '; *p++ = 'a'; *p++ = '='; p = put_int(p, all);
    *p++ = ' '; *p++ = 'l'; *p++ = '='; p = put_int(p, live); *p++ = ' '; *p++ = 'x'; *p++ = '='; p = put_int(p, loop);
    const char *tail = ok ? " -> ok" : " -> FAILED";
    while (*tail) *p++ = *tail++;
    *p = 0;
    puts(line);
    return ok ? 0 : 1;
}
