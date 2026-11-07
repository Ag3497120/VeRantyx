/* SPDX-License-Identifier: MIT */
#include "sgsr_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

static void expectSize(uint32_t sourceWidth, uint32_t sourceHeight,
                       uint32_t targetHeight, uint32_t width, uint32_t height) {
    struct MvrSgsrSize actual = {7, 9, 11};
    CHECK(mvrSgsrOutputSize(sourceWidth, sourceHeight, targetHeight, &actual));
    CHECK(actual.eyeWidth == width);
    CHECK(actual.height == height);
    CHECK(actual.sbsWidth == width * 2u);
}

static void rejectSize(uint32_t width, uint32_t height, uint32_t target) {
    struct MvrSgsrSize actual = {7, 9, 11};
    const struct MvrSgsrSize before = actual;
    CHECK(!mvrSgsrOutputSize(width, height, target, &actual));
    CHECK(!memcmp(&actual, &before, sizeof actual));
}

static void expectSelection(int requested, int ready, int freeTier,
                            uint32_t sourceWidth, uint32_t sourceHeight,
                            uint32_t targetHeight, uint32_t width,
                            uint32_t height, unsigned expectedActive) {
    struct MvrSgsrSize actual = {7, 9, 11};
    unsigned active = 37;
    CHECK(mvrSgsrSelectSize(requested, ready, freeTier, sourceWidth,
                            sourceHeight, targetHeight, &actual, &active));
    CHECK(actual.eyeWidth == width);
    CHECK(actual.height == height);
    CHECK(actual.sbsWidth == width * 2u);
    CHECK(active == expectedActive);
}

int main(void) {
    CHECK(mvrSgsrShouldUpscale(1, 1, 0, 1600, 1760));
    CHECK(!mvrSgsrShouldUpscale(0, 1, 0, 1600, 1760));
    CHECK(!mvrSgsrShouldUpscale(1, 0, 0, 1600, 1760));
    CHECK(!mvrSgsrShouldUpscale(1, 1, 1, 1600, 1760));
    CHECK(mvrSgsrShouldUpscale(-1, 7, 0, 1600, 1760));
    CHECK(!mvrSgsrShouldUpscale(1, 1, -1, 1600, 1760));
    CHECK(mvrSgsrShouldUpscale(1, 1, 0, 1899, 2000));
    CHECK(!mvrSgsrShouldUpscale(1, 1, 0, 1900, 2000));
    CHECK(!mvrSgsrShouldUpscale(1, 1, 0, 1901, 2000));
    CHECK(mvrSgsrShouldUpscale(1, 1, 0, 15564, 16384));
    CHECK(!mvrSgsrShouldUpscale(1, 1, 0, 15565, 16384));
    CHECK(!mvrSgsrShouldUpscale(1, 1, 0, 0, 1760));
    CHECK(!mvrSgsrShouldUpscale(1, 1, 0, 1600, 0));
    CHECK(!mvrSgsrShouldUpscale(1, 1, 0, UINT32_MAX, 1760));
    CHECK(!mvrSgsrShouldUpscale(1, 1, 0, 1600, UINT32_MAX));

    expectSize(1280, 1600, 1760, 1408, 1760);
    expectSize(1408, 1600, 1760, 1548, 1760);
    expectSize(1600, 1824, 2208, 1936, 2208);
    expectSize(1728, 1984, 2208, 1923, 2208);
    expectSize(2048, 1024, 1760, 2048, 1024);
    expectSize(3000, 1000, 2208, 2048, 682);
    /* Native two-stage floor gives 933, not the one-step ratio's 932. */
    expectSize(2000, 911, 1760, 2048, 933);
    expectSize(2048, 16384, 16384, 2048, 16384);
    rejectSize(0, 1600, 1760);
    rejectSize(1280, 0, 1760);
    rejectSize(1280, 1600, 0);
    rejectSize(8193, 1600, 1760);
    rejectSize(1280, 16385, 1760);
    rejectSize(1280, 1600, 16385);
    rejectSize(UINT32_MAX, UINT32_MAX, UINT32_MAX);
    rejectSize(1, 16384, 1);       /* Rounded eye width would be zero. */
    rejectSize(8192, 1, 16384);    /* Capped output height would be zero. */
    CHECK(!mvrSgsrOutputSize(1280, 1600, 1760, NULL));

    expectSelection(0, 1, 0, 1728, 1984, 2208, 1728, 1984, 0);
    expectSelection(1, 0, 0, 1728, 1984, 2208, 1728, 1984, 0);
    expectSelection(1, 1, 1, 1728, 1984, 2208, 1728, 1984, 0);
    expectSelection(1, 1, 0, 1728, 1984, 1760, 1728, 1984, 0);
    expectSelection(1, 1, 0, 1280, 1900, 2000, 1280, 1900, 0);
    expectSelection(1, 1, 0, 1408, 1600, 1760, 1548, 1760, 1);
    expectSelection(0, 0, 0, 8192, 16384, 0, 8192, 16384, 0);
    expectSelection(1, 1, 0, 1280, 1600, 0, 1280, 1600, 0);
    {
        struct MvrSgsrSize actual = {7, 9, 11};
        const struct MvrSgsrSize before = actual;
        unsigned active = 37;
        CHECK(!mvrSgsrSelectSize(1, 1, 0, 8192, 1, 16384, &actual, &active));
        CHECK(!memcmp(&actual, &before, sizeof actual));
        CHECK(active == 37);
        CHECK(!mvrSgsrSelectSize(0, 1, 0, 8193, 1600, 1760, &actual, &active));
        CHECK(!memcmp(&actual, &before, sizeof actual));
        CHECK(active == 37);
        CHECK(!mvrSgsrSelectSize(1, 1, 0, 1280, 1600, 1760, NULL, &active));
        CHECK(active == 37);
        CHECK(mvrSgsrSelectSize(1, 1, 0, 1280, 1600, 1760, &actual, NULL));
    }
    {
        struct MvrSgsrSize actual = {7, 9, 11};
        CHECK(mvrSgsrSourceSize(8192, 16384, &actual));
        CHECK(actual.sbsWidth == MVR_SGSR_MAX_SBS_WIDTH);
        CHECK(!mvrSgsrSourceSize(8193, 16384, &actual));
        CHECK(!mvrSgsrSourceSize(8192, 16385, &actual));
        CHECK(!mvrSgsrSourceSize(8192, 16384, NULL));
    }
    printf("PASS %u SGSR policy checks; no GL/XR/device/encoder calls\n", checks);
    return 0;
}
