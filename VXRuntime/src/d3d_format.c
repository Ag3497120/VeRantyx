#include "d3d_format.h"

static uint32_t copy_format_family(uint32_t format) {
    if (format >= 1 && format <= 4) return 1;
    if (format >= 5 && format <= 8) return 2;
    if (format >= 9 && format <= 14) return 3;
    if (format >= 15 && format <= 18) return 4;
    if (format >= 19 && format <= 22) return 5;
    if (format >= 23 && format <= 25) return 6;
    if (format >= 27 && format <= 32) return 7;
    if (format >= 33 && format <= 38) return 8;
    if (format >= 39 && format <= 43) return 9;
    if (format >= 44 && format <= 47) return 10;
    if (format >= 48 && format <= 52) return 11;
    if (format >= 53 && format <= 59) return 12;
    if (format >= 60 && format <= 64) return 13;
    if (format >= 70 && format <= 72) return 14;
    if (format >= 73 && format <= 75) return 15;
    if (format >= 76 && format <= 78) return 16;
    if (format >= 79 && format <= 81) return 17;
    if (format >= 82 && format <= 84) return 18;
    if (format >= 94 && format <= 96) return 19;
    if (format >= 97 && format <= 99) return 20;
    if (format == 87 || (format >= 90 && format <= 91)) return 21; // B8G8R8A8 UNORM / typeless / UNORM_SRGB
    if (format == 88 || (format >= 92 && format <= 93)) return 22; // B8G8R8X8 UNORM / typeless / UNORM_SRGB
    return 0;
}

int d3d_copy_formats_compatible(uint32_t source, uint32_t destination) {
    if (source == destination) return 1;
    uint32_t family = copy_format_family(source);
    return family && family == copy_format_family(destination);
}
