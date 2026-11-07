#pragma once
#include <cmath>
#include <cstdint>

namespace vrrt {
// The bridge's seqlock advances by two for each tracked display interval.
// Count packets missed while rendering, but reject reset/discontinuous counters
// and implausible advances. Unsigned subtraction handles ordinary wraparound.
inline uint64_t bridge_display_intervals(uint32_t current, uint32_t previous,
                                         double elapsed, double period) {
    if ((current|previous)&1u || !std::isfinite(elapsed) || elapsed < 0 ||
        !std::isfinite(period) || period <= 0) return 1;
    const uint32_t delta = current-previous;
    if (!delta || (delta&1u) || delta >= 0x80000000u) return 1;
    const uint64_t intervals = delta/2u;
    // Loose bound accommodates scheduler jitter and changed refresh rates while
    // preventing a reset or corrupt sequence from jumping the engine clock.
    const double plausible = std::ceil(elapsed/period*4.)+8.;
    if (intervals > plausible) return 1;
    return intervals ? intervals : 1;
}
}
