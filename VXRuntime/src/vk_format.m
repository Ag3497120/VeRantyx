// Vulkan formats and their Metal pixel and vertex formats, with the features VXRuntime reports for each.
#include "vk_impl.h"

#define P(x) MTLPixelFormat##x
#define V(x) MTLVertexFormat##x
#define NOP MTLPixelFormatInvalid
#define NOV MTLVertexFormatInvalid
//  Vulkan format                             Metal pixel format     vertex format          bytes bw bh aspects numeric srgb compressed substitute
#define C(vk, mtl, vtx, bytes, num, srgb) { VK_FORMAT_##vk, mtl, vtx, bytes, 1, 1, VKF_COLOR, num, srgb, 0, 0 }
#define BC(vk, mtl, bytes, srgb) { VK_FORMAT_##vk, mtl, NOV, bytes, 4, 4, VKF_COLOR, VKF_FLOAT, srgb, 1, 0 }
static const vk_format_info g_formats[] = {
    C(R4G4B4A4_UNORM_PACK16, P(ABGR4Unorm), NOV, 2, VKF_FLOAT, 0),
    C(B4G4R4A4_UNORM_PACK16, P(ABGR4Unorm), NOV, 2, VKF_FLOAT, 0),
    C(R5G6B5_UNORM_PACK16, P(B5G6R5Unorm), NOV, 2, VKF_FLOAT, 0),
    C(B5G6R5_UNORM_PACK16, P(B5G6R5Unorm), NOV, 2, VKF_FLOAT, 0),
    C(R5G5B5A1_UNORM_PACK16, P(A1BGR5Unorm), NOV, 2, VKF_FLOAT, 0),
    C(B5G5R5A1_UNORM_PACK16, P(A1BGR5Unorm), NOV, 2, VKF_FLOAT, 0),
    C(A1R5G5B5_UNORM_PACK16, P(BGR5A1Unorm), NOV, 2, VKF_FLOAT, 0),
    C(R8_UNORM, P(R8Unorm), V(UCharNormalized), 1, VKF_FLOAT, 0),
    C(R8_SNORM, P(R8Snorm), V(CharNormalized), 1, VKF_FLOAT, 0),
    C(R8_USCALED, NOP, V(UChar), 1, VKF_FLOAT, 0),
    C(R8_SSCALED, NOP, V(Char), 1, VKF_FLOAT, 0),
    C(R8_UINT, P(R8Uint), V(UChar), 1, VKF_UINT, 0),
    C(R8_SINT, P(R8Sint), V(Char), 1, VKF_SINT, 0),
    C(R8_SRGB, P(R8Unorm_sRGB), NOV, 1, VKF_FLOAT, 1),
    C(R8G8_UNORM, P(RG8Unorm), V(UChar2Normalized), 2, VKF_FLOAT, 0),
    C(R8G8_SNORM, P(RG8Snorm), V(Char2Normalized), 2, VKF_FLOAT, 0),
    C(R8G8_USCALED, NOP, V(UChar2), 2, VKF_FLOAT, 0),
    C(R8G8_SSCALED, NOP, V(Char2), 2, VKF_FLOAT, 0),
    C(R8G8_UINT, P(RG8Uint), V(UChar2), 2, VKF_UINT, 0),
    C(R8G8_SINT, P(RG8Sint), V(Char2), 2, VKF_SINT, 0),
    C(R8G8_SRGB, P(RG8Unorm_sRGB), NOV, 2, VKF_FLOAT, 1),
    C(R8G8B8_UNORM, NOP, V(UChar3Normalized), 3, VKF_FLOAT, 0),
    C(R8G8B8_SNORM, NOP, V(Char3Normalized), 3, VKF_FLOAT, 0),
    C(R8G8B8_UINT, NOP, V(UChar3), 3, VKF_UINT, 0),
    C(R8G8B8_SINT, NOP, V(Char3), 3, VKF_SINT, 0),
    C(R8G8B8A8_UNORM, P(RGBA8Unorm), V(UChar4Normalized), 4, VKF_FLOAT, 0),
    C(R8G8B8A8_SNORM, P(RGBA8Snorm), V(Char4Normalized), 4, VKF_FLOAT, 0),
    C(R8G8B8A8_USCALED, NOP, V(UChar4), 4, VKF_FLOAT, 0),
    C(R8G8B8A8_SSCALED, NOP, V(Char4), 4, VKF_FLOAT, 0),
    C(R8G8B8A8_UINT, P(RGBA8Uint), V(UChar4), 4, VKF_UINT, 0),
    C(R8G8B8A8_SINT, P(RGBA8Sint), V(Char4), 4, VKF_SINT, 0),
    C(R8G8B8A8_SRGB, P(RGBA8Unorm_sRGB), NOV, 4, VKF_FLOAT, 1),
    C(B8G8R8A8_UNORM, P(BGRA8Unorm), V(UChar4Normalized_BGRA), 4, VKF_FLOAT, 0),
    C(B8G8R8A8_SRGB, P(BGRA8Unorm_sRGB), NOV, 4, VKF_FLOAT, 1),
    C(A8B8G8R8_UNORM_PACK32, P(RGBA8Unorm), V(UChar4Normalized), 4, VKF_FLOAT, 0),
    C(A8B8G8R8_SNORM_PACK32, P(RGBA8Snorm), V(Char4Normalized), 4, VKF_FLOAT, 0),
    C(A8B8G8R8_UINT_PACK32, P(RGBA8Uint), V(UChar4), 4, VKF_UINT, 0),
    C(A8B8G8R8_SINT_PACK32, P(RGBA8Sint), V(Char4), 4, VKF_SINT, 0),
    C(A8B8G8R8_SRGB_PACK32, P(RGBA8Unorm_sRGB), NOV, 4, VKF_FLOAT, 1),
    C(A2R10G10B10_UNORM_PACK32, P(BGR10A2Unorm), NOV, 4, VKF_FLOAT, 0),
    C(A2B10G10R10_UNORM_PACK32, P(RGB10A2Unorm), V(UInt1010102Normalized), 4, VKF_FLOAT, 0),
    C(A2B10G10R10_SNORM_PACK32, NOP, V(Int1010102Normalized), 4, VKF_FLOAT, 0),
    C(A2B10G10R10_UINT_PACK32, P(RGB10A2Uint), NOV, 4, VKF_UINT, 0),
    C(R16_UNORM, P(R16Unorm), V(UShortNormalized), 2, VKF_FLOAT, 0),
    C(R16_SNORM, P(R16Snorm), V(ShortNormalized), 2, VKF_FLOAT, 0),
    C(R16_USCALED, NOP, V(UShort), 2, VKF_FLOAT, 0),
    C(R16_SSCALED, NOP, V(Short), 2, VKF_FLOAT, 0),
    C(R16_UINT, P(R16Uint), V(UShort), 2, VKF_UINT, 0),
    C(R16_SINT, P(R16Sint), V(Short), 2, VKF_SINT, 0),
    C(R16_SFLOAT, P(R16Float), V(Half), 2, VKF_FLOAT, 0),
    C(R16G16_UNORM, P(RG16Unorm), V(UShort2Normalized), 4, VKF_FLOAT, 0),
    C(R16G16_SNORM, P(RG16Snorm), V(Short2Normalized), 4, VKF_FLOAT, 0),
    C(R16G16_USCALED, NOP, V(UShort2), 4, VKF_FLOAT, 0),
    C(R16G16_SSCALED, NOP, V(Short2), 4, VKF_FLOAT, 0),
    C(R16G16_UINT, P(RG16Uint), V(UShort2), 4, VKF_UINT, 0),
    C(R16G16_SINT, P(RG16Sint), V(Short2), 4, VKF_SINT, 0),
    C(R16G16_SFLOAT, P(RG16Float), V(Half2), 4, VKF_FLOAT, 0),
    C(R16G16B16_UNORM, NOP, V(UShort3Normalized), 6, VKF_FLOAT, 0),
    C(R16G16B16_SNORM, NOP, V(Short3Normalized), 6, VKF_FLOAT, 0),
    C(R16G16B16_UINT, NOP, V(UShort3), 6, VKF_UINT, 0),
    C(R16G16B16_SINT, NOP, V(Short3), 6, VKF_SINT, 0),
    C(R16G16B16_SFLOAT, NOP, V(Half3), 6, VKF_FLOAT, 0),
    C(R16G16B16A16_UNORM, P(RGBA16Unorm), V(UShort4Normalized), 8, VKF_FLOAT, 0),
    C(R16G16B16A16_SNORM, P(RGBA16Snorm), V(Short4Normalized), 8, VKF_FLOAT, 0),
    C(R16G16B16A16_USCALED, NOP, V(UShort4), 8, VKF_FLOAT, 0),
    C(R16G16B16A16_SSCALED, NOP, V(Short4), 8, VKF_FLOAT, 0),
    C(R16G16B16A16_UINT, P(RGBA16Uint), V(UShort4), 8, VKF_UINT, 0),
    C(R16G16B16A16_SINT, P(RGBA16Sint), V(Short4), 8, VKF_SINT, 0),
    C(R16G16B16A16_SFLOAT, P(RGBA16Float), V(Half4), 8, VKF_FLOAT, 0),
    C(R32_UINT, P(R32Uint), V(UInt), 4, VKF_UINT, 0),
    C(R32_SINT, P(R32Sint), V(Int), 4, VKF_SINT, 0),
    C(R32_SFLOAT, P(R32Float), V(Float), 4, VKF_FLOAT, 0),
    C(R32G32_UINT, P(RG32Uint), V(UInt2), 8, VKF_UINT, 0),
    C(R32G32_SINT, P(RG32Sint), V(Int2), 8, VKF_SINT, 0),
    C(R32G32_SFLOAT, P(RG32Float), V(Float2), 8, VKF_FLOAT, 0),
    C(R32G32B32_UINT, NOP, V(UInt3), 12, VKF_UINT, 0),
    C(R32G32B32_SINT, NOP, V(Int3), 12, VKF_SINT, 0),
    C(R32G32B32_SFLOAT, NOP, V(Float3), 12, VKF_FLOAT, 0),
    C(R32G32B32A32_UINT, P(RGBA32Uint), V(UInt4), 16, VKF_UINT, 0),
    C(R32G32B32A32_SINT, P(RGBA32Sint), V(Int4), 16, VKF_SINT, 0),
    C(R32G32B32A32_SFLOAT, P(RGBA32Float), V(Float4), 16, VKF_FLOAT, 0),
    C(R64_UINT, P(RG32Uint), NOV, 8, VKF_UINT, 0),
    C(R64_SINT, P(RG32Sint), NOV, 8, VKF_SINT, 0),
    C(B10G11R11_UFLOAT_PACK32, P(RG11B10Float), V(FloatRG11B10), 4, VKF_FLOAT, 0),
    C(E5B9G9R9_UFLOAT_PACK32, P(RGB9E5Float), V(FloatRGB9E5), 4, VKF_FLOAT, 0),
    { VK_FORMAT_D16_UNORM, P(Depth16Unorm), NOV, 2, 1, 1, VKF_DEPTH, VKF_FLOAT, 0, 0, 0 },
    { VK_FORMAT_X8_D24_UNORM_PACK32, P(Depth32Float), NOV, 4, 1, 1, VKF_DEPTH, VKF_FLOAT, 0, 0, 1 },
    { VK_FORMAT_D32_SFLOAT, P(Depth32Float), NOV, 4, 1, 1, VKF_DEPTH, VKF_FLOAT, 0, 0, 0 },
    { VK_FORMAT_S8_UINT, P(Stencil8), NOV, 1, 1, 1, VKF_STENCIL, VKF_UINT, 0, 0, 0 },
    { VK_FORMAT_D24_UNORM_S8_UINT, P(Depth32Float_Stencil8), NOV, 4, 1, 1, VKF_DEPTH | VKF_STENCIL, VKF_FLOAT, 0, 0, 1 },
    { VK_FORMAT_D32_SFLOAT_S8_UINT, P(Depth32Float_Stencil8), NOV, 5, 1, 1, VKF_DEPTH | VKF_STENCIL, VKF_FLOAT, 0, 0, 0 },
    BC(BC1_RGB_UNORM_BLOCK, P(BC1_RGBA), 8, 0), BC(BC1_RGB_SRGB_BLOCK, P(BC1_RGBA_sRGB), 8, 1),
    BC(BC1_RGBA_UNORM_BLOCK, P(BC1_RGBA), 8, 0), BC(BC1_RGBA_SRGB_BLOCK, P(BC1_RGBA_sRGB), 8, 1),
    BC(BC2_UNORM_BLOCK, P(BC2_RGBA), 16, 0), BC(BC2_SRGB_BLOCK, P(BC2_RGBA_sRGB), 16, 1),
    BC(BC3_UNORM_BLOCK, P(BC3_RGBA), 16, 0), BC(BC3_SRGB_BLOCK, P(BC3_RGBA_sRGB), 16, 1),
    BC(BC4_UNORM_BLOCK, P(BC4_RUnorm), 8, 0), BC(BC4_SNORM_BLOCK, P(BC4_RSnorm), 8, 0),
    BC(BC5_UNORM_BLOCK, P(BC5_RGUnorm), 16, 0), BC(BC5_SNORM_BLOCK, P(BC5_RGSnorm), 16, 0),
    BC(BC6H_UFLOAT_BLOCK, P(BC6H_RGBUfloat), 16, 0), BC(BC6H_SFLOAT_BLOCK, P(BC6H_RGBFloat), 16, 0),
    BC(BC7_UNORM_BLOCK, P(BC7_RGBAUnorm), 16, 0), BC(BC7_SRGB_BLOCK, P(BC7_RGBAUnorm_sRGB), 16, 1),
};

const vk_format_info *vk_format(VkFormat format) {
    static const vk_format_info *index[256];
    static int built;
    if (!built) {
        for (size_t i = 0; i < sizeof g_formats / sizeof g_formats[0]; i++)
            if ((uint32_t)g_formats[i].vk < 256) index[g_formats[i].vk] = &g_formats[i];
        built = 1;
    }
    return (uint32_t)format < 256 ? index[format] : NULL;
}

// What a format can do here, from Metal's pixel format capabilities on Apple GPUs (family 7 and later).
VkFormatFeatureFlags vk_format_features(id<MTLDevice> mtl, const vk_format_info *f, int optimal, int buffer) {
    if (!f) return 0;
    VkFormatFeatureFlags r = 0;
    if (buffer) {
        if (f->vertex != MTLVertexFormatInvalid) r |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
        if (f->mtl != MTLPixelFormatInvalid && !f->compressed && (f->aspects & VKF_COLOR) && !f->srgb && f->bytes >= 1 &&
            f->vk != VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 && f->vk != VK_FORMAT_B10G11R11_UFLOAT_PACK32 && f->bytes != 3) {
            r |= VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
            if (f->bytes == 4 || f->bytes == 8 || f->bytes == 16 || f->bytes == 1 || f->bytes == 2)
                r |= VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
            if (f->vk == VK_FORMAT_R32_UINT || f->vk == VK_FORMAT_R32_SINT) r |= VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT;
        }
        return r;
    }
    if (f->mtl == MTLPixelFormatInvalid) return 0;
    if (f->compressed && !mtl.supportsBCTextureCompression) return 0;
    r = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    if (!optimal) {
        // (linear images: 2D, one level, sampled and copied, as Metal's textures over buffers allow)
        if (f->aspects != VKF_COLOR || f->compressed) return 0;
        return r | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    }
    int is32f = f->numeric == VKF_FLOAT && (f->vk == VK_FORMAT_R32_SFLOAT || f->vk == VK_FORMAT_R32G32_SFLOAT ||
                                            f->vk == VK_FORMAT_R32G32B32A32_SFLOAT);
    if (f->aspects & (VKF_DEPTH | VKF_STENCIL)) {
        r |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (f->aspects & VKF_DEPTH) r |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        r &= ~(VkFormatFeatureFlags)VK_FORMAT_FEATURE_BLIT_SRC_BIT;
        return r;
    }
    if (f->numeric == VKF_FLOAT && (!is32f || mtl.supports32BitFloatFiltering))
        r |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT;
    if (f->compressed || f->vk == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32) return r;
    r |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if (f->numeric == VKF_FLOAT) r |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
    if (!f->srgb && f->bytes != 3 && f->vk != VK_FORMAT_R5G6B5_UNORM_PACK16 && f->vk != VK_FORMAT_B5G6R5_UNORM_PACK16 &&
        f->bytes > 2 - (f->numeric != VKF_FLOAT) - 1 && f->vk != VK_FORMAT_R4G4B4A4_UNORM_PACK16 &&
        f->vk != VK_FORMAT_B4G4R4A4_UNORM_PACK16 && f->vk != VK_FORMAT_R5G5B5A1_UNORM_PACK16 &&
        f->vk != VK_FORMAT_B5G5R5A1_UNORM_PACK16 && f->vk != VK_FORMAT_A1R5G5B5_UNORM_PACK16 &&
        f->vk != VK_FORMAT_A2R10G10B10_UNORM_PACK32)
        r |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if (f->vk == VK_FORMAT_R32_UINT || f->vk == VK_FORMAT_R32_SINT) r |= VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
    return r;
}
