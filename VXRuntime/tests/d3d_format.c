#include "d3d_format.h"
#include <stdio.h>

static int expect_pair(uint32_t a, uint32_t b, int expected) {
    int forward = d3d_copy_formats_compatible(a, b);
    int reverse = d3d_copy_formats_compatible(b, a);
    if (forward == expected && reverse == expected) return 1;
    printf("format compatibility (%u, %u): got %d/%d, expected %d\n", a, b, forward, reverse, expected);
    return 0;
}

int main(void) {
    int ok = 1;
    ok &= expect_pair(87, 90, 1);                                  // B8G8R8A8_UNORM / TYPELESS
    ok &= expect_pair(90, 91, 1);                                  // B8G8R8A8_TYPELESS / UNORM_SRGB
    ok &= expect_pair(87, 91, 1);                                  // B8G8R8A8_UNORM / UNORM_SRGB
    ok &= expect_pair(88, 92, 1);                                  // B8G8R8X8_UNORM / TYPELESS
    ok &= expect_pair(92, 93, 1);                                  // B8G8R8X8_TYPELESS / UNORM_SRGB
    ok &= expect_pair(88, 93, 1);                                  // B8G8R8X8_UNORM / UNORM_SRGB
    ok &= expect_pair(27, 29, 1);                                  // RGBA8 typeless / sRGB
    ok &= expect_pair(70, 72, 1);                                  // BC1 typeless / sRGB
    ok &= expect_pair(87, 88, 0);                                  // B8G8R8A8 / B8G8R8X8
    ok &= expect_pair(28, 87, 0);                                  // RGBA8 / BGRA8
    ok &= expect_pair(94, 97, 0);                                  // BC6H / BC7
    puts(ok ? "DXGI copy format families: ok" : "DXGI copy format families: FAILED");
    return ok ? 0 : 1;
}
