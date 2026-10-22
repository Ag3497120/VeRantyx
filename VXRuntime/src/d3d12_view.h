// Normalize D3D12 texture SRV subresources and 3D UAV depth ranges for Metal views.
#ifndef VX_D3D12_VIEW_H
#define VX_D3D12_VIEW_H
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "gpu_ir.h"

typedef struct {
    uint32_t mip, mips, first_slice, slices, plane;
    float min_lod;
} d3d12_srv_texture_range12;

typedef struct {
    uint32_t mip, first_w_slice, w_size;
} d3d12_uav_texture3d_range12;

// D3D12 encodes four 3-bit source selectors and requires bit 12 to be set. Convert the selected channels into the
// API-neutral descriptor representation; zero in a lane means that lane's identity source.
static inline int d3d12_normalize_srv_component_mapping12(uint32_t mapping, uint32_t *out) {
    if (!out || !(mapping & (1u << 12)) || (mapping & ~0x1fffu)) return 0;
    uint32_t normalized = 0;
    for (uint32_t component = 0; component < 4; component++) {
        uint32_t selector = (mapping >> (component * 3)) & 7u;
        if (selector > 5) return 0;
        uint32_t common = selector == component ? GPU_IR_COMPONENT_SWIZZLE_IDENTITY :
            selector < 4 ? GPU_IR_COMPONENT_SWIZZLE_R + selector :
            selector == 4 ? GPU_IR_COMPONENT_SWIZZLE_ZERO : GPU_IR_COMPONENT_SWIZZLE_ONE;
        normalized |= common << (component * 3);
    }
    *out = normalized;
    return 1;
}

// D3D12_RESOURCE_DIMENSION values: BUFFER=1, TEXTURE1D=2, TEXTURE2D=3, TEXTURE3D=4.
static inline uint32_t d3d12_default_srv_texture_dimension12(uint32_t resource_dimension,
                                                              uint32_t array_size,
                                                              uint32_t sample_count) {
    if (!array_size || !sample_count) return 0;
    switch (resource_dimension) {
    case 2:
        return sample_count == 1 ? (array_size > 1 ? 3u : 2u) : 0;
    case 3:
        return sample_count > 1 ? (array_size > 1 ? 7u : 6u) : (array_size > 1 ? 5u : 4u);
    case 4:
        return sample_count == 1 ? 8u : 0;
    default:
        return 0;
    }
}

static inline uint32_t d3d12_default_uav_texture_dimension12(uint32_t resource_dimension,
                                                              uint32_t array_size) {
    if (!array_size) return 0;
    switch (resource_dimension) {
    case 2: return array_size > 1 ? 3u : 2u;
    case 3: return array_size > 1 ? 5u : 4u;
    case 4: return 8u;
    default: return 0;
    }
}

static inline int d3d12_uav_texture_dimension_matches12(uint32_t resource_dimension, uint32_t view_dimension) {
    switch (resource_dimension) {
    case 2: return view_dimension == 2 || view_dimension == 3;
    case 3: return view_dimension == 4 || view_dimension == 5;
    case 4: return view_dimension == 8;
    default: return 0;
    }
}

static inline int d3d12_normalize_uav_texture3d_range12(uint32_t mip, uint32_t first_w_slice,
                                                         uint32_t w_size, uint32_t resource_mips,
                                                         uint32_t resource_depth,
                                                         d3d12_uav_texture3d_range12 *out) {
    if (!out || !resource_mips || !resource_depth || mip >= resource_mips) return 0;
    uint32_t mip_depth = mip >= 32 ? 1u : resource_depth >> mip;
    if (!mip_depth) mip_depth = 1;
    if (first_w_slice > mip_depth) return 0;
    if (w_size == UINT32_MAX) w_size = mip_depth - first_w_slice;
    else if (!w_size || w_size > mip_depth - first_w_slice) return 0;
    if (!w_size) return 0;
    *out = (d3d12_uav_texture3d_range12){ mip, first_w_slice, w_size };
    return 1;
}

// `desc_words` begins at D3D12_SHADER_RESOURCE_VIEW_DESC's union. Cube-array NumCubes counts cubes,
// while Metal's view range counts the six 2D faces that make up each cube.
static inline int d3d12_normalize_srv_texture_range12(uint32_t dimension, const uint32_t desc_words[6],
                                                      uint32_t resource_mips, uint32_t resource_slices,
                                                      d3d12_srv_texture_range12 *out) {
    if (!desc_words || !out || !resource_mips || !resource_slices) return 0;

    uint32_t mip = 0, mips = 0, first = 0, slices = 0, plane = 0;
    uint32_t min_lod_word = UINT32_MAX;
    switch (dimension) {
    case 2: // D3D12_SRV_DIMENSION_TEXTURE1D
    case 4: // D3D12_SRV_DIMENSION_TEXTURE2D
    case 8: // D3D12_SRV_DIMENSION_TEXTURE3D
        mip = desc_words[0];
        mips = desc_words[1];
        if (dimension == 4) plane = desc_words[2];
        min_lod_word = dimension == 4 ? 3 : 2;
        slices = 1;
        break;
    case 3: // D3D12_SRV_DIMENSION_TEXTURE1DARRAY
        mip = desc_words[0];
        mips = desc_words[1];
        first = desc_words[2];
        slices = desc_words[3];
        min_lod_word = 4;
        break;
    case 5: // D3D12_SRV_DIMENSION_TEXTURE2DARRAY
        mip = desc_words[0];
        mips = desc_words[1];
        first = desc_words[2];
        slices = desc_words[3];
        plane = desc_words[4];
        min_lod_word = 5;
        break;
    case 6: // D3D12_SRV_DIMENSION_TEXTURE2DMS
        mips = 1;
        slices = 1;
        break;
    case 7: // D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY: the first two words are array slice and count.
        first = desc_words[0];
        slices = desc_words[1];
        mips = 1;
        break;
    case 9: // D3D12_SRV_DIMENSION_TEXTURECUBE
        mip = desc_words[0];
        mips = desc_words[1];
        min_lod_word = 2;
        slices = 6;
        break;
    case 10: { // D3D12_SRV_DIMENSION_TEXTURECUBEARRAY
        mip = desc_words[0];
        mips = desc_words[1];
        first = desc_words[2]; // First2DArrayFace
        uint32_t cubes = desc_words[3]; // NumCubes
        if (!cubes || cubes > UINT32_MAX / 6u) return 0;
        slices = cubes * 6u;
        min_lod_word = 4;
        break;
    }
    default:
        return 0;
    }

    float min_lod = 0.0f;
    if (min_lod_word != UINT32_MAX) {
        memcpy(&min_lod, &desc_words[min_lod_word], sizeof min_lod);
        if (!isfinite(min_lod) || min_lod < 0.0f) return 0;
        // D3D12 documents different MipLevels interpretation when this clamp is used and
        // recommends leaving MostDetailedMip at zero. Keep the implemented view range exact.
        if (min_lod != 0.0f && mip != 0) return 0;
    }

    if (mip >= resource_mips) return 0;
    if (mips == UINT32_MAX) mips = resource_mips - mip;
    else if (!mips) return 0;
    else if (mips > resource_mips - mip) return 0;
    if (first > resource_slices) return 0;
    if (!slices || slices > resource_slices - first) return 0;

    *out = (d3d12_srv_texture_range12){ mip, mips, first, slices, plane, min_lod };
    return 1;
}

#endif
