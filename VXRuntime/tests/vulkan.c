#include "vx_vulkan.h"
#include "spirv.h"
#include "spirv_ir.h"
#include "ui.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef VX_VULKAN_TEST_HOOKS
VkResult vx_vk_test_specialize_shader(const spirv_module *module, const VkSpecializationInfo *info,
                                      const gpu_ir_shader *shader, gpu_ir_shader **specialized_shader);
VkResult vx_vk_test_select_present_result(const VkResult *results, uint32_t count);
int vx_vk_test_build_color_blend(const VkPipelineColorBlendStateCreateInfo *source,
                                 gpu_ir_color_blend_state *destination);
int vx_vk_test_color_blend_uses_constants(const gpu_ir_color_blend_state *state);
int vx_vk_test_resolve_draw_blend_constants(uint32_t dynamic_enabled, uint32_t dynamic_set,
    const gpu_ir_color_blend_state *blend, const float dynamic_values[4], uint32_t *has_constants,
    float constants[4]);
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "vulkan bootstrap check failed at %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

typedef struct {
    VkQueue queue;
    VkFence fence;
    VkResult result;
} delayed_fence_submit;

static void *submit_empty_batch_after_delay(void *context) {
    delayed_fence_submit *submit = context;
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 20000000 };
    nanosleep(&delay, NULL);
    submit->result = vkQueueSubmit(submit->queue, 0, NULL, submit->fence);
    return NULL;
}

static uint64_t monotonic_time_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static vx_window surface_test_window;
static const HANDLE surface_test_hwnd = (HANDLE)(uintptr_t)0x1234;

vx_window *vx_window_get(HANDLE hwnd) {
    return hwnd == surface_test_hwnd && !surface_test_window.destroyed ? &surface_test_window : NULL;
}

void *vx_cocoa_metal_layer_bgra(vx_window *window, void *metal_device) {
    (void)window;
    (void)metal_device;
    return NULL;
}

static int test_win32_surface_queries(VkInstance instance, VkPhysicalDevice physical) {
    memset(&surface_test_window, 0, sizeof surface_test_window);
    surface_test_window.hwnd = surface_test_hwnd;
    surface_test_window.native = (void *)(uintptr_t)1;
    surface_test_window.client_w = 320;
    surface_test_window.client_h = 240;
    VkWin32SurfaceCreateInfoKHR create_info = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .hinstance = (HANDLE)(uintptr_t)0x5678,
        .hwnd = surface_test_hwnd
    };
    VkSurfaceKHR surface = 0;
    int failed = vkCreateWin32SurfaceKHR(instance, &create_info, NULL, &surface) != VK_SUCCESS || !surface;
    if (!failed) {
        failed |= vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR") !=
                  (PFN_vkVoidFunction)vkCreateWin32SurfaceKHR;
        failed |= vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceSupportKHR") !=
                  (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceSupportKHR;
        VkBool32 supported = 0;
        failed |= vkGetPhysicalDeviceSurfaceSupportKHR(physical, 0, surface, &supported) != VK_SUCCESS || !supported;
        failed |= vkGetPhysicalDeviceSurfaceSupportKHR(physical, 1, surface, &supported) !=
                  VK_ERROR_INITIALIZATION_FAILED;
        failed |= !vkGetPhysicalDeviceWin32PresentationSupportKHR(physical, 0);
        VkSurfaceCapabilitiesKHR capabilities = { 0 };
        failed |= vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities) != VK_SUCCESS ||
                  capabilities.minImageCount != 2 || capabilities.maxImageCount != 3 ||
                  capabilities.minImageExtent.width != 1 || capabilities.maxImageExtent.width != 4096 ||
                  capabilities.maxImageArrayLayers != 1 ||
                  capabilities.supportedTransforms != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
                  capabilities.supportedCompositeAlpha != VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ||
                  capabilities.supportedUsageFlags != VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        uint32_t count = 0;
        failed |= vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, NULL) != VK_SUCCESS || count != 1;
        VkSurfaceFormatKHR format = { 0 };
        count = 1;
        failed |= vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, &format) != VK_SUCCESS || count != 1 ||
                  format.format != VK_FORMAT_B8G8R8A8_UNORM || format.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        count = 0;
        failed |= vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, NULL) != VK_SUCCESS || count != 1;
        VkPresentModeKHR mode = 0;
        count = 1;
        failed |= vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, &mode) != VK_SUCCESS || count != 1 ||
                  mode != VK_PRESENT_MODE_FIFO_KHR;
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = 0,
            .queueCount = 1,
            .pQueuePriorities = &priority
        };
        const char *device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceCreateInfo device_info = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queue_info,
            .enabledExtensionCount = 1,
            .ppEnabledExtensionNames = device_extensions
        };
        VkDevice device = NULL;
        failed |= vkCreateDevice(physical, &device_info, NULL, &device) != VK_SUCCESS || !device;
        VkSwapchainKHR swapchain = 0;
        VkSwapchainCreateInfoKHR swapchain_info = {
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = surface,
            .minImageCount = 2,
            .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
            .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
            .imageExtent = { 320, 240 },
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = VK_PRESENT_MODE_FIFO_KHR,
            .clipped = 1
        };
        if (!failed) {
            failed |= vkGetDeviceProcAddr(device, "vkCreateSwapchainKHR") !=
                      (PFN_vkVoidFunction)vkCreateSwapchainKHR;
            failed |= vkGetDeviceProcAddr(device, "vkQueuePresentKHR") !=
                      (PFN_vkVoidFunction)vkQueuePresentKHR;
            failed |= vkCreateSwapchainKHR(device, &swapchain_info, NULL, &swapchain) != VK_SUCCESS || !swapchain;
        }
        if (swapchain) {
            uint32_t image_count = 0;
            failed |= vkGetSwapchainImagesKHR(device, swapchain, &image_count, NULL) != VK_SUCCESS || image_count != 2;
            VkImage images[3] = { 0 };
            image_count = 1;
            failed |= vkGetSwapchainImagesKHR(device, swapchain, &image_count, images) != VK_INCOMPLETE ||
                      image_count != 1 || !images[0];
            image_count = 3;
            failed |= vkGetSwapchainImagesKHR(device, swapchain, &image_count, images) != VK_SUCCESS ||
                      image_count != 2 || !images[0] || !images[1];
            VkSemaphore acquire_semaphore = 0, spare_semaphore = 0;
            VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            failed |= vkCreateSemaphore(device, &semaphore_info, NULL, &acquire_semaphore) != VK_SUCCESS;
            failed |= vkCreateSemaphore(device, &semaphore_info, NULL, &spare_semaphore) != VK_SUCCESS;
            VkFence acquire_fence = 0;
            VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            failed |= vkCreateFence(device, &fence_info, NULL, &acquire_fence) != VK_SUCCESS;
            uint32_t first_index = UINT32_MAX, second_index = UINT32_MAX;
            if (acquire_semaphore)
                failed |= vkAcquireNextImageKHR(device, swapchain, 0, acquire_semaphore, 0,
                    &first_index) != VK_SUCCESS || first_index >= 2;
            if (acquire_fence)
                failed |= vkAcquireNextImageKHR(device, swapchain, 0, 0, acquire_fence,
                    &second_index) != VK_SUCCESS || second_index >= 2 || second_index == first_index ||
                    vkGetFenceStatus(device, acquire_fence) != VK_SUCCESS;
            if (spare_semaphore) {
                uint32_t unused_index = UINT32_MAX;
                failed |= vkAcquireNextImageKHR(device, swapchain, 0, spare_semaphore, 0,
                    &unused_index) != VK_NOT_READY;
                failed |= vkAcquireNextImageKHR(device, swapchain, 0, 0, 0,
                    &unused_index) != VK_ERROR_INITIALIZATION_FAILED;
            }
            VkQueue queue = NULL;
            vkGetDeviceQueue(device, 0, 0, &queue);
            VkPresentInfoKHR invalid_present = {
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .waitSemaphoreCount = acquire_semaphore ? 1u : 0u,
                .pWaitSemaphores = acquire_semaphore ? &acquire_semaphore : NULL,
                .swapchainCount = 1,
                .pSwapchains = &swapchain,
                .pImageIndices = &first_index
            };
            if (queue && acquire_semaphore && first_index < 2)
                failed |= vkQueuePresentKHR(queue, &invalid_present) != VK_ERROR_INITIALIZATION_FAILED;

            VkSwapchainKHR competing_swapchain = 0;
            failed |= vkCreateSwapchainKHR(device, &swapchain_info, NULL, &competing_swapchain) !=
                      VK_ERROR_NATIVE_WINDOW_IN_USE_KHR || competing_swapchain != 0;
            VkSwapchainCreateInfoKHR replacement_info = swapchain_info;
            replacement_info.oldSwapchain = swapchain;
            replacement_info.imageExtent.width++;
            VkSwapchainKHR replacement = 0;
            failed |= vkCreateSwapchainKHR(device, &replacement_info, NULL, &replacement) !=
                      VK_ERROR_INITIALIZATION_FAILED || replacement != 0;
            if (spare_semaphore) {
                uint32_t retired_index = UINT32_MAX;
                failed |= vkAcquireNextImageKHR(device, swapchain, 0, spare_semaphore, 0,
                    &retired_index) != VK_ERROR_OUT_OF_DATE_KHR;
            }
            if (acquire_semaphore) vkDestroySemaphore(device, acquire_semaphore, NULL);
            if (spare_semaphore) vkDestroySemaphore(device, spare_semaphore, NULL);
            if (acquire_fence) vkDestroyFence(device, acquire_fence, NULL);
            vkDestroySwapchainKHR(device, swapchain, NULL);
        }
        if (device) vkDestroyDevice(device, NULL);
        vkDestroySurfaceKHR(instance, surface, NULL);
    }
    surface_test_window.destroyed = 1;
    return failed;
}

static vx_export registered[256];
static size_t registered_count;
static int allocations, frees;

void vx_register_module(const char *dll, const vx_export *exports, size_t count) {
    if (strcmp(dll, "vulkan-1.dll") != 0) abort();
    if (count > sizeof registered / sizeof registered[0]) abort();
    memcpy(registered, exports, count * sizeof *exports);
    registered_count = count;
}

static void *test_allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    if (user != (void *)0x1234 || alignment < 8 || scope != VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE) abort();
    allocations++;
    return malloc(size);
}

static void test_free(void *user, void *memory) {
    if (user != (void *)0x1234) abort();
    frees++;
    free(memory);
}

static uint8_t *read_fixture(const char *path, size_t *size) {
    *size = 0;
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) || ftell(file) <= 0) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return NULL; }
    uint8_t *bytes = malloc((size_t)length);
    if (!bytes) { fclose(file); return NULL; }
    if (fread(bytes, 1, (size_t)length, file) != (size_t)length || fclose(file)) {
        free(bytes);
        return NULL;
    }
    *size = (size_t)length;
    return bytes;
}

static const gpu_ir_shader_instruction *find_shader_instruction_result(const gpu_ir_shader *shader,
                                                                       uint32_t result_id) {
    if (!shader) return NULL;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (instruction && instruction->result_id == result_id) return instruction;
    }
    return NULL;
}

static int insert_spirv_instruction(uint32_t *words, size_t capacity, size_t *word_count,
                                    uint16_t before_opcode, uint16_t opcode, uint16_t instruction_words,
                                    const uint32_t *operands) {
    if (!words || !word_count || !instruction_words || !operands || *word_count > capacity ||
        instruction_words > capacity - *word_count) return 0;
    for (size_t offset = 5; offset < *word_count;) {
        uint32_t current = words[offset];
        uint32_t current_words = current >> 16;
        if (!current_words || current_words > *word_count - offset) return 0;
        if ((current & 0xffffu) == before_opcode) {
            memmove(words + offset + instruction_words, words + offset,
                    (*word_count - offset) * sizeof *words);
            words[offset] = ((uint32_t)instruction_words << 16) | opcode;
            memcpy(words + offset + 1, operands, (instruction_words - 1u) * sizeof *words);
            *word_count += instruction_words;
            return 1;
        }
        offset += current_words;
    }
    return 0;
}

static uint32_t *make_specialized_vertex_fixture(const uint8_t *base_bytes, size_t base_size,
                                                  size_t *specialized_size, uint32_t ids[12]) {
    if (!base_bytes || !specialized_size || base_size < 5 * sizeof(uint32_t) ||
        (base_size & (sizeof(uint32_t) - 1u)) || !ids) return NULL;
    size_t base_words = base_size / sizeof(uint32_t);
    size_t capacity = base_words + 96;
    uint32_t *words = calloc(capacity, sizeof *words);
    if (!words) return NULL;
    memcpy(words, base_bytes, base_size);
    size_t word_count = base_words;
    if (words[0] != SPIRV_MAGIC || words[3] > UINT32_MAX - 12u) {
        free(words);
        return NULL;
    }
    uint32_t next_id = words[3];
    for (size_t i = 0; i < 12; i++) ids[i] = next_id++;
    const uint32_t spec_id_decoration[] = { ids[0], 1, 7 };
    const uint32_t spec_shift_id_decoration[] = { ids[1], 1, 8 };
    const uint32_t spec_divisor_id_decoration[] = { ids[2], 1, 9 };
    const uint32_t spec_uint[] = { 3, ids[0], 1 };
    const uint32_t spec_shift_amount[] = { 3, ids[1], 1 };
    const uint32_t spec_divisor[] = { 3, ids[2], 2 };
    const uint32_t spec_add[] = { 3, ids[3], 128, ids[0], 11 };
    const uint32_t spec_or[] = { 3, ids[4], 197, ids[3], 11 };
    const uint32_t spec_xor[] = { 3, ids[5], 198, ids[4], 11 };
    const uint32_t spec_and[] = { 3, ids[6], 199, ids[5], ids[3] };
    const uint32_t spec_shift_right_logical[] = { 3, ids[7], 194, ids[0], ids[1] };
    const uint32_t spec_shift_right_arithmetic[] = { 3, ids[8], 195, ids[0], ids[1] };
    const uint32_t spec_shift_left_logical[] = { 3, ids[9], 196, ids[0], ids[1] };
    const uint32_t spec_unsigned_divide[] = { 3, ids[10], 134, ids[0], ids[2] };
    const uint32_t spec_unsigned_modulo[] = { 3, ids[11], 137, ids[0], ids[2] };
    if (
        !insert_spirv_instruction(words, capacity, &word_count, 19, 71, 4, spec_id_decoration) ||
        !insert_spirv_instruction(words, capacity, &word_count, 19, 71, 4, spec_shift_id_decoration) ||
        !insert_spirv_instruction(words, capacity, &word_count, 19, 71, 4, spec_divisor_id_decoration) ||
        !insert_spirv_instruction(words, capacity, &word_count, 54, 50, 4, spec_uint) ||
        !insert_spirv_instruction(words, capacity, &word_count, 54, 50, 4, spec_shift_amount) ||
        !insert_spirv_instruction(words, capacity, &word_count, 54, 50, 4, spec_divisor) ||
        !insert_spirv_instruction(words, capacity, &word_count, 54, 52, 6, spec_add) ||
        !insert_spirv_instruction(words, capacity, &word_count, 54, 52, 6, spec_or) ||
        !insert_spirv_instruction(words, capacity, &word_count, 54, 52, 6, spec_xor) ||
        !insert_spirv_instruction(words, capacity, &word_count, 54, 52, 6, spec_and) ||
        !insert_spirv_instruction(words, capacity, &word_count, 54, 52, 6, spec_shift_right_logical) ||
        !insert_spirv_instruction(words, capacity, &word_count, 54, 52, 6, spec_shift_right_arithmetic) ||
        !insert_spirv_instruction(words, capacity, &word_count, 54, 52, 6, spec_shift_left_logical) ||
        !insert_spirv_instruction(words, capacity, &word_count, 54, 52, 6, spec_unsigned_divide) ||
        !insert_spirv_instruction(words, capacity, &word_count, 54, 52, 6, spec_unsigned_modulo)) {
        free(words);
        return NULL;
    }
    words[3] = next_id;
    *specialized_size = word_count * sizeof *words;
    return words;
}

static int check_specialized_vertex_fixture_cpu(const char *path) {
    size_t base_size = 0, specialized_size = 0;
    uint8_t *base_bytes = read_fixture(path, &base_size);
    if (!base_bytes || (base_size & 3u)) {
        fprintf(stderr, "vulkan specialization fixture: failed to read an aligned input module\n");
        free(base_bytes);
        return 1;
    }
    uint32_t specialization_ids[12];
    uint32_t *words = make_specialized_vertex_fixture(base_bytes, base_size, &specialized_size,
                                                       specialization_ids);
    free(base_bytes);
    if (!words) {
        fprintf(stderr, "vulkan specialization fixture: failed to build the derived-constant module\n");
        return 1;
    }

    spirv_module module = { 0 };
    int rc = spirv_read(words, specialized_size, &module);
    free(words);
    if (rc != SPIRV_OK) {
        fprintf(stderr, "vulkan specialization fixture: SPIR-V reader rejected the module (%d)\n", rc);
        return 1;
    }
    const spirv_entry_point *entry = spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_VERTEX);
    gpu_ir_shader *shader = NULL;
    rc = entry ? spirv_build_gpu_ir_shader(&module, entry, &shader) : GPU_IR_INVALID;
    if (rc != GPU_IR_OK || !shader) {
        fprintf(stderr, "vulkan specialization fixture: common shader construction failed (%d)\n", rc);
        gpu_ir_shader_release(shader);
        spirv_free(&module);
        return 1;
    }

    const gpu_ir_shader_instruction *constant = find_shader_instruction_result(shader, specialization_ids[0]);
    int valid_default = constant && (constant->flags & GPU_IR_SHADER_FLAG_SPECIALIZATION) &&
        constant->literal_word_count == 1 &&
        gpu_ir_shader_literal_word_at(shader, constant->first_literal) == 1;
    constant = find_shader_instruction_result(shader, specialization_ids[1]);
    valid_default &= constant && (constant->flags & GPU_IR_SHADER_FLAG_SPECIALIZATION) &&
        constant->literal_word_count == 1 &&
        gpu_ir_shader_literal_word_at(shader, constant->first_literal) == 1;
    const gpu_ir_shader_instruction *derived = find_shader_instruction_result(shader, specialization_ids[3]);
    valid_default &= derived && (derived->flags & GPU_IR_SHADER_FLAG_SPECIALIZATION) &&
        derived->literal_word_count == 1 && gpu_ir_shader_literal_word_at(shader, derived->first_literal) == 2;
    const uint32_t derived_id_indices[] = { 4, 5, 6, 7, 8, 9, 10, 11 };
    const uint32_t default_values[] = { 3, 2, 2, 0, 0, 2, 0, 1 };
    for (size_t i = 0; i < sizeof derived_id_indices / sizeof derived_id_indices[0]; i++) {
        derived = find_shader_instruction_result(shader, specialization_ids[derived_id_indices[i]]);
        valid_default &= derived && (derived->flags & GPU_IR_SHADER_FLAG_SPECIALIZATION) &&
            derived->literal_word_count == 1 &&
            gpu_ir_shader_literal_word_at(shader, derived->first_literal) == default_values[i];
    }
    uint32_t override_values[] = { 3, 1, 3 };
    VkSpecializationMapEntry map_entries[] = {
        { .constantID = 7, .offset = 0, .size = sizeof override_values[0] },
        { .constantID = 8, .offset = sizeof override_values[0], .size = sizeof override_values[1] },
        { .constantID = 9, .offset = 2 * sizeof override_values[0], .size = sizeof override_values[2] }
    };
    VkSpecializationInfo specialization_info = {
        .mapEntryCount = sizeof map_entries / sizeof map_entries[0], .pMapEntries = map_entries,
        .dataSize = sizeof override_values, .pData = override_values
    };
    gpu_ir_shader *specialized = NULL;
    VkResult specialization_result = valid_default ?
        vx_vk_test_specialize_shader(&module, &specialization_info, shader, &specialized) : VK_ERROR_UNKNOWN;
    if (specialization_result != VK_SUCCESS || !specialized) {
        fprintf(stderr, "vulkan specialization fixture: defaults valid=%d, specialization failed (%d)\n",
                valid_default, specialization_result);
        gpu_ir_shader_release(shader);
        gpu_ir_shader_release(specialized);
        spirv_free(&module);
        return 1;
    }
    constant = find_shader_instruction_result(specialized, specialization_ids[0]);
    int valid_override = constant && !(constant->flags & GPU_IR_SHADER_FLAG_SPECIALIZATION) &&
        constant->literal_word_count == 1 &&
        gpu_ir_shader_literal_word_at(specialized, constant->first_literal) == override_values[0];
    constant = find_shader_instruction_result(specialized, specialization_ids[1]);
    valid_override &= constant && !(constant->flags & GPU_IR_SHADER_FLAG_SPECIALIZATION) &&
        constant->literal_word_count == 1 &&
        gpu_ir_shader_literal_word_at(specialized, constant->first_literal) == override_values[1];
    derived = find_shader_instruction_result(specialized, specialization_ids[3]);
    valid_override &= derived && !(derived->flags & GPU_IR_SHADER_FLAG_SPECIALIZATION) &&
        derived->literal_word_count == 1 &&
        gpu_ir_shader_literal_word_at(specialized, derived->first_literal) == 4;
    const uint32_t specialized_values[] = { 5, 4, 4, 1, 1, 6, 1, 0 };
    for (size_t i = 0; i < sizeof derived_id_indices / sizeof derived_id_indices[0]; i++) {
        derived = find_shader_instruction_result(specialized, specialization_ids[derived_id_indices[i]]);
        valid_override &= derived && !(derived->flags & GPU_IR_SHADER_FLAG_SPECIALIZATION) &&
            derived->literal_word_count == 1 &&
            gpu_ir_shader_literal_word_at(specialized, derived->first_literal) == specialized_values[i];
    }
    uint32_t poison_values[] = { 3, 32, 3 };
    VkSpecializationInfo poison_info = specialization_info;
    poison_info.pData = poison_values;
    gpu_ir_shader *poison_variant = NULL;
    VkResult poison_result = vx_vk_test_specialize_shader(&module, &poison_info, shader, &poison_variant);
    int rejected_poison = poison_result == VK_ERROR_FEATURE_NOT_PRESENT && !poison_variant;
    gpu_ir_shader_release(poison_variant);
    uint32_t zero_divisor_values[] = { 3, 1, 0 };
    poison_info.pData = zero_divisor_values;
    poison_variant = NULL;
    VkResult zero_divisor_result = vx_vk_test_specialize_shader(&module, &poison_info, shader, &poison_variant);
    rejected_poison &= zero_divisor_result == VK_ERROR_FEATURE_NOT_PRESENT && !poison_variant;
    gpu_ir_shader_release(poison_variant);
    gpu_ir_shader_release(shader);
    gpu_ir_shader_release(specialized);
    spirv_free(&module);
    if (!valid_override || !rejected_poison) {
        fprintf(stderr, "vulkan specialization fixture: override valid=%d, undefined values rejected=%d\n",
                valid_override, rejected_poison);
        return 1;
    }
    puts("vulkan graphics specialization: scalar SpecId and arithmetic/division/bitwise/shift OpSpecConstantOp resolve into an immutable common shader variant: ok");
    return 0;
}

static int run_compute(VkDevice device, VkQueue queue, VkCommandBuffer command_buffer, VkFence fence,
    const char *shader_path) {
    enum { ELEMENTS = 64, BUFFER_BYTES = ELEMENTS * sizeof(uint32_t) };
    size_t shader_size = 0;
    uint8_t *shader_bytes = read_fixture(shader_path, &shader_size);
    CHECK(shader_bytes != NULL && !(shader_size & 3u));
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader_size,
        .pCode = (const uint32_t *)shader_bytes
    };
    CHECK(vkGetDeviceProcAddr(device, "vkCreateShaderModule") == (PFN_vkVoidFunction)vkCreateShaderModule);
    CHECK(vkGetDeviceProcAddr(device, "vkCreateDescriptorSetLayout") == (PFN_vkVoidFunction)vkCreateDescriptorSetLayout);
    CHECK(vkGetDeviceProcAddr(device, "vkCreatePipelineLayout") == (PFN_vkVoidFunction)vkCreatePipelineLayout);
    CHECK(vkGetDeviceProcAddr(device, "vkCreateComputePipelines") == (PFN_vkVoidFunction)vkCreateComputePipelines);
    CHECK(vkGetDeviceProcAddr(device, "vkAllocateDescriptorSets") == (PFN_vkVoidFunction)vkAllocateDescriptorSets);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdBindDescriptorSets") == (PFN_vkVoidFunction)vkCmdBindDescriptorSets);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdClearColorImage") == (PFN_vkVoidFunction)vkCmdClearColorImage);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdDispatch") == (PFN_vkVoidFunction)vkCmdDispatch);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdDispatchIndirect") == (PFN_vkVoidFunction)vkCmdDispatchIndirect);
    VkShaderModule shader = 0;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader) == VK_SUCCESS && shader != 0);
    free(shader_bytes);

    VkDescriptorSetLayoutBinding descriptor_binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
    };
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &descriptor_binding
    };
    VkDescriptorSetLayout set_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout) == VK_SUCCESS && set_layout != 0);
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout
    };
    VkPipelineLayout pipeline_layout = 0;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout) == VK_SUCCESS && pipeline_layout != 0);
    uint32_t specialized_group_size_x = 16;
    VkSpecializationMapEntry workgroup_size_map = { .constantID = 0, .offset = 0, .size = sizeof(uint32_t) };
    VkSpecializationInfo workgroup_specialization = {
        .mapEntryCount = 1, .pMapEntries = &workgroup_size_map,
        .dataSize = sizeof specialized_group_size_x, .pData = &specialized_group_size_x
    };
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader,
            .pName = "main",
            .pSpecializationInfo = &workgroup_specialization
        },
        .layout = pipeline_layout,
        .basePipelineIndex = -1
    };
    VkSpecializationMapEntry duplicate_maps[2] = {
        { .constantID = 0, .offset = 0, .size = sizeof(uint32_t) },
        { .constantID = 0, .offset = sizeof(uint32_t), .size = sizeof(uint32_t) }
    };
    uint32_t duplicate_data[2] = { 16, 16 };
    VkSpecializationInfo duplicate_specialization = {
        .mapEntryCount = 2, .pMapEntries = duplicate_maps,
        .dataSize = sizeof duplicate_data, .pData = duplicate_data
    };
    VkComputePipelineCreateInfo invalid_pipeline_info = pipeline_info;
    invalid_pipeline_info.stage.pSpecializationInfo = &duplicate_specialization;
    VkPipeline rejected_pipeline = 0;
    CHECK(vkCreateComputePipelines(device, 0, 1, &invalid_pipeline_info, NULL, &rejected_pipeline) ==
          VK_ERROR_INITIALIZATION_FAILED && rejected_pipeline == 0);
    VkSpecializationMapEntry wrong_size_map = { .constantID = 0, .offset = 0, .size = 2 };
    VkSpecializationInfo wrong_size_specialization = {
        .mapEntryCount = 1, .pMapEntries = &wrong_size_map,
        .dataSize = sizeof specialized_group_size_x, .pData = &specialized_group_size_x
    };
    invalid_pipeline_info.stage.pSpecializationInfo = &wrong_size_specialization;
    CHECK(vkCreateComputePipelines(device, 0, 1, &invalid_pipeline_info, NULL, &rejected_pipeline) ==
          VK_ERROR_INITIALIZATION_FAILED && rejected_pipeline == 0);
    VkSpecializationMapEntry out_of_bounds_map = { .constantID = 0, .offset = sizeof(uint32_t),
                                                    .size = sizeof(uint32_t) };
    VkSpecializationInfo out_of_bounds_specialization = {
        .mapEntryCount = 1, .pMapEntries = &out_of_bounds_map,
        .dataSize = sizeof specialized_group_size_x, .pData = &specialized_group_size_x
    };
    invalid_pipeline_info.stage.pSpecializationInfo = &out_of_bounds_specialization;
    CHECK(vkCreateComputePipelines(device, 0, 1, &invalid_pipeline_info, NULL, &rejected_pipeline) ==
          VK_ERROR_INITIALIZATION_FAILED && rejected_pipeline == 0);
    VkPipeline pipeline = 0;
    CHECK(vkCreateComputePipelines(device, 0, 1, &pipeline_info, NULL, &pipeline) == VK_SUCCESS && pipeline != 0);
    VkSpecializationMapEntry unused_map = { .constantID = 999, .offset = 0, .size = sizeof(uint32_t) };
    VkSpecializationInfo unused_specialization = {
        .mapEntryCount = 1, .pMapEntries = &unused_map,
        .dataSize = sizeof specialized_group_size_x, .pData = &specialized_group_size_x
    };
    VkComputePipelineCreateInfo unused_map_pipeline_info = pipeline_info;
    unused_map_pipeline_info.stage.pSpecializationInfo = &unused_specialization;
    VkPipeline unused_map_pipeline = 0;
    CHECK(vkCreateComputePipelines(device, 0, 1, &unused_map_pipeline_info, NULL, &unused_map_pipeline) ==
          VK_SUCCESS && unused_map_pipeline != 0);
    vkDestroyPipeline(device, unused_map_pipeline, NULL);

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = BUFFER_BYTES,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkBuffer buffer = 0;
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &buffer) == VK_SUCCESS && buffer != 0);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory memory = 0;
    CHECK(vkAllocateMemory(device, &allocation, NULL, &memory) == VK_SUCCESS && memory != 0);
    CHECK(vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS);
    void *mapped = NULL;
    CHECK(vkMapMemory(device, memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    for (uint32_t i = 0; i < ELEMENTS; i++) ((uint32_t *)mapped)[i] = 0x100u + i;
    vkUnmapMemory(device, memory);

    VkDescriptorPoolSize pool_size = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size
    };
    VkDescriptorPool descriptor_pool = 0;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool) == VK_SUCCESS && descriptor_pool != 0);
    VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &set_layout
    };
    VkDescriptorSet descriptor_set = 0;
    CHECK(vkAllocateDescriptorSets(device, &set_info, &descriptor_set) == VK_SUCCESS && descriptor_set != 0);
    VkDescriptorBufferInfo buffer_descriptor = { .buffer = buffer, .range = BUFFER_BYTES };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptor_set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffer_descriptor
    };
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    VkSubmitInfo stale_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &stale_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    vkCmdDispatch(command_buffer, 4, 1, 1);
    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer,
        .size = BUFFER_BYTES
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host_barrier, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    for (uint32_t i = 0; i < ELEMENTS; i++)
        CHECK(((uint32_t *)mapped)[i] == (i == 7 ? 999u : i * 2u + 1u));
    vkUnmapMemory(device, memory);

    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    VkBufferCreateInfo indirect_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 12,
        .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkBuffer indirect_buffer = 0;
    CHECK(vkCreateBuffer(device, &indirect_info, NULL, &indirect_buffer) == VK_SUCCESS && indirect_buffer != 0);
    VkMemoryRequirements indirect_requirements;
    vkGetBufferMemoryRequirements(device, indirect_buffer, &indirect_requirements);
    VkMemoryAllocateInfo indirect_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = indirect_requirements.size,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory indirect_memory = 0;
    CHECK(vkAllocateMemory(device, &indirect_allocation, NULL, &indirect_memory) == VK_SUCCESS && indirect_memory != 0);
    CHECK(vkBindBufferMemory(device, indirect_buffer, indirect_memory, 0) == VK_SUCCESS);
    uint32_t dispatch_groups[3] = { 4, 1, 1 };
    CHECK(vkMapMemory(device, indirect_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    memcpy(mapped, dispatch_groups, sizeof dispatch_groups);
    vkUnmapMemory(device, indirect_memory);

    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    vkCmdDispatchIndirect(command_buffer, indirect_buffer, 0);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host_barrier, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    for (uint32_t i = 0; i < ELEMENTS; i++)
        CHECK(((uint32_t *)mapped)[i] == (i == 7 ? 999u : i * 2u + 1u));
    vkUnmapMemory(device, memory);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    // A descriptor that exposes only element zero must drop later stores, even though the
    // underlying VkBuffer and Metal buffer remain larger.
    const uint32_t sentinel = 0x6a09e667u;
    CHECK(vkMapMemory(device, memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    for (uint32_t i = 0; i < ELEMENTS; i++) ((uint32_t *)mapped)[i] = sentinel + i;
    vkUnmapMemory(device, memory);
    buffer_descriptor.offset = 4 * sizeof(uint32_t);
    buffer_descriptor.range = sizeof(uint32_t);
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    vkCmdDispatch(command_buffer, 4, 1, 1);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host_barrier, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    for (uint32_t i = 0; i < ELEMENTS; i++)
        CHECK(((uint32_t *)mapped)[i] == (i == 4 ? 1u : sentinel + i));
    vkUnmapMemory(device, memory);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkFreeDescriptorSets(device, descriptor_pool, 1, &descriptor_set) == VK_SUCCESS);
    VkSubmitInfo invalid_descriptor_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &invalid_descriptor_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkAllocateDescriptorSets(device, &set_info, &descriptor_set) == VK_SUCCESS && descriptor_set);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetDescriptorPool(device, descriptor_pool, 0) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &invalid_descriptor_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkAllocateDescriptorSets(device, &set_info, &descriptor_set) == VK_SUCCESS && descriptor_set);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    CHECK(vkQueueSubmit(queue, 1, &invalid_descriptor_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    vkDestroyBuffer(device, indirect_buffer, NULL);
    vkFreeMemory(device, indirect_memory, NULL);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, memory, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    puts("vulkan compute: SPIR-V storage-buffer pipeline, direct and indirect dispatch, stale descriptor-set command buffers rejected: ok");
    return 0;
}

static int run_atomic_buffer_shader(VkDevice device, VkQueue queue, VkCommandBuffer command_buffer, VkFence fence,
                                    const char *shader_path, int mode) {
    enum { INVOCATIONS = 64 };
    size_t buffer_words = mode == 2 ? 13u : mode == 1 ? 2u : 1u;
    size_t buffer_bytes = sizeof(uint32_t) * buffer_words;
    size_t shader_size = 0;
    uint8_t *shader_bytes = read_fixture(shader_path, &shader_size);
    CHECK(shader_bytes != NULL && !(shader_size & 3u));
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader_size,
        .pCode = (const uint32_t *)shader_bytes
    };
    VkShaderModule shader = 0;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader) == VK_SUCCESS && shader);
    free(shader_bytes);

    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
    };
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding
    };
    VkDescriptorSetLayout set_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout) == VK_SUCCESS && set_layout);
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout
    };
    VkPipelineLayout pipeline_layout = 0;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout) == VK_SUCCESS && pipeline_layout);
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader,
            .pName = "main"
        },
        .layout = pipeline_layout,
        .basePipelineIndex = -1
    };
    VkPipeline pipeline = 0;
    CHECK(vkCreateComputePipelines(device, 0, 1, &pipeline_info, NULL, &pipeline) == VK_SUCCESS && pipeline);

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buffer_bytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkBuffer buffer = 0;
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &buffer) == VK_SUCCESS && buffer);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory memory = 0;
    CHECK(vkAllocateMemory(device, &allocation, NULL, &memory) == VK_SUCCESS && memory);
    CHECK(vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS);
    void *mapped = NULL;
    CHECK(vkMapMemory(device, memory, 0, buffer_bytes, 0, &mapped) == VK_SUCCESS && mapped);
    if (mode == 2) {
        const uint32_t initial[13] = {
            0, 63, 42, 42, (uint32_t)-42, 2, 0x0fffu, 0, 0xaaaau, 0, 63, 123, 0
        };
        memcpy(mapped, initial, buffer_bytes);
    } else {
        uint32_t initial[2] = { mode == 1 ? 41u : 0u, 0u };
        memcpy(mapped, initial, buffer_bytes);
    }
    vkUnmapMemory(device, memory);

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size
    };
    VkDescriptorPool descriptor_pool = 0;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool) == VK_SUCCESS && descriptor_pool);
    VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &set_layout
    };
    VkDescriptorSet descriptor_set = 0;
    CHECK(vkAllocateDescriptorSets(device, &set_info, &descriptor_set) == VK_SUCCESS && descriptor_set);
    VkDescriptorBufferInfo descriptor_buffer = {
        .buffer = buffer,
        .range = buffer_bytes
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptor_set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &descriptor_buffer
    };
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    vkCmdDispatch(command_buffer, 1, 1, 1);
    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer,
        .size = buffer_bytes
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host_barrier, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, memory, 0, buffer_bytes, 0, &mapped) == VK_SUCCESS && mapped);
    uint32_t result[13] = { 0 };
    memcpy(result, mapped, buffer_bytes);
    vkUnmapMemory(device, memory);
    if (mode == 1) {
        CHECK(result[0] == 1 && (result[1] == 1 || result[1] == 41));
    } else if (mode == 2) {
        static const uint32_t expected[13] = {
            7, 0, 17, 17, 17, 17, 0x0ff0u, 0x5a00u, 0xaa55u, 63, 0, 123, 123
        };
        CHECK(memcmp(result, expected, sizeof expected) == 0);
    } else CHECK(result[0] == INVOCATIONS);

    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, memory, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    puts(mode == 1 ?
         "vulkan compute: 64 contending Device-scope AcquireRelease/Acquire OpAtomicCompareExchange preserve old-value chaining: ok" :
         mode == 2 ?
         "vulkan compute: 63-way AcquireRelease atomic RMW contention plus Acquire OpAtomicLoad and Release OpAtomicStore through shared IR: ok" :
         "vulkan compute: 64 concurrent Device-scope Relaxed storage-buffer atomic adds produce 64: ok");
    return 0;
}

static int run_storage_image_copy(VkDevice device, VkQueue queue, VkCommandBuffer command_buffer,
                                 VkFence fence, const char *shader_path, int sampled_fetch) {
    const uint8_t expected[4] = { 37, 99, 171, 255 };
    const uint8_t sampled_r32_expected[4] = { 128, 0, 0, 255 };
    const uint8_t sampled_rgba16f_expected[4] = { 128, 64, 191, 255 };
    const uint8_t sampled_bgra8_expected[4] = { 171, 99, 37, 255 };
    const uint8_t sampled_rg8_expected[4] = { 37, 99, 0, 255 };
    const uint8_t sampled_r8_expected[4] = { 37, 0, 0, 255 };
    const uint16_t sampled_rgba16f_source[4] = { 0x3800, 0x3400, 0x3a00, 0x3c00 };
    const uint8_t sampled_rg8_source[2] = { 37, 99 };
    const uint8_t sampled_r8_source[1] = { 37 };
    const uint8_t swizzled_expected[4] = { 99, 0, 255, 255 };
    const uint8_t dynamic_image_expected[4] = { 64, 128, 191, 255 };
    const uint8_t array_decoy[4] = { 3, 7, 11, 255 };
    const uint8_t dynamic_sampled_expected[8] = { 3, 7, 11, 255, 37, 99, 171, 255 };
    const uint8_t image_query_source[16] = {
        3, 7, 11, 255,  5, 9, 13, 255,
        17, 21, 25, 255, 37, 99, 171, 255
    };
    int dynamic_storage_image_array = sampled_fetch == 6;
    int dynamic_sampled_array = sampled_fetch == 7 || sampled_fetch == 8;
    int swizzled_sampled = sampled_fetch == 13;
    int sampled_r32 = sampled_fetch == 14;
    int sampled_rgba16f = sampled_fetch == 15;
    int sampled_bgra8 = sampled_fetch == 16;
    int sampled_rg8 = sampled_fetch == 17;
    int sampled_r8 = sampled_fetch == 18;
    int swizzled_source = swizzled_sampled;
    int image_size_query = sampled_fetch == 9 || sampled_fetch == 10 ||
                           sampled_fetch == 11 || sampled_fetch == 12;
    int sampled_source = sampled_fetch != 0 && !dynamic_storage_image_array;
    int combined_sampler = sampled_fetch == 2 || sampled_fetch == 4 || sampled_fetch == 7;
    int separate_sampler = sampled_fetch == 3 || sampled_fetch == 5 || sampled_fetch == 8;
    int image_array = (sampled_fetch >= 4 && sampled_fetch <= 5) || dynamic_sampled_array;
    int descriptor_array = image_array || dynamic_storage_image_array;
    uint32_t output_binding = dynamic_storage_image_array ? 0u : separate_sampler ? 2u : 1u;
    uint32_t output_image_index = image_array ? 2u : 1u;
    uint32_t image_count = dynamic_storage_image_array ? 2u : image_array ? 3u : 2u;
    uint32_t buffer_count = dynamic_storage_image_array ? 2u : image_array ? 3u : 2u;
    size_t shader_size = 0;
    uint8_t *shader_bytes = read_fixture(shader_path, &shader_size);
    CHECK(shader_bytes && !(shader_size & 3u));
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader_size,
        .pCode = (const uint32_t *)shader_bytes
    };
    VkShaderModule shader = 0;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader) == VK_SUCCESS && shader);
    free(shader_bytes);

    VkDescriptorSetLayoutBinding bindings[3] = {
        { .binding = 0, .descriptorType = combined_sampler ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
              sampled_source ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = descriptor_array ? 2u : 1u,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = separate_sampler ? VK_DESCRIPTOR_TYPE_SAMPLER :
              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = image_array && separate_sampler ? 2u : 1u,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT }
    };
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = dynamic_storage_image_array ? 1u : separate_sampler ? 3u : 2u,
        .pBindings = bindings
    };
    VkDescriptorSetLayout set_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout) == VK_SUCCESS && set_layout);
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout
    };
    VkPipelineLayout pipeline_layout = 0;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout) == VK_SUCCESS && pipeline_layout);
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader,
            .pName = "main"
        },
        .layout = pipeline_layout,
        .basePipelineIndex = -1
    };
    VkPipeline pipeline = 0;
    CHECK(vkCreateComputePipelines(device, 0, 1, &pipeline_info, NULL, &pipeline) == VK_SUCCESS && pipeline);

    VkImage images[3] = { 0, 0, 0 };
    VkDeviceMemory image_memories[3] = { 0, 0, 0 };
    VkImageView views[3] = { 0, 0, 0 };
    const VkComponentMapping identity_components = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY
    };
    const VkComponentMapping read_swizzle = {
        VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ZERO,
        VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ONE
    };
    for (uint32_t i = 0; i < image_count; i++) {
        VkImageUsageFlags usage = dynamic_storage_image_array ?
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT : image_array ?
            (i < 2 ? VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT :
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) :
            (i == 0 ? (sampled_source ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_STORAGE_BIT) |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT :
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        VkImageCreateInfo image_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = i == 0 && sampled_r32 ? VK_FORMAT_R32_SFLOAT :
                i == 0 && sampled_rgba16f ? VK_FORMAT_R16G16B16A16_SFLOAT :
                i == 0 && sampled_bgra8 ? VK_FORMAT_B8G8R8A8_UNORM :
                i == 0 && sampled_rg8 ? VK_FORMAT_R8G8_UNORM :
                i == 0 && sampled_r8 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM,
            .extent = { image_size_query ? 2u : dynamic_sampled_array && i == output_image_index ? 2u : 1u,
                        image_size_query ? 2u : 1u, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
        };
        CHECK(vkCreateImage(device, &image_info, NULL, &images[i]) == VK_SUCCESS && images[i]);
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, images[i], &requirements);
        VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = 0
        };
        CHECK(vkAllocateMemory(device, &allocation, NULL, &image_memories[i]) == VK_SUCCESS && image_memories[i]);
        CHECK(vkBindImageMemory(device, images[i], image_memories[i], 0) == VK_SUCCESS);
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = image_info.format,
            .components = i == 0 && swizzled_source ?
                read_swizzle : identity_components,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        };
        CHECK(vkCreateImageView(device, &view_info, NULL, &views[i]) == VK_SUCCESS && views[i]);
    }

    VkBuffer buffers[3] = { 0, 0, 0 };
    VkDeviceMemory buffer_memories[3] = { 0, 0, 0 };
    const VkBufferUsageFlags buffer_usages[3] = {
        dynamic_storage_image_array ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        dynamic_storage_image_array ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_TRANSFER_SRC_BIT
    };
    for (uint32_t i = 0; i < buffer_count; i++) {
        VkBufferCreateInfo buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = image_size_query && i == 0 ? sizeof image_query_source :
                    dynamic_sampled_array && i == 1 ? sizeof dynamic_sampled_expected :
                    sampled_rgba16f && i == 0 ? sizeof sampled_rgba16f_source :
                    sampled_rg8 && i == 0 ? sizeof sampled_rg8_source :
                    sampled_r8 && i == 0 ? sizeof sampled_r8_source : sizeof expected,
            .usage = buffer_usages[i],
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        CHECK(vkCreateBuffer(device, &buffer_info, NULL, &buffers[i]) == VK_SUCCESS && buffers[i]);
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, buffers[i], &requirements);
        VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = 0
        };
        CHECK(vkAllocateMemory(device, &allocation, NULL, &buffer_memories[i]) == VK_SUCCESS && buffer_memories[i]);
        CHECK(vkBindBufferMemory(device, buffers[i], buffer_memories[i], 0) == VK_SUCCESS);
    }
    void *mapped = NULL;
    if (!dynamic_storage_image_array) {
        size_t upload_size = image_size_query ? sizeof image_query_source :
            sampled_rgba16f ? sizeof sampled_rgba16f_source :
            sampled_rg8 ? sizeof sampled_rg8_source :
            sampled_r8 ? sizeof sampled_r8_source : sizeof expected;
        CHECK(vkMapMemory(device, buffer_memories[0], 0, upload_size, 0, &mapped) == VK_SUCCESS && mapped);
        if (sampled_r32) {
            const float source_value = 0.5f;
            memcpy(mapped, &source_value, sizeof source_value);
        } else if (sampled_rgba16f) {
            memcpy(mapped, sampled_rgba16f_source, sizeof sampled_rgba16f_source);
        } else if (sampled_rg8) {
            memcpy(mapped, sampled_rg8_source, sizeof sampled_rg8_source);
        } else if (sampled_r8) {
            memcpy(mapped, sampled_r8_source, sizeof sampled_r8_source);
        } else {
            memcpy(mapped, image_size_query ? image_query_source : expected, upload_size);
        }
        vkUnmapMemory(device, buffer_memories[0]);
    }
    if (image_array) {
        CHECK(vkMapMemory(device, buffer_memories[2], 0, sizeof array_decoy, 0, &mapped) == VK_SUCCESS && mapped);
        memcpy(mapped, array_decoy, sizeof array_decoy);
        vkUnmapMemory(device, buffer_memories[2]);
    }

    VkDescriptorPoolSize pool_sizes[3] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, dynamic_storage_image_array ? 2u : sampled_fetch ? 1u : 2u },
        { combined_sampler ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          sampled_source ? (image_array ? 2u : 1u) : 0u },
        { VK_DESCRIPTOR_TYPE_SAMPLER, separate_sampler ? (image_array ? 2u : 1u) : 0u }
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = dynamic_storage_image_array ? 1u : sampled_source ? (separate_sampler ? 3u : 2u) : 1u,
        .pPoolSizes = pool_sizes
    };
    VkDescriptorPool pool = 0;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &pool) == VK_SUCCESS && pool);
    VkDescriptorSetAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &set_layout
    };
    VkDescriptorSet descriptor_set = 0;
    CHECK(vkAllocateDescriptorSets(device, &allocate_info, &descriptor_set) == VK_SUCCESS && descriptor_set);
    VkSampler sampler = 0;
    if (combined_sampler || separate_sampler) {
        VkSamplerCreateInfo sampler_info = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .maxAnisotropy = 1.0f,
            .compareOp = VK_COMPARE_OP_NEVER,
            .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
            .maxLod = 0.0f,
            .unnormalizedCoordinates = 0
        };
        CHECK(vkCreateSampler(device, &sampler_info, NULL, &sampler) == VK_SUCCESS && sampler);
    }
    VkDescriptorImageInfo image_infos[3] = {
        { .sampler = sampler, .imageView = dynamic_storage_image_array ? views[0] : image_array ? views[1] : views[0], .imageLayout = sampled_source ?
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL },
        { .sampler = image_array ? sampler : 0,
          .imageView = dynamic_storage_image_array ? views[1] : image_array ? views[0] : views[1],
          .imageLayout = image_array ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL },
        { .imageView = image_array ? views[2] : 0, .imageLayout = VK_IMAGE_LAYOUT_GENERAL }
    };
    VkDescriptorImageInfo sampler_descriptors[2] = { { .sampler = sampler }, { .sampler = sampler } };
    VkWriteDescriptorSet writes[3] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptor_set, .dstBinding = 0,
          .descriptorCount = descriptor_array ? 2u : 1u,
          .descriptorType = combined_sampler ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
              sampled_source ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .pImageInfo = image_infos },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptor_set,
          .dstBinding = 1u,
          .descriptorCount = image_array && separate_sampler ? 2u : 1u,
          .descriptorType = separate_sampler ? VK_DESCRIPTOR_TYPE_SAMPLER :
              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .pImageInfo = separate_sampler ? sampler_descriptors :
              image_array ? &image_infos[2] : &image_infos[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptor_set,
          .dstBinding = output_binding,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .pImageInfo = image_array ? &image_infos[2] : &image_infos[1] }
    };
    vkUpdateDescriptorSets(device, dynamic_storage_image_array ? 1u : separate_sampler ? 3u : 2u, writes, 0, NULL);

    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    VkImageMemoryBarrier initial_barriers[3] = { 0 };
    for (uint32_t i = 0; i < image_count; i++) {
        int upload_image = !dynamic_storage_image_array && (i == 0 || (image_array && i == 1));
        initial_barriers[i] = (VkImageMemoryBarrier) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = upload_image ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = upload_image ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[i],
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        };
    }
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, NULL, 0, NULL, image_count, initial_barriers);
    VkBufferImageCopy copy_region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { image_size_query ? 2u : 1u, image_size_query ? 2u : 1u, 1 }
    };
    VkBufferImageCopy output_copy_region = copy_region;
    if (dynamic_sampled_array) output_copy_region.imageExtent.width = 2;
    if (image_size_query) {
        output_copy_region.imageOffset = (VkOffset3D){ 1, 1, 0 };
        output_copy_region.imageExtent = (VkExtent3D){ 1, 1, 1 };
    }
    if (!dynamic_storage_image_array)
        vkCmdCopyBufferToImage(command_buffer, buffers[0], images[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &copy_region);
    if (image_array)
        vkCmdCopyBufferToImage(command_buffer, buffers[2], images[1], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &copy_region);
    uint32_t sampled_image_count = dynamic_storage_image_array ? 0u : image_array ? 2u : 1u;
    VkImageMemoryBarrier source_barriers[2] = { 0 };
    for (uint32_t i = 0; i < sampled_image_count; i++) {
        source_barriers[i] = (VkImageMemoryBarrier) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = sampled_source ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[i],
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        };
    }
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, sampled_image_count, source_barriers);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    vkCmdDispatch(command_buffer, (dynamic_storage_image_array || dynamic_sampled_array) ? 2u : 1u, 1, 1);
    VkImageMemoryBarrier destination_to_transfer[2] = { 0 };
    uint32_t destination_count = dynamic_storage_image_array ? 2u : 1u;
    for (uint32_t i = 0; i < destination_count; i++) {
        destination_to_transfer[i] = (VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[dynamic_storage_image_array ? i : output_image_index],
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        };
    }
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, destination_count, destination_to_transfer);
    VkBufferMemoryBarrier host_barriers[2] = { 0 };
    for (uint32_t i = 0; i < destination_count; i++) {
        uint32_t image_index = dynamic_storage_image_array ? i : output_image_index;
        uint32_t buffer_index = dynamic_storage_image_array ? i : 1u;
        vkCmdCopyImageToBuffer(command_buffer, images[image_index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               buffers[buffer_index], 1, &output_copy_region);
        host_barriers[i] = (VkBufferMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buffers[buffer_index],
            .size = dynamic_sampled_array && buffer_index == 1 ? sizeof dynamic_sampled_expected : sizeof expected
        };
    }
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, destination_count, host_barriers, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    for (uint32_t i = 0; i < destination_count; i++) {
        uint32_t buffer_index = dynamic_storage_image_array ? i : 1u;
        size_t readback_size = dynamic_sampled_array ? sizeof dynamic_sampled_expected : sizeof expected;
        CHECK(vkMapMemory(device, buffer_memories[buffer_index], 0, readback_size, 0, &mapped) ==
              VK_SUCCESS && mapped);
        const uint8_t *pixel_expected = sampled_r32 ? sampled_r32_expected :
            sampled_rgba16f ? sampled_rgba16f_expected :
            sampled_bgra8 ? sampled_bgra8_expected :
            sampled_rg8 ? sampled_rg8_expected :
            sampled_r8 ? sampled_r8_expected :
            swizzled_source ? swizzled_expected :
            dynamic_storage_image_array ? dynamic_image_expected :
            dynamic_sampled_array ? dynamic_sampled_expected : expected;
        int matches = memcmp(mapped, pixel_expected, readback_size) == 0;
        if (!matches && dynamic_storage_image_array) {
            const uint8_t *actual = mapped;
            fprintf(stderr, "vulkan dynamic storage-image array element %u readback: %u,%u,%u,%u\n",
                    i, actual[0], actual[1], actual[2], actual[3]);
        } else if (!matches && dynamic_sampled_array) {
            const uint8_t *actual = mapped;
            fprintf(stderr, "vulkan dynamic sampled-image/sampler array readback: %u,%u,%u,%u / %u,%u,%u,%u\n",
                    actual[0], actual[1], actual[2], actual[3],
                    actual[4], actual[5], actual[6], actual[7]);
        }
        vkUnmapMemory(device, buffer_memories[buffer_index]);
        CHECK(matches);
    }
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    vkDestroyDescriptorPool(device, pool, NULL);
    if (sampler) vkDestroySampler(device, sampler, NULL);
    for (uint32_t i = 0; i < image_count; i++) {
        vkDestroyImageView(device, views[i], NULL);
        vkDestroyImage(device, images[i], NULL);
        vkFreeMemory(device, image_memories[i], NULL);
    }
    for (uint32_t i = 0; i < buffer_count; i++) {
        vkDestroyBuffer(device, buffers[i], NULL);
        vkFreeMemory(device, buffer_memories[i], NULL);
    }
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    puts(sampled_bgra8 ?
         "vulkan compute: BGRA8 sampled OpImageFetch preserves Vulkan component order: ok" :
         sampled_rg8 ?
         "vulkan compute: RG8 sampled OpImageFetch fills missing channels and reads back: ok" :
         sampled_r8 ?
         "vulkan compute: R8 sampled OpImageFetch fills missing channels and reads back: ok" :
         sampled_rgba16f ?
         "vulkan compute: RGBA16_SFLOAT sampled OpImageFetch converts half texels and reads back: ok" :
         sampled_r32 ?
         "vulkan compute: R32_SFLOAT sampled OpImageFetch returns R/zero/one components and readback: ok" :
         swizzled_sampled ?
         "vulkan compute: VkImageView sampled-image R/G/B/A component swizzle reaches OpImageFetch readback: ok" :
         image_size_query ?
         sampled_fetch == 9 ?
         "vulkan compute: OpImageQuerySize drives 2D mip-zero read and write coordinates: ok" :
         sampled_fetch == 10 ?
        "vulkan compute: OpImageQuerySizeLod(GlobalInvocationId.x=0) drives 2D mip-zero read and write coordinates: ok" :
         sampled_fetch == 11 ?
         "vulkan compute: sampled-image OpImageQueryLevels returns the accessible image-view mip count: ok" :
         "vulkan compute: storage-image OpImageQueryLevels returns the accessible image-view mip count: ok" :
         dynamic_storage_image_array ?
         "vulkan compute: dynamically uniform WorkgroupId storage-image-array selection and readback: ok" :
         dynamic_sampled_array ? separate_sampler ?
         "vulkan compute: dynamically uniform sampled-image and reverse-indexed sampler arrays with readback: ok" :
         "vulkan compute: dynamically uniform combined image-sampler array selection and readback: ok" :
         image_array ? separate_sampler ?
         "vulkan compute: separate sampled-image and sampler arrays, element 1 sampled and read back: ok" :
         "vulkan compute: combined image-sampler array, element 1 sampled and read back: ok" :
         separate_sampler ?
         "vulkan compute: separate sampled image and sampler, OpSampledImage and explicit-LOD sampling: ok" :
         combined_sampler ?
         "vulkan compute: combined sampler, explicit-LOD SPIR-V sampling, storage-image write/readback: ok" :
         sampled_fetch ?
             "vulkan compute: RGBA8 sampled-image OpImageFetch and storage-image write/readback: ok" :
             "vulkan compute: RGBA8 storage-image OpImageRead/OpImageWrite dispatch and transfer readback: ok");
    return 0;
}

static int run_descriptor_array(VkDevice device, VkQueue queue, VkCommandBuffer command_buffer, VkFence fence,
    const char *shader_path, VkDescriptorType descriptor_type, int dynamic_index) {
    int dynamic_storage = descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    size_t shader_size = 0;
    uint8_t *shader_bytes = read_fixture(shader_path, &shader_size);
    CHECK(shader_bytes && !(shader_size & 3u));
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader_size,
        .pCode = (const uint32_t *)shader_bytes
    };
    VkShaderModule shader = 0;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader) == VK_SUCCESS && shader);
    free(shader_bytes);

    VkDescriptorSetLayoutBinding descriptor_bindings[2] = {
        {
            .binding = 0,
            .descriptorType = descriptor_type,
            .descriptorCount = 3,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
        },
        {
            .binding = 1,
            .descriptorType = descriptor_type,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
        }
    };
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = descriptor_bindings
    };
    VkDescriptorSetLayout set_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout) == VK_SUCCESS && set_layout);
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout
    };
    VkPipelineLayout pipeline_layout = 0;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout) == VK_SUCCESS && pipeline_layout);
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader,
            .pName = "main"
        },
        .layout = pipeline_layout,
        .basePipelineIndex = -1
    };
    VkPipeline pipeline = 0;
    CHECK(vkCreateComputePipelines(device, 0, 1, &pipeline_info, NULL, &pipeline) == VK_SUCCESS && pipeline);

    VkBuffer buffers[4] = { 0 };
    VkDeviceMemory memories[4] = { 0 };
    uint32_t initial_values[4] = { 0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u };
    for (uint32_t i = 0; i < 4; i++) {
        VkBufferCreateInfo buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = sizeof(uint32_t) * 8,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        CHECK(vkCreateBuffer(device, &buffer_info, NULL, &buffers[i]) == VK_SUCCESS && buffers[i]);
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, buffers[i], &requirements);
        VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = 0
        };
        CHECK(vkAllocateMemory(device, &allocation, NULL, &memories[i]) == VK_SUCCESS && memories[i]);
        CHECK(vkBindBufferMemory(device, buffers[i], memories[i], 0) == VK_SUCCESS);
        void *mapped = NULL;
        CHECK(vkMapMemory(device, memories[i], 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
        uint32_t *words = mapped;
        for (uint32_t j = 0; j < 8; j++) words[j] = 0;
        words[0] = initial_values[i];
        vkUnmapMemory(device, memories[i]);
    }

    VkDescriptorPoolSize pool_size = {
        .type = descriptor_type,
        .descriptorCount = 8
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 2,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size
    };
    VkDescriptorPool descriptor_pool = 0;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool) == VK_SUCCESS && descriptor_pool);
    VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 2,
        .pSetLayouts = (VkDescriptorSetLayout[]){ set_layout, set_layout }
    };
    VkDescriptorSet descriptor_sets[2] = { 0 };
    CHECK(vkAllocateDescriptorSets(device, &set_info, descriptor_sets) == VK_SUCCESS &&
          descriptor_sets[0] && descriptor_sets[1]);
    VkDescriptorBufferInfo buffer_infos[4] = {
        { .buffer = buffers[0], .range = sizeof(uint32_t) },
        { .buffer = buffers[1], .range = sizeof(uint32_t) },
        { .buffer = buffers[2], .range = sizeof(uint32_t) },
        { .buffer = buffers[3], .range = sizeof(uint32_t) }
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptor_sets[0],
        .dstBinding = 0,
        .descriptorCount = 4,
        .descriptorType = descriptor_type,
        .pBufferInfo = buffer_infos
    };
    VkWriteDescriptorSet second_element_write = write;
    second_element_write.dstArrayElement = 1;
    second_element_write.descriptorCount = 1;
    second_element_write.pBufferInfo = &buffer_infos[1];
    VkWriteDescriptorSet destination_seed_write = write;
    destination_seed_write.dstSet = descriptor_sets[1];
    destination_seed_write.dstArrayElement = 1;
    destination_seed_write.descriptorCount = 1;
    destination_seed_write.pBufferInfo = &buffer_infos[3];
    VkWriteDescriptorSet descriptor_writes[3] = { write, second_element_write, destination_seed_write };
    VkCopyDescriptorSet descriptor_copy = {
        .sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
        .srcSet = descriptor_sets[0],
        .srcBinding = 0,
        .dstSet = descriptor_sets[1],
        .dstBinding = 0,
        .descriptorCount = 4
    };
    vkUpdateDescriptorSets(device, 3, descriptor_writes, 1, &descriptor_copy);

    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    uint32_t dynamic_offsets[4] = { 0, 16, 0, 0 };
    if (dynamic_storage) {
        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                0, 1, &descriptor_sets[1], 3, dynamic_offsets);
        CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
        CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

        uint32_t misaligned_offsets[4] = { 0, 4, 0, 0 };
        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                0, 1, &descriptor_sets[1], 4, misaligned_offsets);
        CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
        CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

        uint32_t out_of_range_offsets[4] = { 0, 32, 0, 0 };
        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                0, 1, &descriptor_sets[1], 4, out_of_range_offsets);
        CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
        CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    }

    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_sets[1], dynamic_storage ? 4 : 0,
                            dynamic_storage ? dynamic_offsets : NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_sets[1], dynamic_storage ? 4 : 0,
                            dynamic_storage ? dynamic_offsets : NULL);
    vkCmdDispatch(command_buffer, dynamic_index ? 3 : 1, 1, 1);
    VkBufferMemoryBarrier barriers[2] = { 0 };
    for (uint32_t i = 0; i < 2; i++) barriers[i] = (VkBufferMemoryBarrier){
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffers[i],
        .size = sizeof(uint32_t) * 8
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 2, barriers, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    for (uint32_t i = 0; i < 4; i++) {
        void *mapped = NULL;
        CHECK(vkMapMemory(device, memories[i], 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
        uint32_t *words = mapped;
        uint32_t expected_first = dynamic_index ?
            (i == 0 ? 123u : i == 1 && !dynamic_storage ? 124u : initial_values[i]) :
            (i == 1 && !dynamic_storage ? 123u : initial_values[i]);
        uint32_t expected_dynamic_offset = i == 1 && dynamic_storage ? (dynamic_index ? 124u : 123u) : 0u;
        CHECK(words[0] == expected_first);
        CHECK(words[4] == expected_dynamic_offset);
        vkUnmapMemory(device, memories[i]);
    }
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_sets[1], dynamic_storage ? 4 : 0,
                            dynamic_storage ? dynamic_offsets : NULL);
    vkCmdDispatch(command_buffer, 1, 1, 1);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    vkDestroyPipeline(device, pipeline, NULL);
    pipeline = 0;
    VkSubmitInfo invalid_pipeline_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &invalid_pipeline_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    VkPipeline replacement_pipeline = 0;
    CHECK(vkCreateComputePipelines(device, 0, 1, &pipeline_info, NULL, &replacement_pipeline) == VK_SUCCESS &&
          replacement_pipeline);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, replacement_pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_sets[1], dynamic_storage ? 4 : 0,
                            dynamic_storage ? dynamic_offsets : NULL);
    vkCmdDispatch(command_buffer, 1, 1, 1);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    vkDestroyBuffer(device, buffers[1], NULL);
    buffers[1] = 0;
    VkSubmitInfo invalid_buffer_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &invalid_buffer_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, replacement_pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    pipeline_layout = 0;
    VkSubmitInfo invalid_pipeline_layout_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &invalid_pipeline_layout_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkFreeDescriptorSets(device, descriptor_pool, 2, descriptor_sets) == VK_SUCCESS);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    for (uint32_t i = 0; i < 4; i++) {
        vkDestroyBuffer(device, buffers[i], NULL);
        vkFreeMemory(device, memories[i], NULL);
    }
    vkDestroyPipeline(device, replacement_pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    puts(dynamic_index ? (dynamic_storage ?
        "vulkan descriptors: dynamic SPIR-V storage-buffer indexing, bounds and dynamic offsets: ok" :
        "vulkan descriptors: dynamic SPIR-V storage-buffer selection, bounds and per-element writes: ok") :
        dynamic_storage ?
        "vulkan descriptors: dynamic storage-buffer arrays, ordered copies and offset dispatch: ok" :
        "vulkan descriptors: storage-buffer arrays and ordered cross-set copies: ok");
    return 0;
}

static int check_descriptor_array_pipeline_feature_gate(VkDevice device, const char *shader_path,
    VkDescriptorType descriptor_type, uint32_t descriptor_count, VkResult expected_result) {
    size_t shader_size = 0;
    uint8_t *shader_bytes = read_fixture(shader_path, &shader_size);
    CHECK(shader_bytes && !(shader_size & 3u));
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader_size,
        .pCode = (const uint32_t *)shader_bytes
    };
    VkShaderModule shader = 0;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader) == VK_SUCCESS && shader);
    free(shader_bytes);

    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = descriptor_type,
        .descriptorCount = descriptor_count,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
    };
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding
    };
    VkDescriptorSetLayout set_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout) == VK_SUCCESS && set_layout);
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout
    };
    VkPipelineLayout pipeline_layout = 0;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout) == VK_SUCCESS && pipeline_layout);
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader,
            .pName = "main"
        },
        .layout = pipeline_layout,
        .basePipelineIndex = -1
    };
    VkPipeline pipeline = 0;
    VkResult result = vkCreateComputePipelines(device, 0, 1, &pipeline_info, NULL, &pipeline);
    CHECK(result == expected_result && (expected_result != VK_SUCCESS || pipeline));
    if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    return 0;
}

static int check_dynamic_sampled_descriptor_pipeline_gate(VkDevice device, const char *shader_path,
                                                            int separate_sampler) {
    size_t shader_size = 0;
    uint8_t *shader_bytes = read_fixture(shader_path, &shader_size);
    CHECK(shader_bytes && !(shader_size & 3u));
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader_size,
        .pCode = (const uint32_t *)shader_bytes
    };
    VkShaderModule shader = 0;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader) == VK_SUCCESS && shader);
    free(shader_bytes);

    VkDescriptorSetLayoutBinding bindings[3] = {
        { .binding = 0, .descriptorType = separate_sampler ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE :
              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 2, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = separate_sampler ? VK_DESCRIPTOR_TYPE_SAMPLER :
              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = separate_sampler ? 2u : 1u, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT }
    };
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = separate_sampler ? 3u : 2u,
        .pBindings = bindings
    };
    VkDescriptorSetLayout set_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout) == VK_SUCCESS && set_layout);
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout
    };
    VkPipelineLayout pipeline_layout = 0;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout) == VK_SUCCESS && pipeline_layout);
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader,
            .pName = "main"
        },
        .layout = pipeline_layout,
        .basePipelineIndex = -1
    };
    VkPipeline pipeline = 0;
    CHECK(vkCreateComputePipelines(device, 0, 1, &pipeline_info, NULL, &pipeline) ==
          VK_ERROR_FEATURE_NOT_PRESENT && !pipeline);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    return 0;
}

static int run_uniform_buffer(VkDevice device, VkQueue queue, VkCommandBuffer command_buffer, VkFence fence,
    const char *shader_path, int dynamic_uniform) {
    size_t shader_size = 0;
    uint8_t *shader_bytes = read_fixture(shader_path, &shader_size);
    CHECK(shader_bytes && !(shader_size & 3u));
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader_size,
        .pCode = (const uint32_t *)shader_bytes
    };
    VkShaderModule shader = 0;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader) == VK_SUCCESS && shader);
    free(shader_bytes);

    VkDescriptorSetLayoutBinding bindings[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = dynamic_uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC :
              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT }
    };
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings
    };
    VkDescriptorSetLayout set_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout) == VK_SUCCESS && set_layout);
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout
    };
    VkPipelineLayout pipeline_layout = 0;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout) == VK_SUCCESS && pipeline_layout);
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main" },
        .layout = pipeline_layout,
        .basePipelineIndex = -1
    };
    VkPipeline pipeline = 0;
    CHECK(vkCreateComputePipelines(device, 0, 1, &pipeline_info, NULL, &pipeline) == VK_SUCCESS && pipeline);

    VkBuffer output = 0, uniform = 0;
    VkDeviceMemory output_memory = 0, uniform_memory = 0;
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 16,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &output) == VK_SUCCESS && output);
    buffer_info.size = dynamic_uniform ? 48 : 32;
    buffer_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &uniform) == VK_SUCCESS && uniform);
    VkMemoryRequirements output_requirements, uniform_requirements;
    vkGetBufferMemoryRequirements(device, output, &output_requirements);
    vkGetBufferMemoryRequirements(device, uniform, &uniform_requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = output_requirements.size,
        .memoryTypeIndex = 0
    };
    CHECK(vkAllocateMemory(device, &allocation, NULL, &output_memory) == VK_SUCCESS && output_memory);
    allocation.allocationSize = uniform_requirements.size;
    CHECK(vkAllocateMemory(device, &allocation, NULL, &uniform_memory) == VK_SUCCESS && uniform_memory);
    CHECK(vkBindBufferMemory(device, output, output_memory, 0) == VK_SUCCESS);
    CHECK(vkBindBufferMemory(device, uniform, uniform_memory, 0) == VK_SUCCESS);
    void *mapped = NULL;
    CHECK(vkMapMemory(device, output_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    memset(mapped, 0, 16);
    vkUnmapMemory(device, output_memory);
    const uint32_t expected = 0xc001d00du;
    CHECK(vkMapMemory(device, uniform_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    memset(mapped, 0, (size_t)uniform_requirements.size);
    ((uint32_t *)mapped)[dynamic_uniform ? 4 : 0] = expected;
    ((uint32_t *)mapped)[dynamic_uniform ? 8 : 4] = 1;
    ((uint32_t *)mapped)[dynamic_uniform ? 9 : 5] = (uint32_t)-7;
    ((uint32_t *)mapped)[dynamic_uniform ? 10 : 6] = 0x40600000u;
    vkUnmapMemory(device, uniform_memory);

    VkDescriptorPoolSize pool_sizes[2] = {
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 3 },
        { .type = dynamic_uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .descriptorCount = 3 }
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 3,
        .poolSizeCount = 2,
        .pPoolSizes = pool_sizes
    };
    VkDescriptorPool descriptor_pool = 0;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool) == VK_SUCCESS && descriptor_pool);
    VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 3,
        .pSetLayouts = (VkDescriptorSetLayout[]){ set_layout, set_layout, set_layout }
    };
    VkDescriptorSet sets[3] = { 0 };
    CHECK(vkAllocateDescriptorSets(device, &set_info, sets) == VK_SUCCESS && sets[0] && sets[1] && sets[2]);
    VkDescriptorBufferInfo output_info = { .buffer = output, .range = 16 };
    VkDescriptorBufferInfo uniform_info = { .buffer = uniform, .range = 32 };
    VkDescriptorBufferInfo short_uniform_info = { .buffer = uniform, .range = 16 };
    VkWriteDescriptorSet writes[4] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[0], .dstBinding = 1,
          .descriptorCount = 1,
          .descriptorType = dynamic_uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC :
              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .pBufferInfo = &uniform_info },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[1], .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &output_info },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[2], .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &output_info },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[2], .dstBinding = 1,
          .descriptorCount = 1,
          .descriptorType = dynamic_uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC :
              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .pBufferInfo = &short_uniform_info }
    };
    VkCopyDescriptorSet copy_uniform = {
        .sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
        .srcSet = sets[0], .srcBinding = 1,
        .dstSet = sets[1], .dstBinding = 1,
        .descriptorCount = 1
    };
    vkUpdateDescriptorSets(device, 4, writes, 1, &copy_uniform);

    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    uint32_t dynamic_offset = dynamic_uniform ? 16u : 0u;
    if (dynamic_uniform) {
        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                0, 1, &sets[1], 0, NULL);
        CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
        CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

        uint32_t misaligned_offset = 4;
        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                0, 1, &sets[1], 1, &misaligned_offset);
        CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
        CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

        uint32_t out_of_range_offset = 32;
        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                0, 1, &sets[1], 1, &out_of_range_offset);
        CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
        CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    }
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &sets[1], dynamic_uniform ? 1 : 0,
                            dynamic_uniform ? &dynamic_offset : NULL);
    vkCmdDispatch(command_buffer, 1, 1, 1);
    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = output,
        .size = 16
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host_barrier, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, output_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    CHECK(*(uint32_t *)mapped == expected + 1u);
    vkUnmapMemory(device, output_memory);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkMapMemory(device, output_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    memset(mapped, 0, 16);
    vkUnmapMemory(device, output_memory);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &sets[2], dynamic_uniform ? 1 : 0,
                            dynamic_uniform ? &dynamic_offset : NULL);
    vkCmdDispatch(command_buffer, 1, 1, 1);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host_barrier, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, output_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    CHECK(*(uint32_t *)mapped == expected);
    vkUnmapMemory(device, output_memory);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkFreeDescriptorSets(device, descriptor_pool, 3, sets) == VK_SUCCESS);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    vkDestroyBuffer(device, uniform, NULL);
    vkFreeMemory(device, uniform_memory, NULL);
    vkDestroyBuffer(device, output, NULL);
    vkFreeMemory(device, output_memory, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    puts(dynamic_uniform ?
         "vulkan descriptors: dynamic uniform-buffer offset, scalar members, clipped range, copied descriptor and dispatch: ok" :
         "vulkan descriptors: scalar uniform-buffer members, clipped range, copied descriptor and dispatch: ok");
    return 0;
}

static int run_push_constants(VkDevice device, VkQueue queue, VkCommandBuffer command_buffer, VkFence fence,
    const char *shader_path) {
    size_t shader_size = 0;
    uint8_t *shader_bytes = read_fixture(shader_path, &shader_size);
    CHECK(shader_bytes && !(shader_size & 3u));
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader_size,
        .pCode = (const uint32_t *)shader_bytes
    };
    VkShaderModule shader = 0;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader) == VK_SUCCESS && shader);
    free(shader_bytes);

    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
    };
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding
    };
    VkDescriptorSetLayout set_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout) == VK_SUCCESS && set_layout);

    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = 8
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range
    };
    VkPushConstantRange invalid_range = push_range;
    invalid_range.offset = 2;
    VkPipelineLayout invalid_layout = 0;
    VkPipelineLayoutCreateInfo invalid_layout_info = layout_info;
    invalid_layout_info.pPushConstantRanges = &invalid_range;
    CHECK(vkCreatePipelineLayout(device, &invalid_layout_info, NULL, &invalid_layout) ==
          VK_ERROR_INITIALIZATION_FAILED && invalid_layout == 0);
    invalid_range = push_range;
    invalid_range.stageFlags = 1u; // Vertex stage is not implemented by this Vulkan device.
    CHECK(vkCreatePipelineLayout(device, &invalid_layout_info, NULL, &invalid_layout) ==
          VK_ERROR_FEATURE_NOT_PRESENT && invalid_layout == 0);
    VkPipelineLayout pipeline_layout = 0;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout) == VK_SUCCESS && pipeline_layout);

    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader,
            .pName = "main"
        },
        .layout = pipeline_layout,
        .basePipelineIndex = -1
    };
    VkPipeline pipeline = 0;
    CHECK(vkCreateComputePipelines(device, 0, 1, &pipeline_info, NULL, &pipeline) == VK_SUCCESS && pipeline);

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(uint32_t),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkBuffer output = 0;
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &output) == VK_SUCCESS && output);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, output, &requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory memory = 0;
    CHECK(vkAllocateMemory(device, &allocation, NULL, &memory) == VK_SUCCESS && memory);
    CHECK(vkBindBufferMemory(device, output, memory, 0) == VK_SUCCESS);
    void *mapped = NULL;
    CHECK(vkMapMemory(device, memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    *(uint32_t *)mapped = 0;
    vkUnmapMemory(device, memory);

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size
    };
    VkDescriptorPool descriptor_pool = 0;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool) == VK_SUCCESS && descriptor_pool);
    VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &set_layout
    };
    VkDescriptorSet descriptor_set = 0;
    CHECK(vkAllocateDescriptorSets(device, &set_info, &descriptor_set) == VK_SUCCESS && descriptor_set);
    VkDescriptorBufferInfo buffer_descriptor = { .buffer = output, .range = sizeof(uint32_t) };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptor_set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffer_descriptor
    };
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

    CHECK(vkGetDeviceProcAddr(device, "vkCmdPushConstants") == (PFN_vkVoidFunction)vkCmdPushConstants);
    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    uint32_t first = 19, second = 23;
    vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof first, &first);
    first = 0; // The command must retain a copy, not this caller-owned address.
    vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       sizeof first, sizeof second, &second);
    second = 0;
    vkCmdDispatch(command_buffer, 1, 1, 1);
    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = output,
        .size = sizeof(uint32_t)
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host_barrier, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    CHECK(*(uint32_t *)mapped == 42u);
    vkUnmapMemory(device, memory);

    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkFreeDescriptorSets(device, descriptor_pool, 1, &descriptor_set) == VK_SUCCESS);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    vkDestroyBuffer(device, output, NULL);
    vkFreeMemory(device, memory, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    puts("vulkan compute: partial push-constant updates copied, translated and read back through Metal: ok");
    return 0;
}

static int check_buffer_descriptor_slot_limits(VkDevice device) {
    VkDescriptorSetLayoutBinding mixed_bindings[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 12,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 18,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT }
    };
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = mixed_bindings
    };
    VkDescriptorSetLayout set_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout) == VK_SUCCESS && set_layout);
    VkPipelineLayoutCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout
    };
    VkPipelineLayout pipeline_layout = 0;
    CHECK(vkCreatePipelineLayout(device, &pipeline_info, NULL, &pipeline_layout) == VK_SUCCESS && pipeline_layout);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);

    VkDescriptorSetLayoutBinding overflow_binding = mixed_bindings[0];
    overflow_binding.descriptorCount++;
    VkDescriptorSetLayoutCreateInfo overflow_info = set_info;
    overflow_info.bindingCount = 1;
    overflow_info.pBindings = &overflow_binding;
    VkDescriptorSetLayout overflow_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &overflow_info, NULL, &overflow_layout) ==
          VK_ERROR_FEATURE_NOT_PRESENT && !overflow_layout);

    overflow_binding = mixed_bindings[1];
    overflow_binding.descriptorCount++;
    CHECK(vkCreateDescriptorSetLayout(device, &overflow_info, NULL, &overflow_layout) ==
          VK_ERROR_FEATURE_NOT_PRESENT && !overflow_layout);

    VkDescriptorSetLayoutBinding dynamic_uniform_binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
        .descriptorCount = 8,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
    };
    VkDescriptorSetLayoutCreateInfo dynamic_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &dynamic_uniform_binding
    };
    VkDescriptorSetLayout dynamic_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &dynamic_info, NULL, &dynamic_layout) == VK_SUCCESS && dynamic_layout);
    VkPipelineLayoutCreateInfo dynamic_pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &dynamic_layout
    };
    CHECK(vkCreatePipelineLayout(device, &dynamic_pipeline_info, NULL, &pipeline_layout) == VK_SUCCESS &&
          pipeline_layout);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, dynamic_layout, NULL);
    dynamic_uniform_binding.descriptorCount++;
    CHECK(vkCreateDescriptorSetLayout(device, &dynamic_info, NULL, &dynamic_layout) ==
          VK_ERROR_FEATURE_NOT_PRESENT && !dynamic_layout);

    dynamic_uniform_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    dynamic_uniform_binding.descriptorCount = 4;
    CHECK(vkCreateDescriptorSetLayout(device, &dynamic_info, NULL, &dynamic_layout) == VK_SUCCESS && dynamic_layout);
    vkDestroyDescriptorSetLayout(device, dynamic_layout, NULL);
    dynamic_uniform_binding.descriptorCount++;
    CHECK(vkCreateDescriptorSetLayout(device, &dynamic_info, NULL, &dynamic_layout) ==
          VK_ERROR_FEATURE_NOT_PRESENT && !dynamic_layout);
    puts("vulkan descriptors: 12 uniform and 18 storage buffers share Metal slots; 8 dynamic uniforms fit: ok");
    return 0;
}

static int check_sampler_allocation_limit(VkDevice device, uint32_t limit) {
    VkSampler *samplers = calloc(limit, sizeof *samplers);
    if (!samplers) return 1;
    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .maxAnisotropy = 1.0f,
        .compareOp = VK_COMPARE_OP_NEVER,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
    };
    uint32_t created = 0;
    for (; created < limit; created++) {
        if (vkCreateSampler(device, &sampler_info, NULL, &samplers[created]) != VK_SUCCESS ||
            !samplers[created]) {
            for (uint32_t i = 0; i < created; i++) vkDestroySampler(device, samplers[i], NULL);
            free(samplers);
            return 1;
        }
    }
    VkSampler over_limit = 0;
    VkResult result = vkCreateSampler(device, &sampler_info, NULL, &over_limit);
    for (uint32_t i = 0; i < created; i++) vkDestroySampler(device, samplers[i], NULL);
    free(samplers);
    CHECK(result == VK_ERROR_TOO_MANY_OBJECTS && !over_limit);
    puts("vulkan sampler: allocation limit is enforced and slots are reusable after destruction: ok");
    return 0;
}

static int check_descriptor_pool_exhaustion(VkDevice device, VkResult expected) {
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    VkDescriptorSetLayout layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &layout_info, NULL, &layout) == VK_SUCCESS && layout);
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 1
    };
    VkDescriptorPool pool = 0;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &pool) == VK_SUCCESS && pool);
    VkDescriptorSetAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout
    };
    VkDescriptorSet first = 0, overflow = 0;
    CHECK(vkAllocateDescriptorSets(device, &allocate_info, &first) == VK_SUCCESS && first);
    CHECK(vkAllocateDescriptorSets(device, &allocate_info, &overflow) == expected && !overflow);
    CHECK(vkFreeDescriptorSets(device, pool, 1, &first) == VK_SUCCESS);
    vkDestroyDescriptorPool(device, pool, NULL);
    vkDestroyDescriptorSetLayout(device, layout, NULL);
    return 0;
}

static int check_descriptor_type_pool_exhaustion(VkDevice device, VkResult expected) {
    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
    };
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding
    };
    VkDescriptorSetLayout layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &layout_info, NULL, &layout) == VK_SUCCESS && layout);
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1
    };
    VkDescriptorPool pool = 0;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &pool) == VK_SUCCESS && pool);
    VkDescriptorSetAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout
    };
    VkDescriptorSet set = 0;
    CHECK(vkAllocateDescriptorSets(device, &allocate_info, &set) == expected && !set);
    vkDestroyDescriptorPool(device, pool, NULL);
    vkDestroyDescriptorSetLayout(device, layout, NULL);
    return 0;
}

static int check_texture_descriptor_limits(VkDevice device, int storage_images_supported) {
    VkDescriptorSetLayoutBinding bindings[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 16,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 4,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT }
    };
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = storage_images_supported ? 2u : 1u,
        .pBindings = bindings
    };
    VkDescriptorSetLayout set_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout) == VK_SUCCESS && set_layout);
    VkPipelineLayoutCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout
    };
    VkPipelineLayout pipeline_layout = 0;
    CHECK(vkCreatePipelineLayout(device, &pipeline_info, NULL, &pipeline_layout) == VK_SUCCESS && pipeline_layout);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    puts(storage_images_supported ?
         "vulkan descriptors: 16 sampled and 4 storage images fit the shared Metal texture table: ok" :
         "vulkan descriptors: 16 sampled images fit the shared Metal texture table: ok");
    return 0;
}

static int check_high_texture_pipeline(VkDevice device, const char *shader_path) {
    size_t shader_size = 0;
    uint8_t *shader_bytes = read_fixture(shader_path, &shader_size);
    CHECK(shader_bytes && !(shader_size & 3u));
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader_size,
        .pCode = (const uint32_t *)shader_bytes
    };
    VkShaderModule shader = 0;
    VkResult result = vkCreateShaderModule(device, &shader_info, NULL, &shader);
    free(shader_bytes);
    CHECK(result == VK_SUCCESS && shader);

    VkDescriptorSetLayoutBinding bindings[20];
    for (uint32_t i = 0; i < 16; i++) {
        bindings[i] = (VkDescriptorSetLayoutBinding){
            .binding = i,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
        };
    }
    for (uint32_t i = 16; i < 20; i++) {
        bindings[i] = (VkDescriptorSetLayoutBinding){
            .binding = i,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
        };
    }
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 20,
        .pBindings = bindings
    };
    VkDescriptorSetLayout set_layout = 0;
    result = vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout);
    CHECK(result == VK_SUCCESS && set_layout);
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout
    };
    VkPipelineLayout layout = 0;
    result = vkCreatePipelineLayout(device, &layout_info, NULL, &layout);
    CHECK(result == VK_SUCCESS && layout);
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader,
            .pName = "main"
        },
        .layout = layout,
        .basePipelineIndex = -1
    };
    VkPipeline pipeline = 0;
    result = vkCreateComputePipelines(device, 0, 1, &pipeline_info, NULL, &pipeline);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    CHECK(result == VK_SUCCESS && pipeline);
    puts("vulkan compute: sampled texture slot 15 and storage-image slot 19 compile through Metal: ok");
    return 0;
}

static int check_graphics_sampled_descriptor_submission(VkDevice device, VkQueue queue,
    VkCommandBuffer command_buffer, const VkRenderPassBeginInfo *clear_begin,
    VkRenderPass render_pass, const VkRenderPassBeginInfo *render_begin, VkImage target_image,
    VkBuffer readback, VkDeviceMemory readback_memory, VkShaderModule vertex_shader, VkBuffer vertex_buffer,
    int separate_sampler, int implicit_lod, const char *fragment_shader_path) {
    enum { WIDTH = 4, HEIGHT = 3, PIXEL_BYTES = 4, READBACK_BYTES = WIDTH * HEIGHT * PIXEL_BYTES };
    size_t shader_size = 0;
    uint8_t *shader_bytes = read_fixture(fragment_shader_path, &shader_size);
    CHECK(shader_bytes && !(shader_size & 3u));
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader_size,
        .pCode = (const uint32_t *)shader_bytes
    };
    VkShaderModule fragment_shader = 0;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &fragment_shader) == VK_SUCCESS && fragment_shader);
    free(shader_bytes);

    VkDescriptorSetLayoutBinding descriptor_bindings[2] = {
        {
            .binding = 0,
            .descriptorType = separate_sampler ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE :
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
        }
    };
    VkDescriptorSetLayoutCreateInfo descriptor_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = separate_sampler ? 2u : 1u,
        .pBindings = descriptor_bindings
    };
    VkDescriptorSetLayout descriptor_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &descriptor_layout_info, NULL, &descriptor_layout) == VK_SUCCESS &&
          descriptor_layout);
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_layout
    };
    VkPipelineLayout pipeline_layout = 0;
    CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, NULL, &pipeline_layout) == VK_SUCCESS &&
          pipeline_layout);

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex_shader, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment_shader, .pName = "main" }
    };
    VkVertexInputBindingDescription vertex_binding = {
        .binding = 0, .stride = 4u * sizeof(float), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };
    VkVertexInputAttributeDescription vertex_attribute = {
        .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vertex_binding,
        .vertexAttributeDescriptionCount = 1,
        .pVertexAttributeDescriptions = &vertex_attribute
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };
    VkViewport viewport = { .x = 0, .y = 0, .width = 4, .height = 3, .minDepth = 0, .maxDepth = 1 };
    VkRect2D scissor = { .offset = { 0, 0 }, .extent = { 4, 3 } };
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport,
        .scissorCount = 1, .pScissors = &scissor
    };
    VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
    };
    VkPipelineColorBlendStateCreateInfo color_blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment
    };
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pColorBlendState = &color_blend,
        .layout = pipeline_layout,
        .renderPass = render_pass,
        .basePipelineIndex = -1
    };
    VkPipeline pipeline = 0;
    CHECK(vkCreateGraphicsPipelines(device, 0, 1, &pipeline_info, NULL, &pipeline) == VK_SUCCESS && pipeline);

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { 1, 1, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkImage image = 0;
    CHECK(vkCreateImage(device, &image_info, NULL, &image) == VK_SUCCESS && image);
    VkMemoryRequirements image_requirements = { 0 };
    vkGetImageMemoryRequirements(device, image, &image_requirements);
    VkMemoryAllocateInfo image_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = image_requirements.size,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory image_memory = 0;
    CHECK(vkAllocateMemory(device, &image_allocation, NULL, &image_memory) == VK_SUCCESS && image_memory);
    CHECK(vkBindImageMemory(device, image, image_memory, 0) == VK_SUCCESS);
    VkBufferCreateInfo upload_buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkBuffer upload_buffer = 0;
    CHECK(vkCreateBuffer(device, &upload_buffer_info, NULL, &upload_buffer) == VK_SUCCESS && upload_buffer);
    VkMemoryRequirements upload_requirements = { 0 };
    vkGetBufferMemoryRequirements(device, upload_buffer, &upload_requirements);
    VkMemoryAllocateInfo upload_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = upload_requirements.size,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory upload_memory = 0;
    CHECK(vkAllocateMemory(device, &upload_allocation, NULL, &upload_memory) == VK_SUCCESS && upload_memory);
    CHECK(vkBindBufferMemory(device, upload_buffer, upload_memory, 0) == VK_SUCCESS);
    static const uint8_t sampled_pixel[PIXEL_BYTES] = { 31, 137, 223, 255 };
    void *upload_mapped = NULL;
    CHECK(vkMapMemory(device, upload_memory, 0, upload_requirements.size, 0, &upload_mapped) == VK_SUCCESS &&
          upload_mapped);
    memcpy(upload_mapped, sampled_pixel, sizeof sampled_pixel);
    vkUnmapMemory(device, upload_memory);
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    VkImageView view = 0;
    CHECK(vkCreateImageView(device, &view_info, NULL, &view) == VK_SUCCESS && view);
    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxAnisotropy = 1.0f,
        .compareOp = VK_COMPARE_OP_NEVER,
        .maxLod = 0.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
    };
    VkSampler sampler = 0;
    CHECK(vkCreateSampler(device, &sampler_info, NULL, &sampler) == VK_SUCCESS && sampler);
    VkDescriptorPoolSize pool_sizes[2] = {
        {
            .type = separate_sampler ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1
        },
        { .type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1 }
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = separate_sampler ? 2u : 1u,
        .pPoolSizes = pool_sizes
    };
    VkDescriptorPool pool = 0;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &pool) == VK_SUCCESS && pool);
    VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &descriptor_layout
    };
    VkDescriptorSet set = 0;
    CHECK(vkAllocateDescriptorSets(device, &set_info, &set) == VK_SUCCESS && set);
    VkDescriptorImageInfo descriptor_images[2] = {
        {
            .sampler = separate_sampler ? 0 : sampler,
            .imageView = view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        },
        { .sampler = sampler }
    };
    VkWriteDescriptorSet writes[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = separate_sampler ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE :
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &descriptor_images[0]
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .pImageInfo = &descriptor_images[1]
        }
    };
    vkUpdateDescriptorSets(device, separate_sampler ? 2u : 1u, writes, 0, NULL);

    CHECK(vkBeginCommandBuffer(command_buffer,
        &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }) == VK_SUCCESS);
    VkImageMemoryBarrier sample_to_transfer = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &sample_to_transfer);
    VkBufferImageCopy sample_copy = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { 1, 1, 1 }
    };
    vkCmdCopyBufferToImage(command_buffer, upload_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &sample_copy);
    VkImageMemoryBarrier sample_to_shader = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &sample_to_shader);
    vkCmdBeginRenderPass(command_buffer, clear_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(command_buffer);
    VkImageMemoryBarrier attachment_write_to_write = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = target_image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &attachment_write_to_write);
    vkCmdBeginRenderPass(command_buffer, render_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
        0, 1, &set, 0, NULL);
    vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer, (VkDeviceSize[]){ 0 });
    vkCmdDraw(command_buffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(command_buffer);
    VkImageMemoryBarrier attachment_to_copy = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = target_image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &attachment_to_copy);
    VkBufferImageCopy readback_copy = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { WIDTH, HEIGHT, 1 }
    };
    vkCmdCopyImageToBuffer(command_buffer, target_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        readback, 1, &readback_copy);
    VkBufferMemoryBarrier host_read = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = readback,
        .size = READBACK_BYTES
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, NULL, 1, &host_read, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = 0;
    CHECK(vkCreateFence(device, &fence_info, NULL, &fence) == VK_SUCCESS && fence);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    void *readback_mapped = NULL;
    CHECK(vkMapMemory(device, readback_memory, 0, READBACK_BYTES, 0, &readback_mapped) == VK_SUCCESS &&
          readback_mapped);
    static const uint8_t clear_blue[PIXEL_BYTES] = { 0, 0, 255, 255 };
    for (uint32_t y = 0; y < HEIGHT; y++) {
        for (uint32_t x = 0; x < WIDTH; x++) {
            const uint8_t *expected = y == 1 && (x == 1 || x == 2) ? sampled_pixel : clear_blue;
            CHECK(memcmp((const uint8_t *)readback_mapped + (y * WIDTH + x) * PIXEL_BYTES,
                         expected, PIXEL_BYTES) == 0);
        }
    }
    vkUnmapMemory(device, readback_memory);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    vkDestroyDescriptorPool(device, pool, NULL);
    vkDestroySampler(device, sampler, NULL);
    vkDestroyFence(device, fence, NULL);
    vkDestroyBuffer(device, upload_buffer, NULL);
    vkFreeMemory(device, upload_memory, NULL);
    vkDestroyImageView(device, view, NULL);
    vkDestroyImage(device, image, NULL);
    vkFreeMemory(device, image_memory, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, descriptor_layout, NULL);
    vkDestroyShaderModule(device, fragment_shader, NULL);
    puts(separate_sampler ?
        (implicit_lod ?
         "vulkan graphics: separate image/sampler implicit-LOD draw, submit and sampled-pixel readback: ok" :
         "vulkan graphics: separate image/sampler explicit-LOD draw, submit and sampled-pixel readback: ok") :
        (implicit_lod ?
         "vulkan graphics: combined image-sampler implicit-LOD draw, submit and sampled-pixel readback: ok" :
         "vulkan graphics: combined image-sampler explicit-LOD draw, submit and sampled-pixel readback: ok"));
    return 0;
}

static int run_render_pass_clear(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
    const char *vertex_shader_path, const char *fragment_shader_path,
    const char *uint_vertex_shader_path, const char *sint_vertex_shader_path,
    const char *sampled_fragment_shader_path, const char *separate_sampled_fragment_shader_path,
    const char *implicit_sampled_fragment_shader_path,
    const char *implicit_separate_sampled_fragment_shader_path) {
    enum { WIDTH = 4, HEIGHT = 3, PIXEL_BYTES = 4, READBACK_BYTES = WIDTH * HEIGHT * PIXEL_BYTES };
    VkFormatProperties format_properties = { 0 };
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, &format_properties);
    if (!(format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
        puts("vulkan render pass: RGBA8 color attachment unsupported by this Metal device; readback test skipped");
        return 0;
    }
    CHECK(vkGetDeviceProcAddr(device, "vkCreateRenderPass") == (PFN_vkVoidFunction)vkCreateRenderPass);
    CHECK(vkGetDeviceProcAddr(device, "vkDestroyRenderPass") == (PFN_vkVoidFunction)vkDestroyRenderPass);
    CHECK(vkGetDeviceProcAddr(device, "vkCreateFramebuffer") == (PFN_vkVoidFunction)vkCreateFramebuffer);
    CHECK(vkGetDeviceProcAddr(device, "vkDestroyFramebuffer") == (PFN_vkVoidFunction)vkDestroyFramebuffer);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdBeginRenderPass") == (PFN_vkVoidFunction)vkCmdBeginRenderPass);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdEndRenderPass") == (PFN_vkVoidFunction)vkCmdEndRenderPass);
    if (vertex_shader_path || fragment_shader_path) {
        CHECK(vertex_shader_path && fragment_shader_path);
        CHECK(vkGetDeviceProcAddr(device, "vkCreateGraphicsPipelines") ==
              (PFN_vkVoidFunction)vkCreateGraphicsPipelines);
        CHECK(vkGetDeviceProcAddr(device, "vkCmdDraw") == (PFN_vkVoidFunction)vkCmdDraw);
        CHECK(vkGetDeviceProcAddr(device, "vkCmdDrawIndirect") ==
              (PFN_vkVoidFunction)vkCmdDrawIndirect);
        CHECK(vkGetDeviceProcAddr(device, "vkCmdBindIndexBuffer") ==
              (PFN_vkVoidFunction)vkCmdBindIndexBuffer);
        CHECK(vkGetDeviceProcAddr(device, "vkCmdBindVertexBuffers") ==
              (PFN_vkVoidFunction)vkCmdBindVertexBuffers);
        CHECK(vkGetDeviceProcAddr(device, "vkCmdDrawIndexed") ==
              (PFN_vkVoidFunction)vkCmdDrawIndexed);
        CHECK(vkGetDeviceProcAddr(device, "vkCmdDrawIndexedIndirect") ==
              (PFN_vkVoidFunction)vkCmdDrawIndexedIndirect);
        CHECK(vkGetDeviceProcAddr(device, "vkCmdSetViewport") ==
              (PFN_vkVoidFunction)vkCmdSetViewport);
        CHECK(vkGetDeviceProcAddr(device, "vkCmdSetScissor") ==
              (PFN_vkVoidFunction)vkCmdSetScissor);
        CHECK(vkGetDeviceProcAddr(device, "vkCmdSetBlendConstants") ==
              (PFN_vkVoidFunction)vkCmdSetBlendConstants);
    }

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { WIDTH, HEIGHT, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    VkImage image = 0;
    CHECK(vkCreateImage(device, &image_info, NULL, &image) == VK_SUCCESS && image);
    VkMemoryRequirements image_requirements;
    vkGetImageMemoryRequirements(device, image, &image_requirements);
    VkMemoryAllocateInfo image_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = image_requirements.size,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory image_memory = 0;
    CHECK(vkAllocateMemory(device, &image_allocation, NULL, &image_memory) == VK_SUCCESS && image_memory);
    CHECK(vkBindImageMemory(device, image, image_memory, 0) == VK_SUCCESS);

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    VkImageView view = 0;
    CHECK(vkCreateImageView(device, &view_info, NULL, &view) == VK_SUCCESS && view);
    VkAttachmentReference color_reference = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_reference
    };
    VkAttachmentDescription first_attachment = {
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    VkAttachmentDescription second_attachment = first_attachment;
    second_attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    second_attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkRenderPassCreateInfo first_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &first_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass
    };
    VkRenderPassCreateInfo second_pass_info = first_pass_info;
    second_pass_info.pAttachments = &second_attachment;
    VkAttachmentDescription invalid_load_attachment = first_attachment;
    invalid_load_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkRenderPassCreateInfo invalid_load_info = first_pass_info;
    invalid_load_info.pAttachments = &invalid_load_attachment;
    VkRenderPass invalid_load_pass = 0;
    CHECK(vkCreateRenderPass(device, &invalid_load_info, NULL, &invalid_load_pass) ==
          VK_ERROR_INITIALIZATION_FAILED && !invalid_load_pass);
    VkRenderPass first_pass = 0, second_pass = 0;
    CHECK(vkCreateRenderPass(device, &first_pass_info, NULL, &first_pass) == VK_SUCCESS && first_pass);
    CHECK(vkCreateRenderPass(device, &second_pass_info, NULL, &second_pass) == VK_SUCCESS && second_pass);
    VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = first_pass,
        .attachmentCount = 1,
        .pAttachments = &view,
        .width = WIDTH,
        .height = HEIGHT,
        .layers = 1
    };
    VkFramebuffer first_framebuffer = 0, second_framebuffer = 0;
    CHECK(vkCreateFramebuffer(device, &framebuffer_info, NULL, &first_framebuffer) == VK_SUCCESS && first_framebuffer);
    framebuffer_info.renderPass = second_pass;
    CHECK(vkCreateFramebuffer(device, &framebuffer_info, NULL, &second_framebuffer) == VK_SUCCESS && second_framebuffer);

    VkShaderModule vertex_shader = 0, fragment_shader = 0;
    VkPipelineLayout graphics_layout = 0;
    VkPipeline graphics_pipeline = 0, dynamic_graphics_pipeline = 0;
    const float dynamic_blend_constants[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    int draw_enabled = vertex_shader_path && fragment_shader_path;
    if (draw_enabled) {
        size_t shader_size = 0;
        uint8_t *shader_bytes = read_fixture(vertex_shader_path, &shader_size);
        CHECK(shader_bytes && !(shader_size & 3u));
        VkShaderModuleCreateInfo shader_info = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = shader_size,
            .pCode = (const uint32_t *)shader_bytes
        };
        CHECK(vkCreateShaderModule(device, &shader_info, NULL, &vertex_shader) == VK_SUCCESS && vertex_shader);
        free(shader_bytes);
        shader_bytes = read_fixture(fragment_shader_path, &shader_size);
        CHECK(shader_bytes && !(shader_size & 3u));
        shader_info.codeSize = shader_size;
        shader_info.pCode = (const uint32_t *)shader_bytes;
        CHECK(vkCreateShaderModule(device, &shader_info, NULL, &fragment_shader) == VK_SUCCESS && fragment_shader);
        free(shader_bytes);

        VkPipelineLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &graphics_layout) == VK_SUCCESS && graphics_layout);
        VkPipelineShaderStageCreateInfo stages[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex_shader, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment_shader, .pName = "main" }
        };
        VkVertexInputBindingDescription vertex_binding = {
            .binding = 0, .stride = 4u * sizeof(float), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
        };
        VkVertexInputAttributeDescription vertex_attribute = {
            .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0
        };
        VkPipelineVertexInputStateCreateInfo vertex_input = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &vertex_binding,
            .vertexAttributeDescriptionCount = 1,
            .pVertexAttributeDescriptions = &vertex_attribute
        };
        VkPipelineInputAssemblyStateCreateInfo input_assembly = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
        };
        VkViewport viewport = { .x = 1.0f, .y = HEIGHT, .width = 2.0f, .height = -HEIGHT,
                               .minDepth = 0.0f, .maxDepth = 1.0f };
        VkRect2D scissor = { .offset = { 1, 1 }, .extent = { 1, 1 } };
        VkPipelineViewportStateCreateInfo viewport_state = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .pViewports = &viewport,
            .scissorCount = 1, .pScissors = &scissor
        };
        VkPipelineRasterizationStateCreateInfo rasterization = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .lineWidth = 1.0f
        };
        VkPipelineMultisampleStateCreateInfo multisample = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
        };
        VkPipelineColorBlendAttachmentState blend_attachment = {
            .blendEnable = 1,
            .srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
        };
        VkPipelineColorBlendStateCreateInfo color_blend = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &blend_attachment,
            .blendConstants = { 1.0f, 1.0f, 1.0f, 1.0f }
        };
        VkGraphicsPipelineCreateInfo pipeline_info = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2, .pStages = stages,
            .pVertexInputState = &vertex_input,
            .pInputAssemblyState = &input_assembly,
            .pViewportState = &viewport_state,
            .pRasterizationState = &rasterization,
            .pMultisampleState = &multisample,
            .pColorBlendState = &color_blend,
            .layout = graphics_layout,
            .renderPass = second_pass,
            .basePipelineIndex = -1
        };
        CHECK(vkCreateGraphicsPipelines(device, 0, 1, &pipeline_info, NULL, &graphics_pipeline) ==
              VK_SUCCESS && graphics_pipeline);

        size_t specialized_size = 0;
        uint8_t *base_vertex_bytes = read_fixture(vertex_shader_path, &shader_size);
        CHECK(base_vertex_bytes && !(shader_size & 3u));
        uint32_t specialization_ids[12];
        uint32_t *specialized_words = make_specialized_vertex_fixture(base_vertex_bytes, shader_size,
                                                                       &specialized_size, specialization_ids);
        free(base_vertex_bytes);
        CHECK(specialized_words && !(specialized_size & 3u));
        VkShaderModuleCreateInfo specialized_shader_info = shader_info;
        specialized_shader_info.codeSize = specialized_size;
        specialized_shader_info.pCode = specialized_words;
        VkShaderModule specialized_vertex_shader = 0;
        CHECK(vkCreateShaderModule(device, &specialized_shader_info, NULL, &specialized_vertex_shader) ==
              VK_SUCCESS && specialized_vertex_shader);
        free(specialized_words);

        uint32_t specialization_value = 0x3f000000u;
        VkSpecializationMapEntry specialization_entry = {
            .constantID = 7, .offset = 0, .size = sizeof specialization_value
        };
        VkSpecializationInfo specialization_info = {
            .mapEntryCount = 1, .pMapEntries = &specialization_entry,
            .dataSize = sizeof specialization_value, .pData = &specialization_value
        };
        VkPipelineShaderStageCreateInfo specialized_stages[2] = { stages[0], stages[1] };
        specialized_stages[0].module = specialized_vertex_shader;
        specialized_stages[0].pSpecializationInfo = &specialization_info;
        VkGraphicsPipelineCreateInfo specialized_pipeline_info = pipeline_info;
        specialized_pipeline_info.pStages = specialized_stages;
        VkPipeline specialized_pipeline = 0;
        CHECK(vkCreateGraphicsPipelines(device, 0, 1, &specialized_pipeline_info, NULL,
                                        &specialized_pipeline) == VK_SUCCESS && specialized_pipeline);
        vkDestroyPipeline(device, specialized_pipeline, NULL);

        VkSpecializationMapEntry invalid_specialization_entry = specialization_entry;
        invalid_specialization_entry.size = sizeof(uint16_t);
        VkSpecializationInfo invalid_specialization_info = specialization_info;
        invalid_specialization_info.pMapEntries = &invalid_specialization_entry;
        specialized_stages[0].pSpecializationInfo = &invalid_specialization_info;
        VkPipeline invalid_specialized_pipeline = UINT64_MAX;
        CHECK(vkCreateGraphicsPipelines(device, 0, 1, &specialized_pipeline_info, NULL,
                                        &invalid_specialized_pipeline) == VK_ERROR_INITIALIZATION_FAILED &&
              !invalid_specialized_pipeline);
        vkDestroyShaderModule(device, specialized_vertex_shader, NULL);

        if (uint_vertex_shader_path || sint_vertex_shader_path) {
            CHECK(uint_vertex_shader_path && sint_vertex_shader_path);
            const char *integer_vertex_paths[] = {
                uint_vertex_shader_path, uint_vertex_shader_path, uint_vertex_shader_path,
                sint_vertex_shader_path, sint_vertex_shader_path, sint_vertex_shader_path
            };
            const VkFormat integer_vertex_formats[] = {
                VK_FORMAT_R32_UINT, VK_FORMAT_R8_UINT, VK_FORMAT_R16_UINT,
                VK_FORMAT_R32_SINT, VK_FORMAT_R8_SINT, VK_FORMAT_R16_SINT
            };
            for (size_t integer_index = 0;
                 integer_index < sizeof integer_vertex_paths / sizeof integer_vertex_paths[0]; integer_index++) {
                uint8_t *integer_shader_bytes = read_fixture(integer_vertex_paths[integer_index], &shader_size);
                CHECK(integer_shader_bytes && !(shader_size & 3u));
                VkShaderModuleCreateInfo integer_shader_info = shader_info;
                integer_shader_info.codeSize = shader_size;
                integer_shader_info.pCode = (const uint32_t *)integer_shader_bytes;
                VkShaderModule integer_vertex_shader = 0;
                VkResult integer_result = vkCreateShaderModule(device, &integer_shader_info, NULL,
                                                                &integer_vertex_shader);
                free(integer_shader_bytes);
                CHECK(integer_result == VK_SUCCESS && integer_vertex_shader);

                VkPipelineShaderStageCreateInfo integer_stages[2] = { stages[0], stages[1] };
                integer_stages[0].module = integer_vertex_shader;
                VkVertexInputAttributeDescription integer_attribute = vertex_attribute;
                integer_attribute.format = integer_vertex_formats[integer_index];
                VkPipelineVertexInputStateCreateInfo integer_vertex_input = vertex_input;
                integer_vertex_input.pVertexAttributeDescriptions = &integer_attribute;
                VkGraphicsPipelineCreateInfo integer_pipeline_info = pipeline_info;
                integer_pipeline_info.pStages = integer_stages;
                integer_pipeline_info.pVertexInputState = &integer_vertex_input;
                VkPipeline integer_pipeline = 0;
                integer_result = vkCreateGraphicsPipelines(device, 0, 1, &integer_pipeline_info, NULL,
                                                           &integer_pipeline);
                vkDestroyPipeline(device, integer_pipeline, NULL);
                vkDestroyShaderModule(device, integer_vertex_shader, NULL);
                CHECK(integer_result == VK_SUCCESS && integer_pipeline);
            }
        }
        const VkFormat converted_float_vertex_formats[] = {
            VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM,
            VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM,
            VK_FORMAT_R16G16B16A16_SFLOAT
        };
        for (size_t converted_index = 0;
             converted_index < sizeof converted_float_vertex_formats / sizeof converted_float_vertex_formats[0];
             converted_index++) {
            VkVertexInputAttributeDescription converted_attribute = vertex_attribute;
            converted_attribute.format = converted_float_vertex_formats[converted_index];
            VkPipelineVertexInputStateCreateInfo converted_vertex_input = vertex_input;
            converted_vertex_input.pVertexAttributeDescriptions = &converted_attribute;
            VkGraphicsPipelineCreateInfo converted_pipeline_info = pipeline_info;
            converted_pipeline_info.pVertexInputState = &converted_vertex_input;
            VkPipeline converted_pipeline = 0;
            VkResult converted_result = vkCreateGraphicsPipelines(device, 0, 1, &converted_pipeline_info,
                NULL, &converted_pipeline);
            vkDestroyPipeline(device, converted_pipeline, NULL);
            CHECK(converted_result == VK_SUCCESS && converted_pipeline);
        }
        const VkDynamicState dynamic_states[3] = {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS
        };
        VkPipelineDynamicStateCreateInfo dynamic_state = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 3,
            .pDynamicStates = dynamic_states
        };
        VkPipelineViewportStateCreateInfo dynamic_viewport_state = viewport_state;
        dynamic_viewport_state.pViewports = NULL;
        dynamic_viewport_state.pScissors = NULL;
        VkPipelineColorBlendStateCreateInfo dynamic_color_blend = color_blend;
        memset(dynamic_color_blend.blendConstants, 0, sizeof dynamic_color_blend.blendConstants);
        VkGraphicsPipelineCreateInfo dynamic_pipeline_info = pipeline_info;
        dynamic_pipeline_info.pViewportState = &dynamic_viewport_state;
        dynamic_pipeline_info.pDynamicState = &dynamic_state;
        dynamic_pipeline_info.pColorBlendState = &dynamic_color_blend;
        CHECK(vkCreateGraphicsPipelines(device, 0, 1, &dynamic_pipeline_info, NULL,
                                       &dynamic_graphics_pipeline) == VK_SUCCESS && dynamic_graphics_pipeline);
        VkDynamicState duplicate_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_VIEWPORT };
        VkPipelineDynamicStateCreateInfo duplicate_state = dynamic_state;
        duplicate_state.pDynamicStates = duplicate_states;
        duplicate_state.dynamicStateCount = 2;
        VkGraphicsPipelineCreateInfo invalid_dynamic_info = dynamic_pipeline_info;
        invalid_dynamic_info.pDynamicState = &duplicate_state;
        VkPipeline rejected_dynamic_pipeline = UINT64_MAX;
        CHECK(vkCreateGraphicsPipelines(device, 0, 1, &invalid_dynamic_info, NULL,
                                       &rejected_dynamic_pipeline) == VK_ERROR_INITIALIZATION_FAILED &&
              !rejected_dynamic_pipeline);
        VkDynamicState unsupported_state = (VkDynamicState)2;
        VkPipelineDynamicStateCreateInfo unsupported_dynamic_state = dynamic_state;
        unsupported_dynamic_state.dynamicStateCount = 1;
        unsupported_dynamic_state.pDynamicStates = &unsupported_state;
        invalid_dynamic_info.pDynamicState = &unsupported_dynamic_state;
        rejected_dynamic_pipeline = UINT64_MAX;
        CHECK(vkCreateGraphicsPipelines(device, 0, 1, &invalid_dynamic_info, NULL,
                                       &rejected_dynamic_pipeline) == VK_ERROR_FEATURE_NOT_PRESENT &&
              !rejected_dynamic_pipeline);
        VkPipelineInputAssemblyStateCreateInfo unsupported_assembly = input_assembly;
        unsupported_assembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        VkGraphicsPipelineCreateInfo unsupported_info = pipeline_info;
        unsupported_info.pInputAssemblyState = &unsupported_assembly;
        VkPipeline rejected_pipeline = UINT64_MAX;
        CHECK(vkCreateGraphicsPipelines(device, 0, 1, &unsupported_info, NULL, &rejected_pipeline) ==
              VK_ERROR_FEATURE_NOT_PRESENT && !rejected_pipeline);
        VkVertexInputAttributeDescription unsupported_attribute = vertex_attribute;
        unsupported_attribute.format = VK_FORMAT_R32_UINT;
        VkPipelineVertexInputStateCreateInfo unsupported_vertex_input = vertex_input;
        unsupported_vertex_input.pVertexAttributeDescriptions = &unsupported_attribute;
        VkGraphicsPipelineCreateInfo unsupported_vertex_info = pipeline_info;
        unsupported_vertex_info.pVertexInputState = &unsupported_vertex_input;
        rejected_pipeline = UINT64_MAX;
        CHECK(vkCreateGraphicsPipelines(device, 0, 1, &unsupported_vertex_info, NULL, &rejected_pipeline) ==
              VK_ERROR_FEATURE_NOT_PRESENT && !rejected_pipeline);
        VkVertexInputAttributeDescription expanded_attribute = vertex_attribute;
        expanded_attribute.format = VK_FORMAT_R32G32B32_SFLOAT;
        VkPipelineVertexInputStateCreateInfo expanded_vertex_input = vertex_input;
        expanded_vertex_input.pVertexAttributeDescriptions = &expanded_attribute;
        VkGraphicsPipelineCreateInfo expanded_vertex_info = pipeline_info;
        expanded_vertex_info.pVertexInputState = &expanded_vertex_input;
        VkPipeline expanded_pipeline = 0;
        CHECK(vkCreateGraphicsPipelines(device, 0, 1, &expanded_vertex_info, NULL, &expanded_pipeline) ==
              VK_SUCCESS && expanded_pipeline);
        vkDestroyPipeline(device, expanded_pipeline, NULL);
    }

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = READBACK_BYTES,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkBuffer readback = 0;
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &readback) == VK_SUCCESS && readback);
    VkMemoryRequirements buffer_requirements;
    vkGetBufferMemoryRequirements(device, readback, &buffer_requirements);
    VkMemoryAllocateInfo buffer_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = buffer_requirements.size,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory buffer_memory = 0;
    CHECK(vkAllocateMemory(device, &buffer_allocation, NULL, &buffer_memory) == VK_SUCCESS && buffer_memory);
    CHECK(vkBindBufferMemory(device, readback, buffer_memory, 0) == VK_SUCCESS);

    VkBuffer index_buffer = 0;
    VkDeviceMemory index_memory = 0;
    VkBuffer vertex_buffer = 0;
    VkDeviceMemory vertex_memory = 0;
    VkBuffer index_buffer32 = 0;
    VkDeviceMemory index_memory32 = 0;
    VkBuffer indirect_draw_buffer = 0;
    VkDeviceMemory indirect_draw_memory = 0;
    if (draw_enabled) {
        VkBufferCreateInfo vertex_buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 3u * 4u * sizeof(float),
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        CHECK(vkCreateBuffer(device, &vertex_buffer_info, NULL, &vertex_buffer) == VK_SUCCESS && vertex_buffer);
        VkMemoryRequirements vertex_requirements;
        vkGetBufferMemoryRequirements(device, vertex_buffer, &vertex_requirements);
        VkMemoryAllocateInfo vertex_allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = vertex_requirements.size,
            .memoryTypeIndex = 0
        };
        CHECK(vkAllocateMemory(device, &vertex_allocation, NULL, &vertex_memory) == VK_SUCCESS && vertex_memory);
        CHECK(vkBindBufferMemory(device, vertex_buffer, vertex_memory, 0) == VK_SUCCESS);
        void *vertices_mapped = NULL;
        CHECK(vkMapMemory(device, vertex_memory, 0, vertex_requirements.size, 0, &vertices_mapped) == VK_SUCCESS &&
              vertices_mapped);
        static const float vertices[3][4] = {
            { -1.0f, -1.0f, 0.0f, 1.0f },
            {  3.0f, -1.0f, 0.0f, 1.0f },
            { -1.0f,  3.0f, 0.0f, 1.0f }
        };
        memcpy(vertices_mapped, vertices, sizeof vertices);
        vkUnmapMemory(device, vertex_memory);

        VkBufferCreateInfo index_buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 16,
            .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        CHECK(vkCreateBuffer(device, &index_buffer_info, NULL, &index_buffer) == VK_SUCCESS && index_buffer);
        VkMemoryRequirements index_requirements;
        vkGetBufferMemoryRequirements(device, index_buffer, &index_requirements);
        VkMemoryAllocateInfo index_allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = index_requirements.size,
            .memoryTypeIndex = 0
        };
        CHECK(vkAllocateMemory(device, &index_allocation, NULL, &index_memory) == VK_SUCCESS && index_memory);
        CHECK(vkBindBufferMemory(device, index_buffer, index_memory, 0) == VK_SUCCESS);
        void *indices_mapped = NULL;
        CHECK(vkMapMemory(device, index_memory, 0, index_requirements.size, 0, &indices_mapped) == VK_SUCCESS &&
              indices_mapped);
        static const uint16_t indices[8] = { 99, 99, 1, 2, 3, 99, 99, 99 };
        memcpy(indices_mapped, indices, sizeof indices);
        vkUnmapMemory(device, index_memory);

        VkBufferCreateInfo index_buffer32_info = index_buffer_info;
        index_buffer32_info.size = 3 * sizeof(uint32_t);
        CHECK(vkCreateBuffer(device, &index_buffer32_info, NULL, &index_buffer32) == VK_SUCCESS && index_buffer32);
        VkMemoryRequirements index_requirements32;
        vkGetBufferMemoryRequirements(device, index_buffer32, &index_requirements32);
        VkMemoryAllocateInfo index_allocation32 = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = index_requirements32.size,
            .memoryTypeIndex = 0
        };
        CHECK(vkAllocateMemory(device, &index_allocation32, NULL, &index_memory32) == VK_SUCCESS && index_memory32);
        CHECK(vkBindBufferMemory(device, index_buffer32, index_memory32, 0) == VK_SUCCESS);
        CHECK(vkMapMemory(device, index_memory32, 0, index_requirements32.size, 0, &indices_mapped) == VK_SUCCESS &&
              indices_mapped);
        static const uint32_t indices32[3] = { 0x01000000u, 0x01000001u, 0x01000002u };
        memcpy(indices_mapped, indices32, sizeof indices32);
        vkUnmapMemory(device, index_memory32);

        VkDrawIndirectCommand draw_arguments[2] = {
            { 3, 1, 0, 0 }, { 3, 1, 0, 0 }
        };
        VkDrawIndexedIndirectCommand indexed_arguments[2] = {
            { 3, 1, 0, -0x01000000, 0 }, { 3, 1, 0, -0x01000000, 0 }
        };
        VkBufferCreateInfo indirect_buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = sizeof draw_arguments + sizeof indexed_arguments,
            .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        CHECK(vkCreateBuffer(device, &indirect_buffer_info, NULL, &indirect_draw_buffer) ==
              VK_SUCCESS && indirect_draw_buffer);
        VkMemoryRequirements indirect_requirements;
        vkGetBufferMemoryRequirements(device, indirect_draw_buffer, &indirect_requirements);
        VkMemoryAllocateInfo indirect_allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = indirect_requirements.size,
            .memoryTypeIndex = 0
        };
        CHECK(vkAllocateMemory(device, &indirect_allocation, NULL, &indirect_draw_memory) ==
              VK_SUCCESS && indirect_draw_memory);
        CHECK(vkBindBufferMemory(device, indirect_draw_buffer, indirect_draw_memory, 0) == VK_SUCCESS);
        void *indirect_mapped = NULL;
        CHECK(vkMapMemory(device, indirect_draw_memory, 0, indirect_requirements.size, 0,
                          &indirect_mapped) == VK_SUCCESS && indirect_mapped);
        memcpy(indirect_mapped, draw_arguments, sizeof draw_arguments);
        memcpy((uint8_t *)indirect_mapped + sizeof draw_arguments, indexed_arguments, sizeof indexed_arguments);
        vkUnmapMemory(device, indirect_draw_memory);
    }

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = 0
    };
    VkCommandPool pool = 0;
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool) == VK_SUCCESS && pool);
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkCommandBuffer command_buffer = NULL;
    CHECK(vkAllocateCommandBuffers(device, &command_info, &command_buffer) == VK_SUCCESS && command_buffer);
    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkClearValue full_clear = { .color = { .float32 = { 0.0f, 0.0f, 1.0f, 1.0f } } };
    VkRenderPassBeginInfo first_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = first_pass,
        .framebuffer = first_framebuffer,
        .renderArea = { .offset = { 0, 0 }, .extent = { WIDTH, HEIGHT } },
        .clearValueCount = 1,
        .pClearValues = &full_clear
    };
    VkClearValue partial_clear = { .color = { .float32 = { 0.0f, 1.0f, 0.0f, 1.0f } } };
    VkRenderPassBeginInfo second_begin = first_begin;
    second_begin.renderPass = second_pass;
    second_begin.framebuffer = second_framebuffer;
    second_begin.renderArea = (VkRect2D){ .offset = { 1, 1 }, .extent = { 2, 1 } };
    second_begin.pClearValues = &partial_clear;
    if (draw_enabled && sampled_fragment_shader_path)
        CHECK(check_graphics_sampled_descriptor_submission(device, queue, command_buffer, &first_begin, second_pass,
            &second_begin, image, readback, buffer_memory, vertex_shader, vertex_buffer, 0,
            0, sampled_fragment_shader_path) == 0);
    if (draw_enabled && separate_sampled_fragment_shader_path)
        CHECK(check_graphics_sampled_descriptor_submission(device, queue, command_buffer, &first_begin, second_pass,
            &second_begin, image, readback, buffer_memory, vertex_shader, vertex_buffer, 1,
            0, separate_sampled_fragment_shader_path) == 0);
    if (draw_enabled && implicit_sampled_fragment_shader_path)
        CHECK(check_graphics_sampled_descriptor_submission(device, queue, command_buffer, &first_begin, second_pass,
            &second_begin, image, readback, buffer_memory, vertex_shader, vertex_buffer, 0,
            1, implicit_sampled_fragment_shader_path) == 0);
    if (draw_enabled && implicit_separate_sampled_fragment_shader_path)
        CHECK(check_graphics_sampled_descriptor_submission(device, queue, command_buffer, &first_begin, second_pass,
            &second_begin, image, readback, buffer_memory, vertex_shader, vertex_buffer, 1,
            1, implicit_separate_sampled_fragment_shader_path) == 0);
    if (draw_enabled) {
        VkViewport seeded_viewport = { .x = 1.0f, .y = HEIGHT, .width = 2.0f, .height = -HEIGHT,
                                       .minDepth = 0.0f, .maxDepth = 1.0f };
        VkRect2D seeded_scissor = { .offset = { 1, 1 }, .extent = { 1, 1 } };
        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
        vkCmdBeginRenderPass(command_buffer, &second_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
        vkCmdDraw(command_buffer, 3, 1, 0, 0);
        vkCmdEndRenderPass(command_buffer);
        CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
        CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
        vkCmdBeginRenderPass(command_buffer, &first_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetBlendConstants(command_buffer, dynamic_blend_constants);
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, dynamic_graphics_pipeline);
        vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer, (VkDeviceSize[]){ 0 });
        vkCmdSetViewport(command_buffer, 0, 1, &seeded_viewport);
        vkCmdSetScissor(command_buffer, 0, 1, &seeded_scissor);
        vkCmdDraw(command_buffer, 3, 1, 0, 0);
        vkCmdEndRenderPass(command_buffer);
        CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
        CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
        vkCmdBeginRenderPass(command_buffer, &first_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, dynamic_graphics_pipeline);
        vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer, (VkDeviceSize[]){ 0 });
        vkCmdSetBlendConstants(command_buffer, dynamic_blend_constants);
        vkCmdDraw(command_buffer, 3, 1, 0, 0);
        CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
        CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
        vkCmdBeginRenderPass(command_buffer, &first_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, dynamic_graphics_pipeline);
        vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer, (VkDeviceSize[]){ 0 });
        vkCmdSetViewport(command_buffer, 0, 1, &seeded_viewport);
        vkCmdSetScissor(command_buffer, 0, 1, &seeded_scissor);
        vkCmdDraw(command_buffer, 3, 1, 0, 0);
        CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
        CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
        vkCmdBeginRenderPass(command_buffer, &second_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
        vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer, (VkDeviceSize[]){ 0 });
        vkCmdDrawIndirect(command_buffer, indirect_draw_buffer, 0, 2, 12);
        vkCmdEndRenderPass(command_buffer);
        CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
        CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    }
    VkBufferImageCopy copy = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { WIDTH, HEIGHT, 1 }
    };
    VkBufferMemoryBarrier host_read = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = readback,
        .size = READBACK_BYTES
    };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBeginRenderPass(command_buffer, &first_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(command_buffer);
    vkCmdBeginRenderPass(command_buffer, &second_begin, VK_SUBPASS_CONTENTS_INLINE);
    if (draw_enabled) {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
        vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer, (VkDeviceSize[]){ 0 });
        vkCmdDrawIndirect(command_buffer, indirect_draw_buffer, 0, 2, sizeof(VkDrawIndirectCommand));
        VkViewport dynamic_viewport = { .x = 1.0f, .y = HEIGHT, .width = 2.0f, .height = -HEIGHT,
                                        .minDepth = 0.0f, .maxDepth = 1.0f };
        VkRect2D dynamic_scissor = { .offset = { 2, 1 }, .extent = { 1, 1 } };
        vkCmdSetBlendConstants(command_buffer, dynamic_blend_constants);
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, dynamic_graphics_pipeline);
        vkCmdBindIndexBuffer(command_buffer, index_buffer32, 0, VK_INDEX_TYPE_UINT32);
        vkCmdSetViewport(command_buffer, 0, 1, &dynamic_viewport);
        vkCmdSetScissor(command_buffer, 0, 1, &dynamic_scissor);
        vkCmdDrawIndexedIndirect(command_buffer, indirect_draw_buffer,
            sizeof(VkDrawIndirectCommand) * 2u, 2, sizeof(VkDrawIndexedIndirectCommand));
    }
    vkCmdEndRenderPass(command_buffer);
    vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, NULL, 1, &host_read, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = 0;
    CHECK(vkCreateFence(device, &fence_info, NULL, &fence) == VK_SUCCESS && fence);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    void *mapped = NULL;
    CHECK(vkMapMemory(device, buffer_memory, 0, READBACK_BYTES, 0, &mapped) == VK_SUCCESS && mapped);
    const uint8_t blue[4] = { 0, 0, 255, 255 }, red[4] = { 255, 0, 0, 255 };
    const uint8_t green[4] = { 0, 255, 0, 255 };
    const uint8_t dynamic_red[4] = { 255, 0, 0, 255 };
    for (uint32_t y = 0; y < HEIGHT; y++) {
        for (uint32_t x = 0; x < WIDTH; x++) {
            const uint8_t *expected = draw_enabled ?
                (y == 1 && x == 1 ? red : y == 1 && x == 2 ? dynamic_red : blue) :
                (y == 1 && (x == 1 || x == 2) ? green : blue);
            CHECK(memcmp((const uint8_t *)mapped + (y * WIDTH + x) * PIXEL_BYTES,
                         expected, PIXEL_BYTES) == 0);
        }
    }
    vkUnmapMemory(device, buffer_memory);
    CHECK(vkDeviceWaitIdle(device) == VK_SUCCESS);
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyPipeline(device, graphics_pipeline, NULL);
    vkDestroyPipeline(device, dynamic_graphics_pipeline, NULL);
    vkDestroyPipelineLayout(device, graphics_layout, NULL);
    vkDestroyShaderModule(device, fragment_shader, NULL);
    vkDestroyShaderModule(device, vertex_shader, NULL);
    vkDestroyBuffer(device, readback, NULL);
    vkFreeMemory(device, buffer_memory, NULL);
    if (indirect_draw_buffer) vkDestroyBuffer(device, indirect_draw_buffer, NULL);
    if (indirect_draw_memory) vkFreeMemory(device, indirect_draw_memory, NULL);
    if (vertex_buffer) vkDestroyBuffer(device, vertex_buffer, NULL);
    if (vertex_memory) vkFreeMemory(device, vertex_memory, NULL);
    if (index_buffer) vkDestroyBuffer(device, index_buffer, NULL);
    if (index_memory) vkFreeMemory(device, index_memory, NULL);
    if (index_buffer32) vkDestroyBuffer(device, index_buffer32, NULL);
    if (index_memory32) vkFreeMemory(device, index_memory32, NULL);
    vkDestroyFramebuffer(device, second_framebuffer, NULL);
    vkDestroyFramebuffer(device, first_framebuffer, NULL);
    vkDestroyRenderPass(device, second_pass, NULL);
    vkDestroyRenderPass(device, first_pass, NULL);
    vkDestroyImageView(device, view, NULL);
    vkDestroyImage(device, image, NULL);
    vkFreeMemory(device, image_memory, NULL);
    puts(draw_enabled ?
        "vulkan graphics: static/dynamic viewport and scissor, blend constants, indexed uint32 direct/indirect draws, clipped render area and pixel readback: ok" :
        "vulkan render pass: one color attachment, implicit layout transitions, full/partial clear and readback: ok");
    return 0;
}

static int graphics_sync_stage_test(VkDevice device, VkQueue queue) {
    const VkPipelineStageFlags stages[] = {
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
    };
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = 0
    };
    VkCommandPool pool = 0;
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool) == VK_SUCCESS && pool);
    VkCommandBufferAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkCommandBuffer command_buffer = NULL;
    CHECK(vkAllocateCommandBuffers(device, &allocate_info, &command_buffer) == VK_SUCCESS && command_buffer);
    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    for (size_t i = 0; i < sizeof stages / sizeof stages[0]; i++) {
        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
        vkCmdPipelineBarrier(command_buffer, stages[i], stages[i], 0, 0, NULL, 0, NULL, 0, NULL);
        VkResult end_result = vkEndCommandBuffer(command_buffer);
        if (end_result != VK_SUCCESS) {
            fprintf(stderr, "vulkan graphics synchronization: barrier stage %#x returned %d\n",
                    stages[i], end_result);
            return 1;
        }
        CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    }

    const struct {
        VkPipelineStageFlags stage;
        VkAccessFlags access;
    } valid_accesses[] = {
        { VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_INDEX_READ_BIT },
        { VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT },
        { VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_ACCESS_UNIFORM_READ_BIT },
        { VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_INPUT_ATTACHMENT_READ_BIT },
        { VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT },
        { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT },
        { VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT },
        { VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT },
        { VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_READ_BIT },
        { VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT },
        { VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT },
    };
    for (size_t i = 0; i < sizeof valid_accesses / sizeof valid_accesses[0]; i++) {
        VkMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = valid_accesses[i].access,
            .dstAccessMask = valid_accesses[i].access
        };
        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
        vkCmdPipelineBarrier(command_buffer, valid_accesses[i].stage, valid_accesses[i].stage, 0,
                             1, &barrier, 0, NULL, 0, NULL);
        CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
        CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    }
    const struct {
        VkPipelineStageFlags stage;
        VkAccessFlags access;
    } invalid_accesses[] = {
        { VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_READ_BIT },
        { VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_HOST_READ_BIT },
    };
    for (size_t i = 0; i < sizeof invalid_accesses / sizeof invalid_accesses[0]; i++) {
        VkMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = invalid_accesses[i].access,
            .dstAccessMask = invalid_accesses[i].access
        };
        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
        vkCmdPipelineBarrier(command_buffer, invalid_accesses[i].stage, invalid_accesses[i].stage, 0,
                             1, &barrier, 0, NULL, 0, NULL);
        CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
        CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    }

    VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore semaphore = 0;
    CHECK(vkCreateSemaphore(device, &semaphore_info, NULL, &semaphore) == VK_SUCCESS && semaphore);
    VkSubmitInfo signal = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &semaphore
    };
    VkSubmitInfo wait = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &semaphore
    };
    for (size_t i = 0; i < sizeof stages / sizeof stages[0]; i++) {
        wait.pWaitDstStageMask = &stages[i];
        CHECK(vkQueueSubmit(queue, 1, &signal, 0) == VK_SUCCESS);
        CHECK(vkQueueSubmit(queue, 1, &wait, 0) == VK_SUCCESS);
    }

    const VkPipelineStageFlags unsupported_stages[] = {
        VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT,
        VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT,
        VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT,
    };
    // Reuse one signaled semaphore across rejected waits. A failed submit must
    // leave its binary semaphore state untouched.
    CHECK(vkQueueSubmit(queue, 1, &signal, 0) == VK_SUCCESS);
    for (size_t i = 0; i < sizeof unsupported_stages / sizeof unsupported_stages[0]; i++) {
        VkPipelineStageFlags unsupported_stage = unsupported_stages[i];
        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
        vkCmdPipelineBarrier(command_buffer, unsupported_stage, unsupported_stage, 0, 0, NULL, 0, NULL, 0, NULL);
        CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
        CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
        wait.pWaitDstStageMask = &unsupported_stage;
        CHECK(vkQueueSubmit(queue, 1, &wait, 0) == VK_ERROR_INITIALIZATION_FAILED);
    }

    vkDestroySemaphore(device, semaphore, NULL);
    vkFreeCommandBuffers(device, pool, 1, &command_buffer);
    vkDestroyCommandPool(device, pool, NULL);
    puts("vulkan synchronization: supported graphics barrier and semaphore-wait stages: ok");
    return 0;
}

static int check_present_result_priority(void) {
#ifdef VX_VULKAN_TEST_HOOKS
    const VkResult out_of_date_then_surface_lost[] = { VK_ERROR_OUT_OF_DATE_KHR, VK_ERROR_SURFACE_LOST_KHR };
    const VkResult surface_lost_then_out_of_date[] = { VK_ERROR_SURFACE_LOST_KHR, VK_ERROR_OUT_OF_DATE_KHR };
    const VkResult surface_lost_then_device_lost[] = { VK_ERROR_SURFACE_LOST_KHR, VK_ERROR_DEVICE_LOST };
    const VkResult suboptimal_then_success[] = { VK_SUBOPTIMAL_KHR, VK_SUCCESS };
    const VkResult success_then_out_of_date[] = { VK_SUCCESS, VK_ERROR_OUT_OF_DATE_KHR };
    CHECK(vx_vk_test_select_present_result(NULL, 0) == VK_SUCCESS);
    CHECK(vx_vk_test_select_present_result(out_of_date_then_surface_lost, 2) == VK_ERROR_SURFACE_LOST_KHR);
    CHECK(vx_vk_test_select_present_result(surface_lost_then_out_of_date, 2) == VK_ERROR_SURFACE_LOST_KHR);
    CHECK(vx_vk_test_select_present_result(surface_lost_then_device_lost, 2) == VK_ERROR_DEVICE_LOST);
    CHECK(vx_vk_test_select_present_result(suboptimal_then_success, 2) == VK_SUBOPTIMAL_KHR);
    CHECK(vx_vk_test_select_present_result(success_then_out_of_date, 2) == VK_ERROR_OUT_OF_DATE_KHR);
    return 0;
#else
    return 1;
#endif
}

static int check_color_blend_conversion(void) {
#ifdef VX_VULKAN_TEST_HOOKS
    VkPipelineColorBlendAttachmentState attachment = {
        .blendEnable = 1,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_B_BIT
    };
    VkPipelineColorBlendStateCreateInfo source = {
        .attachmentCount = 1,
        .pAttachments = &attachment,
        .blendConstants = { 0.2f, 0.4f, 0.6f, 0.8f }
    };
    gpu_ir_color_blend_state normalized = { 0 };
    CHECK(vx_vk_test_build_color_blend(&source, &normalized) == GPU_IR_OK);
    CHECK(normalized.attachment_count == 1 && normalized.attachments[0].enabled);
    CHECK(normalized.attachments[0].source_color_factor == GPU_IR_BLEND_FACTOR_SOURCE_ALPHA);
    CHECK(normalized.attachments[0].destination_color_factor == GPU_IR_BLEND_FACTOR_ONE_MINUS_SOURCE_ALPHA);
    CHECK(normalized.attachments[0].alpha_operation == GPU_IR_BLEND_OPERATION_REVERSE_SUBTRACT);
    CHECK(normalized.attachments[0].write_mask == (GPU_IR_COLOR_WRITE_RED | GPU_IR_COLOR_WRITE_BLUE));
    CHECK(normalized.blend_constants[1] == 0.4f);
    CHECK(!vx_vk_test_color_blend_uses_constants(&normalized));

    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
    CHECK(vx_vk_test_build_color_blend(&source, &normalized) == GPU_IR_OK);
    CHECK(vx_vk_test_color_blend_uses_constants(&normalized));
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
    CHECK(vx_vk_test_build_color_blend(&source, &normalized) == GPU_IR_OK);
    CHECK(vx_vk_test_color_blend_uses_constants(&normalized));
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    attachment.blendEnable = 0;
    CHECK(vx_vk_test_build_color_blend(&source, &normalized) == GPU_IR_OK);
    CHECK(!vx_vk_test_color_blend_uses_constants(&normalized));
    attachment.blendEnable = 1;

    uint32_t has_dynamic_constants = 99;
    float resolved_constants[4] = { 9.0f, 9.0f, 9.0f, 9.0f };
    const float dynamic_constants[4] = { 1.25f, -0.25f, 0.5f, 0.75f };
    CHECK(vx_vk_test_resolve_draw_blend_constants(1, 0, &normalized, NULL,
          &has_dynamic_constants, resolved_constants) == GPU_IR_OK && !has_dynamic_constants);
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
    CHECK(vx_vk_test_build_color_blend(&source, &normalized) == GPU_IR_OK);
    CHECK(vx_vk_test_resolve_draw_blend_constants(1, 0, &normalized, NULL,
          &has_dynamic_constants, resolved_constants) == GPU_IR_INVALID);
    CHECK(vx_vk_test_resolve_draw_blend_constants(1, 1, &normalized, dynamic_constants,
          &has_dynamic_constants, resolved_constants) == GPU_IR_OK && has_dynamic_constants);
    CHECK(memcmp(resolved_constants, dynamic_constants, sizeof dynamic_constants) == 0);
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;

    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC1_COLOR;
    CHECK(vx_vk_test_build_color_blend(&source, &normalized) == GPU_IR_UNSUPPORTED);
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    source.logicOpEnable = 1;
    CHECK(vx_vk_test_build_color_blend(&source, &normalized) == GPU_IR_UNSUPPORTED);
    source.logicOpEnable = 0;
    source.attachmentCount = 2;
    CHECK(vx_vk_test_build_color_blend(&source, &normalized) == GPU_IR_UNSUPPORTED);
    source.attachmentCount = 1;
    source.logicOp = -1;
    CHECK(vx_vk_test_build_color_blend(&source, &normalized) == GPU_IR_INVALID);
    source.logicOp = VK_LOGIC_OP_COPY;
    source.blendConstants[0] = 1.5f;
    CHECK(vx_vk_test_build_color_blend(&source, &normalized) == GPU_IR_OK);
    CHECK(normalized.blend_constants[0] == 1.5f);
    return 0;
#else
    return 1;
#endif
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--check-color-blend-conversion")) {
        int result = check_color_blend_conversion();
        if (!result) puts("vulkan pipeline: fixed/dynamic color blend state conversion and unsupported cases: ok");
        return result;
    }
    if (argc == 2 && !strcmp(argv[1], "--check-present-result-priority")) {
        int result = check_present_result_priority();
        if (!result) puts("vulkan WSI: multi-swapchain present result precedence: ok");
        return result;
    }
    if (argc == 3 && !strcmp(argv[1], "--check-graphics-specialization-fixture"))
        return check_specialized_vertex_fixture_cpu(argv[2]);
    if (argc > 38) return 2;
    vx_vulkan_init();
    // (vulkan-1.dll exports every implemented command, as the Khronos loader does: once each, by its own function)
    CHECK(registered_count > 100);
    CHECK(strcmp(registered[0].name, "vkGetInstanceProcAddr") == 0);
    CHECK(registered[0].fn == (void *)vkGetInstanceProcAddr);
    for (size_t i = 0; i < registered_count; i++)
        for (size_t j = i + 1; j < registered_count; j++) CHECK(strcmp(registered[i].name, registered[j].name) != 0);
    int exports_create_device = 0, exports_cmd_draw = 0;
    for (size_t i = 0; i < registered_count; i++) {
        if (!strcmp(registered[i].name, "vkCreateDevice")) exports_create_device = registered[i].fn == (void *)vkCreateDevice;
        if (!strcmp(registered[i].name, "vkCmdDraw")) exports_cmd_draw = registered[i].fn == (void *)vkCmdDraw;
    }
    CHECK(exports_create_device && exports_cmd_draw);

    uint32_t version = 0;
    CHECK(vkEnumerateInstanceVersion(&version) == VK_SUCCESS);
    CHECK(version == VK_API_VERSION_1_0);
    CHECK(vkEnumerateInstanceVersion(NULL) == VK_ERROR_INITIALIZATION_FAILED);

    uint32_t count = 10;
    CHECK(vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) == VK_SUCCESS && count == 2);
    VkExtensionProperties instance_extensions[2] = { 0 };
    count = 0;
    CHECK(vkEnumerateInstanceExtensionProperties(NULL, &count, instance_extensions) == VK_INCOMPLETE && count == 0);
    count = 1;
    CHECK(vkEnumerateInstanceExtensionProperties(NULL, &count, instance_extensions) == VK_INCOMPLETE && count == 1 &&
          !strcmp(instance_extensions[0].extensionName, VK_KHR_SURFACE_EXTENSION_NAME) &&
          instance_extensions[0].specVersion == VK_KHR_SURFACE_SPEC_VERSION);
    count = 2;
    CHECK(vkEnumerateInstanceExtensionProperties(NULL, &count, instance_extensions) == VK_SUCCESS && count == 2 &&
          !strcmp(instance_extensions[1].extensionName, VK_KHR_WIN32_SURFACE_EXTENSION_NAME) &&
          instance_extensions[1].specVersion == VK_KHR_WIN32_SURFACE_SPEC_VERSION);
    count = 10;
    CHECK(vkEnumerateInstanceLayerProperties(&count, NULL) == VK_SUCCESS && count == 0);
    count = 10;
    CHECK(vkEnumerateInstanceExtensionProperties("missing-layer", &count, NULL) == VK_ERROR_LAYER_NOT_PRESENT);

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .apiVersion = VK_API_VERSION_1_0 | 7 };
    VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                  .pApplicationInfo = &app };
    const char *instance_extension_names[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME
    };
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = instance_extension_names;
    VkAllocationCallbacks allocator = { .pUserData = (void *)0x1234,
                                        .pfnAllocation = test_allocate,
                                        .pfnFree = test_free };
    VkInstance instance = NULL;
    const char *win32_only_extension[] = { VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = win32_only_extension;
    CHECK(vkCreateInstance(&info, NULL, &instance) == VK_ERROR_EXTENSION_NOT_PRESENT && instance == NULL);
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = instance_extension_names;
    CHECK(vkCreateInstance(&info, &allocator, &instance) == VK_SUCCESS);
    CHECK(instance != NULL && allocations == 1 && frees == 0);
    CHECK(vkGetInstanceProcAddr(NULL, "vkCreateInstance") != NULL);
    CHECK(vkGetInstanceProcAddr(NULL, "vkDestroyInstance") == NULL);
    CHECK(vkGetInstanceProcAddr(instance, "vkDestroyInstance") == (PFN_vkVoidFunction)vkDestroyInstance);
    CHECK(vkGetInstanceProcAddr(instance, "vkUnknownCommand") == NULL);

    uint32_t physical_count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &physical_count, NULL) == VK_SUCCESS && physical_count == 1);
    VkPhysicalDevice physical = NULL;
    physical_count = 1;
    CHECK(vkEnumeratePhysicalDevices(instance, &physical_count, &physical) == VK_SUCCESS && physical_count == 1);
    CHECK(test_win32_surface_queries(instance, physical) == 0);
    puts("vulkan WSI: Win32 surface extension negotiation, surface support/capability/format/FIFO queries: ok");
    uint32_t device_extension_count = 0;
    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &device_extension_count, NULL) == VK_SUCCESS &&
          device_extension_count == 2);
    VkExtensionProperties device_extensions[2];
    device_extension_count = 0;
    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &device_extension_count, device_extensions) ==
          VK_INCOMPLETE && device_extension_count == 0);
    device_extension_count = 1;
    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &device_extension_count, device_extensions) ==
          VK_INCOMPLETE && device_extension_count == 1 &&
          !strcmp(device_extensions[0].extensionName, VK_KHR_MAINTENANCE1_EXTENSION_NAME) &&
          device_extensions[0].specVersion == VK_KHR_MAINTENANCE1_SPEC_VERSION);
    device_extension_count = 2;
    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &device_extension_count, device_extensions) ==
          VK_SUCCESS && device_extension_count == 2 &&
          !strcmp(device_extensions[1].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) &&
          device_extensions[1].specVersion == VK_KHR_SWAPCHAIN_SPEC_VERSION);
    device_extension_count = 1;
    CHECK(vkEnumerateDeviceExtensionProperties(physical, "missing-layer", &device_extension_count,
          device_extensions) == VK_ERROR_LAYER_NOT_PRESENT);
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(physical, &features);
    CHECK(features.robustBufferAccess == 1 && features.fullDrawIndexUint32 == 1 &&
          features.multiDrawIndirect == 1 && features.drawIndirectFirstInstance == 1 &&
          features.shaderSampledImageArrayDynamicIndexing == 1 &&
          features.shaderStorageBufferArrayDynamicIndexing == 1 &&
          features.samplerAnisotropy <= 1 &&
          features.geometryShader == 0 && features.tessellationShader == 0 && features.shaderFloat64 == 0);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    CHECK(properties.apiVersion == VK_API_VERSION_1_0 &&
          properties.limits.maxImageDimension1D == 4096 && properties.limits.maxImageDimension2D == 4096 &&
          properties.limits.maxImageDimension3D == 256 && properties.limits.maxImageDimensionCube == 4096 &&
          properties.limits.maxVertexInputAttributes == 16 && properties.limits.maxVertexInputBindings == 16 &&
          properties.limits.maxVertexInputAttributeOffset == 2047 &&
          properties.limits.maxVertexInputBindingStride == 2048 &&
          properties.limits.maxDrawIndexedIndexValue == UINT32_MAX &&
          properties.limits.maxDrawIndirectCount == 4096 &&
          properties.limits.viewportBoundsRange[0] <= -8192.0f &&
          properties.limits.viewportBoundsRange[1] >= 8191.0f &&
          properties.limits.maxImageArrayLayers == 256 &&
          properties.limits.maxPerStageDescriptorSampledImages == 16 &&
          properties.limits.maxDescriptorSetSampledImages == 16 &&
          properties.limits.maxPerStageDescriptorSamplers == 16 &&
          properties.limits.maxDescriptorSetSamplers == 16 &&
          properties.limits.maxSamplerAllocationCount == 4096 &&
          properties.limits.maxSamplerAnisotropy >= 1.0f &&
          properties.limits.maxPerStageDescriptorUniformBuffers == 12 &&
          properties.limits.maxPerStageDescriptorStorageBuffers == 18 &&
          properties.limits.maxPerStageResources == 46 + properties.limits.maxPerStageDescriptorStorageImages &&
          properties.limits.maxDescriptorSetUniformBuffers == 12 &&
          properties.limits.maxDescriptorSetUniformBuffersDynamic == 8 &&
          properties.limits.maxDescriptorSetStorageBuffers == 18 &&
          properties.limits.maxDescriptorSetStorageBuffersDynamic == 4 &&
          properties.limits.maxPushConstantsSize == 128 &&
          properties.limits.maxComputeWorkGroupInvocations == 128 &&
          properties.limits.maxComputeSharedMemorySize > 0 &&
          properties.limits.maxComputeWorkGroupCount[0] == 65535 &&
          properties.limits.maxComputeWorkGroupCount[1] == 65535 &&
          properties.limits.maxComputeWorkGroupCount[2] == 65535);
    CHECK(features.samplerAnisotropy == (properties.limits.maxSamplerAnisotropy > 1.0f));
    CHECK(features.shaderStorageImageArrayDynamicIndexing ==
          (properties.limits.maxPerStageDescriptorStorageImages ? 1u : 0u));
    VkFormatProperties format_properties = { UINT32_MAX, UINT32_MAX, UINT32_MAX };
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, &format_properties);
    const VkFormatFeatureFlags transfer_features = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT_KHR |
                                                   VK_FORMAT_FEATURE_TRANSFER_DST_BIT_KHR;
    const VkFormatFeatureFlags color_attachment_feature = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    CHECK(format_properties.linearTilingFeatures == transfer_features &&
          (format_properties.optimalTilingFeatures & ~color_attachment_feature) ==
              (transfer_features | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
               (properties.limits.maxPerStageDescriptorStorageImages ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT : 0)) &&
          format_properties.bufferFeatures == VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
    const VkFormat transfer_only_formats[] = {
        VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT
    };
    for (size_t i = 0; i < sizeof transfer_only_formats / sizeof transfer_only_formats[0]; i++) {
        vkGetPhysicalDeviceFormatProperties(physical, transfer_only_formats[i], &format_properties);
        CHECK(format_properties.linearTilingFeatures == transfer_features &&
              (format_properties.optimalTilingFeatures & ~color_attachment_feature) == transfer_features &&
              format_properties.bufferFeatures == VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
    }
    const VkFormat sampled_formats[] = {
        VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT
    };
    for (size_t i = 0; i < sizeof sampled_formats / sizeof sampled_formats[0]; i++) {
        vkGetPhysicalDeviceFormatProperties(physical, sampled_formats[i], &format_properties);
        CHECK(format_properties.linearTilingFeatures == transfer_features &&
              (format_properties.optimalTilingFeatures & ~color_attachment_feature) ==
                  (transfer_features | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
              format_properties.bufferFeatures == (sampled_formats[i] == VK_FORMAT_R8_UNORM ||
                  sampled_formats[i] == VK_FORMAT_R8G8_UNORM ||
                  sampled_formats[i] == VK_FORMAT_R16G16B16A16_SFLOAT ||
                  sampled_formats[i] == VK_FORMAT_R32_SFLOAT ?
                  VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT : 0));
    }
    const VkFormat vertex_formats[] = {
        VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM,
        VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
        VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SFLOAT,
        VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SINT,
        VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_UINT
    };
    for (size_t i = 0; i < sizeof vertex_formats / sizeof vertex_formats[0]; i++) {
        vkGetPhysicalDeviceFormatProperties(physical, vertex_formats[i], &format_properties);
        CHECK(format_properties.bufferFeatures == VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
    }
    VkImageFormatProperties image_format_properties;
    CHECK(vkGetPhysicalDeviceImageFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        0, &image_format_properties) == VK_SUCCESS);
    CHECK(image_format_properties.maxExtent.width == 4096 && image_format_properties.maxExtent.height == 4096 &&
          image_format_properties.maxExtent.depth == 1 && image_format_properties.maxMipLevels == 1 &&
          image_format_properties.maxArrayLayers == 256 && image_format_properties.sampleCounts == VK_SAMPLE_COUNT_1_BIT);
    CHECK(vkGetPhysicalDeviceImageFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, &image_format_properties) ==
        VK_SUCCESS);
    CHECK(image_format_properties.maxMipLevels == 1 && image_format_properties.maxArrayLayers == 256);
    CHECK(vkGetPhysicalDeviceImageFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT, 0, &image_format_properties) ==
        (properties.limits.maxPerStageDescriptorStorageImages ? VK_SUCCESS : VK_ERROR_FORMAT_NOT_SUPPORTED));
    CHECK(vkGetPhysicalDeviceImageFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0, &image_format_properties) == VK_SUCCESS);
    CHECK(image_format_properties.maxMipLevels == 1 && image_format_properties.maxArrayLayers == 1);
    CHECK(vkGetPhysicalDeviceImageFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_SAMPLED_BIT, 0, &image_format_properties) ==
        VK_ERROR_FORMAT_NOT_SUPPORTED);
    CHECK(vkGetPhysicalDeviceImageFormatProperties(physical, VK_FORMAT_R32_UINT, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &image_format_properties) == VK_SUCCESS &&
        image_format_properties.maxArrayLayers == 256);
    CHECK(vkGetPhysicalDeviceImageFormatProperties(physical, VK_FORMAT_R32_UINT, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, &image_format_properties) == VK_SUCCESS &&
        image_format_properties.maxArrayLayers == 256);
    CHECK(vkGetPhysicalDeviceImageFormatProperties(physical, VK_FORMAT_R32_UINT, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0, &image_format_properties) ==
        VK_ERROR_FORMAT_NOT_SUPPORTED);
    for (size_t i = 0; i < sizeof sampled_formats / sizeof sampled_formats[0]; i++) {
        CHECK(vkGetPhysicalDeviceImageFormatProperties(physical, sampled_formats[i], VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            0, &image_format_properties) == VK_SUCCESS && image_format_properties.maxArrayLayers == 1);
        CHECK(vkGetPhysicalDeviceImageFormatProperties(physical, sampled_formats[i], VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_SAMPLED_BIT, 0, &image_format_properties) ==
            VK_ERROR_FORMAT_NOT_SUPPORTED);
    }
    CHECK(properties.limits.maxComputeWorkGroupSize[0] >= 128 &&
          properties.limits.maxComputeWorkGroupSize[1] >= 128 &&
          properties.limits.maxComputeWorkGroupSize[2] >= 64);
    CHECK(properties.limits.maxColorAttachments == 1 && properties.limits.maxViewports == 1 &&
          ((properties.limits.maxVertexOutputComponents == 64 &&
            properties.limits.maxFragmentInputComponents == 60) ||
           (properties.limits.maxVertexOutputComponents == 128 &&
            properties.limits.maxFragmentInputComponents == 124)) &&
          properties.limits.maxFramebufferWidth == 4096 && properties.limits.maxFramebufferHeight == 4096 &&
          properties.limits.framebufferColorSampleCounts == VK_SAMPLE_COUNT_1_BIT);
    CHECK(properties.limits.minMemoryMapAlignment >= 64 &&
          !(properties.limits.minMemoryMapAlignment & (properties.limits.minMemoryMapAlignment - 1)));
    CHECK(properties.limits.nonCoherentAtomSize == 1);
    VkQueueFamilyProperties family;
    uint32_t family_count = 1;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, &family);
    CHECK(family_count == 1 && family.queueCount == 1 &&
          family.queueFlags == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT));
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    CHECK(memory_properties.memoryTypeCount == 1 && memory_properties.memoryHeapCount == 1 &&
          (memory_properties.memoryTypes[0].propertyFlags &
           (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
          (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &priority
    };
    const char *maintenance1_extension[] = { VK_KHR_MAINTENANCE1_EXTENSION_NAME };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = maintenance1_extension
    };
    VkPhysicalDeviceFeatures unsupported_features = { .geometryShader = 1 };
    VkDeviceCreateInfo unsupported_device_info = device_info;
    unsupported_device_info.pEnabledFeatures = &unsupported_features;
    VkDevice rejected_device = NULL;
    CHECK(vkCreateDevice(physical, &unsupported_device_info, NULL, &rejected_device) ==
          VK_ERROR_FEATURE_NOT_PRESENT && rejected_device == NULL);
    VkPhysicalDeviceFeatures invalid_full_index = { .fullDrawIndexUint32 = 2 };
    unsupported_device_info = device_info;
    unsupported_device_info.pEnabledFeatures = &invalid_full_index;
    CHECK(vkCreateDevice(physical, &unsupported_device_info, NULL, &rejected_device) ==
          VK_ERROR_FEATURE_NOT_PRESENT && rejected_device == NULL);
    const char *unsupported_device_extension[] = { "VK_EXT_not_implemented" };
    unsupported_device_info = device_info;
    unsupported_device_info.enabledExtensionCount = 1;
    unsupported_device_info.ppEnabledExtensionNames = unsupported_device_extension;
    CHECK(vkCreateDevice(physical, &unsupported_device_info, NULL, &rejected_device) ==
          VK_ERROR_EXTENSION_NOT_PRESENT && rejected_device == NULL);
    VkPhysicalDeviceFeatures invalid_sampled_indexing = { .shaderSampledImageArrayDynamicIndexing = 2 };
    unsupported_device_info = device_info;
    unsupported_device_info.pEnabledFeatures = &invalid_sampled_indexing;
    CHECK(vkCreateDevice(physical, &unsupported_device_info, NULL, &rejected_device) ==
          VK_ERROR_FEATURE_NOT_PRESENT && rejected_device == NULL);
    device_info.pEnabledFeatures = &features;
    VkDevice device = NULL;
    CHECK(vkCreateDevice(physical, &device_info, NULL, &device) == VK_SUCCESS && device != NULL);
    const uint32_t missing_memory_model_code[] = { SPIRV_MAGIC, 0x00010000u, 0, 1, 0 };
    VkShaderModuleCreateInfo invalid_shader_module_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof missing_memory_model_code,
        .pCode = missing_memory_model_code
    };
    VkShaderModule invalid_shader_module = UINT64_MAX;
    CHECK(vkCreateShaderModule(device, &invalid_shader_module_info, NULL, &invalid_shader_module) ==
          VK_ERROR_INITIALIZATION_FAILED && !invalid_shader_module);
    const uint32_t subgroup_builtin_shader[] = {
        SPIRV_MAGIC, 0x00010000u, 0, 14, 0,
        0x00020011u, 1,                                      // OpCapability Shader
        0x0003000eu, SPIRV_ADDRESSING_MODEL_LOGICAL, SPIRV_MEMORY_MODEL_GLSL450,
        0x0007000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 10, 0x6e69616du, 0, 12, 13,
        0x00060010u, 10, 17, 1, 1, 1,                       // LocalSize 1, 1, 1
        0x00040047u, 12, 11, 36,                            // BuiltIn SubgroupSize
        0x00040047u, 13, 11, 41,                            // BuiltIn SubgroupLocalInvocationId
        0x00020013u, 1,                                      // OpTypeVoid
        0x00040015u, 2, 32, 0,                               // OpTypeInt uint32
        0x00040020u, 3, 1, 2,                                // Input pointer to uint32
        0x00030021u, 4, 1,                                   // void()
        0x0004003bu, 3, 12, 1,                               // SubgroupSize input
        0x0004003bu, 3, 13, 1,                               // SubgroupLocalInvocationId input
        0x00050036u, 1, 10, 0, 4,                            // main
        0x000200f8u, 11,
        0x000100fdu,
        0x00010038u
    };
    VkShaderModuleCreateInfo subgroup_shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof subgroup_builtin_shader,
        .pCode = subgroup_builtin_shader
    };
    VkShaderModule subgroup_shader = 0;
    CHECK(vkCreateShaderModule(device, &subgroup_shader_info, NULL, &subgroup_shader) == VK_SUCCESS &&
          subgroup_shader);
    VkPipelineLayoutCreateInfo empty_pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    VkPipelineLayout subgroup_pipeline_layout = 0;
    CHECK(vkCreatePipelineLayout(device, &empty_pipeline_layout_info, NULL, &subgroup_pipeline_layout) ==
          VK_SUCCESS && subgroup_pipeline_layout);
    VkComputePipelineCreateInfo subgroup_pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = subgroup_shader,
            .pName = "main"
        },
        .layout = subgroup_pipeline_layout,
        .basePipelineIndex = -1
    };
    VkPipeline rejected_subgroup_pipeline = UINT64_MAX;
    CHECK(vkCreateComputePipelines(device, 0, 1, &subgroup_pipeline_info, NULL,
          &rejected_subgroup_pipeline) == VK_ERROR_FEATURE_NOT_PRESENT && !rejected_subgroup_pipeline);
    vkDestroyPipelineLayout(device, subgroup_pipeline_layout, NULL);
    vkDestroyShaderModule(device, subgroup_shader, NULL);
    const uint32_t unsupported_memory_model_code[] = {
        SPIRV_MAGIC, 0x00010000u, 0, 1, 0,
        0x0003000eu, SPIRV_ADDRESSING_MODEL_LOGICAL, 3
    };
    invalid_shader_module_info.codeSize = sizeof unsupported_memory_model_code;
    invalid_shader_module_info.pCode = unsupported_memory_model_code;
    invalid_shader_module = UINT64_MAX;
    CHECK(vkCreateShaderModule(device, &invalid_shader_module_info, NULL, &invalid_shader_module) ==
          VK_ERROR_INITIALIZATION_FAILED && !invalid_shader_module);
    VkSamplerCreateInfo anisotropic_sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = 1,
        .maxAnisotropy = properties.limits.maxSamplerAnisotropy > 8.0f ?
            8.0f : properties.limits.maxSamplerAnisotropy,
        .compareOp = VK_COMPARE_OP_NEVER,
        .minLod = 0.0f,
        .maxLod = 16.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
    };
    VkSampler anisotropic_sampler = 0;
    if (features.samplerAnisotropy) {
        CHECK(anisotropic_sampler_info.maxAnisotropy > 1.0f);
        CHECK(vkCreateSampler(device, &anisotropic_sampler_info, NULL, &anisotropic_sampler) == VK_SUCCESS &&
              anisotropic_sampler);
        vkDestroySampler(device, anisotropic_sampler, NULL);
        anisotropic_sampler_info.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
        anisotropic_sampler = 0;
        CHECK(vkCreateSampler(device, &anisotropic_sampler_info, NULL, &anisotropic_sampler) == VK_SUCCESS &&
              anisotropic_sampler);
        vkDestroySampler(device, anisotropic_sampler, NULL);
        anisotropic_sampler_info.maxAnisotropy += 1.0f;
        CHECK(vkCreateSampler(device, &anisotropic_sampler_info, NULL, &anisotropic_sampler) ==
              VK_ERROR_INITIALIZATION_FAILED && !anisotropic_sampler);
    }
    VkPhysicalDeviceFeatures no_enabled_features = { 0 };
    VkDeviceCreateInfo no_features_info = device_info;
    no_features_info.pEnabledFeatures = &no_enabled_features;
    VkDevice no_features_device = NULL;
    CHECK(vkCreateDevice(physical, &no_features_info, NULL, &no_features_device) == VK_SUCCESS &&
          no_features_device);
    anisotropic_sampler = 0;
    CHECK(vkCreateSampler(no_features_device, &anisotropic_sampler_info, NULL, &anisotropic_sampler) ==
          VK_ERROR_FEATURE_NOT_PRESENT && !anisotropic_sampler);
    vkDestroyDevice(no_features_device, NULL);
    puts("vulkan sampler: anisotropy capability probe, feature enable, limit query and sampler validation: ok");
    VkDeviceCreateInfo device_without_maintenance_info = device_info;
    device_without_maintenance_info.enabledExtensionCount = 0;
    device_without_maintenance_info.ppEnabledExtensionNames = NULL;
    VkDevice device_without_maintenance = NULL;
    CHECK(vkCreateDevice(physical, &device_without_maintenance_info, NULL, &device_without_maintenance) ==
          VK_SUCCESS && device_without_maintenance);
    CHECK(vkGetDeviceProcAddr(device_without_maintenance, "vkTrimCommandPoolKHR") == NULL);
    CHECK(vkGetDeviceProcAddr(device, "vkTrimCommandPoolKHR") ==
          (PFN_vkVoidFunction)vkTrimCommandPoolKHR);
    CHECK(check_descriptor_pool_exhaustion(device, VK_ERROR_OUT_OF_POOL_MEMORY_KHR) == 0);
    CHECK(check_descriptor_pool_exhaustion(device_without_maintenance, VK_ERROR_OUT_OF_DEVICE_MEMORY) == 0);
    CHECK(check_descriptor_type_pool_exhaustion(device, VK_ERROR_OUT_OF_POOL_MEMORY_KHR) == 0);
    CHECK(check_descriptor_type_pool_exhaustion(device_without_maintenance, VK_ERROR_OUT_OF_DEVICE_MEMORY) == 0);
    if (argc >= 13) {
        VkPhysicalDeviceFeatures without_dynamic_indexing = features;
        without_dynamic_indexing.shaderSampledImageArrayDynamicIndexing = 0;
        without_dynamic_indexing.shaderStorageBufferArrayDynamicIndexing = 0;
        without_dynamic_indexing.shaderStorageImageArrayDynamicIndexing = 0;
        VkDeviceCreateInfo without_dynamic_indexing_info = device_info;
        without_dynamic_indexing_info.pEnabledFeatures = &without_dynamic_indexing;
        VkDevice device_without_dynamic_indexing = NULL;
        CHECK(vkCreateDevice(physical, &without_dynamic_indexing_info, NULL, &device_without_dynamic_indexing) ==
              VK_SUCCESS && device_without_dynamic_indexing);
        CHECK(check_descriptor_array_pipeline_feature_gate(device_without_dynamic_indexing, argv[2],
              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3, VK_SUCCESS) == 0);
        CHECK(check_descriptor_array_pipeline_feature_gate(device_without_dynamic_indexing, argv[12],
              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3, VK_ERROR_FEATURE_NOT_PRESENT) == 0);
        if (argc >= 14 && features.shaderStorageImageArrayDynamicIndexing)
            CHECK(check_descriptor_array_pipeline_feature_gate(device_without_dynamic_indexing, argv[13],
                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2, VK_ERROR_FEATURE_NOT_PRESENT) == 0);
        if (argc >= 15)
            CHECK(check_dynamic_sampled_descriptor_pipeline_gate(device_without_dynamic_indexing, argv[14], 0) == 0);
        if (argc >= 16)
            CHECK(check_dynamic_sampled_descriptor_pipeline_gate(device_without_dynamic_indexing, argv[15], 1) == 0);
        vkDestroyDevice(device_without_dynamic_indexing, NULL);
        puts("vulkan features: sampled-image, storage-buffer and storage-image array dynamic indexing query, enable and pipeline gate: ok");
    }
    CHECK(check_buffer_descriptor_slot_limits(device) == 0);
    CHECK(check_texture_descriptor_limits(device, properties.limits.maxPerStageDescriptorStorageImages != 0) == 0);
    CHECK(check_sampler_allocation_limit(device, properties.limits.maxSamplerAllocationCount) == 0);
    if (argc >= 10) CHECK(check_high_texture_pipeline(device, argv[9]) == 0);
    CHECK(vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr") == (PFN_vkVoidFunction)vkGetDeviceProcAddr);
    CHECK(vkGetInstanceProcAddr(instance, "vkCmdPushConstants") == (PFN_vkVoidFunction)vkCmdPushConstants);
    PFN_vkGetDeviceQueue get_queue = (PFN_vkGetDeviceQueue)vkGetDeviceProcAddr(device, "vkGetDeviceQueue");
    CHECK(get_queue != NULL && vkGetDeviceProcAddr(device, "vkUnknownCommand") == NULL);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdUpdateBuffer") == (PFN_vkVoidFunction)vkCmdUpdateBuffer);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdFillBuffer") == (PFN_vkVoidFunction)vkCmdFillBuffer);
    CHECK(vkGetDeviceProcAddr(device, "vkFlushMappedMemoryRanges") ==
          (PFN_vkVoidFunction)vkFlushMappedMemoryRanges);
    CHECK(vkGetDeviceProcAddr(device, "vkInvalidateMappedMemoryRanges") ==
          (PFN_vkVoidFunction)vkInvalidateMappedMemoryRanges);
    CHECK(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceImageFormatProperties") ==
          (PFN_vkVoidFunction)vkGetPhysicalDeviceImageFormatProperties);
    CHECK(vkGetDeviceProcAddr(device, "vkCreateImage") == (PFN_vkVoidFunction)vkCreateImage);
    CHECK(vkGetDeviceProcAddr(device, "vkCreateImageView") == (PFN_vkVoidFunction)vkCreateImageView);
    CHECK(vkGetDeviceProcAddr(device, "vkDestroyImageView") == (PFN_vkVoidFunction)vkDestroyImageView);
    CHECK(vkGetDeviceProcAddr(device, "vkGetImageSubresourceLayout") ==
          (PFN_vkVoidFunction)vkGetImageSubresourceLayout);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdCopyBufferToImage") == (PFN_vkVoidFunction)vkCmdCopyBufferToImage);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdCopyImageToBuffer") == (PFN_vkVoidFunction)vkCmdCopyImageToBuffer);
    CHECK(vkGetDeviceProcAddr(device, "vkCreateSampler") == (PFN_vkVoidFunction)vkCreateSampler);
    CHECK(vkGetDeviceProcAddr(device, "vkDestroySampler") == (PFN_vkVoidFunction)vkDestroySampler);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdCopyImage") == (PFN_vkVoidFunction)vkCmdCopyImage);
    VkQueue queue = NULL;
    get_queue(device, 0, 0, &queue);
    CHECK(queue != NULL);
    CHECK(graphics_sync_stage_test(device, queue) == 0);
    const char *graphics_vertex_fixture = argc >= 38 ? argv[argc - 8] : argc >= 36 ? argv[argc - 6] :
        argc >= 35 ? argv[argc - 5] : argc >= 32 ? argv[argc - 2] : NULL;
    const char *graphics_fragment_fixture = argc >= 38 ? argv[argc - 7] : argc >= 36 ? argv[argc - 5] :
        argc >= 35 ? argv[argc - 4] : argc >= 32 ? argv[argc - 1] : NULL;
    const char *graphics_uint_vertex_fixture = argc >= 38 ? argv[argc - 6] :
        argc >= 36 ? argv[argc - 4] : argc >= 35 ? argv[argc - 3] : NULL;
    const char *graphics_sint_vertex_fixture = argc >= 38 ? argv[argc - 5] :
        argc >= 36 ? argv[argc - 3] : argc >= 35 ? argv[argc - 2] : NULL;
    const char *graphics_sampled_fragment_fixture = argc >= 38 ? argv[argc - 4] : argc >= 36 ? argv[argc - 2] :
        argc >= 35 ? argv[argc - 1] : NULL;
    const char *graphics_separate_sampled_fragment_fixture = argc >= 38 ? argv[argc - 3] :
        argc >= 36 ? argv[argc - 1] : NULL;
    const char *graphics_implicit_sampled_fragment_fixture = argc >= 38 ? argv[argc - 2] : NULL;
    const char *graphics_implicit_separate_sampled_fragment_fixture = argc >= 38 ? argv[argc - 1] : NULL;
    CHECK(run_render_pass_clear(physical, device, queue, graphics_vertex_fixture, graphics_fragment_fixture,
                                graphics_uint_vertex_fixture, graphics_sint_vertex_fixture,
                                graphics_sampled_fragment_fixture, graphics_separate_sampled_fragment_fixture,
                                graphics_implicit_sampled_fragment_fixture,
                                graphics_implicit_separate_sampled_fragment_fixture) == 0);

    VkMemoryAllocateInfo mapped_range_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 128,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory mapped_range_memory = 0;
    CHECK(vkAllocateMemory(device, &mapped_range_allocation, NULL, &mapped_range_memory) == VK_SUCCESS);
    void *mapped_range_data = NULL;
    CHECK(vkMapMemory(device, mapped_range_memory, 16, 64, 0, &mapped_range_data) == VK_SUCCESS &&
          mapped_range_data != NULL);
    VkMappedMemoryRange mapped_range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = mapped_range_memory,
        .offset = 16,
        .size = 32
    };
    CHECK(vkFlushMappedMemoryRanges(device, 1, &mapped_range) == VK_SUCCESS);
    CHECK(vkInvalidateMappedMemoryRanges(device, 1, &mapped_range) == VK_SUCCESS);
    mapped_range.offset = 32;
    mapped_range.size = VK_WHOLE_SIZE;
    CHECK(vkFlushMappedMemoryRanges(device, 1, &mapped_range) == VK_SUCCESS);
    mapped_range.offset = 80;
    CHECK(vkInvalidateMappedMemoryRanges(device, 1, &mapped_range) == VK_ERROR_MEMORY_MAP_FAILED);
    mapped_range.offset = 16;
    mapped_range.size = 65;
    CHECK(vkFlushMappedMemoryRanges(device, 1, &mapped_range) == VK_ERROR_MEMORY_MAP_FAILED);
    mapped_range.size = VK_WHOLE_SIZE;
    vkUnmapMemory(device, mapped_range_memory);
    CHECK(vkFlushMappedMemoryRanges(device, 1, &mapped_range) == VK_ERROR_MEMORY_MAP_FAILED);
    CHECK(vkFlushMappedMemoryRanges(device, 0, &mapped_range) == VK_ERROR_INITIALIZATION_FAILED);
    vkFreeMemory(device, mapped_range_memory, NULL);

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 66,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkBuffer src_buffer = 0, dst_buffer = 0;
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &src_buffer) == VK_SUCCESS && src_buffer != 0);
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &dst_buffer) == VK_SUCCESS && dst_buffer != 0);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, src_buffer, &requirements);
    CHECK(requirements.size == 66 && requirements.alignment == 16 && requirements.memoryTypeBits == 1);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory src_memory = 0, dst_memory = 0;
    CHECK(vkAllocateMemory(device, &allocation, NULL, &src_memory) == VK_SUCCESS && src_memory != 0);
    CHECK(vkAllocateMemory(device, &allocation, NULL, &dst_memory) == VK_SUCCESS && dst_memory != 0);
    CHECK(vkBindBufferMemory(device, src_buffer, src_memory, 0) == VK_SUCCESS);
    CHECK(vkBindBufferMemory(device, dst_buffer, dst_memory, 0) == VK_SUCCESS);
    void *mapped = NULL;
    CHECK(vkMapMemory(device, src_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    for (uint32_t i = 0; i < 16; i++) ((uint32_t *)mapped)[i] = 0x4000u + i * 17u;
    vkUnmapMemory(device, src_memory);
    CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    memset(mapped, 0, (size_t)requirements.size);
    vkUnmapMemory(device, dst_memory);

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = 0
    };
    VkCommandPool pool = 0;
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool) == VK_SUCCESS && pool != 0);
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkCommandBuffer command_buffer = NULL;
    CHECK(vkAllocateCommandBuffers(device, &command_info, &command_buffer) == VK_SUCCESS && command_buffer != NULL);
    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    uint32_t update_word = 0x12345678u;
    vkCmdUpdateBuffer(command_buffer, dst_buffer, 2, sizeof update_word, &update_word);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdUpdateBuffer(command_buffer, dst_buffer, 0, 3, &update_word);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    uint8_t out_of_range_data[68] = { 0 };
    vkCmdUpdateBuffer(command_buffer, dst_buffer, 0, sizeof out_of_range_data, out_of_range_data);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdFillBuffer(command_buffer, dst_buffer, 2, 4, 0xdeadbeefu);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdFillBuffer(command_buffer, dst_buffer, 0, 68, 0xdeadbeefu);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdFillBuffer(command_buffer, dst_buffer, 0, 0, 0xdeadbeefu);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    VkBufferCopy cross_region_source_destination_overlap[] = {
        { .srcOffset = 0, .dstOffset = 8, .size = 4 },
        { .srcOffset = 16, .dstOffset = 0, .size = 4 }
    };
    vkCmdCopyBuffer(command_buffer, src_buffer, src_buffer, 2,
                    cross_region_source_destination_overlap);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    VkBufferCopy overlapping_destination_regions[] = {
        { .srcOffset = 16, .dstOffset = 0, .size = 4 },
        { .srcOffset = 24, .dstOffset = 0, .size = 4 }
    };
    vkCmdCopyBuffer(command_buffer, src_buffer, src_buffer, 2, overlapping_destination_regions);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    if (argc == 2 && !strcmp(argv[1], "--copy-overlap-only")) {
        CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
        puts("vulkan copy buffer: cross-region read/write alias and duplicate destination ranges rejected: ok");
        vkDestroyCommandPool(device, pool, NULL);
        vkDestroyBuffer(device, src_buffer, NULL);
        vkDestroyBuffer(device, dst_buffer, NULL);
        vkFreeMemory(device, src_memory, NULL);
        vkFreeMemory(device, dst_memory, NULL);
        vkDestroyDevice(device_without_maintenance, NULL);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, &allocator);
        return 0;
    }
    uint32_t recorded_data[16];
    for (uint32_t i = 0; i < 16; i++) recorded_data[i] = 0x8000u + i * 13u;
    vkCmdUpdateBuffer(command_buffer, src_buffer, 0, sizeof recorded_data, recorded_data);
    memset(recorded_data, 0, sizeof recorded_data);
    VkBufferMemoryBarrier update_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = src_buffer,
        .size = 64
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 1, &update_barrier, 0, NULL);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 0, NULL);
    VkBufferCopy copy = { .size = 64 };
    vkCmdCopyBuffer(command_buffer, src_buffer, dst_buffer, 1, &copy);
    vkCmdFillBuffer(command_buffer, dst_buffer, 48, VK_WHOLE_SIZE, 0xa1b2c3d4u);
    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = dst_buffer,
        .size = 64
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host_barrier, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);

    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = 0;
    CHECK(vkCreateFence(device, &fence_info, NULL, &fence) == VK_SUCCESS);
    CHECK(vkGetFenceStatus(device, fence) == VK_NOT_READY);
    CHECK(vkWaitForFences(device, 1, &fence, 1, 0) == VK_TIMEOUT);
    uint64_t wait_start = monotonic_time_ns();
    CHECK(wait_start && vkWaitForFences(device, 1, &fence, 1, 20000000ull) == VK_TIMEOUT);
    uint64_t wait_elapsed = monotonic_time_ns() - wait_start;
    CHECK(wait_elapsed >= 5000000ull);
    VkFenceCreateInfo signaled_fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };
    VkFence signaled_fence = 0;
    CHECK(vkCreateFence(device, &signaled_fence_info, NULL, &signaled_fence) == VK_SUCCESS);
    VkFence any_fences[2] = { fence, signaled_fence };
    CHECK(vkWaitForFences(device, 2, any_fences, 0, 0) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 2, any_fences, 1, 0) == VK_TIMEOUT);
    vkDestroyFence(device, signaled_fence, NULL);
    delayed_fence_submit delayed_submit = { .queue = queue, .fence = fence, .result = VK_ERROR_UNKNOWN };
    pthread_t submit_thread;
    CHECK(pthread_create(&submit_thread, NULL, submit_empty_batch_after_delay, &delayed_submit) == 0);
    VkResult blocked_wait = vkWaitForFences(device, 1, &fence, 1, 2000000000ull);
    int thread_join = pthread_join(submit_thread, NULL);
    CHECK(blocked_wait == VK_SUCCESS && thread_join == 0 && delayed_submit.result == VK_SUCCESS);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
    VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore semaphore = 0;
    CHECK(vkCreateSemaphore(device, &semaphore_info, NULL, &semaphore) == VK_SUCCESS);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &semaphore
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkGetFenceStatus(device, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkQueueWaitIdle(queue) == VK_SUCCESS && vkDeviceWaitIdle(device) == VK_SUCCESS);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo wait_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &semaphore,
        .pWaitDstStageMask = &wait_stage
    };
    CHECK(vkQueueSubmit(queue, 1, &wait_submit, 0) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &wait_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    for (uint32_t i = 0; i < 12; i++) CHECK(((uint32_t *)mapped)[i] == 0x8000u + i * 13u);
    for (uint32_t i = 12; i < 16; i++) CHECK(((uint32_t *)mapped)[i] == 0xa1b2c3d4u);
    CHECK(((uint8_t *)mapped)[64] == 0 && ((uint8_t *)mapped)[65] == 0);
    vkUnmapMemory(device, dst_memory);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS && vkGetFenceStatus(device, fence) == VK_NOT_READY);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { 4, 3, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    VkImage rejected_transfer_only_image = 0;
    CHECK(vkCreateImage(device_without_maintenance, &image_info, NULL, &rejected_transfer_only_image) ==
          VK_ERROR_FORMAT_NOT_SUPPORTED && rejected_transfer_only_image == 0);
    VkImageCreateInfo implicit_transfer_image_info = image_info;
    implicit_transfer_image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    VkImage implicit_transfer_image = 0;
    CHECK(vkCreateImage(device_without_maintenance, &implicit_transfer_image_info, NULL,
          &implicit_transfer_image) == VK_SUCCESS && implicit_transfer_image);
    vkDestroyImage(device_without_maintenance, implicit_transfer_image, NULL);
    VkImage image = 0;
    CHECK(vkCreateImage(device, &image_info, NULL, &image) == VK_SUCCESS && image != 0);
    VkImageViewCreateInfo image_view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image_info.format,
        .components = { VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R,
                        VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE },
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    VkImageView image_view = 0;
    CHECK(vkCreateImageView(device, &image_view_info, NULL, &image_view) == VK_SUCCESS && image_view != 0);
    VkImageViewCreateInfo unsupported_image_view_info = image_view_info;
    unsupported_image_view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    VkImageView unsupported_image_view = UINT64_MAX;
    CHECK(vkCreateImageView(device, &unsupported_image_view_info, NULL, &unsupported_image_view) ==
          VK_ERROR_FORMAT_NOT_SUPPORTED && unsupported_image_view == 0);
    unsupported_image_view_info = image_view_info;
    unsupported_image_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    CHECK(vkCreateImageView(device, &unsupported_image_view_info, NULL, &unsupported_image_view) ==
          VK_ERROR_FORMAT_NOT_SUPPORTED && unsupported_image_view == 0);
    VkImageCreateInfo unsupported_image_info = image_info;
    unsupported_image_info.tiling = (VkImageTiling)UINT32_MAX;
    VkImage unsupported_image = UINT64_MAX;
    CHECK(vkCreateImage(device, &unsupported_image_info, NULL, &unsupported_image) ==
          VK_ERROR_FORMAT_NOT_SUPPORTED && unsupported_image == 0);
    unsupported_image_info = image_info;
    unsupported_image_info.mipLevels = 2;
    unsupported_image = UINT64_MAX;
    CHECK(vkCreateImage(device, &unsupported_image_info, NULL, &unsupported_image) ==
          VK_ERROR_FORMAT_NOT_SUPPORTED && unsupported_image == 0);
    VkMemoryRequirements image_requirements;
    vkGetImageMemoryRequirements(device, image, &image_requirements);
    CHECK(image_requirements.size >= 4u * 3u * 4u && image_requirements.alignment >= 16 &&
          (image_requirements.alignment & (image_requirements.alignment - 1)) == 0 &&
          image_requirements.memoryTypeBits == 1);
    VkImageSubresource color_subresource = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .arrayLayer = 0
    };
    VkSubresourceLayout image_layout;
    vkGetImageSubresourceLayout(device, image, &color_subresource, &image_layout);
    CHECK(image_layout.offset == 0 && image_layout.rowPitch >= 16 && image_layout.rowPitch % image_requirements.alignment == 0 &&
          image_layout.size == image_layout.rowPitch * 3 && image_layout.arrayPitch == image_layout.size &&
          image_layout.depthPitch == image_layout.size);
    allocation.allocationSize = image_requirements.size;
    VkDeviceMemory image_memory = 0;
    CHECK(vkAllocateMemory(device, &allocation, NULL, &image_memory) == VK_SUCCESS && image_memory != 0);
    CHECK(vkBindImageMemory(device, image, image_memory, 0) == VK_SUCCESS);
    CHECK(vkBindImageMemory(device, image, image_memory, 0) == VK_ERROR_INITIALIZATION_FAILED);

    VkImage copy_image = 0;
    CHECK(vkCreateImage(device, &image_info, NULL, &copy_image) == VK_SUCCESS && copy_image != 0);
    VkMemoryRequirements copy_image_requirements;
    vkGetImageMemoryRequirements(device, copy_image, &copy_image_requirements);
    allocation.allocationSize = copy_image_requirements.size;
    VkDeviceMemory copy_image_memory = 0;
    CHECK(vkAllocateMemory(device, &allocation, NULL, &copy_image_memory) == VK_SUCCESS && copy_image_memory != 0);
    CHECK(vkBindImageMemory(device, copy_image, copy_image_memory, 0) == VK_SUCCESS);
    CHECK(vkMapMemory(device, copy_image_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    memset(mapped, 0, copy_image_requirements.size);
    vkUnmapMemory(device, copy_image_memory);

    CHECK(vkMapMemory(device, src_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    const uint32_t padded_row_length = 6;
    const uint32_t padded_row_pitch = padded_row_length * 4;
    memset(mapped, 0xcd, 64);
    for (uint32_t y = 0; y < 3; y++)
        for (uint32_t x = 0; x < 16; x++)
            ((uint8_t *)mapped)[y * padded_row_pitch + x] = (uint8_t)((y * 16u + x) * 3u + 7u);
    vkUnmapMemory(device, src_memory);
    CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    memset(mapped, 0, 64);
    vkUnmapMemory(device, dst_memory);

    VkImageMemoryBarrier image_to_dst = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    VkImageMemoryBarrier image_to_src = image_to_dst;
    image_to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    image_to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    image_to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    image_to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkImageMemoryBarrier copy_image_to_dst = image_to_dst;
    copy_image_to_dst.image = copy_image;
    VkImageMemoryBarrier copy_image_to_src = image_to_src;
    copy_image_to_src.image = copy_image;
    VkBufferImageCopy image_copy = {
        .bufferRowLength = padded_row_length,
        .bufferImageHeight = 4,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { 4, 3, 1 }
    };
    VkBufferImageCopy tight_image_copy = image_copy;
    tight_image_copy.bufferRowLength = 0;
    tight_image_copy.bufferImageHeight = 0;
    VkImageCopy image_to_image = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffset = { 1, 1, 0 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffset = { 2, 0, 0 },
        .extent = { 2, 2, 1 }
    };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &image_to_dst);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &copy_image_to_dst);
    vkCmdCopyBufferToImage(command_buffer, src_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &image_copy);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &image_to_src);
    vkCmdCopyImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        copy_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &image_to_image);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &copy_image_to_src);
    vkCmdCopyImageToBuffer(command_buffer, copy_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst_buffer, 1, &image_copy);
    VkBufferMemoryBarrier copy_to_host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = dst_buffer,
        .size = 64
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, NULL, 1, &copy_to_host, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    VkSubmitInfo image_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &image_submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, image_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    uint8_t *image_bytes = mapped;
    CHECK(image_layout.rowPitch >= 16);
    for (uint32_t y = 0; y < 3; y++)
        for (uint32_t x = 0; x < 16; x++) CHECK(image_bytes[y * image_layout.rowPitch + x] == (uint8_t)((y * 16 + x) * 3u + 7u));
    vkUnmapMemory(device, image_memory);
    CHECK(vkMapMemory(device, copy_image_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    uint8_t *copy_image_bytes = mapped;
    for (uint32_t y = 0; y < 3; y++) {
        for (uint32_t x = 0; x < 16; x++) {
            uint8_t expected = 0;
            if (y < 2 && x >= 8) expected = (uint8_t)(((y + 1u) * 16u + x - 4u) * 3u + 7u);
            CHECK(copy_image_bytes[y * image_layout.rowPitch + x] == expected);
        }
    }
    vkUnmapMemory(device, copy_image_memory);
    CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    for (uint32_t y = 0; y < 3; y++) {
        for (uint32_t x = 0; x < 16; x++) {
            uint8_t expected = 0;
            if (y < 2 && x >= 8) expected = (uint8_t)(((y + 1u) * 16u + x - 4u) * 3u + 7u);
            CHECK(((uint8_t *)mapped)[y * padded_row_pitch + x] == expected);
        }
        for (uint32_t x = 16; x < padded_row_pitch; x++)
            CHECK(((uint8_t *)mapped)[y * padded_row_pitch + x] == 0);
    }
    vkUnmapMemory(device, dst_memory);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS && vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    VkImageMemoryBarrier clear_image_to_dst = image_to_dst;
    clear_image_to_dst.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    clear_image_to_dst.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkClearColorValue clear_color = { .float32 = { 1.0f, 0.5f, 0.25f, 1.0f } };
    VkImageSubresourceRange clear_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkBufferMemoryBarrier clear_readback_to_host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = dst_buffer,
        .size = 64
    };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &clear_image_to_dst);
    vkCmdClearColorImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1,
                         &clear_range);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &image_to_src);
    vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dst_buffer, 1, &image_copy);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, NULL, 1, &clear_readback_to_host, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &image_submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    const uint8_t expected_clear[4] = { 255, 128, 64, 255 };
    for (uint32_t y = 0; y < 3; y++)
        for (uint32_t x = 0; x < 4; x++)
            CHECK(memcmp((const uint8_t *)mapped + y * padded_row_pitch + x * sizeof expected_clear,
                         expected_clear, sizeof expected_clear) == 0);
    vkUnmapMemory(device, dst_memory);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS && vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    VkImageMemoryBarrier stale_transition = clear_image_to_dst;
    stale_transition.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &stale_transition);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &image_submit, fence) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkGetFenceStatus(device, fence) == VK_NOT_READY);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    vkTrimCommandPoolKHR(device, pool, 0);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdClearColorImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1,
                         &clear_range);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &image_submit, fence) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkGetFenceStatus(device, fence) == VK_NOT_READY);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    VkImageSubresourceRange invalid_clear_range = clear_range;
    invalid_clear_range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdClearColorImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1,
                         &invalid_clear_range);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdClearColorImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, &clear_color, 1,
                         &clear_range);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdCopyImage(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_GENERAL,
                   1, &image_to_image);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    VkImageCopy nonoverlapping_self_copy = image_to_image;
    nonoverlapping_self_copy.srcOffset = (VkOffset3D){ 0, 0, 0 };
    nonoverlapping_self_copy.dstOffset = (VkOffset3D){ 1, 0, 0 };
    nonoverlapping_self_copy.extent = (VkExtent3D){ 1, 1, 1 };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdCopyImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &nonoverlapping_self_copy);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    VkImageCopy repeated_destination_regions[2] = { image_to_image, image_to_image };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdCopyImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        copy_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, repeated_destination_regions);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    VkImageCreateInfo optimal_image_info = image_info;
    optimal_image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    VkImage optimal_image = 0;
    CHECK(vkCreateImage(device, &optimal_image_info, NULL, &optimal_image) == VK_SUCCESS && optimal_image);
    VkMemoryRequirements optimal_requirements;
    vkGetImageMemoryRequirements(device, optimal_image, &optimal_requirements);
    allocation.allocationSize = optimal_requirements.size;
    VkDeviceMemory optimal_memory = 0;
    CHECK(vkAllocateMemory(device, &allocation, NULL, &optimal_memory) == VK_SUCCESS && optimal_memory);
    CHECK(vkBindImageMemory(device, optimal_image, optimal_memory, 0) == VK_SUCCESS);
    CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    memset(mapped, 0, 64);
    vkUnmapMemory(device, dst_memory);
    CHECK(vkMapMemory(device, src_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    for (uint32_t i = 0; i < 48; i++) ((uint8_t *)mapped)[i] = (uint8_t)(i * 3u + 7u);
    vkUnmapMemory(device, src_memory);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    VkImageMemoryBarrier optimal_to_dst = image_to_dst;
    optimal_to_dst.image = optimal_image;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &optimal_to_dst);
    vkCmdCopyBufferToImage(command_buffer, src_buffer, optimal_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &tight_image_copy);
    VkImageMemoryBarrier optimal_to_src = image_to_src;
    optimal_to_src.image = optimal_image;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &optimal_to_src);
    vkCmdCopyImageToBuffer(command_buffer, optimal_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst_buffer, 1, &tight_image_copy);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, NULL, 1, &copy_to_host, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &image_submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    for (uint32_t i = 0; i < 48; i++) CHECK(((uint8_t *)mapped)[i] == (uint8_t)(i * 3u + 7u));
    vkUnmapMemory(device, dst_memory);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS && vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    vkDestroyImage(device, optimal_image, NULL);
    vkFreeMemory(device, optimal_memory, NULL);

    if (properties.limits.maxPerStageDescriptorStorageImages) {
        VkImageCreateInfo storage_image_info = image_info;
        storage_image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        storage_image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        VkImage storage_image = 0;
        CHECK(vkCreateImage(device, &storage_image_info, NULL, &storage_image) == VK_SUCCESS && storage_image);
        VkMemoryRequirements storage_requirements;
        vkGetImageMemoryRequirements(device, storage_image, &storage_requirements);
        allocation.allocationSize = storage_requirements.size;
        VkDeviceMemory storage_memory = 0;
        CHECK(vkAllocateMemory(device, &allocation, NULL, &storage_memory) == VK_SUCCESS && storage_memory);
        CHECK(vkBindImageMemory(device, storage_image, storage_memory, 0) == VK_SUCCESS);
        VkImageViewCreateInfo storage_view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = storage_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = storage_image_info.format,
            .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        };
        VkImageView storage_view = 0;
        CHECK(vkCreateImageView(device, &storage_view_info, NULL, &storage_view) == VK_SUCCESS && storage_view);
        VkDescriptorSetLayoutBinding storage_binding = {
            .binding = 3,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
        };
        VkDescriptorSetLayoutCreateInfo storage_set_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &storage_binding
        };
        VkDescriptorSetLayout storage_set_layout = 0;
        CHECK(vkCreateDescriptorSetLayout(device, &storage_set_info, NULL, &storage_set_layout) == VK_SUCCESS &&
              storage_set_layout);
        VkPipelineLayoutCreateInfo storage_pipeline_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &storage_set_layout
        };
        VkPipelineLayout storage_pipeline_layout = 0;
        CHECK(vkCreatePipelineLayout(device, &storage_pipeline_info, NULL, &storage_pipeline_layout) == VK_SUCCESS &&
              storage_pipeline_layout);
        VkDescriptorPoolSize storage_pool_size = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 };
        VkDescriptorPoolCreateInfo storage_pool_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 2,
            .poolSizeCount = 1,
            .pPoolSizes = &storage_pool_size
        };
        VkDescriptorPool storage_pool = 0;
        CHECK(vkCreateDescriptorPool(device, &storage_pool_info, NULL, &storage_pool) == VK_SUCCESS && storage_pool);
        VkDescriptorSetAllocateInfo storage_allocate_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = storage_pool,
            .descriptorSetCount = 2,
            .pSetLayouts = (VkDescriptorSet[2]){ storage_set_layout, storage_set_layout }
        };
        VkDescriptorSet storage_sets[2] = { 0 };
        CHECK(vkAllocateDescriptorSets(device, &storage_allocate_info, storage_sets) == VK_SUCCESS &&
              storage_sets[0] && storage_sets[1]);
        VkDescriptorImageInfo storage_descriptor_image = {
            .imageView = storage_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL
        };
        VkWriteDescriptorSet storage_write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = storage_sets[0],
            .dstBinding = 3,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &storage_descriptor_image
        };
        vkUpdateDescriptorSets(device, 1, &storage_write, 0, NULL);
        VkCopyDescriptorSet storage_copy = {
            .sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
            .srcSet = storage_sets[0],
            .srcBinding = 3,
            .dstSet = storage_sets[1],
            .dstBinding = 3,
            .descriptorCount = 1
        };
        vkUpdateDescriptorSets(device, 0, NULL, 1, &storage_copy);
        vkDestroyImageView(device, storage_view, NULL);
        vkDestroyImage(device, storage_image, NULL);
        vkDestroyDescriptorPool(device, storage_pool, NULL);
        vkDestroyPipelineLayout(device, storage_pipeline_layout, NULL);
        vkDestroyDescriptorSetLayout(device, storage_set_layout, NULL);
        vkFreeMemory(device, storage_memory, NULL);
    }

    vkDestroyImage(device, copy_image, NULL);
    vkFreeMemory(device, copy_image_memory, NULL);

    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdClearColorImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1,
                         &clear_range);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    vkDestroyImageView(device, image_view, NULL);
    vkDestroyImage(device, image, NULL);
    image = 0;
    VkSubmitInfo invalid_image_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &invalid_image_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    vkFreeMemory(device, image_memory, NULL);

    const VkFormat transfer_formats[] = {
        VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT
    };
    for (size_t i = 0; i < sizeof transfer_formats / sizeof transfer_formats[0]; i++) {
        VkImageCreateInfo format_info = image_info;
        format_info.format = transfer_formats[i];
        VkImage format_image = 0;
        CHECK(vkCreateImage(device, &format_info, NULL, &format_image) == VK_SUCCESS && format_image != 0);
        VkMemoryRequirements format_requirements;
        vkGetImageMemoryRequirements(device, format_image, &format_requirements);
        allocation.allocationSize = format_requirements.size;
        VkDeviceMemory format_memory = 0;
        CHECK(vkAllocateMemory(device, &allocation, NULL, &format_memory) == VK_SUCCESS && format_memory != 0);
        CHECK(vkBindImageMemory(device, format_image, format_memory, 0) == VK_SUCCESS);
        vkDestroyImage(device, format_image, NULL);
        vkFreeMemory(device, format_memory, NULL);
    }
    const VkFormat integer_clear_formats[] = { VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT };
    for (size_t i = 0; i < sizeof integer_clear_formats / sizeof integer_clear_formats[0]; i++) {
        VkImageCreateInfo integer_image_info = image_info;
        integer_image_info.format = integer_clear_formats[i];
        VkImage integer_image = 0;
        CHECK(vkCreateImage(device, &integer_image_info, NULL, &integer_image) == VK_SUCCESS && integer_image);
        VkMemoryRequirements integer_requirements;
        vkGetImageMemoryRequirements(device, integer_image, &integer_requirements);
        allocation.allocationSize = integer_requirements.size;
        VkDeviceMemory integer_memory = 0;
        CHECK(vkAllocateMemory(device, &allocation, NULL, &integer_memory) == VK_SUCCESS && integer_memory);
        CHECK(vkBindImageMemory(device, integer_image, integer_memory, 0) == VK_SUCCESS);
        VkClearColorValue integer_clear = { 0 };
        uint32_t uint_clear[4] = { 0xfedcba98u, 0x80000001u, 0x12345678u, 0xffffffffu };
        int32_t sint_clear[4] = { -123456789, -1, 0x1234567, INT32_MAX };
        const uint32_t *expected_clear;
        if (integer_clear_formats[i] == VK_FORMAT_R32_UINT) {
            memcpy(integer_clear.uint32, uint_clear, sizeof uint_clear);
            expected_clear = uint_clear;
        } else {
            memcpy(integer_clear.int32, sint_clear, sizeof sint_clear);
            expected_clear = (const uint32_t *)sint_clear;
        }
        VkImageMemoryBarrier integer_to_dst = image_to_dst;
        integer_to_dst.image = integer_image;
        VkImageMemoryBarrier integer_to_src = image_to_src;
        integer_to_src.image = integer_image;
        VkBufferMemoryBarrier host_read_to_copy = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = dst_buffer,
            .size = 48
        };
        VkBufferImageCopy integer_image_copy = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { 4, 3, 1 }
        };
        CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 1, &host_read_to_copy, 0, NULL);
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &integer_to_dst);
        vkCmdClearColorImage(command_buffer, integer_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &integer_clear, 1, &clear_range);
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &integer_to_src);
        vkCmdCopyImageToBuffer(command_buffer, integer_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            dst_buffer, 1, &integer_image_copy);
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 0, NULL, 1, &copy_to_host, 0, NULL);
        CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
        CHECK(vkQueueSubmit(queue, 1, &image_submit, fence) == VK_SUCCESS);
        CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
        CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
        for (uint32_t pixel = 0; pixel < 12; pixel++)
            CHECK(((uint32_t *)mapped)[pixel] == expected_clear[0]);
        vkUnmapMemory(device, dst_memory);
        CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
        vkDestroyImage(device, integer_image, NULL);
        vkFreeMemory(device, integer_memory, NULL);
    }
    puts("vulkan clear: R32_UINT and R32_SINT retain exact 32-bit color clear values: ok");
    VkImageCreateInfo array_image_info = image_info;
    array_image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    array_image_info.arrayLayers = 2;
    VkImage array_image = 0;
    CHECK(vkCreateImage(device, &array_image_info, NULL, &array_image) == VK_SUCCESS && array_image);
    VkMemoryRequirements array_requirements;
    vkGetImageMemoryRequirements(device, array_image, &array_requirements);
    CHECK(array_requirements.size >= 2u * array_image_info.extent.width * array_image_info.extent.height * 4u);
    allocation.allocationSize = array_requirements.size;
    VkDeviceMemory array_memory = 0;
    CHECK(vkAllocateMemory(device, &allocation, NULL, &array_memory) == VK_SUCCESS && array_memory);
    CHECK(vkBindImageMemory(device, array_image, array_memory, 0) == VK_SUCCESS);

    uint8_t array_source[32];
    for (uint32_t i = 0; i < sizeof array_source; i++) array_source[i] = (uint8_t)(i * 7u + 3u);
    CHECK(vkMapMemory(device, src_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    memcpy(mapped, array_source, sizeof array_source);
    vkUnmapMemory(device, src_memory);
    CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    memset(mapped, 0, 66);
    vkUnmapMemory(device, dst_memory);

    VkImageMemoryBarrier array_to_dst = image_to_dst;
    array_to_dst.image = array_image;
    array_to_dst.subresourceRange.layerCount = 2;
    VkImageMemoryBarrier array_to_src = image_to_src;
    array_to_src.image = array_image;
    array_to_src.subresourceRange.layerCount = 2;
    VkBufferImageCopy array_copy = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2 },
        .imageExtent = { 2, 2, 1 }
    };
    VkBufferMemoryBarrier array_copy_to_host = copy_to_host;
    array_copy_to_host.size = sizeof array_source;
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &array_to_dst);
    vkCmdCopyBufferToImage(command_buffer, src_buffer, array_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &array_copy);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &array_to_src);
    vkCmdCopyImageToBuffer(command_buffer, array_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst_buffer, 1, &array_copy);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, NULL, 1, &array_copy_to_host, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &image_submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    CHECK(memcmp(mapped, array_source, sizeof array_source) == 0);
    vkUnmapMemory(device, dst_memory);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS && vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    VkImageMemoryBarrier array_to_general = array_to_src;
    array_to_general.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    array_to_general.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    array_to_general.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    array_to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkImageMemoryBarrier array_from_general = array_to_src;
    array_from_general.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    array_from_general.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    array_from_general.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    array_from_general.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkImageCopy array_layer_copy = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1 },
        .extent = { 2, 2, 1 }
    };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &array_to_general);
    vkCmdCopyImage(command_buffer, array_image, VK_IMAGE_LAYOUT_GENERAL, array_image, VK_IMAGE_LAYOUT_GENERAL,
        1, &array_layer_copy);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &array_from_general);
    vkCmdCopyImageToBuffer(command_buffer, array_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst_buffer, 1, &array_copy);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, NULL, 1, &array_copy_to_host, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &image_submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    CHECK(memcmp(mapped, array_source, 16) == 0 && memcmp((const uint8_t *)mapped + 16, array_source, 16) == 0);
    vkUnmapMemory(device, dst_memory);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS && vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    VkImageMemoryBarrier array_layer_to_general = array_to_general;
    array_layer_to_general.subresourceRange.baseArrayLayer = 1;
    array_layer_to_general.subresourceRange.layerCount = 1;
    VkImageMemoryBarrier invalid_array_layer_transition = array_layer_to_general;
    invalid_array_layer_transition.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &invalid_array_layer_transition);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &image_submit, fence) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    VkImageMemoryBarrier array_layer_from_general = array_from_general;
    array_layer_from_general.subresourceRange.baseArrayLayer = 1;
    array_layer_from_general.subresourceRange.layerCount = 1;
    VkBufferImageCopy array_layer0_read = array_copy;
    array_layer0_read.imageSubresource.layerCount = 1;
    VkBufferImageCopy array_layer1_read = array_layer0_read;
    array_layer1_read.bufferOffset = 16;
    array_layer1_read.imageSubresource.baseArrayLayer = 1;
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &array_layer_to_general);
    vkCmdCopyImageToBuffer(command_buffer, array_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst_buffer, 1, &array_layer0_read);
    vkCmdCopyImageToBuffer(command_buffer, array_image, VK_IMAGE_LAYOUT_GENERAL,
        dst_buffer, 1, &array_layer1_read);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &array_layer_from_general);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, NULL, 1, &array_copy_to_host, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &image_submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    CHECK(memcmp(mapped, array_source, 16) == 0 && memcmp((const uint8_t *)mapped + 16, array_source, 16) == 0);
    vkUnmapMemory(device, dst_memory);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS && vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    VkImageMemoryBarrier array_to_clear = array_to_dst;
    array_to_clear.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    array_to_clear.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkClearColorValue array_clear = { .float32 = { 1.0f, 0.0f, 0.0f, 1.0f } };
    VkImageSubresourceRange array_clear_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, 1 };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &array_to_clear);
    vkCmdClearColorImage(command_buffer, array_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        &array_clear, 1, &array_clear_range);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &array_to_src);
    vkCmdCopyImageToBuffer(command_buffer, array_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst_buffer, 1, &array_copy);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, NULL, 1, &array_copy_to_host, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &image_submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    const uint8_t clear_array_pixel[4] = { 255, 0, 0, 255 };
    CHECK(memcmp(mapped, array_source, 16) == 0);
    for (uint32_t pixel = 0; pixel < 4; pixel++)
        CHECK(memcmp((const uint8_t *)mapped + 16 + pixel * sizeof clear_array_pixel,
                     clear_array_pixel, sizeof clear_array_pixel) == 0);
    vkUnmapMemory(device, dst_memory);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    vkDestroyImage(device, array_image, NULL);
    vkFreeMemory(device, array_memory, NULL);
    puts("vulkan images: two-layer transfer/readback, cross-layer copy, partial barriers, transactional layout validation and selected-layer clear: ok");
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    uint32_t recorded_value = 0x1234abcdu;
    vkCmdUpdateBuffer(command_buffer, src_buffer, 0, sizeof recorded_value, &recorded_value);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    vkDestroyBuffer(device, src_buffer, NULL);
    src_buffer = 0;
    VkSubmitInfo invalid_buffer_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &invalid_buffer_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    if (argc >= 2) CHECK(run_compute(device, queue, command_buffer, fence, argv[1]) == 0);
    if (argc >= 3) {
        CHECK(run_descriptor_array(device, queue, command_buffer, fence, argv[2],
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0) == 0);
        CHECK(run_descriptor_array(device, queue, command_buffer, fence, argv[2],
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 0) == 0);
    }
    if (argc >= 4) {
        CHECK(run_uniform_buffer(device, queue, command_buffer, fence, argv[3], 0) == 0);
        CHECK(run_uniform_buffer(device, queue, command_buffer, fence, argv[3], 1) == 0);
    }
    if (argc >= 5) CHECK(run_push_constants(device, queue, command_buffer, fence, argv[4]) == 0);
    if (argc >= 6) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[5], 0) == 0);
    if (argc >= 7) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[6], 1) == 0);
    if (argc >= 8) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[7], 2) == 0);
    if (argc >= 9) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[8], 3) == 0);
    if (argc >= 11) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[10], 4) == 0);
    if (argc >= 12) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[11], 5) == 0);
    if (argc >= 13) {
        CHECK(run_descriptor_array(device, queue, command_buffer, fence, argv[12],
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1) == 0);
        CHECK(run_descriptor_array(device, queue, command_buffer, fence, argv[12],
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1) == 0);
    }
    if (argc >= 14 && features.shaderStorageImageArrayDynamicIndexing)
        CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[13], 6) == 0);
    if (argc >= 15 && features.shaderSampledImageArrayDynamicIndexing)
        CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[14], 7) == 0);
    if (argc >= 16 && features.shaderSampledImageArrayDynamicIndexing)
        CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[15], 8) == 0);
    if (argc >= 17) {
        CHECK(check_dynamic_sampled_descriptor_pipeline_gate(device, argv[16], 0) == 0);
        puts("vulkan features: dynamic sampled-image indexing from GlobalInvocationId rejected without nonuniform support: ok");
    }
    if (argc >= 18) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[17], 9) == 0);
    if (argc >= 19) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[18], 10) == 0);
    if (argc >= 20) CHECK(run_atomic_buffer_shader(device, queue, command_buffer, fence, argv[19], 0) == 0);
    if (argc >= 21) CHECK(run_atomic_buffer_shader(device, queue, command_buffer, fence, argv[20], 1) == 0);
    if (argc >= 22) CHECK(run_atomic_buffer_shader(device, queue, command_buffer, fence, argv[21], 2) == 0);
    if (argc >= 23) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[22], 11) == 0);
    if (argc >= 24) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[23], 12) == 0);
    if (argc >= 25) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[24], 13) == 0);
    if (argc >= 26) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[25], 14) == 0);
    if (argc >= 27) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[26], 15) == 0);
    if (argc >= 28) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[27], 16) == 0);
    if (argc >= 29) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[28], 17) == 0);
    if (argc >= 30) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[29], 18) == 0);
    puts(properties.limits.maxPerStageDescriptorStorageImages ?
         "vulkan descriptors: optimal RGBA8 storage-image write/copy and retained lifetime: ok" :
         "vulkan descriptors: storage-image path unavailable on this Metal device: ok");
    puts("vulkan transfer: buffer/image alias, image views, padded row copies, linear and opaque optimal layouts, image barriers/layout validation, clears and roundtrips: ok");
    vkDestroySemaphore(device, semaphore, NULL);
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyBuffer(device, src_buffer, NULL);
    vkDestroyBuffer(device, dst_buffer, NULL);
    vkFreeMemory(device, src_memory, NULL);
    vkFreeMemory(device, dst_memory, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyDevice(device_without_maintenance, NULL);

    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)vkGetInstanceProcAddr(instance, "vkDestroyInstance");
    destroy(instance, &allocator);
    CHECK(allocations == 1 && frees == 1);

    const char *unsupported_extension[] = { "VK_EXT_not_implemented" };
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = unsupported_extension;
    instance = (VkInstance)(uintptr_t)1;
    CHECK(vkCreateInstance(&info, NULL, &instance) == VK_ERROR_EXTENSION_NOT_PRESENT);
    CHECK(instance == NULL);

    const char *unsupported_layer[] = { "VK_LAYER_not_implemented" };
    info.enabledExtensionCount = 0;
    info.ppEnabledExtensionNames = NULL;
    info.enabledLayerCount = 1;
    info.ppEnabledLayerNames = unsupported_layer;
    CHECK(vkCreateInstance(&info, NULL, &instance) == VK_ERROR_LAYER_NOT_PRESENT);

    info.enabledLayerCount = 0;
    info.ppEnabledLayerNames = NULL;
    app.apiVersion = (1u << 22) | (1u << 12);
    CHECK(vkCreateInstance(&info, NULL, &instance) == VK_ERROR_INCOMPATIBLE_DRIVER);
    app.apiVersion = VK_API_VERSION_1_0;
    info.flags = 1;
    CHECK(vkCreateInstance(&info, NULL, &instance) == VK_ERROR_INITIALIZATION_FAILED);
    return 0;
}
