// Conservative Metal backend for the API-neutral compute pipeline and command stream.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "gpu_ir_metal.h"
#include "gpu_ir_msl.h"
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(GPU_IR_METAL_VERTEX_BUFFER_BASE + GPU_IR_MAX_VERTEX_BUFFERS == GPU_IR_METAL_BUFFER_RANGE_SLOT,
               "Metal vertex buffers must fit below the runtime range-table slot");

struct gpu_ir_metal_device {
    __strong id<MTLDevice> device;
    __strong id<MTLCommandQueue> queue;
    __strong NSMutableDictionary *clear_pipeline_cache;
    __strong id<MTLRenderPipelineState> present_pipeline;
    pthread_mutex_t clear_pipeline_lock;
    pthread_mutex_t present_pipeline_lock;
};

typedef struct {
    gpu_ir_metal_device *device;
    __strong id<MTLCommandBuffer> command_buffer;
    __strong id<MTLComputeCommandEncoder> compute_encoder;
    __strong id<MTLBlitCommandEncoder> blit_encoder;
    __strong id<MTLRenderCommandEncoder> render_encoder;
    __strong id<MTLTexture> render_target;
    MTLPixelFormat render_pixel_format;
    NSUInteger render_width, render_height;
    MTLScissorRect render_area_scissor;
    __strong NSMutableArray *upload_buffers;
    const gpu_ir_descriptor_state *descriptors;
} gpu_ir_metal_submission;

typedef struct {
    void *state;
    MTLPixelFormat pixel_format;
} gpu_ir_metal_graphics_pipeline;

static void release_metal_object(void *object) {
    if (object) CFRelease(object);
}

static void release_metal_graphics_pipeline(void *object) {
    gpu_ir_metal_graphics_pipeline *pipeline = object;
    if (!pipeline) return;
    if (pipeline->state) CFRelease(pipeline->state);
    free(pipeline);
}

static MTLPixelFormat metal_pixel_format(uint32_t format) {
    switch (format) {
        case GPU_IR_FORMAT_R8_UNORM: return MTLPixelFormatR8Unorm;
        case GPU_IR_FORMAT_RG8_UNORM: return MTLPixelFormatRG8Unorm;
        case GPU_IR_FORMAT_RGBA8_UNORM: return MTLPixelFormatRGBA8Unorm;
        case GPU_IR_FORMAT_BGRA8_UNORM: return MTLPixelFormatBGRA8Unorm;
        case GPU_IR_FORMAT_R16G16B16A16_FLOAT: return MTLPixelFormatRGBA16Float;
        case GPU_IR_FORMAT_R32_FLOAT: return MTLPixelFormatR32Float;
        case GPU_IR_FORMAT_R32_UINT: return MTLPixelFormatR32Uint;
        case GPU_IR_FORMAT_R32_SINT: return MTLPixelFormatR32Sint;
        default: return MTLPixelFormatInvalid;
    }
}

uint64_t gpu_ir_metal_linear_texture_alignment(gpu_ir_metal_device *device, uint32_t format) {
    MTLPixelFormat pixel_format = metal_pixel_format(format);
    if (!device || !device->device || pixel_format == MTLPixelFormatInvalid) return 0;
    NSUInteger alignment = [device->device minimumLinearTextureAlignmentForPixelFormat:pixel_format];
    return alignment ? (uint64_t)alignment : 0;
}

uint64_t gpu_ir_metal_linear_texture_row_pitch(gpu_ir_metal_device *device, uint32_t format, uint32_t width) {
    uint32_t bytes_per_pixel = 0;
    switch (format) {
        case GPU_IR_FORMAT_R8_UNORM: bytes_per_pixel = 1; break;
        case GPU_IR_FORMAT_RG8_UNORM: bytes_per_pixel = 2; break;
        case GPU_IR_FORMAT_RGBA8_UNORM:
        case GPU_IR_FORMAT_BGRA8_UNORM:
        case GPU_IR_FORMAT_R32_FLOAT:
        case GPU_IR_FORMAT_R32_UINT:
        case GPU_IR_FORMAT_R32_SINT: bytes_per_pixel = 4; break;
        case GPU_IR_FORMAT_R16G16B16A16_FLOAT: bytes_per_pixel = 8; break;
        default: return 0;
    }
    uint64_t alignment = gpu_ir_metal_linear_texture_alignment(device, format);
    uint64_t tight = (uint64_t)width * bytes_per_pixel;
    if (!width || !alignment || tight > UINT64_MAX - (alignment - 1)) return 0;
    uint64_t pitch = ((tight + alignment - 1) / alignment) * alignment;
    return pitch <= NSUIntegerMax ? pitch : 0;
}

static void copy_error(char *buffer, size_t capacity, NSString *message) {
    if (!buffer || !capacity) return;
    const char *utf8 = message.UTF8String;
    snprintf(buffer, capacity, "%s", utf8 ? utf8 : "Metal operation failed");
}

static gpu_ir_metal_device *metal_device_wrap(id<MTLDevice> metal_device) {
    if (!metal_device) return NULL;
    id<MTLCommandQueue> queue = [metal_device newCommandQueue];
    if (!queue) return NULL;
    gpu_ir_metal_device *device = calloc(1, sizeof *device);
    if (!device) return NULL;
    device->device = metal_device;
    device->queue = queue;
    device->clear_pipeline_cache = [NSMutableDictionary new];
    pthread_mutex_init(&device->clear_pipeline_lock, NULL);
    pthread_mutex_init(&device->present_pipeline_lock, NULL);
    return device;
}

gpu_ir_metal_device *gpu_ir_metal_device_create(void) {
    return metal_device_wrap(MTLCreateSystemDefaultDevice());
}

gpu_ir_metal_device *gpu_ir_metal_device_create_for_metal_device(void *metal_device) {
    return metal_device_wrap((__bridge id<MTLDevice>)metal_device);
}

void gpu_ir_metal_device_release(gpu_ir_metal_device *device) {
    if (!device) return;
    pthread_mutex_destroy(&device->present_pipeline_lock);
    pthread_mutex_destroy(&device->clear_pipeline_lock);
    device->present_pipeline = nil;
    device->clear_pipeline_cache = nil;
    device->queue = nil;
    device->device = nil;
    free(device);
}

void *gpu_ir_metal_native_device(const gpu_ir_metal_device *device) {
    return device && device->device ? (__bridge void *)device->device : NULL;
}

const char *gpu_ir_metal_device_name(const gpu_ir_metal_device *device) {
    return device && device->device ? device->device.name.UTF8String : NULL;
}

uint64_t gpu_ir_metal_max_buffer_length(const gpu_ir_metal_device *device) {
    return device && device->device ? (uint64_t)device->device.maxBufferLength : 0;
}

uint64_t gpu_ir_metal_max_threadgroup_memory_length(const gpu_ir_metal_device *device) {
    return device && device->device ? (uint64_t)device->device.maxThreadgroupMemoryLength : 0;
}

uint32_t gpu_ir_metal_max_fragment_input_components(const gpu_ir_metal_device *device) {
    if (!device || !device->device) return 0;
    if (@available(macOS 10.15, *)) {
        if ([device->device supportsFamily:MTLGPUFamilyApple4] ||
            [device->device supportsFamily:MTLGPUFamilyMac2]) return 124;
    }
    return 60;
}

uint64_t gpu_ir_metal_recommended_working_set_size(const gpu_ir_metal_device *device) {
    return device && device->device ? (uint64_t)device->device.recommendedMaxWorkingSetSize : 0;
}

int gpu_ir_metal_has_unified_memory(const gpu_ir_metal_device *device) {
    return device && device->device && device->device.hasUnifiedMemory;
}

void gpu_ir_metal_max_threads_per_threadgroup(const gpu_ir_metal_device *device, uint32_t size[3]) {
    if (!size) return;
    size[0] = size[1] = size[2] = 0;
    if (!device || !device->device) return;
    MTLSize maximum = device->device.maxThreadsPerThreadgroup;
    size[0] = (uint32_t)maximum.width;
    size[1] = (uint32_t)maximum.height;
    size[2] = (uint32_t)maximum.depth;
}

gpu_ir_resource *gpu_ir_metal_buffer_create(gpu_ir_metal_device *device, uint64_t size,
                                            const void *initial_data) {
    if (!device || !device->device || !size || size > NSUIntegerMax) return NULL;
    id<MTLBuffer> buffer = initial_data ?
        [device->device newBufferWithBytes:initial_data length:(NSUInteger)size options:MTLResourceStorageModeShared] :
        [device->device newBufferWithLength:(NSUInteger)size options:MTLResourceStorageModeShared];
    if (!buffer) return NULL;
    void *backend_object = (__bridge_retained void *)buffer;
    gpu_ir_resource *resource = gpu_ir_buffer_create(size, backend_object, release_metal_object);
    if (!resource) CFRelease(backend_object);
    return resource;
}

gpu_ir_resource *gpu_ir_metal_texture_create_from_buffer(gpu_ir_metal_device *device,
    gpu_ir_resource *buffer_resource, uint64_t offset, uint64_t row_pitch,
    const gpu_ir_texture_desc *description, int shader_read, int shader_write) {
    if (!device || !device->device || !gpu_ir_resource_is_buffer(buffer_resource) || !description ||
        description->format == GPU_IR_FORMAT_UNDEFINED || description->is_3d || description->depth != 1 ||
        description->array_layers != 1 || description->mip_levels != 1 || description->sample_count != 1 ||
        description->block_width != 1 || description->block_height != 1 ||
        description->bytes_per_block != description->bytes_per_texel ||
        offset > NSUIntegerMax || row_pitch > NSUIntegerMax) return NULL;
    MTLPixelFormat pixel_format = metal_pixel_format(description->format);
    uint64_t alignment = gpu_ir_metal_linear_texture_alignment(device, description->format);
    uint64_t minimum_pitch = gpu_ir_metal_linear_texture_row_pitch(device, description->format, description->width);
    if (pixel_format == MTLPixelFormatInvalid || !alignment || !minimum_pitch ||
        offset % alignment || row_pitch % alignment || row_pitch < minimum_pitch ||
        row_pitch > UINT64_MAX / description->height) return NULL;
    uint64_t image_pitch = row_pitch * description->height;
    uint64_t required_size = image_pitch;
    if (offset > gpu_ir_buffer_size(buffer_resource) ||
        required_size > gpu_ir_buffer_size(buffer_resource) - offset) return NULL;
    id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)gpu_ir_backend_object(buffer_resource);
    if (!buffer || buffer.storageMode != MTLStorageModeShared) return NULL;
    MTLTextureDescriptor *descriptor = [[MTLTextureDescriptor alloc] init];
    descriptor.textureType = MTLTextureType2D;
    descriptor.pixelFormat = pixel_format;
    descriptor.width = description->width;
    descriptor.height = description->height;
    descriptor.depth = 1;
    descriptor.arrayLength = 1;
    descriptor.mipmapLevelCount = 1;
    descriptor.sampleCount = 1;
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageRenderTarget |
                       (shader_read ? MTLTextureUsageShaderRead : MTLTextureUsageUnknown) |
                       (shader_write ? MTLTextureUsageShaderWrite : MTLTextureUsageUnknown);
    id<MTLTexture> texture = [buffer newTextureWithDescriptor:descriptor offset:(NSUInteger)offset
                                                  bytesPerRow:(NSUInteger)row_pitch];
    if (!texture) return NULL;
    void *backend_object = (__bridge_retained void *)texture;
    gpu_ir_resource *resource = gpu_ir_texture_create(description, backend_object, release_metal_object);
    if (!resource) CFRelease(backend_object);
    return resource;
}

int gpu_ir_metal_storage_texture_supported(gpu_ir_metal_device *device, uint32_t format) {
    if (!device || !device->device || !gpu_ir_metal_linear_texture_alignment(device, format)) return 0;
    MTLPixelFormat pixel_format = metal_pixel_format(format);
    uint64_t row_pitch = gpu_ir_metal_linear_texture_row_pitch(device, format, 1);
    if (pixel_format == MTLPixelFormatInvalid || !row_pitch || row_pitch > NSUIntegerMax) return 0;
    id<MTLBuffer> buffer = [device->device newBufferWithLength:(NSUInteger)row_pitch
        options:MTLResourceStorageModeShared];
    if (!buffer) return 0;
    MTLTextureDescriptor *descriptor = [[MTLTextureDescriptor alloc] init];
    descriptor.textureType = MTLTextureType2D;
    descriptor.pixelFormat = pixel_format;
    descriptor.width = 1;
    descriptor.height = 1;
    descriptor.depth = 1;
    descriptor.mipmapLevelCount = 1;
    descriptor.sampleCount = 1;
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> texture = [buffer newTextureWithDescriptor:descriptor offset:0 bytesPerRow:(NSUInteger)row_pitch];
    return texture != nil;
}

void *gpu_ir_metal_sampler_create(gpu_ir_metal_device *device,
                                  const gpu_ir_metal_sampler_desc *description) {
    if (!device || !device->device || !description) return NULL;
    MTLSamplerMinMagFilter min_filter, mag_filter;
    switch (description->min_filter) {
        case 0: min_filter = MTLSamplerMinMagFilterNearest; break;
        case 1: min_filter = MTLSamplerMinMagFilterLinear; break;
        default: return NULL;
    }
    switch (description->mag_filter) {
        case 0: mag_filter = MTLSamplerMinMagFilterNearest; break;
        case 1: mag_filter = MTLSamplerMinMagFilterLinear; break;
        default: return NULL;
    }
    MTLSamplerMipFilter mip_filter;
    switch (description->mip_filter) {
        case 0: mip_filter = MTLSamplerMipFilterNearest; break;
        case 1: mip_filter = MTLSamplerMipFilterLinear; break;
        default: return NULL;
    }
    MTLSamplerAddressMode address_modes[3];
    const uint32_t vulkan_address_modes[3] = {
        description->address_u, description->address_v, description->address_w
    };
    for (uint32_t i = 0; i < 3; i++) {
        switch (vulkan_address_modes[i]) {
            case 0: address_modes[i] = MTLSamplerAddressModeRepeat; break;
            case 1: address_modes[i] = MTLSamplerAddressModeMirrorRepeat; break;
            case 2: address_modes[i] = MTLSamplerAddressModeClampToEdge; break;
            case 3: address_modes[i] = MTLSamplerAddressModeClampToBorderColor; break;
            default: return NULL;
        }
    }
    MTLSamplerBorderColor border_color;
    switch (description->border_color) {
        case 0: border_color = MTLSamplerBorderColorTransparentBlack; break;
        case 2: border_color = MTLSamplerBorderColorOpaqueBlack; break;
        case 4: border_color = MTLSamplerBorderColorOpaqueWhite; break;
        default: return NULL;
    }
    if (!isfinite(description->lod_bias) || !isfinite(description->min_lod) ||
        !isfinite(description->max_lod) || description->min_lod < 0.0f ||
        description->max_lod < description->min_lod) return NULL;
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    if (!descriptor) return NULL;
    descriptor.minFilter = min_filter;
    descriptor.magFilter = mag_filter;
    descriptor.mipFilter = mip_filter;
    descriptor.sAddressMode = address_modes[0];
    descriptor.tAddressMode = address_modes[1];
    descriptor.rAddressMode = address_modes[2];
    descriptor.normalizedCoordinates = description->normalized_coordinates != 0;
    if (@available(macOS 26.0, *)) {
        descriptor.lodBias = description->lod_bias;
    } else if (description->lod_bias != 0.0f) {
        return NULL;
    }
    descriptor.lodMinClamp = description->min_lod;
    descriptor.lodMaxClamp = description->max_lod;
    // Metal stores this as an integer while Vulkan exposes a float. Round to the nearest
    // representable Metal sample count after the Vulkan frontend clamps it to the device limit.
    descriptor.maxAnisotropy = (NSUInteger)MAX(1.0, round((double)description->max_anisotropy));
    descriptor.borderColor = border_color;
    id<MTLSamplerState> sampler = [device->device newSamplerStateWithDescriptor:descriptor];
    return sampler ? (__bridge_retained void *)sampler : NULL;
}

uint32_t gpu_ir_metal_sampler_max_anisotropy(gpu_ir_metal_device *device) {
    if (!device || !device->device) return 1;
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    if (!descriptor) return 1;
    for (uint32_t value = GPU_IR_METAL_MAX_ANISOTROPY; value > 1; value--) {
        descriptor.maxAnisotropy = value;
        id<MTLSamplerState> sampler = [device->device newSamplerStateWithDescriptor:descriptor];
        if (sampler) return value;
    }
    return 1;
}

void gpu_ir_metal_sampler_retain(void *sampler) {
    if (sampler) CFRetain(sampler);
}

void gpu_ir_metal_sampler_release(void *sampler) {
    if (sampler) CFRelease(sampler);
}

static MTLTextureSwizzle metal_texture_swizzle(uint32_t selector, uint32_t output_component) {
    if (!selector) selector = output_component + 1u;
    switch (selector) {
        case GPU_IR_COMPONENT_SWIZZLE_R: return MTLTextureSwizzleRed;
        case GPU_IR_COMPONENT_SWIZZLE_G: return MTLTextureSwizzleGreen;
        case GPU_IR_COMPONENT_SWIZZLE_B: return MTLTextureSwizzleBlue;
        case GPU_IR_COMPONENT_SWIZZLE_A: return MTLTextureSwizzleAlpha;
        case GPU_IR_COMPONENT_SWIZZLE_ZERO: return MTLTextureSwizzleZero;
        case GPU_IR_COMPONENT_SWIZZLE_ONE: return MTLTextureSwizzleOne;
        default: return MTLTextureSwizzleZero;
    }
}

void *gpu_ir_metal_texture_view_with_component_mapping(void *metal_texture, uint32_t component_mapping) {
    if (!metal_texture || !component_mapping || (component_mapping & ~0xfffu)) return NULL;
    for (uint32_t component = 0; component < 4; component++)
        if (((component_mapping >> (component * 3u)) & 7u) > GPU_IR_COMPONENT_SWIZZLE_ONE) return NULL;

    id<MTLTexture> texture = (__bridge id<MTLTexture>)metal_texture;
    NSUInteger slice_count = 1;
    switch (texture.textureType) {
        case MTLTextureType1DArray:
        case MTLTextureType2DArray:
        case MTLTextureType2DMultisampleArray:
            slice_count = texture.arrayLength;
            break;
        case MTLTextureTypeCube:
            slice_count = 6;
            break;
        case MTLTextureTypeCubeArray:
            if (texture.arrayLength > NSUIntegerMax / 6u) return NULL;
            slice_count = texture.arrayLength * 6u;
            break;
        default:
            break;
    }
    if (!texture.mipmapLevelCount || !slice_count) return NULL;
    MTLTextureSwizzleChannels swizzle = MTLTextureSwizzleChannelsMake(
        metal_texture_swizzle((component_mapping >> 0u) & 7u, 0),
        metal_texture_swizzle((component_mapping >> 3u) & 7u, 1),
        metal_texture_swizzle((component_mapping >> 6u) & 7u, 2),
        metal_texture_swizzle((component_mapping >> 9u) & 7u, 3));
    id<MTLTexture> view = [texture newTextureViewWithPixelFormat:texture.pixelFormat
        textureType:texture.textureType levels:NSMakeRange(0, texture.mipmapLevelCount)
        slices:NSMakeRange(0, slice_count) swizzle:swizzle];
    return view ? (__bridge_retained void *)view : NULL;
}

void gpu_ir_metal_texture_view_release(void *metal_texture_view) {
    if (metal_texture_view) CFRelease(metal_texture_view);
}

void *gpu_ir_metal_buffer_contents(const gpu_ir_resource *resource) {
    if (!resource || !gpu_ir_resource_is_buffer(resource)) return NULL;
    id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)gpu_ir_backend_object(resource);
    return buffer.contents;
}

static gpu_ir_pipeline *compute_pipeline_create(id<MTLDevice> metal_device,
                                               const gpu_ir_shader *shader,
                                               gpu_ir_pipeline_layout *layout,
                                               char *error, size_t error_capacity) {
    if (error && error_capacity) error[0] = 0;
    if (!metal_device || !shader || gpu_ir_shader_stage(shader) != GPU_IR_STAGE_COMPUTE)
        return NULL;

    char *msl = NULL;
    size_t msl_size = 0;
    int emit_result = gpu_ir_msl_emit_compute_with_layout(shader, layout, &msl, &msl_size);
    if (emit_result != GPU_IR_OK) {
        if (error && error_capacity) snprintf(error, error_capacity, "common IR MSL emission failed (%d)", emit_result);
        return NULL;
    }
    NSString *source = [[NSString alloc] initWithBytes:msl length:msl_size encoding:NSUTF8StringEncoding];
    free(msl);
    if (!source) {
        copy_error(error, error_capacity, @"generated MSL is not UTF-8");
        return NULL;
    }

    NSError *metal_error = nil;
    MTLCompileOptions *compile_options = nil;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (instruction && instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
            instruction->operation == GPU_IR_SHADER_ARITH_FLOAT_MAD) {
            compile_options = [MTLCompileOptions new];
            if (@available(macOS 15.0, *)) compile_options.mathMode = MTLMathModeSafe;
            else compile_options.fastMathEnabled = NO;
            break;
        }
    }
    id<MTLLibrary> library = [metal_device newLibraryWithSource:source options:compile_options error:&metal_error];
    if (!library) {
        copy_error(error, error_capacity, metal_error.localizedDescription);
        return NULL;
    }
    id<MTLFunction> function = [library newFunctionWithName:@"vx_main"];
    if (!function) {
        copy_error(error, error_capacity, @"Metal library has no vx_main function");
        return NULL;
    }
    id<MTLComputePipelineState> state = [metal_device newComputePipelineStateWithFunction:function
                                                                                   error:&metal_error];
    if (!state) {
        copy_error(error, error_capacity, metal_error.localizedDescription);
        return NULL;
    }
    if ((uint64_t)state.staticThreadgroupMemoryLength > (uint64_t)metal_device.maxThreadgroupMemoryLength) {
        NSString *message = [NSString stringWithFormat:
            @"compute pipeline requires %lu bytes of threadgroup memory; Metal limit is %lu bytes",
            (unsigned long)state.staticThreadgroupMemoryLength,
            (unsigned long)metal_device.maxThreadgroupMemoryLength];
        copy_error(error, error_capacity, message);
        return NULL;
    }

    uint32_t threadgroup_size[3];
    gpu_ir_shader_threadgroup_size(shader, threadgroup_size);
    void *backend_object = (__bridge_retained void *)state;
    gpu_ir_pipeline_desc description = {
        .info = { .kind = GPU_IR_PIPELINE_COMPUTE, .stage_mask = GPU_IR_STAGE_COMPUTE,
                  .threadgroup_size = { threadgroup_size[0], threadgroup_size[1], threadgroup_size[2] } },
        .layout = layout,
        .backend_object = backend_object,
        .release = release_metal_object,
        .stage_shaders = { [GPU_IR_STAGE_SLOT_COMPUTE] = (gpu_ir_shader *)shader }
    };
    gpu_ir_pipeline *pipeline = gpu_ir_pipeline_create(&description);
    if (!pipeline) {
        CFRelease(backend_object);
        copy_error(error, error_capacity, @"common IR rejected the Metal compute pipeline");
    }
    return pipeline;
}

gpu_ir_pipeline *gpu_ir_metal_compute_pipeline_create(gpu_ir_metal_device *device,
                                                      const gpu_ir_shader *shader,
                                                      gpu_ir_pipeline_layout *layout,
                                                      char *error, size_t error_capacity) {
    if (!device) {
        if (error && error_capacity) snprintf(error, error_capacity, "Metal device is unavailable");
        return NULL;
    }
    return compute_pipeline_create(device->device, shader, layout, error, error_capacity);
}

gpu_ir_pipeline *gpu_ir_metal_compute_pipeline_create_for_metal_device(void *metal_device,
                                                                       const gpu_ir_shader *shader,
                                                                       gpu_ir_pipeline_layout *layout,
                                                                       char *error, size_t error_capacity) {
    id<MTLDevice> device = (__bridge id<MTLDevice>)metal_device;
    return compute_pipeline_create(device, shader, layout, error, error_capacity);
}

static MTLVertexFormat metal_vertex_format(uint32_t format) {
    switch (format) {
        case GPU_IR_VERTEX_FORMAT_FLOAT32: return MTLVertexFormatFloat;
        case GPU_IR_VERTEX_FORMAT_FLOAT32X2: return MTLVertexFormatFloat2;
        case GPU_IR_VERTEX_FORMAT_FLOAT32X3: return MTLVertexFormatFloat3;
        case GPU_IR_VERTEX_FORMAT_FLOAT32X4: return MTLVertexFormatFloat4;
        case GPU_IR_VERTEX_FORMAT_SINT32: return MTLVertexFormatInt;
        case GPU_IR_VERTEX_FORMAT_SINT32X2: return MTLVertexFormatInt2;
        case GPU_IR_VERTEX_FORMAT_SINT32X3: return MTLVertexFormatInt3;
        case GPU_IR_VERTEX_FORMAT_SINT32X4: return MTLVertexFormatInt4;
        case GPU_IR_VERTEX_FORMAT_UINT32: return MTLVertexFormatUInt;
        case GPU_IR_VERTEX_FORMAT_UINT32X2: return MTLVertexFormatUInt2;
        case GPU_IR_VERTEX_FORMAT_UINT32X3: return MTLVertexFormatUInt3;
        case GPU_IR_VERTEX_FORMAT_UINT32X4: return MTLVertexFormatUInt4;
        default: break;
    }
#define METAL_VERTEX_RANGE(base, ...) \
    if (format >= (base) && format <= (base) + 3) { \
        static const MTLVertexFormat formats[] = { __VA_ARGS__ }; \
        return formats[format - (base)]; \
    }
    METAL_VERTEX_RANGE(GPU_IR_VERTEX_FORMAT_FLOAT16,
                       MTLVertexFormatHalf, MTLVertexFormatHalf2, MTLVertexFormatHalf3, MTLVertexFormatHalf4)
    METAL_VERTEX_RANGE(GPU_IR_VERTEX_FORMAT_UNORM8,
                       MTLVertexFormatUCharNormalized, MTLVertexFormatUChar2Normalized,
                       MTLVertexFormatUChar3Normalized, MTLVertexFormatUChar4Normalized)
    METAL_VERTEX_RANGE(GPU_IR_VERTEX_FORMAT_SNORM8,
                       MTLVertexFormatCharNormalized, MTLVertexFormatChar2Normalized,
                       MTLVertexFormatChar3Normalized, MTLVertexFormatChar4Normalized)
    METAL_VERTEX_RANGE(GPU_IR_VERTEX_FORMAT_UINT8,
                       MTLVertexFormatUChar, MTLVertexFormatUChar2, MTLVertexFormatUChar3, MTLVertexFormatUChar4)
    METAL_VERTEX_RANGE(GPU_IR_VERTEX_FORMAT_SINT8,
                       MTLVertexFormatChar, MTLVertexFormatChar2, MTLVertexFormatChar3, MTLVertexFormatChar4)
    METAL_VERTEX_RANGE(GPU_IR_VERTEX_FORMAT_UNORM16,
                       MTLVertexFormatUShortNormalized, MTLVertexFormatUShort2Normalized,
                       MTLVertexFormatUShort3Normalized, MTLVertexFormatUShort4Normalized)
    METAL_VERTEX_RANGE(GPU_IR_VERTEX_FORMAT_SNORM16,
                       MTLVertexFormatShortNormalized, MTLVertexFormatShort2Normalized,
                       MTLVertexFormatShort3Normalized, MTLVertexFormatShort4Normalized)
    METAL_VERTEX_RANGE(GPU_IR_VERTEX_FORMAT_UINT16,
                       MTLVertexFormatUShort, MTLVertexFormatUShort2, MTLVertexFormatUShort3, MTLVertexFormatUShort4)
    METAL_VERTEX_RANGE(GPU_IR_VERTEX_FORMAT_SINT16,
                       MTLVertexFormatShort, MTLVertexFormatShort2, MTLVertexFormatShort3, MTLVertexFormatShort4)
#undef METAL_VERTEX_RANGE
    return MTLVertexFormatInvalid;
}

static int metal_vertex_input_valid(const gpu_ir_vertex_input_desc *input) {
    if (!input) return 1;
    if (input->binding_count > GPU_IR_MAX_VERTEX_BUFFERS ||
        input->attribute_count > GPU_IR_MAX_VERTEX_ATTRIBUTES) return 0;
    for (uint32_t i = 0; i < input->binding_count; i++) {
        const gpu_ir_vertex_binding_desc *binding = &input->bindings[i];
        if (binding->binding >= GPU_IR_MAX_VERTEX_BUFFERS || !binding->step_rate ||
            (binding->input_rate != GPU_IR_VERTEX_INPUT_RATE_VERTEX &&
             binding->input_rate != GPU_IR_VERTEX_INPUT_RATE_INSTANCE)) return 0;
        for (uint32_t previous = 0; previous < i; previous++)
            if (input->bindings[previous].binding == binding->binding) return 0;
    }
    for (uint32_t i = 0; i < input->attribute_count; i++) {
        const gpu_ir_vertex_attribute_desc *attribute = &input->attributes[i];
        int has_binding = 0;
        if (attribute->location >= 31 || attribute->binding >= GPU_IR_MAX_VERTEX_BUFFERS ||
            metal_vertex_format(attribute->format) == MTLVertexFormatInvalid) return 0;
        for (uint32_t binding = 0; binding < input->binding_count; binding++)
            if (input->bindings[binding].binding == attribute->binding) { has_binding = 1; break; }
        if (!has_binding) return 0;
        for (uint32_t previous = 0; previous < i; previous++)
            if (input->attributes[previous].location == attribute->location) return 0;
    }
    return 1;
}

static MTLBlendFactor metal_blend_factor(uint32_t factor) {
    switch (factor) {
        case GPU_IR_BLEND_FACTOR_ZERO: return MTLBlendFactorZero;
        case GPU_IR_BLEND_FACTOR_ONE: return MTLBlendFactorOne;
        case GPU_IR_BLEND_FACTOR_SOURCE_COLOR: return MTLBlendFactorSourceColor;
        case GPU_IR_BLEND_FACTOR_ONE_MINUS_SOURCE_COLOR: return MTLBlendFactorOneMinusSourceColor;
        case GPU_IR_BLEND_FACTOR_DESTINATION_COLOR: return MTLBlendFactorDestinationColor;
        case GPU_IR_BLEND_FACTOR_ONE_MINUS_DESTINATION_COLOR: return MTLBlendFactorOneMinusDestinationColor;
        case GPU_IR_BLEND_FACTOR_SOURCE_ALPHA: return MTLBlendFactorSourceAlpha;
        case GPU_IR_BLEND_FACTOR_ONE_MINUS_SOURCE_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
        case GPU_IR_BLEND_FACTOR_DESTINATION_ALPHA: return MTLBlendFactorDestinationAlpha;
        case GPU_IR_BLEND_FACTOR_ONE_MINUS_DESTINATION_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
        case GPU_IR_BLEND_FACTOR_CONSTANT_COLOR: return MTLBlendFactorBlendColor;
        case GPU_IR_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return MTLBlendFactorOneMinusBlendColor;
        case GPU_IR_BLEND_FACTOR_CONSTANT_ALPHA: return MTLBlendFactorBlendAlpha;
        case GPU_IR_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: return MTLBlendFactorOneMinusBlendAlpha;
        case GPU_IR_BLEND_FACTOR_SOURCE_ALPHA_SATURATE: return MTLBlendFactorSourceAlphaSaturated;
        case GPU_IR_BLEND_FACTOR_SOURCE1_COLOR: return MTLBlendFactorSource1Color;
        case GPU_IR_BLEND_FACTOR_ONE_MINUS_SOURCE1_COLOR: return MTLBlendFactorOneMinusSource1Color;
        case GPU_IR_BLEND_FACTOR_SOURCE1_ALPHA: return MTLBlendFactorSource1Alpha;
        case GPU_IR_BLEND_FACTOR_ONE_MINUS_SOURCE1_ALPHA: return MTLBlendFactorOneMinusSource1Alpha;
        default: return MTLBlendFactorZero;
    }
}

static MTLBlendOperation metal_blend_operation(uint32_t operation) {
    switch (operation) {
        case GPU_IR_BLEND_OPERATION_ADD: return MTLBlendOperationAdd;
        case GPU_IR_BLEND_OPERATION_SUBTRACT: return MTLBlendOperationSubtract;
        case GPU_IR_BLEND_OPERATION_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
        case GPU_IR_BLEND_OPERATION_MINIMUM: return MTLBlendOperationMin;
        case GPU_IR_BLEND_OPERATION_MAXIMUM: return MTLBlendOperationMax;
        default: return MTLBlendOperationAdd;
    }
}

static MTLColorWriteMask metal_color_write_mask(uint32_t mask) {
    MTLColorWriteMask result = 0;
    if (mask & GPU_IR_COLOR_WRITE_RED) result |= MTLColorWriteMaskRed;
    if (mask & GPU_IR_COLOR_WRITE_GREEN) result |= MTLColorWriteMaskGreen;
    if (mask & GPU_IR_COLOR_WRITE_BLUE) result |= MTLColorWriteMaskBlue;
    if (mask & GPU_IR_COLOR_WRITE_ALPHA) result |= MTLColorWriteMaskAlpha;
    return result;
}

static void metal_set_blend_constants(id<MTLRenderCommandEncoder> encoder,
                                      const gpu_ir_pipeline_info *pipeline_info,
                                      uint32_t has_draw_constants, const float draw_constants[4]) {
    if (!encoder || !pipeline_info) return;
    if (!has_draw_constants && !pipeline_info->color_blend.attachment_count) return;
    const float *constants = has_draw_constants ? draw_constants : pipeline_info->color_blend.blend_constants;
    if (!constants) return;
    [encoder setBlendColorRed:constants[0] green:constants[1] blue:constants[2] alpha:constants[3]];
}

gpu_ir_pipeline *gpu_ir_metal_graphics_pipeline_create_with_blend(gpu_ir_metal_device *device,
    const char *vertex_source, const char *fragment_source, uint32_t color_format,
    const gpu_ir_vertex_input_desc *vertex_input, const gpu_ir_color_blend_state *color_blend,
    gpu_ir_pipeline_layout *layout,
    char *error, size_t error_capacity) {
    if (error && error_capacity) error[0] = 0;
    if (!device || !device->device || !vertex_source || !*vertex_source || !fragment_source || !*fragment_source) {
        if (error && error_capacity) snprintf(error, error_capacity, "graphics pipeline needs a device and both MSL stages");
        return NULL;
    }
    if (!metal_vertex_input_valid(vertex_input)) {
        if (error && error_capacity) snprintf(error, error_capacity, "invalid normalized vertex input layout");
        return NULL;
    }
    if (!gpu_ir_color_blend_state_valid(color_blend) ||
        (color_blend && color_blend->attachment_count > 1)) {
        if (error && error_capacity) snprintf(error, error_capacity, "invalid normalized color blend state");
        return NULL;
    }
    MTLPixelFormat pixel_format = metal_pixel_format(color_format);
    if (pixel_format == MTLPixelFormatInvalid) {
        if (error && error_capacity) snprintf(error, error_capacity, "unsupported common color format %u", color_format);
        return NULL;
    }
    NSString *vertex = [[NSString alloc] initWithUTF8String:vertex_source];
    NSString *fragment = [[NSString alloc] initWithUTF8String:fragment_source];
    if (!vertex || !fragment) {
        if (error && error_capacity) snprintf(error, error_capacity, "graphics stage source is not valid UTF-8");
        return NULL;
    }
    NSString *source = [NSString stringWithFormat:
        @"#include <metal_stdlib>\nusing namespace metal;\n%@\n%@\n", vertex, fragment];
    NSError *metal_error = nil;
    id<MTLLibrary> library = [device->device newLibraryWithSource:source options:nil error:&metal_error];
    if (!library) {
        copy_error(error, error_capacity, metal_error.localizedDescription);
        return NULL;
    }
    id<MTLFunction> vertex_function = [library newFunctionWithName:@"vx_vertex"];
    id<MTLFunction> fragment_function = [library newFunctionWithName:@"vx_fragment"];
    if (!vertex_function || !fragment_function) {
        if (error && error_capacity) snprintf(error, error_capacity, "graphics MSL must define vx_vertex and vx_fragment");
        return NULL;
    }
    MTLRenderPipelineDescriptor *description = [MTLRenderPipelineDescriptor new];
    description.vertexFunction = vertex_function;
    description.fragmentFunction = fragment_function;
    description.rasterSampleCount = 1;
    description.alphaToCoverageEnabled = color_blend && color_blend->alpha_to_coverage_enabled;
    description.colorAttachments[0].pixelFormat = pixel_format;
    const gpu_ir_color_blend_attachment *blend = color_blend && color_blend->attachment_count ?
        &color_blend->attachments[0] : NULL;
    description.colorAttachments[0].writeMask = blend ? metal_color_write_mask(blend->write_mask) : MTLColorWriteMaskAll;
    if (blend) {
        MTLRenderPipelineColorAttachmentDescriptor *attachment = description.colorAttachments[0];
        attachment.blendingEnabled = blend->enabled != 0;
        attachment.sourceRGBBlendFactor = metal_blend_factor(blend->source_color_factor);
        attachment.destinationRGBBlendFactor = metal_blend_factor(blend->destination_color_factor);
        attachment.rgbBlendOperation = metal_blend_operation(blend->color_operation);
        attachment.sourceAlphaBlendFactor = metal_blend_factor(blend->source_alpha_factor);
        attachment.destinationAlphaBlendFactor = metal_blend_factor(blend->destination_alpha_factor);
        attachment.alphaBlendOperation = metal_blend_operation(blend->alpha_operation);
    }
    if (vertex_input && vertex_input->attribute_count) {
        MTLVertexDescriptor *vertex_descriptor = [MTLVertexDescriptor vertexDescriptor];
        for (uint32_t i = 0; i < vertex_input->binding_count; i++) {
            const gpu_ir_vertex_binding_desc *binding = &vertex_input->bindings[i];
            NSUInteger buffer_index = GPU_IR_METAL_VERTEX_BUFFER_BASE + binding->binding;
            MTLVertexBufferLayoutDescriptor *layout = vertex_descriptor.layouts[buffer_index];
            layout.stride = binding->stride;
            layout.stepFunction = binding->input_rate == GPU_IR_VERTEX_INPUT_RATE_INSTANCE ?
                MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
            layout.stepRate = binding->step_rate;
        }
        for (uint32_t i = 0; i < vertex_input->attribute_count; i++) {
            const gpu_ir_vertex_attribute_desc *attribute = &vertex_input->attributes[i];
            MTLVertexAttributeDescriptor *layout = vertex_descriptor.attributes[attribute->location];
            layout.format = metal_vertex_format(attribute->format);
            layout.offset = attribute->offset;
            layout.bufferIndex = GPU_IR_METAL_VERTEX_BUFFER_BASE + attribute->binding;
        }
        description.vertexDescriptor = vertex_descriptor;
    }
    id<MTLRenderPipelineState> state = [device->device newRenderPipelineStateWithDescriptor:description
                                                                                       error:&metal_error];
    if (!state) {
        copy_error(error, error_capacity, metal_error.localizedDescription);
        return NULL;
    }
    gpu_ir_metal_graphics_pipeline *backend = calloc(1, sizeof *backend);
    if (!backend) {
        if (error && error_capacity) snprintf(error, error_capacity, "out of memory retaining graphics pipeline");
        return NULL;
    }
    backend->state = (__bridge_retained void *)state;
    backend->pixel_format = pixel_format;
    gpu_ir_pipeline_desc pipeline_description = {
        .info = { .kind = GPU_IR_PIPELINE_GRAPHICS,
                  .stage_mask = GPU_IR_STAGE_VERTEX | GPU_IR_STAGE_FRAGMENT,
                  .vertex_input = vertex_input ? *vertex_input : (gpu_ir_vertex_input_desc){ 0 },
                  .color_blend = color_blend ? *color_blend : (gpu_ir_color_blend_state){ 0 } },
        .layout = layout,
        .backend_object = backend,
        .release = release_metal_graphics_pipeline
    };
    gpu_ir_pipeline *pipeline = gpu_ir_pipeline_create(&pipeline_description);
    if (!pipeline) {
        release_metal_graphics_pipeline(backend);
        if (error && error_capacity) snprintf(error, error_capacity, "common IR rejected the Metal graphics pipeline");
    }
    return pipeline;
}

gpu_ir_pipeline *gpu_ir_metal_graphics_pipeline_create_with_layout(gpu_ir_metal_device *device,
    const char *vertex_source, const char *fragment_source, uint32_t color_format,
    const gpu_ir_vertex_input_desc *vertex_input, gpu_ir_pipeline_layout *layout,
    char *error, size_t error_capacity) {
    return gpu_ir_metal_graphics_pipeline_create_with_blend(device, vertex_source, fragment_source,
        color_format, vertex_input, NULL, layout, error, error_capacity);
}

gpu_ir_pipeline *gpu_ir_metal_graphics_pipeline_create_with_vertex_input(gpu_ir_metal_device *device,
    const char *vertex_source, const char *fragment_source, uint32_t color_format,
    const gpu_ir_vertex_input_desc *vertex_input, char *error, size_t error_capacity) {
    return gpu_ir_metal_graphics_pipeline_create_with_layout(device, vertex_source, fragment_source,
        color_format, vertex_input, NULL, error, error_capacity);
}

gpu_ir_pipeline *gpu_ir_metal_graphics_pipeline_create(gpu_ir_metal_device *device,
    const char *vertex_source, const char *fragment_source, uint32_t color_format,
    char *error, size_t error_capacity) {
    return gpu_ir_metal_graphics_pipeline_create_with_vertex_input(device, vertex_source, fragment_source,
        color_format, NULL, error, error_capacity);
}

static void end_compute_encoder(gpu_ir_metal_submission *submission) {
    if (!submission || !submission->compute_encoder) return;
    [submission->compute_encoder endEncoding];
    submission->compute_encoder = nil;
}

static void end_render_encoder(gpu_ir_metal_submission *submission) {
    if (!submission || !submission->render_encoder) return;
    [submission->render_encoder endEncoding];
    submission->render_encoder = nil;
    submission->render_target = nil;
    submission->render_pixel_format = MTLPixelFormatInvalid;
    submission->render_width = submission->render_height = 0;
    submission->render_area_scissor = (MTLScissorRect){ 0, 0, 0, 0 };
}

static void end_blit_encoder(gpu_ir_metal_submission *submission) {
    if (!submission || !submission->blit_encoder) return;
    [submission->blit_encoder endEncoding];
    submission->blit_encoder = nil;
}

static int encode_descriptor_state(gpu_ir_metal_submission *submission,
                                   const gpu_ir_descriptor_state *state);

static int ensure_compute_encoder(gpu_ir_metal_submission *submission) {
    if (submission && submission->render_encoder) return GPU_IR_INVALID;
    if (submission->compute_encoder) return GPU_IR_OK;
    end_blit_encoder(submission);
    submission->compute_encoder = [submission->command_buffer
        computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
    if (!submission->compute_encoder) return GPU_IR_UNSUPPORTED;
    if (submission->descriptors) {
        int rc = encode_descriptor_state(submission, submission->descriptors);
        if (rc != GPU_IR_OK) {
            end_compute_encoder(submission);
            return rc;
        }
    }
    return GPU_IR_OK;
}

static int ensure_blit_encoder(gpu_ir_metal_submission *submission) {
    if (submission && submission->render_encoder) return GPU_IR_INVALID;
    if (submission->blit_encoder) return GPU_IR_OK;
    end_compute_encoder(submission);
    submission->blit_encoder = [submission->command_buffer blitCommandEncoder];
    return submission->blit_encoder ? GPU_IR_OK : GPU_IR_UNSUPPORTED;
}

static MTLResourceUsage metal_resource_usage(uint32_t access_mask) {
    MTLResourceUsage usage = 0;
    if (access_mask & (GPU_IR_ACCESS_UNIFORM_READ | GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_COPY_READ))
        usage |= MTLResourceUsageRead;
    if (access_mask & (GPU_IR_ACCESS_SHADER_WRITE | GPU_IR_ACCESS_COPY_WRITE))
        usage |= MTLResourceUsageWrite;
    return usage;
}

static int metal_clear_color(void *context, const gpu_ir_resource *resource,
                             const gpu_ir_clear_color_info *clear);

static int metal_begin_render_pass(void *context, const gpu_ir_resource *color_attachment,
                                   const gpu_ir_render_pass_desc *description) {
    gpu_ir_metal_submission *submission = context;
    const gpu_ir_texture_desc *resource_description = gpu_ir_texture_description(color_attachment);
    id<MTLTexture> texture = (__bridge id<MTLTexture>)gpu_ir_backend_object(color_attachment);
    if (!submission || !submission->command_buffer || submission->render_encoder || !resource_description ||
        !description || !texture || resource_description->is_3d || resource_description->sample_count != 1 ||
        description->mip_level >= resource_description->mip_levels ||
        description->array_layer >= resource_description->array_layers ||
        texture.mipmapLevelCount <= description->mip_level || texture.arrayLength <= description->array_layer ||
        !(texture.usage & MTLTextureUsageRenderTarget) ||
        texture.pixelFormat != metal_pixel_format(resource_description->format)) return GPU_IR_INVALID;
    NSUInteger width = texture.width, height = texture.height;
    for (uint32_t i = 0; i < description->mip_level; i++) {
        if (width > 1) width /= 2;
        if (height > 1) height /= 2;
    }
    if (description->render_area.left < 0 || description->render_area.top < 0 ||
        description->render_area.right <= description->render_area.left ||
        description->render_area.bottom <= description->render_area.top ||
        (NSUInteger)description->render_area.right > width ||
        (NSUInteger)description->render_area.bottom > height) return GPU_IR_RANGE;
    end_compute_encoder(submission);
    end_blit_encoder(submission);
    if (description->load_action == GPU_IR_RENDER_PASS_CLEAR) {
        if (description->mip_level) return GPU_IR_UNSUPPORTED;
        gpu_ir_clear_color_info clear = {
            .value_type = description->clear_value_type,
            .base_array_layer = description->array_layer,
            .array_layer_count = 1,
            .rectangles = &description->render_area,
            .rectangle_count = 1
        };
        if (clear.value_type == GPU_IR_CLEAR_COLOR_VALUE_UINT)
            memcpy(clear.color_uint, description->clear_value.color_uint, sizeof clear.color_uint);
        else if (clear.value_type == GPU_IR_CLEAR_COLOR_VALUE_SINT)
            memcpy(clear.color_sint, description->clear_value.color_sint, sizeof clear.color_sint);
        else memcpy(clear.color, description->clear_value.color, sizeof clear.color);
        int clear_result = metal_clear_color(submission, color_attachment, &clear);
        if (clear_result != GPU_IR_OK) return clear_result;
    }
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].level = description->mip_level;
    pass.colorAttachments[0].slice = description->array_layer;
    // Vulkan load/store effects are confined to the render area. Metal's native pass actions apply to
    // the whole attachment, so preserve out-of-area pixels and do CLEAR as a separately scissored draw.
    // Keeping values for DONT_CARE is valid; discarding the entire Metal attachment would affect pixels
    // outside the Vulkan render area, whose contents must remain unchanged.
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [submission->command_buffer renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return GPU_IR_UNSUPPORTED;
    [encoder setViewport:(MTLViewport){ 0, 0, (double)width, (double)height, 0, 1 }];
    [encoder setScissorRect:(MTLScissorRect){
        (NSUInteger)description->render_area.left,
        (NSUInteger)description->render_area.top,
        (NSUInteger)(description->render_area.right - description->render_area.left),
        (NSUInteger)(description->render_area.bottom - description->render_area.top)
    }];
    [encoder setCullMode:MTLCullModeNone];
    submission->render_encoder = encoder;
    submission->render_target = texture;
    submission->render_pixel_format = texture.pixelFormat;
    submission->render_width = width;
    submission->render_height = height;
    submission->render_area_scissor = (MTLScissorRect){
        (NSUInteger)description->render_area.left,
        (NSUInteger)description->render_area.top,
        (NSUInteger)(description->render_area.right - description->render_area.left),
        (NSUInteger)(description->render_area.bottom - description->render_area.top)
    };
    return GPU_IR_OK;
}

static int metal_end_render_pass(void *context) {
    gpu_ir_metal_submission *submission = context;
    if (!submission || !submission->render_encoder) return GPU_IR_INVALID;
    end_render_encoder(submission);
    return GPU_IR_OK;
}

static int metal_draw(void *context, const gpu_ir_pipeline *pipeline, const gpu_ir_draw_info *draw,
                      const gpu_ir_resource_access *accesses, uint32_t access_count) {
    gpu_ir_metal_submission *submission = context;
    const gpu_ir_pipeline_info *info = gpu_ir_pipeline_description(pipeline);
    gpu_ir_metal_graphics_pipeline *backend = gpu_ir_pipeline_backend_object(pipeline);
    if (!submission || !submission->render_encoder || !info || info->kind != GPU_IR_PIPELINE_GRAPHICS ||
        info->stage_mask != (GPU_IR_STAGE_VERTEX | GPU_IR_STAGE_FRAGMENT) || !backend || !backend->state ||
        backend->pixel_format != submission->render_pixel_format || !draw ||
        (access_count && !accesses)) return GPU_IR_INVALID;
    if ((!draw->indexed && (!draw->vertex_count || !draw->instance_count)) ||
        (draw->indexed && (!draw->index_count || !draw->instance_count))) return GPU_IR_OK;
    MTLPrimitiveType primitive;
    switch (draw->topology) {
        case GPU_IR_TOPOLOGY_POINT_LIST: primitive = MTLPrimitiveTypePoint; break;
        case GPU_IR_TOPOLOGY_LINE_LIST: primitive = MTLPrimitiveTypeLine; break;
        case GPU_IR_TOPOLOGY_LINE_STRIP: primitive = MTLPrimitiveTypeLineStrip; break;
        case GPU_IR_TOPOLOGY_TRIANGLE_LIST: primitive = MTLPrimitiveTypeTriangle; break;
        case GPU_IR_TOPOLOGY_TRIANGLE_STRIP: primitive = MTLPrimitiveTypeTriangleStrip; break;
        default: return GPU_IR_UNSUPPORTED;
    }
    MTLViewport viewport = draw->has_viewport ? (MTLViewport){
        draw->viewport.x, draw->viewport.y, draw->viewport.width, draw->viewport.height,
        draw->viewport.min_depth, draw->viewport.max_depth
    } : (MTLViewport){ 0, 0, (double)submission->render_width, (double)submission->render_height, 0, 1 };
    MTLScissorRect scissor = submission->render_area_scissor;
    if (draw->has_scissor) {
        int64_t x0 = draw->scissor.x > (int64_t)scissor.x ? draw->scissor.x : (int64_t)scissor.x;
        int64_t y0 = draw->scissor.y > (int64_t)scissor.y ? draw->scissor.y : (int64_t)scissor.y;
        int64_t draw_x1 = (int64_t)draw->scissor.x + draw->scissor.width;
        int64_t draw_y1 = (int64_t)draw->scissor.y + draw->scissor.height;
        int64_t pass_x1 = (int64_t)scissor.x + scissor.width;
        int64_t pass_y1 = (int64_t)scissor.y + scissor.height;
        int64_t x1 = draw_x1 < pass_x1 ? draw_x1 : pass_x1;
        int64_t y1 = draw_y1 < pass_y1 ? draw_y1 : pass_y1;
        if (x0 >= x1 || y0 >= y1) return GPU_IR_OK;
        scissor = (MTLScissorRect){ (NSUInteger)x0, (NSUInteger)y0,
                                    (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0) };
    }
    id<MTLBuffer> index_buffer = nil;
    MTLIndexType index_type = MTLIndexTypeUInt16;
    NSUInteger index_buffer_offset = 0;
    if (draw->indexed) {
        uint64_t index_size = draw->index_type == GPU_IR_INDEX_TYPE_UINT16 ? 2u :
            draw->index_type == GPU_IR_INDEX_TYPE_UINT32 ? 4u : 0u;
        uint64_t first_offset = (uint64_t)draw->first_index * index_size;
        if (!index_size || !draw->index_buffer || !draw->index_buffer_size ||
            draw->index_buffer_offset > gpu_ir_buffer_size(draw->index_buffer) ||
            draw->index_buffer_size > gpu_ir_buffer_size(draw->index_buffer) - draw->index_buffer_offset ||
            first_offset > UINT64_MAX - draw->index_buffer_offset ||
            first_offset > draw->index_buffer_size ||
            draw->index_count > UINT64_MAX / index_size ||
            (uint64_t)draw->index_count * index_size > draw->index_buffer_size - first_offset ||
            draw->index_buffer_offset + first_offset > NSUIntegerMax)
            return GPU_IR_INVALID;
        index_buffer = (__bridge id<MTLBuffer>)gpu_ir_backend_object(draw->index_buffer);
        if (!index_buffer || draw->index_buffer_offset + draw->index_buffer_size > index_buffer.length)
            return GPU_IR_RANGE;
        index_type = draw->index_type == GPU_IR_INDEX_TYPE_UINT16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
        index_buffer_offset = (NSUInteger)(draw->index_buffer_offset + first_offset);
        [submission->render_encoder useResource:index_buffer usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
    }
    for (uint32_t i = 0; i < access_count; i++) {
        id<MTLResource> resource = (__bridge id<MTLResource>)gpu_ir_backend_object(accesses[i].resource);
        if (!resource) return GPU_IR_INVALID;
        MTLResourceUsage usage = metal_resource_usage(accesses[i].access_mask);
        MTLRenderStages stages = 0;
        if (accesses[i].stage_mask & GPU_IR_STAGE_VERTEX) stages |= MTLRenderStageVertex;
        if (accesses[i].stage_mask & GPU_IR_STAGE_FRAGMENT) stages |= MTLRenderStageFragment;
        if (usage && stages) [submission->render_encoder useResource:resource usage:usage stages:stages];
    }
    id<MTLRenderPipelineState> state = (__bridge id<MTLRenderPipelineState>)backend->state;
    [submission->render_encoder setRenderPipelineState:state];
    const gpu_ir_pipeline_info *pipeline_info = gpu_ir_pipeline_description(pipeline);
    if (!pipeline_info) return GPU_IR_INVALID;
    metal_set_blend_constants(submission->render_encoder, pipeline_info,
                              draw->has_blend_constants, draw->blend_constants);
    [submission->render_encoder setViewport:viewport];
    [submission->render_encoder setScissorRect:scissor];
    for (uint32_t binding_index = 0; binding_index < pipeline_info->vertex_input.binding_count; binding_index++) {
        uint32_t binding = pipeline_info->vertex_input.bindings[binding_index].binding;
        const gpu_ir_vertex_buffer_binding *vertex = NULL;
        for (uint32_t i = 0; i < draw->vertex_buffer_count; i++)
            if (draw->vertex_buffers[i].binding == binding) { vertex = &draw->vertex_buffers[i]; break; }
        if (!vertex) {
            [submission->render_encoder setVertexBuffer:nil offset:0
                                                 atIndex:GPU_IR_METAL_VERTEX_BUFFER_BASE + binding];
            continue;
        }
        uint64_t buffer_size = gpu_ir_buffer_size(vertex->resource);
        if (!gpu_ir_resource_is_buffer(vertex->resource) || !vertex->size ||
            vertex->offset > buffer_size || vertex->size > buffer_size - vertex->offset ||
            vertex->offset > NSUIntegerMax)
            return GPU_IR_INVALID;
        id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)gpu_ir_backend_object(vertex->resource);
        if (!buffer || vertex->offset > buffer.length || vertex->size > buffer.length - vertex->offset)
            return GPU_IR_RANGE;
        [submission->render_encoder setVertexBuffer:buffer offset:(NSUInteger)vertex->offset
                                             atIndex:GPU_IR_METAL_VERTEX_BUFFER_BASE + binding];
        [submission->render_encoder useResource:buffer usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
    }
    if (draw->indexed) {
        [submission->render_encoder drawIndexedPrimitives:primitive indexCount:draw->index_count
            indexType:index_type indexBuffer:index_buffer indexBufferOffset:index_buffer_offset
            instanceCount:draw->instance_count baseVertex:draw->vertex_offset baseInstance:draw->first_instance];
    } else {
        [submission->render_encoder drawPrimitives:primitive vertexStart:draw->first_vertex
            vertexCount:draw->vertex_count instanceCount:draw->instance_count baseInstance:draw->first_instance];
    }
    return GPU_IR_OK;
}

static int metal_draw_indirect(void *context, const gpu_ir_pipeline *pipeline,
                               const gpu_ir_resource *arguments, const gpu_ir_resource *count_arguments,
                               const gpu_ir_indirect_draw_info *draw,
                               const gpu_ir_resource_access *accesses, uint32_t access_count) {
    gpu_ir_metal_submission *submission = context;
    const gpu_ir_pipeline_info *info = gpu_ir_pipeline_description(pipeline);
    gpu_ir_metal_graphics_pipeline *backend = gpu_ir_pipeline_backend_object(pipeline);
    if (!submission || !submission->render_encoder || !info || info->kind != GPU_IR_PIPELINE_GRAPHICS ||
        info->stage_mask != (GPU_IR_STAGE_VERTEX | GPU_IR_STAGE_FRAGMENT) || !backend || !backend->state ||
        backend->pixel_format != submission->render_pixel_format || !gpu_ir_resource_is_buffer(arguments) ||
        count_arguments || !draw || draw->indexed > 1 || draw->has_viewport > 1 || draw->has_scissor > 1 ||
        (access_count && !accesses)) return GPU_IR_INVALID;
    if (!draw->draw_count) return GPU_IR_OK;

    uint32_t argument_size = draw->indexed ? 20u : 16u;
    uint64_t last_offset, required_end;
    if (draw->stride < argument_size || (draw->stride & 3u) || (draw->argument_offset & 3u) ||
        draw->draw_count - 1u > UINT64_MAX / draw->stride) return GPU_IR_INVALID;
    last_offset = (uint64_t)(draw->draw_count - 1u) * draw->stride;
    if (draw->argument_offset > UINT64_MAX - last_offset ||
        draw->argument_offset + last_offset > UINT64_MAX - argument_size) return GPU_IR_RANGE;
    required_end = draw->argument_offset + last_offset + argument_size;
    id<MTLBuffer> argument_buffer = (__bridge id<MTLBuffer>)gpu_ir_backend_object(arguments);
    if (!argument_buffer || draw->argument_offset > argument_buffer.length ||
        required_end > argument_buffer.length || draw->argument_offset > NSUIntegerMax) return GPU_IR_RANGE;

    MTLPrimitiveType primitive;
    switch (draw->topology) {
        case GPU_IR_TOPOLOGY_POINT_LIST: primitive = MTLPrimitiveTypePoint; break;
        case GPU_IR_TOPOLOGY_LINE_LIST: primitive = MTLPrimitiveTypeLine; break;
        case GPU_IR_TOPOLOGY_LINE_STRIP: primitive = MTLPrimitiveTypeLineStrip; break;
        case GPU_IR_TOPOLOGY_TRIANGLE_LIST: primitive = MTLPrimitiveTypeTriangle; break;
        case GPU_IR_TOPOLOGY_TRIANGLE_STRIP: primitive = MTLPrimitiveTypeTriangleStrip; break;
        default: return GPU_IR_UNSUPPORTED;
    }

    id<MTLBuffer> index_buffer = nil;
    MTLIndexType index_type = MTLIndexTypeUInt16;
    if (draw->indexed) {
        uint64_t index_size = draw->index_type == GPU_IR_INDEX_TYPE_UINT16 ? 2u :
            draw->index_type == GPU_IR_INDEX_TYPE_UINT32 ? 4u : 0u;
        if (!index_size || !draw->index_buffer ||
            (draw->index_buffer_offset % index_size) || draw->index_buffer_offset > NSUIntegerMax ||
            !draw->index_buffer_size || draw->index_buffer_offset > gpu_ir_buffer_size(draw->index_buffer) ||
            draw->index_buffer_size > gpu_ir_buffer_size(draw->index_buffer) - draw->index_buffer_offset)
            return GPU_IR_INVALID;
        index_buffer = (__bridge id<MTLBuffer>)gpu_ir_backend_object(draw->index_buffer);
        if (!index_buffer || draw->index_buffer_offset > index_buffer.length ||
            draw->index_buffer_size > index_buffer.length - draw->index_buffer_offset) return GPU_IR_RANGE;
        index_type = draw->index_type == GPU_IR_INDEX_TYPE_UINT16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
        [submission->render_encoder useResource:index_buffer usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
    }
    [submission->render_encoder useResource:argument_buffer usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
    for (uint32_t i = 0; i < access_count; i++) {
        id<MTLResource> resource = (__bridge id<MTLResource>)gpu_ir_backend_object(accesses[i].resource);
        if (!resource) return GPU_IR_INVALID;
        MTLResourceUsage usage = metal_resource_usage(accesses[i].access_mask);
        MTLRenderStages stages = 0;
        if (accesses[i].stage_mask & GPU_IR_STAGE_VERTEX) stages |= MTLRenderStageVertex;
        if (accesses[i].stage_mask & GPU_IR_STAGE_FRAGMENT) stages |= MTLRenderStageFragment;
        if (usage && stages) [submission->render_encoder useResource:resource usage:usage stages:stages];
    }

    MTLViewport viewport = draw->has_viewport ? (MTLViewport){
        draw->viewport.x, draw->viewport.y, draw->viewport.width, draw->viewport.height,
        draw->viewport.min_depth, draw->viewport.max_depth
    } : (MTLViewport){ 0, 0, (double)submission->render_width, (double)submission->render_height, 0, 1 };
    MTLScissorRect scissor = submission->render_area_scissor;
    if (draw->has_scissor) {
        int64_t x0 = draw->scissor.x > (int64_t)scissor.x ? draw->scissor.x : (int64_t)scissor.x;
        int64_t y0 = draw->scissor.y > (int64_t)scissor.y ? draw->scissor.y : (int64_t)scissor.y;
        int64_t draw_x1 = (int64_t)draw->scissor.x + draw->scissor.width;
        int64_t draw_y1 = (int64_t)draw->scissor.y + draw->scissor.height;
        int64_t pass_x1 = (int64_t)scissor.x + scissor.width;
        int64_t pass_y1 = (int64_t)scissor.y + scissor.height;
        int64_t x1 = draw_x1 < pass_x1 ? draw_x1 : pass_x1;
        int64_t y1 = draw_y1 < pass_y1 ? draw_y1 : pass_y1;
        if (x0 >= x1 || y0 >= y1) return GPU_IR_OK;
        scissor = (MTLScissorRect){ (NSUInteger)x0, (NSUInteger)y0,
                                    (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0) };
    }
    [submission->render_encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)backend->state];
    metal_set_blend_constants(submission->render_encoder, info,
                              draw->has_blend_constants, draw->blend_constants);
    [submission->render_encoder setViewport:viewport];
    [submission->render_encoder setScissorRect:scissor];
    for (uint32_t binding_index = 0; binding_index < info->vertex_input.binding_count; binding_index++) {
        uint32_t binding = info->vertex_input.bindings[binding_index].binding;
        const gpu_ir_vertex_buffer_binding *vertex = NULL;
        for (uint32_t i = 0; i < draw->vertex_buffer_count; i++)
            if (draw->vertex_buffers[i].binding == binding) { vertex = &draw->vertex_buffers[i]; break; }
        if (!vertex) {
            [submission->render_encoder setVertexBuffer:nil offset:0
                                                 atIndex:GPU_IR_METAL_VERTEX_BUFFER_BASE + binding];
            continue;
        }
        uint64_t vertex_size = gpu_ir_buffer_size(vertex->resource);
        if (!gpu_ir_resource_is_buffer(vertex->resource) || !vertex->size ||
            vertex->offset > vertex_size || vertex->size > vertex_size - vertex->offset ||
            vertex->offset > NSUIntegerMax)
            return GPU_IR_INVALID;
        id<MTLBuffer> vertex_buffer = (__bridge id<MTLBuffer>)gpu_ir_backend_object(vertex->resource);
        if (!vertex_buffer || vertex->offset > vertex_buffer.length ||
            vertex->size > vertex_buffer.length - vertex->offset) return GPU_IR_RANGE;
        [submission->render_encoder setVertexBuffer:vertex_buffer offset:(NSUInteger)vertex->offset
                                             atIndex:GPU_IR_METAL_VERTEX_BUFFER_BASE + binding];
        [submission->render_encoder useResource:vertex_buffer usage:MTLResourceUsageRead
                                         stages:MTLRenderStageVertex];
    }
    for (uint32_t i = 0; i < draw->draw_count; i++) {
        NSUInteger offset = (NSUInteger)(draw->argument_offset + (uint64_t)i * draw->stride);
        if (!draw->indexed) {
            [submission->render_encoder drawPrimitives:primitive indirectBuffer:argument_buffer
                indirectBufferOffset:offset];
        } else {
            [submission->render_encoder drawIndexedPrimitives:primitive indexType:index_type
                indexBuffer:index_buffer indexBufferOffset:(NSUInteger)draw->index_buffer_offset
                indirectBuffer:argument_buffer indirectBufferOffset:offset];
        }
    }
    return GPU_IR_OK;
}

static int metal_compute_buffer_binding_supported(const gpu_ir_descriptor_binding *binding) {
    if (!binding || !binding->count || binding->count == GPU_IR_DESCRIPTOR_COUNT_UNBOUNDED) return 0;
    if (binding->binding_namespace == 0)
        return binding->kind == GPU_IR_DESCRIPTOR_STORAGE_BUFFER ||
               binding->kind == GPU_IR_DESCRIPTOR_UNIFORM_BUFFER ||
               binding->kind == GPU_IR_DESCRIPTOR_READ_ONLY_RESOURCE;
    if (binding->binding_namespace == 1) return binding->kind == GPU_IR_DESCRIPTOR_STORAGE_RESOURCE;
    return 0;
}

static int metal_compute_texture_binding_supported(const gpu_ir_descriptor_binding *binding) {
    return binding && (binding->kind == GPU_IR_DESCRIPTOR_STORAGE_IMAGE ||
                       binding->kind == GPU_IR_DESCRIPTOR_SAMPLED_IMAGE ||
                       binding->kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER) &&
           binding->binding_namespace == 0 && binding->count &&
           binding->count != GPU_IR_DESCRIPTOR_COUNT_UNBOUNDED;
}

static int metal_compute_sampler_binding_supported(const gpu_ir_descriptor_binding *binding) {
    return binding && (binding->kind == GPU_IR_DESCRIPTOR_SAMPLER ||
                       binding->kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER) &&
           binding->binding_namespace == 0 && binding->count &&
           binding->count != GPU_IR_DESCRIPTOR_COUNT_UNBOUNDED;
}

static int encode_descriptor_state(gpu_ir_metal_submission *submission,
                                   const gpu_ir_descriptor_state *state) {
    if (!submission || !submission->compute_encoder || !state) return GPU_IR_INVALID;
    const gpu_ir_pipeline_layout *layout = gpu_ir_descriptor_state_layout(state);
    if (!layout) return GPU_IR_INVALID;

    int rc = GPU_IR_OK;
    uint32_t metal_slot = 0;
    uint32_t metal_texture_slot = 0;
    uint32_t metal_sampler_slot = 0;
    int has_runtime_data = 0;
    uint64_t buffer_ranges[GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS] = { 0 };
    for (uint32_t i = 0; i < gpu_ir_pipeline_layout_binding_count(layout); i++) {
        gpu_ir_descriptor_binding binding;
        if (gpu_ir_pipeline_layout_binding_at(layout, i, &binding) != GPU_IR_OK) return GPU_IR_INVALID;
        if (!(binding.stage_mask & GPU_IR_STAGE_COMPUTE)) continue;
        if (binding.set == GPU_IR_INTERNAL_SET &&
            binding.binding_namespace == GPU_IR_INTERNAL_BINDING_NAMESPACE &&
            binding.binding == GPU_IR_INTERNAL_PUSH_CONSTANT_BINDING) {
            gpu_ir_descriptor_value value = { 0 };
            rc = gpu_ir_descriptor_state_find_for_stage(state, binding.set, binding.binding_namespace,
                binding.binding, 0, GPU_IR_STAGE_COMPUTE, &value);
            if (binding.kind != GPU_IR_DESCRIPTOR_UNIFORM_BUFFER || binding.count != 1 ||
                rc != GPU_IR_OK || !value.resource || !gpu_ir_resource_is_buffer(value.resource) ||
                value.offset || value.range < GPU_IR_METAL_RUNTIME_DATA_SIZE)
                return GPU_IR_INVALID;
            id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)gpu_ir_backend_object(value.resource);
            if (!buffer || buffer.length < GPU_IR_METAL_RUNTIME_DATA_SIZE) return GPU_IR_RANGE;
            [submission->compute_encoder setBuffer:buffer offset:0 atIndex:GPU_IR_METAL_BUFFER_RANGE_SLOT];
            [submission->compute_encoder useResource:buffer usage:MTLResourceUsageRead];
            has_runtime_data = 1;
            continue;
        }
        if (metal_compute_texture_binding_supported(&binding)) {
            if (binding.count > GPU_IR_METAL_MAX_COMPUTE_TEXTURES - metal_texture_slot)
                return GPU_IR_UNSUPPORTED;
            for (uint32_t array_element = 0; array_element < binding.count; array_element++) {
                gpu_ir_descriptor_value value = { 0 };
                rc = gpu_ir_descriptor_state_find_for_stage(state, binding.set, binding.binding_namespace,
                    binding.binding, array_element, GPU_IR_STAGE_COMPUTE, &value);
                if (rc == GPU_IR_NOT_FOUND || (rc == GPU_IR_OK &&
                    (value.flags & GPU_IR_DESCRIPTOR_VALUE_NULL))) {
                    [submission->compute_encoder setTexture:nil atIndex:metal_texture_slot++];
                    continue;
                }
                if (rc != GPU_IR_OK || !value.resource ||
                    !gpu_ir_texture_description(value.resource)) return GPU_IR_INVALID;
                id<MTLTexture> texture = (__bridge id<MTLTexture>)gpu_ir_backend_object(value.resource);
                if (!texture) return GPU_IR_INVALID;
                if (value.component_mapping) {
                    void *swizzled_view = gpu_ir_metal_texture_view_with_component_mapping(
                        (__bridge void *)texture, value.component_mapping);
                    if (!swizzled_view) return GPU_IR_UNSUPPORTED;
                    texture = (__bridge_transfer id<MTLTexture>)swizzled_view;
                }
                [submission->compute_encoder setTexture:texture atIndex:metal_texture_slot];
                MTLResourceUsage usage = metal_resource_usage(value.access_mask);
                if (usage) [submission->compute_encoder useResource:texture usage:usage];
                metal_texture_slot++;
                if (binding.kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER) {
                    if (!value.backend_object || metal_sampler_slot >= GPU_IR_METAL_MAX_COMPUTE_SAMPLERS)
                        return GPU_IR_INVALID;
                    id<MTLSamplerState> sampler = (__bridge id<MTLSamplerState>)value.backend_object;
                    [submission->compute_encoder setSamplerState:sampler atIndex:metal_sampler_slot++];
                }
            }
            continue;
        }
        if (metal_compute_sampler_binding_supported(&binding)) {
            if (binding.count > GPU_IR_METAL_MAX_COMPUTE_SAMPLERS - metal_sampler_slot)
                return GPU_IR_UNSUPPORTED;
            for (uint32_t array_element = 0; array_element < binding.count; array_element++) {
                gpu_ir_descriptor_value value = { 0 };
                rc = gpu_ir_descriptor_state_find_for_stage(state, binding.set, binding.binding_namespace,
                    binding.binding, array_element, GPU_IR_STAGE_COMPUTE, &value);
                if (rc != GPU_IR_OK || !value.backend_object) return GPU_IR_INVALID;
                id<MTLSamplerState> sampler = (__bridge id<MTLSamplerState>)value.backend_object;
                [submission->compute_encoder setSamplerState:sampler atIndex:metal_sampler_slot++];
            }
            continue;
        }
        if (!metal_compute_buffer_binding_supported(&binding) ||
            binding.count > GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS - metal_slot)
            return GPU_IR_UNSUPPORTED;
        for (uint32_t array_element = 0; array_element < binding.count; array_element++) {
            gpu_ir_descriptor_value value = { 0 };
            rc = gpu_ir_descriptor_state_find_for_stage(state, binding.set, binding.binding_namespace,
                binding.binding, array_element, GPU_IR_STAGE_COMPUTE, &value);
            if (rc == GPU_IR_NOT_FOUND || (rc == GPU_IR_OK &&
                (value.flags & GPU_IR_DESCRIPTOR_VALUE_NULL))) {
                [submission->compute_encoder setBuffer:nil offset:0 atIndex:metal_slot++];
                continue;
            }
            if (rc != GPU_IR_OK) return GPU_IR_INVALID;
            if (!value.resource || !gpu_ir_resource_is_buffer(value.resource)) return GPU_IR_INVALID;
            id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)gpu_ir_backend_object(value.resource);
            if (!buffer) return GPU_IR_INVALID;
            uint64_t offset = value.offset;
            uint64_t range = value.range;
            if (value.buffer_view) {
                const gpu_ir_buffer_view_desc *view = gpu_ir_buffer_view_description(value.buffer_view);
                if (!view || offset || value.range) return GPU_IR_INVALID;
                offset = view->byte_offset;
                range = view->byte_size;
            }
            if (offset > buffer.length) return GPU_IR_RANGE;
            if (!range) range = buffer.length - offset;
            if (range > buffer.length - offset) return GPU_IR_RANGE;
            buffer_ranges[metal_slot] = range;
            [submission->compute_encoder setBuffer:buffer offset:(NSUInteger)offset atIndex:metal_slot];
            MTLResourceUsage usage = metal_resource_usage(value.access_mask);
            if (usage) [submission->compute_encoder useResource:buffer usage:usage];
            metal_slot++;
        }
    }
    if (metal_slot && !has_runtime_data)
        [submission->compute_encoder setBytes:buffer_ranges length:sizeof buffer_ranges
                                       atIndex:GPU_IR_METAL_BUFFER_RANGE_SLOT];
    return GPU_IR_OK;
}

static int graphics_descriptor_slots(const gpu_ir_pipeline_layout *layout,
                                     const gpu_ir_descriptor_binding *selected, uint32_t stage_mask,
                                     uint32_t *texture_slot, uint32_t *sampler_slot) {
    if (!layout || !selected || !texture_slot || !sampler_slot) return GPU_IR_INVALID;
    uint32_t next_texture = 0, next_sampler = 0;
    int found_texture = 0, found_sampler = 0;
    for (uint32_t i = 0; i < gpu_ir_pipeline_layout_binding_count(layout); i++) {
        gpu_ir_descriptor_binding binding;
        if (gpu_ir_pipeline_layout_binding_at(layout, i, &binding) != GPU_IR_OK) return GPU_IR_INVALID;
        if (!(binding.stage_mask & stage_mask) || binding.binding_namespace != 0) continue;
        int image = binding.kind == GPU_IR_DESCRIPTOR_STORAGE_IMAGE ||
            binding.kind == GPU_IR_DESCRIPTOR_SAMPLED_IMAGE ||
            binding.kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER;
        int sampler = binding.kind == GPU_IR_DESCRIPTOR_SAMPLER ||
            binding.kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER;
        int same = binding.set == selected->set && binding.binding_namespace == selected->binding_namespace &&
            binding.binding == selected->binding;
        if (image) {
            if (same) { *texture_slot = next_texture; found_texture = 1; }
            if (binding.count > UINT32_MAX - next_texture) return GPU_IR_RANGE;
            next_texture += binding.count;
        }
        if (sampler) {
            if (same) { *sampler_slot = next_sampler; found_sampler = 1; }
            if (binding.count > UINT32_MAX - next_sampler) return GPU_IR_RANGE;
            next_sampler += binding.count;
        }
    }
    int needs_texture = selected->kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER ||
        selected->kind == GPU_IR_DESCRIPTOR_SAMPLED_IMAGE;
    int needs_sampler = selected->kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER ||
        selected->kind == GPU_IR_DESCRIPTOR_SAMPLER;
    return (!needs_texture || found_texture) && (!needs_sampler || found_sampler) ?
        GPU_IR_OK : GPU_IR_NOT_FOUND;
}

static int encode_graphics_descriptor_state(gpu_ir_metal_submission *submission,
                                            const gpu_ir_descriptor_state *state) {
    if (!submission || !submission->render_encoder || !state) return GPU_IR_INVALID;
    const gpu_ir_pipeline_layout *layout = gpu_ir_descriptor_state_layout(state);
    if (!layout) return GPU_IR_INVALID;
    for (uint32_t i = 0; i < gpu_ir_descriptor_state_value_count(state); i++) {
        gpu_ir_descriptor_value value;
        if (gpu_ir_descriptor_state_value_at(state, i, &value) != GPU_IR_OK) return GPU_IR_INVALID;
        if (!(value.stage_mask & GPU_IR_STAGE_FRAGMENT)) continue;
        gpu_ir_descriptor_binding binding;
        int rc = gpu_ir_pipeline_layout_find_binding_for_stage(layout, value.set, value.binding_namespace,
            value.binding, GPU_IR_STAGE_FRAGMENT, &binding);
        if (rc != GPU_IR_OK) return GPU_IR_UNSUPPORTED;
        int has_texture = binding.kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER ||
            binding.kind == GPU_IR_DESCRIPTOR_SAMPLED_IMAGE;
        int has_sampler = binding.kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER ||
            binding.kind == GPU_IR_DESCRIPTOR_SAMPLER;
        if ((binding.kind != GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER &&
             binding.kind != GPU_IR_DESCRIPTOR_SAMPLED_IMAGE &&
             binding.kind != GPU_IR_DESCRIPTOR_SAMPLER) ||
            value.kind != binding.kind || (has_texture && !value.resource) ||
            (has_sampler && !value.backend_object))
            return GPU_IR_UNSUPPORTED;
        uint32_t texture_slot = 0, sampler_slot = 0;
        rc = graphics_descriptor_slots(layout, &binding, GPU_IR_STAGE_FRAGMENT, &texture_slot, &sampler_slot);
        if (rc != GPU_IR_OK || value.array_element >= binding.count ||
            texture_slot > UINT32_MAX - value.array_element || sampler_slot > UINT32_MAX - value.array_element)
            return rc == GPU_IR_OK ? GPU_IR_RANGE : rc;
        if (has_texture) {
            id<MTLTexture> texture = (__bridge id<MTLTexture>)gpu_ir_backend_object(value.resource);
            if (!texture) return GPU_IR_INVALID;
            if (value.component_mapping) {
                void *swizzled_view = gpu_ir_metal_texture_view_with_component_mapping(
                    (__bridge void *)texture, value.component_mapping);
                if (!swizzled_view) return GPU_IR_UNSUPPORTED;
                texture = (__bridge_transfer id<MTLTexture>)swizzled_view;
            }
            [submission->render_encoder setFragmentTexture:texture atIndex:texture_slot + value.array_element];
            MTLResourceUsage usage = metal_resource_usage(value.access_mask);
            if (usage) [submission->render_encoder useResource:texture usage:usage stages:MTLRenderStageFragment];
        }
        if (has_sampler) {
            id<MTLSamplerState> sampler = (__bridge id<MTLSamplerState>)value.backend_object;
            if (!sampler) return GPU_IR_INVALID;
            [submission->render_encoder setFragmentSamplerState:sampler atIndex:sampler_slot + value.array_element];
        }
    }
    return GPU_IR_OK;
}

static int metal_bind_descriptor_state(void *context, const gpu_ir_descriptor_state *state) {
    gpu_ir_metal_submission *submission = context;
    if (!submission || !state) return GPU_IR_INVALID;
    if (submission->render_encoder) {
        int rc = encode_graphics_descriptor_state(submission, state);
        if (rc == GPU_IR_OK) submission->descriptors = state;
        return rc;
    }
    int rc = ensure_compute_encoder(submission);
    if (rc != GPU_IR_OK) return rc;
    rc = encode_descriptor_state(submission, state);
    if (rc == GPU_IR_OK) submission->descriptors = state;
    return rc;
}

static int metal_dispatch(void *context, const gpu_ir_pipeline *pipeline,
                          uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z,
                          const gpu_ir_resource_access *accesses, uint32_t access_count) {
    gpu_ir_metal_submission *submission = context;
    const gpu_ir_pipeline_info *info = gpu_ir_pipeline_description(pipeline);
    if (!submission || !info || info->kind != GPU_IR_PIPELINE_COMPUTE ||
        info->stage_mask != GPU_IR_STAGE_COMPUTE ||
        (access_count && !accesses)) return GPU_IR_INVALID;
    int rc = ensure_compute_encoder(submission);
    if (rc != GPU_IR_OK) return rc;
    id<MTLComputePipelineState> state = (__bridge id<MTLComputePipelineState>)gpu_ir_pipeline_backend_object(pipeline);
    if (!state || !info->threadgroup_size[0] || !info->threadgroup_size[1] || !info->threadgroup_size[2])
        return GPU_IR_INVALID;
    for (uint32_t i = 0; i < access_count; i++) {
        id<MTLResource> resource = (__bridge id<MTLResource>)gpu_ir_backend_object(accesses[i].resource);
        if (!resource) return GPU_IR_INVALID;
        MTLResourceUsage usage = metal_resource_usage(accesses[i].access_mask);
        if (usage) [submission->compute_encoder useResource:resource usage:usage];
    }
    [submission->compute_encoder setComputePipelineState:state];
    [submission->compute_encoder dispatchThreadgroups:MTLSizeMake(group_count_x, group_count_y, group_count_z)
                                    threadsPerThreadgroup:MTLSizeMake(info->threadgroup_size[0],
                                        info->threadgroup_size[1], info->threadgroup_size[2])];
    return GPU_IR_OK;
}

static int metal_dispatch_indirect(void *context, const gpu_ir_pipeline *pipeline,
                                   const gpu_ir_resource *arguments, uint64_t argument_offset,
                                   const gpu_ir_resource_access *accesses, uint32_t access_count) {
    gpu_ir_metal_submission *submission = context;
    const gpu_ir_pipeline_info *info = gpu_ir_pipeline_description(pipeline);
    if (!submission || !info || info->kind != GPU_IR_PIPELINE_COMPUTE ||
        info->stage_mask != GPU_IR_STAGE_COMPUTE || !arguments ||
        !gpu_ir_resource_is_buffer(arguments) || (access_count && !accesses)) return GPU_IR_INVALID;
    if (argument_offset > NSUIntegerMax || argument_offset > gpu_ir_buffer_size(arguments) ||
        sizeof(uint32_t) * 3u > gpu_ir_buffer_size(arguments) - argument_offset) return GPU_IR_RANGE;
    int rc = ensure_compute_encoder(submission);
    if (rc != GPU_IR_OK) return rc;
    id<MTLComputePipelineState> state = (__bridge id<MTLComputePipelineState>)gpu_ir_pipeline_backend_object(pipeline);
    if (!state || !info->threadgroup_size[0] || !info->threadgroup_size[1] || !info->threadgroup_size[2])
        return GPU_IR_INVALID;
    for (uint32_t i = 0; i < access_count; i++) {
        id<MTLResource> resource = (__bridge id<MTLResource>)gpu_ir_backend_object(accesses[i].resource);
        if (!resource) return GPU_IR_INVALID;
        MTLResourceUsage usage = metal_resource_usage(accesses[i].access_mask);
        if (usage) [submission->compute_encoder useResource:resource usage:usage];
    }
    id<MTLBuffer> argument_buffer = (__bridge id<MTLBuffer>)gpu_ir_backend_object(arguments);
    if (!argument_buffer) return GPU_IR_INVALID;
    [submission->compute_encoder setComputePipelineState:state];
    [submission->compute_encoder dispatchThreadgroupsWithIndirectBuffer:argument_buffer
        indirectBufferOffset:(NSUInteger)argument_offset
        threadsPerThreadgroup:MTLSizeMake(info->threadgroup_size[0], info->threadgroup_size[1],
                                          info->threadgroup_size[2])];
    return GPU_IR_OK;
}

static int metal_copy_buffer(void *context, const gpu_ir_resource *src, uint64_t src_offset,
                             const gpu_ir_resource *dst, uint64_t dst_offset, uint64_t size) {
    gpu_ir_metal_submission *submission = context;
    if (!submission || !src || !dst || !gpu_ir_resource_is_buffer(src) ||
        !gpu_ir_resource_is_buffer(dst)) return GPU_IR_INVALID;
    if (src_offset > NSUIntegerMax || dst_offset > NSUIntegerMax || size > NSUIntegerMax ||
        src_offset > gpu_ir_buffer_size(src) || size > gpu_ir_buffer_size(src) - src_offset ||
        dst_offset > gpu_ir_buffer_size(dst) || size > gpu_ir_buffer_size(dst) - dst_offset)
        return GPU_IR_RANGE;
    int rc = ensure_blit_encoder(submission);
    if (rc != GPU_IR_OK) return rc;
    id<MTLBuffer> source = (__bridge id<MTLBuffer>)gpu_ir_backend_object(src);
    id<MTLBuffer> destination = (__bridge id<MTLBuffer>)gpu_ir_backend_object(dst);
    if (!source || !destination) return GPU_IR_INVALID;
    if (size) [submission->blit_encoder copyFromBuffer:source sourceOffset:(NSUInteger)src_offset
        toBuffer:destination destinationOffset:(NSUInteger)dst_offset size:(NSUInteger)size];
    return GPU_IR_OK;
}

static int metal_update_buffer(void *context, const gpu_ir_resource *dst, uint64_t dst_offset,
                               const void *data, uint64_t size) {
    gpu_ir_metal_submission *submission = context;
    if (!submission || !dst || !data || !gpu_ir_resource_is_buffer(dst)) return GPU_IR_INVALID;
    if (!size || dst_offset > NSUIntegerMax || size > NSUIntegerMax ||
        dst_offset > gpu_ir_buffer_size(dst) || size > gpu_ir_buffer_size(dst) - dst_offset)
        return GPU_IR_RANGE;
    id<MTLBuffer> destination = (__bridge id<MTLBuffer>)gpu_ir_backend_object(dst);
    if (!destination) return GPU_IR_INVALID;
    id<MTLBuffer> upload = [submission->device->device newBufferWithBytes:data length:(NSUInteger)size
                                                                    options:MTLResourceStorageModeShared];
    if (!upload) return GPU_IR_OUT_OF_MEMORY;
    [submission->upload_buffers addObject:upload];
    int rc = ensure_blit_encoder(submission);
    if (rc != GPU_IR_OK) return rc;
    [submission->blit_encoder copyFromBuffer:upload sourceOffset:0 toBuffer:destination
        destinationOffset:(NSUInteger)dst_offset size:(NSUInteger)size];
    return GPU_IR_OK;
}

static int metal_fill_buffer_pattern(void *context, const gpu_ir_resource *dst, uint64_t dst_offset,
                                     uint64_t size, const void *pattern, uint32_t pattern_size) {
    gpu_ir_metal_submission *submission = context;
    if (!submission || !dst || !pattern || !pattern_size ||
        pattern_size > GPU_IR_MAX_FILL_PATTERN_SIZE || (pattern_size & (pattern_size - 1u)) ||
        !gpu_ir_resource_is_buffer(dst)) return GPU_IR_INVALID;
    if (dst_offset % pattern_size || size % pattern_size || dst_offset > NSUIntegerMax || size > NSUIntegerMax ||
        dst_offset > gpu_ir_buffer_size(dst) || size > gpu_ir_buffer_size(dst) - dst_offset)
        return GPU_IR_RANGE;
    if (!size) return GPU_IR_OK;
    id<MTLBuffer> destination = (__bridge id<MTLBuffer>)gpu_ir_backend_object(dst);
    if (!destination) return GPU_IR_INVALID;

    // Stage a bounded repeated pattern, then issue ordered blit copies. This keeps temporary
    // memory independent of the guest fill size and supports typed buffer element widths.
    const NSUInteger chunk_size = 1024u * 1024u;
    NSUInteger upload_size = (NSUInteger)(size < chunk_size ? size : chunk_size);
    upload_size -= upload_size % pattern_size;
    if (!upload_size) return GPU_IR_RANGE;
    id<MTLBuffer> upload = [submission->device->device newBufferWithLength:upload_size
        options:MTLResourceStorageModeShared];
    if (!upload || !upload.contents) return GPU_IR_OUT_OF_MEMORY;
    uint8_t *bytes = upload.contents;
    for (NSUInteger i = 0; i < upload_size; i += pattern_size)
        memcpy(bytes + i, pattern, pattern_size);
    [submission->upload_buffers addObject:upload];

    int rc = ensure_blit_encoder(submission);
    if (rc != GPU_IR_OK) return rc;
    uint64_t copied = 0;
    while (copied < size) {
        NSUInteger block = (NSUInteger)((size - copied) < upload_size ? (size - copied) : upload_size);
        [submission->blit_encoder copyFromBuffer:upload sourceOffset:0 toBuffer:destination
            destinationOffset:(NSUInteger)(dst_offset + copied) size:block];
        copied += block;
    }
    return GPU_IR_OK;
}

static int metal_fill_buffer(void *context, const gpu_ir_resource *dst, uint64_t dst_offset,
                             uint64_t size, uint32_t value) {
    return metal_fill_buffer_pattern(context, dst, dst_offset, size, &value, sizeof value);
}

static int metal_buffer_location_offset(const gpu_ir_texture_location *location,
                                        const gpu_ir_texture_desc *format, uint64_t *offset) {
    if (!location || !format || !offset || !format->block_width || !format->block_height ||
        !format->bytes_per_block) return GPU_IR_INVALID;
    uint64_t value = location->buffer_offset;
    uint64_t z = (uint64_t)location->origin_z * location->image_pitch;
    uint64_t y = (uint64_t)(location->origin_y / format->block_height) * location->row_pitch;
    uint64_t x = (uint64_t)(location->origin_x / format->block_width) * format->bytes_per_block;
    if (z > UINT64_MAX - value) return GPU_IR_RANGE;
    value += z;
    if (y > UINT64_MAX - value) return GPU_IR_RANGE;
    value += y;
    if (x > UINT64_MAX - value) return GPU_IR_RANGE;
    *offset = value + x;
    return GPU_IR_OK;
}

static int metal_copy_texture(void *context, const gpu_ir_resource *src,
                              const gpu_ir_texture_location *src_location, const gpu_ir_resource *dst,
                              const gpu_ir_texture_location *dst_location, uint32_t width,
                              uint32_t height, uint32_t depth) {
    gpu_ir_metal_submission *submission = context;
    const gpu_ir_texture_desc *src_desc = gpu_ir_texture_description(src);
    const gpu_ir_texture_desc *dst_desc = gpu_ir_texture_description(dst);
    const gpu_ir_texture_desc *format = src_desc ? src_desc : dst_desc;
    if (!submission || !src || !dst || !src_location || !dst_location || !format ||
        !format->bytes_per_block || format->block_width != 1 || format->block_height != 1 ||
        !width || !height || !depth)
        return GPU_IR_INVALID;
    int rc = ensure_blit_encoder(submission);
    if (rc != GPU_IR_OK) return rc;
    MTLSize size = MTLSizeMake(width, height, depth);
    if (src_desc && dst_desc) {
        if (src_desc->format != dst_desc->format || src_desc->format == GPU_IR_FORMAT_UNDEFINED)
            return GPU_IR_UNSUPPORTED;
        id<MTLTexture> source = (__bridge id<MTLTexture>)gpu_ir_backend_object(src);
        id<MTLTexture> destination = (__bridge id<MTLTexture>)gpu_ir_backend_object(dst);
        if (!source || !destination) return GPU_IR_INVALID;
        [submission->blit_encoder copyFromTexture:source sourceSlice:src_location->array_slice
            sourceLevel:src_location->mip_level
            sourceOrigin:MTLOriginMake(src_location->origin_x, src_location->origin_y, src_location->origin_z)
            sourceSize:size toTexture:destination destinationSlice:dst_location->array_slice
            destinationLevel:dst_location->mip_level
            destinationOrigin:MTLOriginMake(dst_location->origin_x, dst_location->origin_y, dst_location->origin_z)];
        return GPU_IR_OK;
    }
    if (gpu_ir_resource_is_buffer(src) && dst_desc) {
        uint64_t source_offset = 0;
        rc = metal_buffer_location_offset(src_location, dst_desc, &source_offset);
        if (rc != GPU_IR_OK || source_offset > NSUIntegerMax || src_location->row_pitch > NSUIntegerMax ||
            src_location->image_pitch > NSUIntegerMax) return rc == GPU_IR_OK ? GPU_IR_RANGE : rc;
        id<MTLBuffer> source = (__bridge id<MTLBuffer>)gpu_ir_backend_object(src);
        id<MTLTexture> destination = (__bridge id<MTLTexture>)gpu_ir_backend_object(dst);
        if (!source || !destination) return GPU_IR_INVALID;
        [submission->blit_encoder copyFromBuffer:source sourceOffset:(NSUInteger)source_offset
            sourceBytesPerRow:(NSUInteger)src_location->row_pitch
            sourceBytesPerImage:(NSUInteger)src_location->image_pitch sourceSize:size toTexture:destination
            destinationSlice:dst_location->array_slice destinationLevel:dst_location->mip_level
            destinationOrigin:MTLOriginMake(dst_location->origin_x, dst_location->origin_y,
                                             dst_location->origin_z)];
        return GPU_IR_OK;
    }
    if (src_desc && gpu_ir_resource_is_buffer(dst)) {
        uint64_t destination_offset = 0;
        rc = metal_buffer_location_offset(dst_location, src_desc, &destination_offset);
        if (rc != GPU_IR_OK || destination_offset > NSUIntegerMax || dst_location->row_pitch > NSUIntegerMax ||
            dst_location->image_pitch > NSUIntegerMax) return rc == GPU_IR_OK ? GPU_IR_RANGE : rc;
        id<MTLTexture> source = (__bridge id<MTLTexture>)gpu_ir_backend_object(src);
        id<MTLBuffer> destination = (__bridge id<MTLBuffer>)gpu_ir_backend_object(dst);
        if (!source || !destination) return GPU_IR_INVALID;
        [submission->blit_encoder copyFromTexture:source sourceSlice:src_location->array_slice
            sourceLevel:src_location->mip_level
            sourceOrigin:MTLOriginMake(src_location->origin_x, src_location->origin_y, src_location->origin_z)
            sourceSize:size toBuffer:destination destinationOffset:(NSUInteger)destination_offset
            destinationBytesPerRow:(NSUInteger)dst_location->row_pitch
            destinationBytesPerImage:(NSUInteger)dst_location->image_pitch];
        return GPU_IR_OK;
    }
    return GPU_IR_UNSUPPORTED;
}

static int metal_copy_resource(void *context, const gpu_ir_resource *src, const gpu_ir_resource *dst) {
    if (!src || !dst || !gpu_ir_resource_is_buffer(src) || !gpu_ir_resource_is_buffer(dst))
        return GPU_IR_UNSUPPORTED;
    uint64_t size = gpu_ir_buffer_size(src);
    if (size != gpu_ir_buffer_size(dst)) return GPU_IR_RANGE;
    return metal_copy_buffer(context, src, 0, dst, 0, size);
}

static int metal_barrier(void *context, const gpu_ir_barrier_desc *barrier) {
    gpu_ir_metal_submission *submission = context;
    if (!submission || !barrier) return GPU_IR_INVALID;
    if (submission->render_encoder) return GPU_IR_UNSUPPORTED;
    if (barrier->type != GPU_IR_BARRIER_MEMORY_DEPENDENCY &&
        barrier->type != GPU_IR_BARRIER_BUFFER_DEPENDENCY &&
        barrier->type != GPU_IR_BARRIER_IMAGE_DEPENDENCY) return GPU_IR_UNSUPPORTED;
    if (barrier->type == GPU_IR_BARRIER_BUFFER_DEPENDENCY &&
        !gpu_ir_resource_is_buffer(barrier->resource)) return GPU_IR_INVALID;
    if (barrier->type == GPU_IR_BARRIER_IMAGE_DEPENDENCY &&
        !gpu_ir_texture_description(barrier->resource)) return GPU_IR_INVALID;
    if (barrier->type == GPU_IR_BARRIER_BUFFER_DEPENDENCY && !barrier->resource) return GPU_IR_INVALID;
    if (submission->compute_encoder) {
        MTLBarrierScope scope = barrier->type == GPU_IR_BARRIER_IMAGE_DEPENDENCY ?
            (MTLBarrierScopeBuffers | MTLBarrierScopeTextures) : MTLBarrierScopeBuffers;
        [submission->compute_encoder memoryBarrierWithScope:scope];
    } else {
        // Closing the encoder orders earlier blit work before a later compute encoder in this command buffer.
        end_blit_encoder(submission);
    }
    return GPU_IR_OK;
}

static int metal_clear_color_kind(MTLPixelFormat format) {
    switch (format) {
    case MTLPixelFormatR8Uint: case MTLPixelFormatRG8Uint: case MTLPixelFormatRGBA8Uint:
    case MTLPixelFormatR16Uint: case MTLPixelFormatRG16Uint: case MTLPixelFormatRGBA16Uint:
    case MTLPixelFormatR32Uint: case MTLPixelFormatRG32Uint: case MTLPixelFormatRGBA32Uint:
    case MTLPixelFormatRGB10A2Uint:
        return 1;
    case MTLPixelFormatR8Sint: case MTLPixelFormatRG8Sint: case MTLPixelFormatRGBA8Sint:
    case MTLPixelFormatR16Sint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRGBA16Sint:
    case MTLPixelFormatR32Sint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRGBA32Sint:
        return 2;
    default:
        return 0;
    }
}

static id<MTLRenderPipelineState> metal_clear_pipeline(gpu_ir_metal_device *device, id<MTLTexture> texture,
                                                        int depth_stencil, uint32_t value_type) {
    if (!device || !device->device || !texture || !texture.sampleCount) return nil;
    NSString *kind = depth_stencil ? @"depth" : @"color";
    NSString *key = [NSString stringWithFormat:@"%@:%lu:%lu:%u", kind, (unsigned long)texture.pixelFormat,
                     (unsigned long)texture.sampleCount, value_type];
    pthread_mutex_lock(&device->clear_pipeline_lock);
    id<MTLRenderPipelineState> cached = device->clear_pipeline_cache[key];
    pthread_mutex_unlock(&device->clear_pipeline_lock);
    if (cached) return cached;

    NSString *source;
    if (depth_stencil) {
        source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "vertex float4 vx_clear_vs(uint id [[vertex_id]], constant float& clear_depth [[buffer(0)]]) { "
            "constexpr float2 p[3] = { float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f) }; "
            "return float4(p[id], clamp(clear_depth, 0.0f, 1.0f), 1.0f); }\n"
            "fragment float4 vx_clear_fs() { return float4(0.0f); }\n";
    } else {
        int value_kind = metal_clear_color_kind(texture.pixelFormat);
        if (value_type > GPU_IR_CLEAR_COLOR_VALUE_SINT ||
            (value_type == GPU_IR_CLEAR_COLOR_VALUE_UINT && value_kind != 1) ||
            (value_type == GPU_IR_CLEAR_COLOR_VALUE_SINT && value_kind != 2)) return nil;
        NSString *argument_type = value_type == GPU_IR_CLEAR_COLOR_VALUE_UINT ? @"uint4" :
                                  value_type == GPU_IR_CLEAR_COLOR_VALUE_SINT ? @"int4" : @"float4";
        NSString *return_type = value_kind == 1 ? @"uint4" : value_kind == 2 ? @"int4" : @"float4";
        NSString *expression = value_type == GPU_IR_CLEAR_COLOR_VALUE_UINT ||
                               value_type == GPU_IR_CLEAR_COLOR_VALUE_SINT ? @"clear_value" :
                               value_kind == 1 ? @"uint4(max(clear_value, float4(0.0f)))" :
                               value_kind == 2 ? @"int4(clear_value)" : @"clear_value";
        source = [NSString stringWithFormat:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "vertex float4 vx_clear_vs(uint id [[vertex_id]]) { "
             "constexpr float2 p[3] = { float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f) }; "
             "return float4(p[id], 0.0f, 1.0f); }\n"
             "fragment %@ vx_clear_fs(constant %@& clear_value [[buffer(0)]]) { return %@; }\n",
             return_type, argument_type, expression];
    }
    NSError *error = nil;
    id<MTLLibrary> library = [device->device newLibraryWithSource:source options:nil error:&error];
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"vx_clear_vs"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"vx_clear_fs"];
    if (depth_stencil) {
        descriptor.depthAttachmentPixelFormat = texture.pixelFormat;
        if (texture.pixelFormat == MTLPixelFormatDepth32Float_Stencil8)
            descriptor.stencilAttachmentPixelFormat = texture.pixelFormat;
    } else {
        descriptor.colorAttachments[0].pixelFormat = texture.pixelFormat;
        descriptor.colorAttachments[0].writeMask = MTLColorWriteMaskAll;
    }
    descriptor.rasterSampleCount = texture.sampleCount;
    id<MTLRenderPipelineState> pipeline = library && descriptor.vertexFunction && descriptor.fragmentFunction ?
        [device->device newRenderPipelineStateWithDescriptor:descriptor error:&error] : nil;
    if (!pipeline) {
        fprintf(stderr, "gpu_ir_metal: clear pipeline failed for pixel format %lu: %s\n",
                (unsigned long)texture.pixelFormat, error.localizedDescription.UTF8String ?: "unknown Metal error");
        return nil;
    }
    pthread_mutex_lock(&device->clear_pipeline_lock);
    cached = device->clear_pipeline_cache[key];
    if (!cached) device->clear_pipeline_cache[key] = pipeline;
    else pipeline = cached;
    pthread_mutex_unlock(&device->clear_pipeline_lock);
    return pipeline;
}

int gpu_ir_metal_color_attachment_supported(gpu_ir_metal_device *device, uint32_t format,
                                            gpu_ir_clear_color_value_type clear_value_type) {
    if (!device || !device->device ||
        clear_value_type < GPU_IR_CLEAR_COLOR_VALUE_FLOAT ||
        clear_value_type > GPU_IR_CLEAR_COLOR_VALUE_SINT) return 0;
    MTLPixelFormat pixel_format = metal_pixel_format(format);
    uint64_t row_pitch = gpu_ir_metal_linear_texture_row_pitch(device, format, 1);
    if (pixel_format == MTLPixelFormatInvalid || !row_pitch || row_pitch > NSUIntegerMax) return 0;
    id<MTLBuffer> buffer = [device->device newBufferWithLength:(NSUInteger)row_pitch
        options:MTLResourceStorageModeShared];
    if (!buffer) return 0;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = MTLTextureType2D;
    descriptor.pixelFormat = pixel_format;
    descriptor.width = 1;
    descriptor.height = 1;
    descriptor.depth = 1;
    descriptor.mipmapLevelCount = 1;
    descriptor.sampleCount = 1;
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [buffer newTextureWithDescriptor:descriptor offset:0
        bytesPerRow:(NSUInteger)row_pitch];
    return texture && metal_clear_pipeline(device, texture, 0, clear_value_type) != nil;
}

static int metal_clear_rect_bounds(id<MTLTexture> texture, const gpu_ir_rect *rect, MTLScissorRect *scissor) {
    if (!texture || !rect || !scissor) return 0;
    int64_t left = rect->left, top = rect->top, right = rect->right, bottom = rect->bottom;
    int64_t width = (int64_t)texture.width, height = (int64_t)texture.height;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > width) right = width;
    if (bottom > height) bottom = height;
    if (right <= left || bottom <= top) return 0;
    *scissor = (MTLScissorRect){ (NSUInteger)left, (NSUInteger)top,
        (NSUInteger)(right - left), (NSUInteger)(bottom - top) };
    return 1;
}

static int metal_clear_color(void *context, const gpu_ir_resource *resource,
                             const gpu_ir_clear_color_info *clear) {
    gpu_ir_metal_submission *submission = context;
    const gpu_ir_texture_desc *description = gpu_ir_texture_description(resource);
    id<MTLTexture> texture = (__bridge id<MTLTexture>)gpu_ir_backend_object(resource);
    if (!submission || !submission->device || !submission->command_buffer || submission->render_encoder ||
        !description || !texture || !clear ||
        clear->value_type > GPU_IR_CLEAR_COLOR_VALUE_SINT ||
        (clear->rectangle_count && !clear->rectangles) || description->width != texture.width ||
        description->height != texture.height || description->array_layers != texture.arrayLength ||
        description->sample_count != texture.sampleCount) return GPU_IR_INVALID;
    uint32_t base_array_layer = clear->base_array_layer;
    uint32_t array_layer_count = clear->array_layer_count ? clear->array_layer_count : description->array_layers;
    if ((!clear->array_layer_count && base_array_layer) || base_array_layer >= description->array_layers ||
        array_layer_count > description->array_layers - base_array_layer) return GPU_IR_RANGE;
    int value_kind = metal_clear_color_kind(texture.pixelFormat);
    if ((clear->value_type == GPU_IR_CLEAR_COLOR_VALUE_UINT && value_kind != 1) ||
        (clear->value_type == GPU_IR_CLEAR_COLOR_VALUE_SINT && value_kind != 2)) return GPU_IR_INVALID;
    end_compute_encoder(submission);
    end_blit_encoder(submission);
    if (!clear->rectangle_count && clear->value_type == GPU_IR_CLEAR_COLOR_VALUE_FLOAT) {
        double components[4];
        for (uint32_t i = 0; i < 4; i++) {
            components[i] = (double)clear->color[i];
        }
        for (uint32_t layer = base_array_layer; layer < base_array_layer + array_layer_count; layer++) {
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = texture;
            pass.colorAttachments[0].slice = layer;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(components[0], components[1],
                                                                    components[2], components[3]);
            id<MTLRenderCommandEncoder> encoder =
                [submission->command_buffer renderCommandEncoderWithDescriptor:pass];
            if (!encoder) return GPU_IR_UNSUPPORTED;
            [encoder endEncoding];
        }
        return GPU_IR_OK;
    }
    id<MTLRenderPipelineState> pipeline = metal_clear_pipeline(submission->device, texture, 0, clear->value_type);
    if (!pipeline) return GPU_IR_UNSUPPORTED;
    for (uint32_t layer = base_array_layer; layer < base_array_layer + array_layer_count; layer++) {
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].slice = layer;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder =
            [submission->command_buffer renderCommandEncoderWithDescriptor:pass];
        if (!encoder) return GPU_IR_UNSUPPORTED;
        [encoder setRenderPipelineState:pipeline];
        [encoder setCullMode:MTLCullModeNone];
        [encoder setViewport:(MTLViewport){ 0, 0, (double)texture.width, (double)texture.height, 0, 1 }];
        [encoder setFragmentBytes:clear->color length:sizeof clear->color atIndex:0];
        if (!clear->rectangle_count) {
            [encoder setScissorRect:(MTLScissorRect){ 0, 0, texture.width, texture.height }];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        } else {
            for (uint32_t i = 0; i < clear->rectangle_count; i++) {
                MTLScissorRect scissor;
                if (!metal_clear_rect_bounds(texture, &clear->rectangles[i], &scissor)) continue;
                [encoder setScissorRect:scissor];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
        }
        [encoder endEncoding];
    }
    return GPU_IR_OK;
}

static int metal_clear_depth_stencil(void *context, const gpu_ir_resource *resource,
                                     const gpu_ir_clear_depth_stencil_info *clear) {
    gpu_ir_metal_submission *submission = context;
    const gpu_ir_texture_desc *description = gpu_ir_texture_description(resource);
    id<MTLTexture> texture = (__bridge id<MTLTexture>)gpu_ir_backend_object(resource);
    if (!submission || !submission->device || !submission->command_buffer || submission->render_encoder ||
        !description || !texture || !clear ||
        !clear->aspects || (clear->aspects & ~GPU_IR_CLEAR_DEPTH_STENCIL_ALL) ||
        (clear->rectangle_count && !clear->rectangles) || description->width != texture.width ||
        description->height != texture.height || description->sample_count != texture.sampleCount) return GPU_IR_INVALID;
    if ((clear->aspects & GPU_IR_CLEAR_STENCIL) && texture.pixelFormat != MTLPixelFormatDepth32Float_Stencil8)
        return GPU_IR_UNSUPPORTED;
    end_compute_encoder(submission);
    end_blit_encoder(submission);
    if (!clear->rectangle_count) {
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.depthAttachment.texture = texture;
        pass.depthAttachment.loadAction = (clear->aspects & GPU_IR_CLEAR_DEPTH) ? MTLLoadActionClear : MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = clear->depth;
        if (texture.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
            pass.stencilAttachment.texture = texture;
            pass.stencilAttachment.loadAction = (clear->aspects & GPU_IR_CLEAR_STENCIL) ? MTLLoadActionClear : MTLLoadActionLoad;
            pass.stencilAttachment.storeAction = MTLStoreActionStore;
            pass.stencilAttachment.clearStencil = clear->stencil;
        }
        id<MTLRenderCommandEncoder> encoder = [submission->command_buffer renderCommandEncoderWithDescriptor:pass];
        if (!encoder) return GPU_IR_UNSUPPORTED;
        [encoder endEncoding];
        return GPU_IR_OK;
    }
    id<MTLRenderPipelineState> pipeline = metal_clear_pipeline(submission->device, texture, 1,
                                                                GPU_IR_CLEAR_COLOR_VALUE_FLOAT);
    if (!pipeline) return GPU_IR_UNSUPPORTED;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.depthAttachment.texture = texture;
    pass.depthAttachment.loadAction = MTLLoadActionLoad;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    if (texture.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
        pass.stencilAttachment.texture = texture;
        pass.stencilAttachment.loadAction = MTLLoadActionLoad;
        pass.stencilAttachment.storeAction = MTLStoreActionStore;
    }
    id<MTLRenderCommandEncoder> encoder = [submission->command_buffer renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return GPU_IR_UNSUPPORTED;
    MTLDepthStencilDescriptor *depth_descriptor = [MTLDepthStencilDescriptor new];
    depth_descriptor.depthCompareFunction = MTLCompareFunctionAlways;
    depth_descriptor.depthWriteEnabled = (clear->aspects & GPU_IR_CLEAR_DEPTH) != 0;
    if (clear->aspects & GPU_IR_CLEAR_STENCIL) {
        MTLStencilDescriptor *stencil = [MTLStencilDescriptor new];
        stencil.stencilCompareFunction = MTLCompareFunctionAlways;
        stencil.stencilFailureOperation = MTLStencilOperationKeep;
        stencil.depthFailureOperation = MTLStencilOperationKeep;
        stencil.depthStencilPassOperation = MTLStencilOperationReplace;
        stencil.readMask = 0xff;
        stencil.writeMask = 0xff;
        depth_descriptor.frontFaceStencil = stencil;
        depth_descriptor.backFaceStencil = stencil;
    }
    id<MTLDepthStencilState> depth_state = [submission->device->device
        newDepthStencilStateWithDescriptor:depth_descriptor];
    if (!depth_state) { [encoder endEncoding]; return GPU_IR_UNSUPPORTED; }
    [encoder setRenderPipelineState:pipeline];
    [encoder setDepthStencilState:depth_state];
    [encoder setStencilReferenceValue:clear->stencil & 0xffu];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setViewport:(MTLViewport){ 0, 0, (double)texture.width, (double)texture.height, 0, 1 }];
    [encoder setVertexBytes:&clear->depth length:sizeof clear->depth atIndex:0];
    for (uint32_t i = 0; i < clear->rectangle_count; i++) {
        MTLScissorRect scissor;
        if (!metal_clear_rect_bounds(texture, &clear->rectangles[i], &scissor)) continue;
        [encoder setScissorRect:scissor];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [encoder endEncoding];
    return GPU_IR_OK;
}

int gpu_ir_metal_encode_clear_color(gpu_ir_metal_device *device, void *command_buffer,
                                    const gpu_ir_resource *texture,
                                    const gpu_ir_clear_color_info *clear) {
    if (!device || !command_buffer) return GPU_IR_INVALID;
    gpu_ir_metal_submission submission = { .device = device,
        .command_buffer = (__bridge id<MTLCommandBuffer>)command_buffer };
    return metal_clear_color(&submission, texture, clear);
}

int gpu_ir_metal_encode_clear_depth_stencil(gpu_ir_metal_device *device, void *command_buffer,
                                            const gpu_ir_resource *texture,
                                            const gpu_ir_clear_depth_stencil_info *clear) {
    if (!device || !command_buffer) return GPU_IR_INVALID;
    gpu_ir_metal_submission submission = { .device = device,
        .command_buffer = (__bridge id<MTLCommandBuffer>)command_buffer };
    return metal_clear_depth_stencil(&submission, texture, clear);
}

int gpu_ir_metal_execute_stream(gpu_ir_metal_device *device, const gpu_ir_stream *stream) {
    if (!device || !device->device || !device->queue || !stream) return GPU_IR_INVALID;
    if (!gpu_ir_stream_count(stream)) return GPU_IR_OK;
    gpu_ir_metal_submission submission = { .device = device, .upload_buffers = [NSMutableArray array] };
    submission.command_buffer = [device->queue commandBuffer];
    if (!submission.command_buffer) return GPU_IR_UNSUPPORTED;
    gpu_ir_backend backend = { .context = &submission, .copy_resource = metal_copy_resource,
                               .copy_buffer = metal_copy_buffer, .update_buffer = metal_update_buffer,
                               .fill_buffer = metal_fill_buffer,
                               .fill_buffer_pattern = metal_fill_buffer_pattern,
                               .copy_texture = metal_copy_texture,
                               .barrier = metal_barrier,
                               .dispatch = metal_dispatch, .dispatch_indirect = metal_dispatch_indirect,
                               .bind_descriptor_state = metal_bind_descriptor_state,
                               .clear_color = metal_clear_color,
                               .clear_depth_stencil = metal_clear_depth_stencil,
                               .begin_render_pass = metal_begin_render_pass,
                               .end_render_pass = metal_end_render_pass,
                               .draw = metal_draw, .draw_indirect = metal_draw_indirect };
    for (uint32_t i = 0; i < gpu_ir_stream_count(stream); i++) {
        int rc = gpu_ir_execute(stream, i, &backend);
        if (rc != GPU_IR_OK) {
            end_render_encoder(&submission);
            end_compute_encoder(&submission);
            end_blit_encoder(&submission);
            return rc;
        }
    }
    end_render_encoder(&submission);
    end_compute_encoder(&submission);
    end_blit_encoder(&submission);
    [submission.command_buffer commit];
    [submission.command_buffer waitUntilCompleted];
    return submission.command_buffer.status == MTLCommandBufferStatusCompleted ? GPU_IR_OK : GPU_IR_UNSUPPORTED;
}

static const char *g_metal_present_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct V { float4 pos [[position]]; float2 uv; };\n"
    "vertex V present_vs(uint id [[vertex_id]]) {\n"
    "    float2 p = float2((id << 1) & 2, id & 2);\n"
    "    V o; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;\n"
    "}\n"
    "fragment float4 present_fs(V in [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    return float4(t.sample(s, in.uv).rgb, 1);\n"
    "}\n";

static id<MTLRenderPipelineState> metal_present_pipeline(gpu_ir_metal_device *device) {
    if (device->present_pipeline) return device->present_pipeline;
    NSError *error = nil;
    id<MTLLibrary> library = [device->device newLibraryWithSource:@(g_metal_present_msl) options:nil error:&error];
    if (!library) {
        const char *message = error.localizedDescription.UTF8String;
        fprintf(stderr, "VXRuntime: Metal present shader compilation failed: %s\n",
                message ? message : "unknown Metal compiler error");
        return nil;
    }
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"present_vs"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"present_fs"];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> pipeline = [device->device newRenderPipelineStateWithDescriptor:descriptor
                                                                                           error:&error];
    if (!pipeline) {
        const char *message = error.localizedDescription.UTF8String;
        fprintf(stderr, "VXRuntime: Metal present pipeline creation failed: %s\n",
                message ? message : "unknown Metal pipeline error");
        return nil;
    }
    device->present_pipeline = pipeline;
    return pipeline;
}

int gpu_ir_metal_present_texture(gpu_ir_metal_device *device, const gpu_ir_resource *resource,
                                 void *metal_layer, int display_sync_enabled) {
    const gpu_ir_texture_desc *description = gpu_ir_texture_description(resource);
    if (!device || !device->device || !device->queue || !description || !metal_layer ||
        description->format != GPU_IR_FORMAT_BGRA8_UNORM || description->is_3d ||
        description->array_layers != 1 || description->mip_levels != 1 || description->sample_count != 1)
        return GPU_IR_INVALID;
    CAMetalLayer *layer = (__bridge CAMetalLayer *)metal_layer;
    if (layer.device != device->device) return GPU_IR_INVALID;
    id<MTLTexture> source = (__bridge id<MTLTexture>)gpu_ir_backend_object(resource);
    if (!source || source.pixelFormat != MTLPixelFormatBGRA8Unorm ||
        !(source.usage & MTLTextureUsageShaderRead)) return GPU_IR_INVALID;

    pthread_mutex_lock(&device->present_pipeline_lock);
    id<MTLRenderPipelineState> pipeline = metal_present_pipeline(device);
    pthread_mutex_unlock(&device->present_pipeline_lock);
    if (!pipeline) return GPU_IR_UNSUPPORTED;
    layer.displaySyncEnabled = display_sync_enabled != 0;
    @autoreleasepool {
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable) return GPU_IR_RANGE;
        id<MTLCommandBuffer> command_buffer = [device->queue commandBuffer];
        if (!command_buffer) return GPU_IR_UNSUPPORTED;
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
        if (!encoder) return GPU_IR_UNSUPPORTED;
        [encoder setRenderPipelineState:pipeline];
        [encoder setFragmentTexture:source atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        [command_buffer presentDrawable:drawable];
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        return command_buffer.status == MTLCommandBufferStatusCompleted ? GPU_IR_OK : GPU_IR_UNSUPPORTED;
    }
}
