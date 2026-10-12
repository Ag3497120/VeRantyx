// Milestone 2b: C++ exceptions (MinGW unwinds them through Windows SEH: RaiseException + RtlUnwindEx), on the
// main thread and on four threads throwing at the same time.
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
static std::atomic<int> destroyed;
struct Guard { int id; ~Guard() { destroyed += id; } };
static void deep(int n) {
    Guard g{n};
    if (n == 0) throw std::runtime_error("thrown from depth 0");
    deep(n - 1);
}
int main() {
    int caught = 0;
    try { deep(4); } catch (const std::exception &e) { caught = 1; std::printf("caught: %s\n", e.what()); }
    try { throw 42; } catch (int v) { caught += v == 42; }
    try { try { throw std::string("inner"); } catch (...) { throw; } } catch (const std::string &s) { caught += s == "inner"; }
    std::atomic<int> thread_catches{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++)
        threads.emplace_back([&] { for (int i = 0; i < 1000; i++) try { deep(3); } catch (const std::runtime_error &) { thread_catches++; } });
    for (auto &t : threads) t.join();
    int expect = 10 + 4 * 1000 * 6;   // deep(4) once, deep(3) 4000 times
    std::printf("c++ exceptions: caught=%d destructors=%d thread_catches=%d -> %s\n", caught, destroyed.load(), thread_catches.load(),
                caught == 3 && destroyed == expect && thread_catches == 4000 ? "ok" : "FAILED");
    return 0;
}
