#include "d3d12_view.h"
#include <stdio.h>
#include <string.h>

static int check(int condition, const char *name) {
    if (!condition) printf("d3d12 texture view range: %s: FAILED\n", name);
    return condition ? 0 : 1;
}

static int range(uint32_t dimension, const uint32_t words[6], uint32_t resource_mips,
                 uint32_t resource_slices, uint32_t mip, uint32_t mips, uint32_t first,
                 uint32_t slices, uint32_t plane) {
    d3d12_srv_texture_range12 actual;
    return d3d12_normalize_srv_texture_range12(dimension, words, resource_mips, resource_slices, &actual) &&
        actual.mip == mip && actual.mips == mips && actual.first_slice == first &&
        actual.slices == slices && actual.plane == plane;
}

int main(void) {
    uint32_t words[6] = { 0 };
    d3d12_srv_texture_range12 actual;
    d3d12_uav_texture3d_range12 uav3d;
    uint32_t component_mapping = UINT32_MAX;
    int failed = 0;

    const uint32_t default_component_mapping = (1u << 12) | (1u << 3) | (2u << 6) | (3u << 9);
    const uint32_t red_green_zero_one_mapping = (1u << 12) | (4u << 6) | (5u << 9) | 1u;
    failed += check(d3d12_normalize_srv_component_mapping12(default_component_mapping, &component_mapping) &&
                    component_mapping == 0,
                    "D3D12 default SRV swizzle normalizes to identity");
    failed += check(d3d12_normalize_srv_component_mapping12(red_green_zero_one_mapping, &component_mapping) &&
                    component_mapping == (GPU_IR_COMPONENT_SWIZZLE_G | (GPU_IR_COMPONENT_SWIZZLE_R << 3) |
                        (GPU_IR_COMPONENT_SWIZZLE_ZERO << 6) | (GPU_IR_COMPONENT_SWIZZLE_ONE << 9)),
                    "D3D12 channel selections and constants normalize to common selectors");
    failed += check(!d3d12_normalize_srv_component_mapping12(0, &component_mapping) &&
                    !d3d12_normalize_srv_component_mapping12((1u << 12) | (6u << 6), &component_mapping),
                    "unset D3D12 mapping marker and reserved selectors are rejected");

    failed += check(d3d12_default_srv_texture_dimension12(2, 1, 1) == 2 &&
                    d3d12_default_srv_texture_dimension12(2, 4, 1) == 3 &&
                    d3d12_default_srv_texture_dimension12(3, 1, 1) == 4 &&
                    d3d12_default_srv_texture_dimension12(3, 4, 1) == 5 &&
                    d3d12_default_srv_texture_dimension12(3, 1, 4) == 6 &&
                    d3d12_default_srv_texture_dimension12(3, 4, 4) == 7 &&
                    d3d12_default_srv_texture_dimension12(4, 1, 1) == 8,
                    "null SRV defaults preserve resource dimension, arrays, and MSAA");
    failed += check(d3d12_default_srv_texture_dimension12(1, 1, 1) == 0 &&
                    d3d12_default_srv_texture_dimension12(2, 1, 4) == 0 &&
                    d3d12_default_srv_texture_dimension12(4, 1, 4) == 0,
                    "invalid default texture SRV shapes are rejected");
    failed += check(d3d12_default_uav_texture_dimension12(2, 1) == 2 &&
                    d3d12_default_uav_texture_dimension12(2, 4) == 3 &&
                    d3d12_default_uav_texture_dimension12(3, 1) == 4 &&
                    d3d12_default_uav_texture_dimension12(3, 4) == 5 &&
                    d3d12_default_uav_texture_dimension12(4, 1) == 8 &&
                    d3d12_uav_texture_dimension_matches12(4, 8) &&
                    !d3d12_uav_texture_dimension_matches12(4, 4),
                    "null UAV dimensions follow the resource type and mismatches are rejected");

    failed += check(d3d12_normalize_uav_texture3d_range12(1, 1, UINT32_MAX, 4, 8, &uav3d) &&
                    uav3d.mip == 1 && uav3d.first_w_slice == 1 && uav3d.w_size == 3,
                    "3D UAV depth uses the selected mip and all-remaining WSize");
    failed += check(d3d12_normalize_uav_texture3d_range12(0, 1, 2, 4, 8, &uav3d) &&
                    uav3d.first_w_slice == 1 && uav3d.w_size == 2,
                    "3D UAV preserves a bounded FirstWSlice and WSize");
    failed += check(!d3d12_normalize_uav_texture3d_range12(0, 8, 1, 4, 8, &uav3d) &&
                    !d3d12_normalize_uav_texture3d_range12(0, 1, 8, 4, 8, &uav3d) &&
                    !d3d12_normalize_uav_texture3d_range12(0, 0, 0, 4, 8, &uav3d) &&
                    !d3d12_normalize_uav_texture3d_range12(4, 0, 1, 4, 8, &uav3d),
                    "empty and out-of-bounds 3D UAV ranges are rejected");

    words[0] = 2; words[1] = UINT32_MAX;
    failed += check(range(8, words, 5, 1, 2, 3, 0, 1, 0),
                    "Texture3D mip selection and all-remaining mip range");
    words[1] = 0;
    failed += check(!d3d12_normalize_srv_texture_range12(8, words, 5, 1, &actual),
                    "zero mip count is rejected; UINT_MAX is the all-mips sentinel");
    words[1] = UINT32_MAX;
    words[0] = 0;
    words[2] = 0x3f800000u; // ResourceMinLODClamp = 1.0f.
    failed += check(d3d12_normalize_srv_texture_range12(8, words, 5, 1, &actual) && actual.min_lod == 1.0f,
                    "Texture3D ResourceMinLODClamp is preserved as a finite float");
    words[2] = 0xbf800000u;
    failed += check(!d3d12_normalize_srv_texture_range12(8, words, 5, 1, &actual),
                    "negative ResourceMinLODClamp is rejected");
    words[2] = 0x7fc00000u;
    failed += check(!d3d12_normalize_srv_texture_range12(8, words, 5, 1, &actual),
                    "NaN ResourceMinLODClamp is rejected");
    words[0] = 2;
    words[2] = 0x3f800000u;
    failed += check(!d3d12_normalize_srv_texture_range12(8, words, 5, 1, &actual),
                    "nonzero min-LOD clamp with MostDetailedMip is rejected");

    memset(words, 0, sizeof words);
    words[0] = 1; words[1] = 2; words[2] = 5; words[3] = 2; words[4] = 1;
    failed += check(range(5, words, 5, 8, 1, 2, 5, 2, 1),
                    "Texture2DArray mip, slice, and plane fields");
    words[3] = 0;
    failed += check(!d3d12_normalize_srv_texture_range12(5, words, 5, 8, &actual),
                    "zero Texture2DArray size is rejected");
    words[3] = 2;
    words[0] = 0;
    words[5] = 0x3f800000u;
    failed += check(d3d12_normalize_srv_texture_range12(5, words, 5, 8, &actual) && actual.min_lod == 1.0f,
                    "Texture2DArray preserves ResourceMinLODClamp alongside its slice range");
    words[5] = 0;

    memset(words, 0, sizeof words);
    words[0] = 3; words[1] = 2; // Texture2DMSArray has no mip fields.
    failed += check(range(7, words, 1, 8, 0, 1, 3, 2, 0),
                    "Texture2DMSArray interprets its first words as slices");
    words[1] = 0;
    failed += check(!d3d12_normalize_srv_texture_range12(7, words, 1, 8, &actual),
                    "zero Texture2DMSArray size is rejected");

    memset(words, 0, sizeof words);
    words[0] = 1; words[1] = 2;
    failed += check(range(9, words, 5, 12, 1, 2, 0, 6, 0),
                    "TextureCube exposes exactly six faces");
    failed += check(!d3d12_normalize_srv_texture_range12(9, words, 5, 5, &actual),
                    "TextureCube rejects fewer than six resource faces");

    words[2] = 6; // First2DArrayFace
    words[3] = 1; // NumCubes
    failed += check(range(10, words, 5, 12, 1, 2, 6, 6, 0),
                    "one TextureCubeArray element maps to six requested faces");
    words[2] = 3;
    failed += check(range(10, words, 5, 12, 1, 2, 3, 6, 0),
                    "TextureCubeArray preserves its first face");
    words[2] = 6;
    words[3] = 2;
    failed += check(!d3d12_normalize_srv_texture_range12(10, words, 5, 12, &actual),
                    "out-of-range cube count is rejected");
    words[2] = 0;
    words[3] = 0;
    failed += check(!d3d12_normalize_srv_texture_range12(10, words, 5, 12, &actual),
                    "empty cube array is rejected");
    words[3] = UINT32_MAX;
    failed += check(!d3d12_normalize_srv_texture_range12(10, words, 5, UINT32_MAX, &actual),
                    "cube-count multiplication overflow is rejected");

    memset(words, 0, sizeof words);
    words[0] = 4; words[1] = 2;
    failed += check(!d3d12_normalize_srv_texture_range12(8, words, 5, 1, &actual),
                    "out-of-range mip selection is rejected");
    failed += check(!d3d12_normalize_srv_texture_range12(4, words, 5, 1, &actual),
                    "out-of-range mip count is rejected");
    failed += check(!d3d12_normalize_srv_texture_range12(11, words, 5, 1, &actual),
                    "unsupported SRV dimensions are rejected");

    puts(failed ? "d3d12 texture view ranges: FAILED" :
         "d3d12 texture view ranges: SRV mip/slice/plane fields, UAV 3D depth ranges, defaults, bounds and overflow: ok");
    return failed ? 1 : 0;
}
