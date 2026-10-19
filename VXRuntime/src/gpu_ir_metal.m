// Conservative Metal backend for the API-neutral compute pipeline and command stream.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "gpu_ir_metal.h"
#include "gpu_ir_msl.h"
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct gpu_ir_metal_device {
    __strong id<MTLDevice> device;
    __strong id<MTLCommandQueue> queue;
    __strong NSMutableDictionary *clear_pipeline_cache;
    pthread_mutex_t clear_pipeline_lock;
};

typedef struct {
    gpu_ir_metal_device *device;
    __strong id<MTLCommandBuffer> command_buffer;
    __strong id<MTLComputeCommandEncoder> compute_encoder;
    __strong id<MTLBlitCommandEncoder> blit_encoder;
    __strong NSMutableArray *upload_buffers;
    const gpu_ir_descriptor_state *descriptors;
} gpu_ir_metal_submission;

static void release_metal_object(void *object) {
    if (object) CFRelease(object);
}

static MTLPixelFormat metal_pixel_format(uint32_t format) {
    switch (format) {
        case GPU_IR_FORMAT_R8_UNORM: return MTLPixelFormatR8Unorm;
        case GPU_IR_FORMAT_RG8_UNORM: return MTLPixelFormatRG8Unorm;
        case GPU_IR_FORMAT_RGBA8_UNORM: return MTLPixelFormatRGBA8Unorm;
        case GPU_IR_FORMAT_BGRA8_UNORM: return MTLPixelFormatBGRA8Unorm;
        case GPU_IR_FORMAT_R16G16B16A16_FLOAT: return MTLPixelFormatRGBA16Float;
        case GPU_IR_FORMAT_R32_FLOAT: return MTLPixelFormatR32Float;
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
        case GPU_IR_FORMAT_R32_FLOAT: bytes_per_pixel = 4; break;
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
    pthread_mutex_destroy(&device->clear_pipeline_lock);
    device->clear_pipeline_cache = nil;
    device->queue = nil;
    device->device = nil;
    free(device);
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
    uint64_t required_size = row_pitch * description->height;
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
    descriptor.mipmapLevelCount = 1;
    descriptor.sampleCount = 1;
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = (shader_read ? MTLTextureUsageShaderRead : MTLTextureUsageUnknown) |
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
    descriptor.maxAnisotropy = 1;
    descriptor.borderColor = border_color;
    id<MTLSamplerState> sampler = [device->device newSamplerStateWithDescriptor:descriptor];
    return sampler ? (__bridge_retained void *)sampler : NULL;
}

void gpu_ir_metal_sampler_retain(void *sampler) {
    if (sampler) CFRetain(sampler);
}

void gpu_ir_metal_sampler_release(void *sampler) {
    if (sampler) CFRelease(sampler);
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
    id<MTLLibrary> library = [metal_device newLibraryWithSource:source options:nil error:&metal_error];
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

static void end_compute_encoder(gpu_ir_metal_submission *submission) {
    if (!submission || !submission->compute_encoder) return;
    [submission->compute_encoder endEncoding];
    submission->compute_encoder = nil;
}

static void end_blit_encoder(gpu_ir_metal_submission *submission) {
    if (!submission || !submission->blit_encoder) return;
    [submission->blit_encoder endEncoding];
    submission->blit_encoder = nil;
}

static int encode_descriptor_state(gpu_ir_metal_submission *submission,
                                   const gpu_ir_descriptor_state *state);

static int ensure_compute_encoder(gpu_ir_metal_submission *submission) {
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

static int metal_bind_descriptor_state(void *context, const gpu_ir_descriptor_state *state) {
    gpu_ir_metal_submission *submission = context;
    if (!submission || !state) return GPU_IR_INVALID;
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
                                                        int depth_stencil) {
    if (!device || !device->device || !texture || !texture.sampleCount) return nil;
    NSString *kind = depth_stencil ? @"depth" : @"color";
    NSString *key = [NSString stringWithFormat:@"%@:%lu:%lu", kind, (unsigned long)texture.pixelFormat,
                     (unsigned long)texture.sampleCount];
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
        NSString *return_type = value_kind == 1 ? @"uint4" : value_kind == 2 ? @"int4" : @"float4";
        NSString *expression = value_kind == 1 ? @"uint4(max(clear_value, float4(0.0f)))" :
                               value_kind == 2 ? @"int4(clear_value)" : @"clear_value";
        source = [NSString stringWithFormat:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "vertex float4 vx_clear_vs(uint id [[vertex_id]]) { "
             "constexpr float2 p[3] = { float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f) }; "
             "return float4(p[id], 0.0f, 1.0f); }\n"
             "fragment %@ vx_clear_fs(constant float4& clear_value [[buffer(0)]]) { return %@; }\n",
             return_type, expression];
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
    if (!submission || !submission->device || !submission->command_buffer || !description || !texture || !clear ||
        (clear->rectangle_count && !clear->rectangles) || description->width != texture.width ||
        description->height != texture.height || description->sample_count != texture.sampleCount) return GPU_IR_INVALID;
    end_compute_encoder(submission);
    end_blit_encoder(submission);
    if (!clear->rectangle_count) {
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(clear->color[0], clear->color[1],
                                                                clear->color[2], clear->color[3]);
        id<MTLRenderCommandEncoder> encoder = [submission->command_buffer renderCommandEncoderWithDescriptor:pass];
        if (!encoder) return GPU_IR_UNSUPPORTED;
        [encoder endEncoding];
        return GPU_IR_OK;
    }
    id<MTLRenderPipelineState> pipeline = metal_clear_pipeline(submission->device, texture, 0);
    if (!pipeline) return GPU_IR_UNSUPPORTED;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [submission->command_buffer renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return GPU_IR_UNSUPPORTED;
    [encoder setRenderPipelineState:pipeline];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setViewport:(MTLViewport){ 0, 0, (double)texture.width, (double)texture.height, 0, 1 }];
    [encoder setFragmentBytes:clear->color length:sizeof clear->color atIndex:0];
    for (uint32_t i = 0; i < clear->rectangle_count; i++) {
        MTLScissorRect scissor;
        if (!metal_clear_rect_bounds(texture, &clear->rectangles[i], &scissor)) continue;
        [encoder setScissorRect:scissor];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [encoder endEncoding];
    return GPU_IR_OK;
}

static int metal_clear_depth_stencil(void *context, const gpu_ir_resource *resource,
                                     const gpu_ir_clear_depth_stencil_info *clear) {
    gpu_ir_metal_submission *submission = context;
    const gpu_ir_texture_desc *description = gpu_ir_texture_description(resource);
    id<MTLTexture> texture = (__bridge id<MTLTexture>)gpu_ir_backend_object(resource);
    if (!submission || !submission->device || !submission->command_buffer || !description || !texture || !clear ||
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
    id<MTLRenderPipelineState> pipeline = metal_clear_pipeline(submission->device, texture, 1);
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
                               .clear_depth_stencil = metal_clear_depth_stencil };
    for (uint32_t i = 0; i < gpu_ir_stream_count(stream); i++) {
        int rc = gpu_ir_execute(stream, i, &backend);
        if (rc != GPU_IR_OK) {
            end_compute_encoder(&submission);
            end_blit_encoder(&submission);
            return rc;
        }
    }
    end_compute_encoder(&submission);
    end_blit_encoder(&submission);
    [submission.command_buffer commit];
    [submission.command_buffer waitUntilCompleted];
    return submission.command_buffer.status == MTLCommandBufferStatusCompleted ? GPU_IR_OK : GPU_IR_UNSUPPORTED;
}
