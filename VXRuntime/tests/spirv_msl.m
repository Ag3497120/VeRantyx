#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "gpu_ir_metal.h"
#include "gpu_ir_msl.h"
#include "spirv_ir.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int make_dynamic_storage_image_array_shader(uint32_t *words, size_t capacity, size_t *word_count);
static int make_dynamic_sampled_image_array_shader(uint32_t *words, size_t capacity,
                                                   size_t *word_count, int separate_sampler,
                                                   uint32_t index_builtin);

static int append_instruction(uint32_t *words, size_t capacity, size_t *count,
                              uint16_t opcode, uint16_t word_count, const uint32_t *operands) {
    if (!words || !count || *count > capacity || word_count == 0 || word_count > capacity - *count) return 0;
    words[(*count)++] = ((uint32_t)word_count << 16) | opcode;
    for (uint16_t i = 1; i < word_count; i++) words[(*count)++] = operands[i - 1];
    return 1;
}

static int make_graphics_vertex_shader(uint32_t *words, size_t capacity, size_t *word_count,
                                       uint32_t varying_decoration, int vertex_attribute,
                                       int test_instance_index) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 37;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                     // Shader capability
    OP(14, 3, 0, 1);                                  // Logical GLSL450 memory model
    if (vertex_attribute && test_instance_index)
        OP(15, 10, SPIRV_EXECUTION_MODEL_VERTEX, 17, 0x6e69616du, 0, 9, 10, 25, 27, 32);
    else if (vertex_attribute)
        OP(15, 9, SPIRV_EXECUTION_MODEL_VERTEX, 17, 0x6e69616du, 0, 9, 10, 25, 27);
    else if (test_instance_index)
        OP(15, 9, SPIRV_EXECUTION_MODEL_VERTEX, 17, 0x6e69616du, 0, 9, 10, 25, 32);
    else
        OP(15, 8, SPIRV_EXECUTION_MODEL_VERTEX, 17, 0x6e69616du, 0, 9, 10, 25);
    OP(71, 4, 9, 11, 42);                             // VertexIndex
    if (test_instance_index) OP(71, 4, 32, 11, 43);   // InstanceIndex
    OP(71, 4, 10, 11, 0);                             // Position
    OP(71, 4, 25, 30, 0);                             // Location 0 varying
    if (varying_decoration) OP(71, 3, 25, varying_decoration); // optional interpolation qualifier
    if (vertex_attribute) OP(71, 4, 27, 30, 0);       // vertex attribute at location 0
    OP(19, 2, 1);                                     // void
    OP(20, 2, 2);                                     // bool
    OP(21, 4, 3, 32, 0);                              // uint32
    OP(22, 3, 4, 32);                                 // float32
    OP(23, 4, 5, 4, 4);                               // float4
    OP(32, 4, 6, 1, 3);                               // Input pointer to uint32
    if (test_instance_index) OP(32, 4, 31, 1, 3);     // Input pointer to uint32 InstanceIndex
    OP(32, 4, 7, 3, 5);                               // Output pointer to float4
    if (vertex_attribute == 1) OP(32, 4, 28, 1, 5);   // Input pointer to float4 vertex attribute
    if (vertex_attribute == 2) OP(32, 4, 28, 1, 3);   // Input pointer to uint32 vertex attribute
    if (vertex_attribute == 3) {
        OP(21, 4, 30, 32, 1);                          // int32
        OP(32, 4, 28, 1, 30);                          // Input pointer to int32 vertex attribute
    }
    OP(33, 3, 8, 1);                                  // void()
    OP(59, 4, 6, 9, 1);
    if (test_instance_index) OP(59, 4, 31, 32, 1);
    OP(59, 4, 7, 10, 3);
    OP(59, 4, 7, 25, 3);
    if (vertex_attribute) OP(59, 4, 28, 27, 1);
    OP(43, 4, 3, 11, 1);                              // uint 1
    OP(43, 4, 3, 12, 2);                              // uint 2
    OP(43, 4, 4, 13, 0xbf800000u);                    // float -1
    OP(43, 4, 4, 14, 0x40400000u);                    // float 3
    OP(43, 4, 4, 15, 0x00000000u);                    // float 0
    OP(43, 4, 4, 16, 0x3f800000u);                    // float 1
    if (test_instance_index) OP(43, 4, 4, 36, 0x41200000u); // float 10, offscreen
    OP(44, 7, 5, 26, 16, 15, 15, 16);                 // red float4 varying
    OP(54, 5, 1, 17, 0, 8);
    OP(248, 2, 18);
    if (vertex_attribute == 1) {
        OP(61, 4, 5, 29, 27);                         // load float4 vertex position attribute
        OP(62, 3, 10, 29);
    } else {
        OP(61, 4, 3, 19, 9);                         // load VertexIndex
        OP(170, 5, 2, 20, 19, 11);                   // vertex == 1
        OP(169, 6, 4, 21, 20, 14, 13);               // x = vertex == 1 ? 3 : -1
        OP(170, 5, 2, 22, 19, 12);                   // vertex == 2
        OP(169, 6, 4, 23, 22, 14, 13);               // y = vertex == 2 ? 3 : -1
        uint32_t position_x = 21;
        if (test_instance_index) {
            OP(61, 4, 3, 33, 32);                     // load InstanceIndex
            OP(170, 5, 2, 34, 33, 11);               // instance == firstInstance (1)
            OP(169, 6, 4, 35, 34, 21, 36);           // x = visible only for InstanceIndex 1
            position_x = 35;
        }
        OP(80, 7, 5, 24, position_x, 23, 15, 16);     // clip-space position
        OP(62, 3, 10, 24);
    }
    OP(62, 3, 25, 26);
    OP(253, 1);
    OP(56, 1);
#undef OP
    *word_count = count;
    return 1;
}

static int make_graphics_fragment_shader(uint32_t *words, size_t capacity, size_t *word_count) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 21;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                     // Shader capability
    OP(14, 3, 0, 1);                                  // Logical GLSL450 memory model
    OP(15, 9, SPIRV_EXECUTION_MODEL_FRAGMENT, 16, 0x6e69616du, 0, 9, 10, 11, 19);
    OP(16, 3, 16, 7);                                 // OriginUpperLeft
    OP(71, 4, 9, 30, 0);                              // Location 0
    OP(71, 4, 19, 30, 0);                             // Location 0 varying input
    OP(71, 4, 10, 11, 15);                            // FragCoord
    OP(71, 4, 11, 11, 17);                            // FrontFacing
    OP(19, 2, 1);                                     // void
    OP(22, 3, 2, 32);                                 // float32
    OP(23, 4, 3, 2, 4);                               // float4
    OP(32, 4, 4, 3, 3);                               // Output pointer to float4
    OP(33, 3, 5, 1);                                  // void()
    OP(20, 2, 6);                                     // bool
    OP(32, 4, 7, 1, 3);                               // Input pointer to float4
    OP(32, 4, 8, 1, 6);                               // Input pointer to bool
    OP(32, 4, 18, 1, 3);                              // Input pointer to float4 varying
    OP(59, 4, 4, 9, 3);
    OP(59, 4, 7, 10, 1);
    OP(59, 4, 8, 11, 1);
    OP(59, 4, 18, 19, 1);
    OP(43, 4, 2, 12, 0x3f800000u);                    // red
    OP(43, 4, 2, 13, 0x00000000u);                    // green
    OP(43, 4, 2, 14, 0x00000000u);                    // blue
    OP(43, 4, 2, 15, 0x3f800000u);                    // alpha
    OP(54, 5, 1, 16, 0, 5);
    OP(248, 2, 17);
    OP(61, 4, 3, 20, 19);                             // read interpolated Location 0 varying
    OP(62, 3, 9, 20);
    OP(253, 1);
    OP(56, 1);
#undef OP
    *word_count = count;
    return 1;
}

static int check_fragment_termination_shader(const char *output_path) {
    uint32_t words[64] = { SPIRV_MAGIC, 0x00010000u, 0, 9, 0 };
    size_t word_count = 5;
    const uint32_t entry_point[] = { SPIRV_EXECUTION_MODEL_FRAGMENT, 6, 0x6e69616du, 0, 8 };
    const uint32_t origin_upper_left[] = { 6, 7 };
    const uint32_t color_location[] = { 8, 30, 0 };
    const uint32_t type_void[] = { 1 };
    const uint32_t type_float[] = { 2, 32 };
    const uint32_t type_float4[] = { 3, 2, 4 };
    const uint32_t pointer_output[] = { 4, 3, 3 };
    const uint32_t type_function[] = { 5, 1 };
    const uint32_t color_variable[] = { 4, 8, 3 };
    const uint32_t function[] = { 1, 6, 0, 5 };
    const uint32_t label[] = { 7 };
    if (!append_instruction(words, 64, &word_count, 17, 2, (uint32_t[]){ 1 }) ||
        !append_instruction(words, 64, &word_count, 14, 3, (uint32_t[]){ 0, 1 }) ||
        !append_instruction(words, 64, &word_count, 15, 6, entry_point) ||
        !append_instruction(words, 64, &word_count, 16, 3, origin_upper_left) ||
        !append_instruction(words, 64, &word_count, 71, 4, color_location) ||
        !append_instruction(words, 64, &word_count, 19, 2, type_void) ||
        !append_instruction(words, 64, &word_count, 22, 3, type_float) ||
        !append_instruction(words, 64, &word_count, 23, 4, type_float4) ||
        !append_instruction(words, 64, &word_count, 32, 4, pointer_output) ||
        !append_instruction(words, 64, &word_count, 33, 3, type_function) ||
        !append_instruction(words, 64, &word_count, 59, 4, color_variable) ||
        !append_instruction(words, 64, &word_count, 54, 5, function) ||
        !append_instruction(words, 64, &word_count, 248, 2, label) ||
        !append_instruction(words, 64, &word_count, 252, 1, NULL) ||
        !append_instruction(words, 64, &word_count, 56, 1, NULL))
        return 0;

    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    char *msl = NULL;
    size_t msl_size = 0;
    int read_rc = spirv_read(words, word_count * sizeof words[0], &module);
    const spirv_entry_point *entry = read_rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_FRAGMENT) : NULL;
    int rc = entry ? spirv_build_gpu_ir_shader(&module, entry, &shader) : GPU_IR_INVALID;
    const gpu_ir_shader_block *block = shader ? gpu_ir_shader_block_at(shader, 0) : NULL;
    const gpu_ir_shader_instruction *termination = shader ?
        gpu_ir_shader_instruction_at(shader, block ? block->first_instruction + block->instruction_count - 1u : 0u) : NULL;
    int ok = rc == GPU_IR_OK && block && block->terminator_opcode == GPU_IR_SHADER_OP_TERMINATE_INVOCATION &&
        termination && termination->opcode == GPU_IR_SHADER_OP_TERMINATE_INVOCATION &&
        termination->source_opcode == 252 &&
        gpu_ir_msl_emit_fragment(shader, &msl, &msl_size) == GPU_IR_OK && msl && msl_size &&
        strstr(msl, "discard_fragment();") && strstr(msl, "return vx_io_8;");
    if (ok && output_path) {
        FILE *output = fopen(output_path, "wb");
        ok = output && fwrite(msl, 1, msl_size, output) == msl_size;
        if (output && fclose(output)) ok = 0;
    }
    if (!ok)
        fprintf(stderr, "spirv_msl: OpKill fragment termination lowering/emission failed (read=%d lower=%d)\n",
                read_rc, rc);
    else
        puts("spirv_msl: OpKill maps to common fragment termination and MSL discard: ok");
    free(msl);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int make_graphics_sampled_image_fragment_shader(uint32_t *words, size_t capacity,
                                                       size_t *word_count, int implicit_lod) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 20;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                     // Shader capability
    OP(14, 3, 0, 1);                                  // Logical GLSL450 memory model
    OP(15, 7, SPIRV_EXECUTION_MODEL_FRAGMENT, 16, 0x6e69616du, 0, 11, 12);
    OP(16, 3, 16, 7);                                 // OriginUpperLeft
    OP(71, 4, 11, 30, 0);                            // Color output Location 0
    OP(71, 4, 12, 34, 0);                            // Sampled image DescriptorSet 0
    OP(71, 4, 12, 33, 0);                            // Sampled image Binding 0
    OP(19, 2, 1);                                     // void
    OP(22, 3, 2, 32);                                // float32
    OP(23, 4, 3, 2, 2);                              // float2
    OP(23, 4, 4, 2, 4);                              // float4
    OP(25, 9, 6, 2, 1, 0, 0, 0, 1, 0);              // sampled 2D float image
    OP(27, 3, 7, 6);                                 // combined sampled image
    OP(32, 4, 8, 3, 4);                              // Output pointer to float4
    OP(32, 4, 9, 0, 7);                              // UniformConstant pointer to sampled image
    OP(33, 3, 10, 1);                                // void()
    OP(43, 4, 2, 13, 0x3f000000u);                   // coordinate component 0.5
    OP(43, 4, 2, 14, 0);                             // explicit LOD 0
    OP(44, 5, 3, 15, 13, 13);                        // float2(0.5, 0.5)
    OP(59, 4, 8, 11, 3);                             // color output
    OP(59, 4, 9, 12, 0);                             // sampled-image descriptor
    OP(54, 5, 1, 16, 0, 10);                         // main
    OP(248, 2, 17);                                  // entry block
    OP(61, 4, 7, 18, 12);                            // load sampled image
    if (implicit_lod) OP(87, 5, 4, 19, 18, 15);       // sample with fragment derivatives
    else OP(88, 7, 4, 19, 18, 15, 2, 14);            // sample at explicit LOD 0
    OP(62, 3, 11, 19);                               // output sampled color
    OP(253, 1);
    OP(56, 1);
#undef OP
    *word_count = count;
    return 1;
}

static int make_graphics_separate_image_sampler_fragment_shader(uint32_t *words, size_t capacity,
                                                                 size_t *word_count, int implicit_lod) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 25;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                     // Shader capability
    OP(14, 3, 0, 1);                                  // Logical GLSL450 memory model
    OP(15, 8, SPIRV_EXECUTION_MODEL_FRAGMENT, 16, 0x6e69616du, 0, 13, 14, 15);
    OP(16, 3, 16, 7);                                 // OriginUpperLeft
    OP(71, 4, 13, 30, 0);                            // Color output Location 0
    OP(71, 4, 14, 34, 0);                            // Sampled image DescriptorSet 0
    OP(71, 4, 14, 33, 0);                            // Sampled image Binding 0
    OP(71, 4, 15, 34, 0);                            // Sampler DescriptorSet 0
    OP(71, 4, 15, 33, 1);                            // Sampler Binding 1
    OP(19, 2, 1);                                     // void
    OP(22, 3, 2, 32);                                // float32
    OP(23, 4, 3, 2, 2);                              // float2
    OP(23, 4, 4, 2, 4);                              // float4
    OP(25, 9, 6, 2, 1, 0, 0, 0, 1, 0);              // sampled 2D float image
    OP(26, 2, 7);                                     // sampler
    OP(27, 3, 8, 6);                                 // combined sampled-image operation type
    OP(32, 4, 9, 3, 4);                              // Output pointer to float4
    OP(32, 4, 10, 0, 6);                             // UniformConstant pointer to image
    OP(32, 4, 11, 0, 7);                             // UniformConstant pointer to sampler
    OP(33, 3, 12, 1);                                // void()
    OP(43, 4, 2, 18, 0x3f000000u);                   // coordinate component 0.5
    OP(43, 4, 2, 19, 0);                             // explicit LOD 0
    OP(44, 5, 3, 20, 18, 18);                        // float2(0.5, 0.5)
    OP(59, 4, 9, 13, 3);                             // color output
    OP(59, 4, 10, 14, 0);                           // sampled-image descriptor
    OP(59, 4, 11, 15, 0);                           // sampler descriptor
    OP(54, 5, 1, 16, 0, 12);                         // main
    OP(248, 2, 17);                                  // entry block
    OP(61, 4, 6, 21, 14);                            // load image
    OP(61, 4, 7, 22, 15);                            // load sampler
    OP(86, 5, 8, 23, 21, 22);                        // combine image and sampler
    if (implicit_lod) OP(87, 5, 4, 24, 23, 20);       // sample with fragment derivatives
    else OP(88, 7, 4, 24, 23, 20, 2, 19);            // sample at explicit LOD 0
    OP(62, 3, 13, 24);                               // output sampled color
    OP(253, 1);
    OP(56, 1);
#undef OP
    *word_count = count;
    return 1;
}

static int emit_graphics_sampled_image_layout(char **source, size_t *source_size, int implicit_lod) {
    if (source) *source = NULL;
    if (source_size) *source_size = 0;
    if (!source || !source_size) return 0;
    uint32_t words[128];
    size_t word_count = 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    int made = make_graphics_sampled_image_fragment_shader(words,
        sizeof words / sizeof words[0], &word_count, implicit_lod);
    int spirv_rc = made ? spirv_read(words, word_count * sizeof words[0], &module) : SPIRV_INVALID;
    int ok = made && spirv_rc == SPIRV_OK;
    const spirv_entry_point *entry = ok ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_FRAGMENT) : NULL;
    int shader_rc = entry ? spirv_build_gpu_ir_shader(&module, entry, &shader) : GPU_IR_INVALID;
    int layout_rc = entry ? spirv_build_gpu_ir_layout(&module, entry, &layout) : GPU_IR_INVALID;
    if (!entry || shader_rc != GPU_IR_OK || layout_rc != GPU_IR_OK) ok = 0;
    int emit_rc = ok ? gpu_ir_msl_emit_fragment_with_layout(shader, layout, source, source_size) : GPU_IR_INVALID;
    if (ok) ok = emit_rc == GPU_IR_OK && *source && *source_size;
    if (!ok) fprintf(stderr, "spirv_msl: fragment sampled-image layout emission failed "
                     "(made %d, SPIR-V %d, shader %d, layout %d, emitter %d)\n%s",
                     made, spirv_rc, shader_rc, layout_rc, emit_rc,
                     *source ? *source : "<no MSL generated>\n");
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int emit_graphics_separate_image_sampler_layout(char **source, size_t *source_size, int implicit_lod) {
    if (source) *source = NULL;
    if (source_size) *source_size = 0;
    if (!source || !source_size) return 0;
    uint32_t words[160];
    size_t word_count = 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    int made = make_graphics_separate_image_sampler_fragment_shader(words,
        sizeof words / sizeof words[0], &word_count, implicit_lod);
    int spirv_rc = made ? spirv_read(words, word_count * sizeof words[0], &module) : SPIRV_INVALID;
    int ok = made && spirv_rc == SPIRV_OK;
    const spirv_entry_point *entry = ok ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_FRAGMENT) : NULL;
    int shader_rc = entry ? spirv_build_gpu_ir_shader(&module, entry, &shader) : GPU_IR_INVALID;
    int layout_rc = entry ? spirv_build_gpu_ir_layout(&module, entry, &layout) : GPU_IR_INVALID;
    if (!entry || shader_rc != GPU_IR_OK || layout_rc != GPU_IR_OK) ok = 0;
    int emit_rc = ok ? gpu_ir_msl_emit_fragment_with_layout(shader, layout, source, source_size) : GPU_IR_INVALID;
    if (ok) ok = emit_rc == GPU_IR_OK && *source && *source_size;
    if (!ok) fprintf(stderr, "spirv_msl: fragment separate image/sampler layout emission failed "
                     "(made %d, SPIR-V %d, shader %d, layout %d, emitter %d)\n%s",
                     made, spirv_rc, shader_rc, layout_rc, emit_rc,
                     *source ? *source : "<no MSL generated>\n");
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int check_graphics_sampled_image_layout(void) {
    char *source = NULL;
    size_t source_size = 0;
    int combined_ok = emit_graphics_sampled_image_layout(&source, &source_size, 0) && source_size &&
        strstr(source, "texture2d<float, access::sample> vx_image_12 [[texture(0)]]") &&
        strstr(source, "sampler vx_sampler_12 [[sampler(0)]]") &&
        strstr(source, "vx_image_12.sample(vx_sampler_12,") && strstr(source, "level(") &&
        strstr(source, "fragment float4 vx_fragment(");
    free(source);
    source = NULL;
    source_size = 0;
    int separate_ok = emit_graphics_separate_image_sampler_layout(&source, &source_size, 0) && source && source_size &&
        strstr(source, "texture2d<float, access::sample> vx_image_14 [[texture(0)]]") &&
        strstr(source, "sampler vx_sampler_15 [[sampler(0)]]") &&
        strstr(source, "vx_image_14.sample(vx_sampler_15,") && strstr(source, "level(");
    if (separate_ok) puts("spirv_msl: fragment separate image/sampler layout and explicit-LOD sampling: ok");
    else fprintf(stderr, "spirv_msl: fragment separate image/sampler bindings or sample expression were missing\n");
    free(source);
    if (combined_ok) puts("spirv_msl: fragment combined-image-sampler layout and explicit-LOD sampling: ok");
    else fprintf(stderr, "spirv_msl: fragment combined-image-sampler bindings or sample expression were missing\n");

    source = NULL;
    source_size = 0;
    int implicit_combined_ok = emit_graphics_sampled_image_layout(&source, &source_size, 1) && source && source_size &&
        strstr(source, "texture2d<float, access::sample> vx_image_12 [[texture(0)]]") &&
        strstr(source, "sampler vx_sampler_12 [[sampler(0)]]") &&
        strstr(source, "dfdx(") && strstr(source, "dfdy(") && strstr(source, "gradient2d(");
    free(source);
    source = NULL;
    source_size = 0;
    int implicit_separate_ok = emit_graphics_separate_image_sampler_layout(&source, &source_size, 1) &&
        source && source_size && strstr(source, "texture2d<float, access::sample> vx_image_14 [[texture(0)]]") &&
        strstr(source, "sampler vx_sampler_15 [[sampler(0)]]") && strstr(source, "dfdx(") &&
        strstr(source, "dfdy(") && strstr(source, "gradient2d(");
    if (implicit_separate_ok)
        puts("spirv_msl: fragment separate image/sampler layout and implicit-LOD gradients: ok");
    else fprintf(stderr, "spirv_msl: separate image/sampler implicit-LOD gradient lowering failed\n");
    free(source);
    if (implicit_combined_ok)
        puts("spirv_msl: fragment combined-image-sampler layout and implicit-LOD gradients: ok");
    else fprintf(stderr, "spirv_msl: combined image-sampler implicit-LOD gradient lowering failed\n");
    return combined_ok && separate_ok && implicit_combined_ok && implicit_separate_ok;
}

static int write_graphics_sampled_image_layout(const char *path, int implicit_lod) {
    if (!path || !*path) return 0;
    char *source = NULL;
    size_t source_size = 0;
    if (!emit_graphics_sampled_image_layout(&source, &source_size, implicit_lod)) return 0;
    FILE *file = fopen(path, "w");
    int ok = file && fwrite(source, 1, source_size, file) == source_size && !ferror(file);
    if (file && fclose(file)) ok = 0;
    free(source);
    return ok;
}

static int write_graphics_separate_image_sampler_layout(const char *path, int implicit_lod) {
    if (!path || !*path) return 0;
    char *source = NULL;
    size_t source_size = 0;
    if (!emit_graphics_separate_image_sampler_layout(&source, &source_size, implicit_lod)) return 0;
    FILE *file = fopen(path, "w");
    int ok = file && fwrite(source, 1, source_size, file) == source_size && !ferror(file);
    if (file && fclose(file)) ok = 0;
    free(source);
    return ok;
}

static int graphics_vertex_input_msl_for_type(int vertex_attribute, char **source, size_t *source_size) {
    uint32_t words[256];
    size_t word_count = 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    int ok = make_graphics_vertex_shader(words, sizeof words / sizeof words[0], &word_count,
                                         0, vertex_attribute, 0) &&
        spirv_read(words, word_count * sizeof words[0], &module) == SPIRV_OK;
    const spirv_entry_point *entry = ok ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_VERTEX) : NULL;
    if (!entry || spirv_build_gpu_ir_shader(&module, entry, &shader) != GPU_IR_OK) ok = 0;
    if (ok) ok = gpu_ir_msl_emit_vertex(shader, source, source_size) == GPU_IR_OK;
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int graphics_vertex_input_msl(char **source, size_t *source_size) {
    return graphics_vertex_input_msl_for_type(1, source, source_size);
}

static int write_graphics_integer_vertex_input_msl(const char *path, int signed_input) {
    if (!path || !*path) return 0;
    char *source = NULL;
    size_t source_size = 0;
    if (!graphics_vertex_input_msl_for_type(signed_input ? 3 : 2, &source, &source_size)) return 0;
    FILE *file = fopen(path, "w");
    int ok = file && fwrite(source, 1, source_size, file) == source_size && !ferror(file);
    if (file && fclose(file)) ok = 0;
    free(source);
    return ok;
}

static int check_graphics_vertex_attribute_msl(void) {
    char *source = NULL;
    size_t source_size = 0;
    int ok = graphics_vertex_input_msl(&source, &source_size) && source_size &&
        strstr(source, "struct VxVertexInput {") &&
        strstr(source, "float4 vx_attribute_0 [[attribute(0)]];") &&
        strstr(source, "VxVertexInput vx_input [[stage_in]]") &&
        strstr(source, "vx_input.vx_attribute_0") &&
        !strstr(source, "float4 vx_builtin_27 [[attribute(0)]]");
    if (ok) puts("spirv_msl: vertex attributes use a Metal stage_in structure: ok");
    else fprintf(stderr, "spirv_msl: vertex attribute MSL interface was not emitted correctly\n%s",
                  source ? source : "<no MSL generated>\n");
    free(source);
    return ok;
}

static int check_graphics_integer_vertex_attribute_msl(void) {
    const int attribute_kinds[] = { 2, 3 };
    const char *expected_types[] = { "uint vx_attribute_0 [[attribute(0)]];",
                                     "int vx_attribute_0 [[attribute(0)]];" };
    for (size_t i = 0; i < sizeof attribute_kinds / sizeof attribute_kinds[0]; i++) {
        uint32_t words[256];
        size_t word_count = 0, source_size = 0;
        spirv_module module = { 0 };
        gpu_ir_shader *shader = NULL;
        char *source = NULL;
        int ok = make_graphics_vertex_shader(words, sizeof words / sizeof words[0], &word_count,
                                             0, attribute_kinds[i], 0) &&
            spirv_read(words, word_count * sizeof words[0], &module) == SPIRV_OK;
        const spirv_entry_point *entry = ok ?
            spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_VERTEX) : NULL;
        if (!entry || spirv_build_gpu_ir_shader(&module, entry, &shader) != GPU_IR_OK) ok = 0;
        if (ok) ok = gpu_ir_msl_emit_vertex(shader, &source, &source_size) == GPU_IR_OK &&
            source_size && strstr(source, expected_types[i]) &&
            strstr(source, "VxVertexInput vx_input [[stage_in]]");
        free(source);
        gpu_ir_shader_release(shader);
        spirv_free(&module);
        if (!ok) {
            fprintf(stderr, "spirv_msl: integer vertex input type %s was not preserved\n",
                    i ? "sint32" : "uint32");
            return 0;
        }
    }
    puts("spirv_msl: uint32 and sint32 vertex attributes retain their Metal stage_in types: ok");
    return 1;
}

static int write_graphics_vertex_input_msl(const char *path) {
    if (!path || !*path) return 0;
    char *source = NULL;
    size_t source_size = 0;
    if (!graphics_vertex_input_msl(&source, &source_size)) return 0;
    FILE *file = fopen(path, "w");
    int ok = file && fwrite(source, 1, source_size, file) == source_size && !ferror(file);
    if (file && fclose(file)) ok = 0;
    free(source);
    return ok;
}

static int set_spirv_decoration_literal(uint32_t *words, size_t word_count,
                                       uint32_t target_id, uint32_t decoration, uint32_t value) {
    if (!words || word_count < 5) return 0;
    for (size_t offset = 5; offset < word_count;) {
        uint32_t instruction = words[offset];
        uint32_t count = instruction >> 16;
        uint32_t opcode = instruction & 0xffffu;
        if (!count || count > word_count - offset) return 0;
        if (opcode == 71 && count == 4 && words[offset + 1] == target_id &&
            words[offset + 2] == decoration) {
            words[offset + 3] = value;
            return 1;
        }
        offset += count;
    }
    return 0;
}

static int check_graphics_sparse_location_link(void) {
    uint32_t vertex_words[256], fragment_words[256];
    size_t vertex_word_count = 0, fragment_word_count = 0;
    spirv_module vertex_module = { 0 }, fragment_module = { 0 };
    gpu_ir_shader *vertex_shader = NULL, *fragment_shader = NULL;
    char *vertex_msl = NULL, *fragment_msl = NULL;
    size_t vertex_msl_size = 0, fragment_msl_size = 0;
    int ok = make_graphics_vertex_shader(vertex_words, 256, &vertex_word_count, 0, 0, 0) &&
        make_graphics_fragment_shader(fragment_words, 256, &fragment_word_count) &&
        set_spirv_decoration_literal(vertex_words, vertex_word_count, 25, 30, 20) &&
        set_spirv_decoration_literal(fragment_words, fragment_word_count, 19, 30, 20) &&
        spirv_read(vertex_words, vertex_word_count * sizeof vertex_words[0], &vertex_module) == SPIRV_OK &&
        spirv_read(fragment_words, fragment_word_count * sizeof fragment_words[0], &fragment_module) == SPIRV_OK;
    const spirv_entry_point *vertex_entry = ok ?
        spirv_find_entry_point(&vertex_module, "main", SPIRV_EXECUTION_MODEL_VERTEX) : NULL;
    const spirv_entry_point *fragment_entry = ok ?
        spirv_find_entry_point(&fragment_module, "main", SPIRV_EXECUTION_MODEL_FRAGMENT) : NULL;
    if (!vertex_entry || !fragment_entry ||
        spirv_build_gpu_ir_shader(&vertex_module, vertex_entry, &vertex_shader) != GPU_IR_OK ||
        spirv_build_gpu_ir_shader(&fragment_module, fragment_entry, &fragment_shader) != GPU_IR_OK) ok = 0;
    if (ok) ok = gpu_ir_msl_validate_graphics_link(vertex_shader, fragment_shader, 64, 60) == GPU_IR_OK &&
        gpu_ir_msl_emit_vertex(vertex_shader, &vertex_msl, &vertex_msl_size) == GPU_IR_OK &&
        gpu_ir_msl_emit_fragment(fragment_shader, &fragment_msl, &fragment_msl_size) == GPU_IR_OK &&
        strstr(vertex_msl, "[[user(locn20)]]") && strstr(fragment_msl, "[[user(locn20)]]");
    free(vertex_msl);
    free(fragment_msl);
    gpu_ir_shader_release(vertex_shader);
    gpu_ir_shader_release(fragment_shader);
    spirv_free(&vertex_module);
    spirv_free(&fragment_module);
    if (ok) puts("spirv_msl: sparse Location 20 links within 60-component interface limits: ok");
    else fprintf(stderr, "spirv_msl: sparse interface location was incorrectly limited by component count\n");
    return ok;
}

static int check_graphics_stage_msl(void) {
    uint32_t vertex_words[256], fragment_words[256];
    size_t vertex_word_count = 0, fragment_word_count = 0;
    if (!make_graphics_vertex_shader(vertex_words, 256, &vertex_word_count, 0, 0, 1) ||
        !make_graphics_fragment_shader(fragment_words, 256, &fragment_word_count)) return 0;
    spirv_module vertex_module = { 0 }, fragment_module = { 0 };
    gpu_ir_shader *vertex_shader = NULL, *fragment_shader = NULL;
    char *vertex_msl = NULL, *fragment_msl = NULL;
    size_t vertex_msl_size = 0, fragment_msl_size = 0;
    int ok = 0;
    gpu_ir_metal_device *device = NULL;
    gpu_ir_pipeline *pipeline = NULL;
    gpu_ir_resource *backing = NULL, *texture = NULL;
    gpu_ir_stream stream;
    int stream_initialized = 0;
    const char *failure_stage = "vertex SPIR-V read";
    int rc = spirv_read(vertex_words, vertex_word_count * sizeof vertex_words[0], &vertex_module);
    const spirv_entry_point *vertex_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&vertex_module, "main", SPIRV_EXECUTION_MODEL_VERTEX) : NULL;
    if (rc != SPIRV_OK || !vertex_entry) goto done;
    failure_stage = "vertex common-IR lowering";
    if (spirv_build_gpu_ir_shader(&vertex_module, vertex_entry, &vertex_shader) != GPU_IR_OK) goto done;
    failure_stage = "vertex MSL emission";
    if (gpu_ir_msl_emit_vertex(vertex_shader, &vertex_msl, &vertex_msl_size) != GPU_IR_OK) goto done;
    failure_stage = "fragment SPIR-V read";
    rc = spirv_read(fragment_words, fragment_word_count * sizeof fragment_words[0], &fragment_module);
    const spirv_entry_point *fragment_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&fragment_module, "main", SPIRV_EXECUTION_MODEL_FRAGMENT) : NULL;
    if (rc != SPIRV_OK || !fragment_entry) goto done;
    failure_stage = "fragment common-IR lowering";
    if (spirv_build_gpu_ir_shader(&fragment_module, fragment_entry, &fragment_shader) != GPU_IR_OK) goto done;
    failure_stage = "fragment MSL emission";
    if (gpu_ir_msl_emit_fragment(fragment_shader, &fragment_msl, &fragment_msl_size) != GPU_IR_OK) goto done;
    failure_stage = "graphics interface validation";
    if (gpu_ir_msl_validate_graphics_link(vertex_shader, fragment_shader, 128, 124) != GPU_IR_OK) goto done;
    if (!vertex_msl || !fragment_msl || !strstr(vertex_msl, "[[vertex_id]]") ||
        !strstr(vertex_msl, "[[instance_id]]") ||
        !strstr(vertex_msl, "vx_output.position = vx_io_10;") ||
        !strstr(vertex_msl, "float4 vx_location_0 [[user(locn0)]];") ||
        !strstr(vertex_msl, "vx_output.vx_location_0 = vx_io_25;") ||
        !strstr(fragment_msl, "VxFragmentInput vx_input [[stage_in]]") ||
        !strstr(fragment_msl, "float4 vx_location_0 [[user(locn0)]];") ||
        !strstr(fragment_msl, "[[position]]") || !strstr(fragment_msl, "[[front_facing]]") ||
        !strstr(fragment_msl, "fragment float4 vx_fragment(") ||
        !strstr(fragment_msl, "float4 vx_value_20 = vx_input.vx_location_0;") ||
        !strstr(fragment_msl, "return vx_io_9;")) goto done;
    failure_stage = "Metal device creation";
    device = gpu_ir_metal_device_create();
    if (!device) goto done;
    char pipeline_error[512] = { 0 };
    failure_stage = "Metal graphics pipeline creation";
    pipeline = gpu_ir_metal_graphics_pipeline_create(device, vertex_msl, fragment_msl,
        GPU_IR_FORMAT_RGBA8_UNORM, pipeline_error, sizeof pipeline_error);
    if (!pipeline) {
        if (pipeline_error[0]) fprintf(stderr, "spirv_msl: graphics Metal pipeline failed: %s\n", pipeline_error);
        goto done;
    }
    enum { WIDTH = 8, HEIGHT = 8, BYTES_PER_PIXEL = 4 };
    uint64_t row_pitch = gpu_ir_metal_linear_texture_row_pitch(device, GPU_IR_FORMAT_RGBA8_UNORM, WIDTH);
    uint64_t alignment = gpu_ir_metal_linear_texture_alignment(device, GPU_IR_FORMAT_RGBA8_UNORM);
    if (row_pitch < WIDTH * BYTES_PER_PIXEL || !alignment || row_pitch % alignment) goto done;
    failure_stage = "Metal attachment creation";
    backing = gpu_ir_metal_buffer_create(device, row_pitch * HEIGHT, NULL);
    if (!backing) goto done;
    gpu_ir_texture_desc texture_description = {
        .width = WIDTH, .height = HEIGHT, .depth = 1, .array_layers = 1, .mip_levels = 1,
        .bytes_per_texel = BYTES_PER_PIXEL, .sample_count = 1, .block_width = 1, .block_height = 1,
        .bytes_per_block = BYTES_PER_PIXEL, .format = GPU_IR_FORMAT_RGBA8_UNORM
    };
    texture = gpu_ir_metal_texture_create_from_buffer(device, backing, 0, row_pitch,
        &texture_description, 0, 0);
    if (!texture) goto done;
    failure_stage = "render-pass recording/execution";
    gpu_ir_stream_init(&stream);
    stream_initialized = 1;
    gpu_ir_render_pass_desc pass = {
        .mip_level = 0, .array_layer = 0, .render_area = { 0, 0, WIDTH, HEIGHT },
        .load_action = GPU_IR_RENDER_PASS_CLEAR, .store_action = GPU_IR_RENDER_PASS_STORE,
        .clear_value_type = GPU_IR_CLEAR_COLOR_VALUE_FLOAT,
        .clear_value = { .color = { 0.0f, 0.0f, 0.0f, 1.0f } }
    };
    gpu_ir_draw_info draw = {
        .topology = GPU_IR_TOPOLOGY_TRIANGLE_LIST, .vertex_count = 3, .instance_count = 1,
        .first_instance = 1
    };
    if (gpu_ir_record_begin_render_pass(&stream, texture, &pass, NULL) != GPU_IR_OK ||
        gpu_ir_record_draw(&stream, pipeline, &draw, NULL) != GPU_IR_OK ||
        gpu_ir_record_end_render_pass(&stream, NULL) != GPU_IR_OK ||
        gpu_ir_metal_execute_stream(device, &stream) != GPU_IR_OK) goto done;
    failure_stage = "Metal pixel readback";
    const uint8_t *pixels = gpu_ir_metal_buffer_contents(backing);
    static const uint8_t red[4] = { 255, 0, 0, 255 };
    if (!pixels || memcmp(pixels + (HEIGHT / 2) * row_pitch + (WIDTH / 2) * BYTES_PER_PIXEL,
                          red, sizeof red)) goto done;
    ok = 1;
done:
    if (stream_initialized) gpu_ir_stream_destroy(&stream);
    gpu_ir_pipeline_release(pipeline);
    gpu_ir_resource_release(texture);
    gpu_ir_resource_release(backing);
    gpu_ir_metal_device_release(device);
    free(vertex_msl);
    free(fragment_msl);
    gpu_ir_shader_release(vertex_shader);
    gpu_ir_shader_release(fragment_shader);
    spirv_free(&vertex_module);
    spirv_free(&fragment_module);
    if (ok) puts("spirv_msl: SPIR-V VertexIndex/InstanceIndex preserve a nonzero firstInstance through common IR/Metal with red-pixel readback: ok");
    else fprintf(stderr, "spirv_msl: graphics-stage test failed during %s\n", failure_stage);
    return ok;
}

static int check_graphics_interpolation_rejected(uint32_t spirv_decoration, uint32_t gpu_decoration,
                                                const char *name) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    char *source = NULL;
    size_t source_size = 0;
    int ok = name && gpu_decoration &&
        make_graphics_vertex_shader(words, sizeof words / sizeof words[0], &word_count,
                                    spirv_decoration, 0, 0) &&
        spirv_read(words, word_count * sizeof words[0], &module) == SPIRV_OK;
    const spirv_entry_point *entry = ok ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_VERTEX) : NULL;
    if (!entry || spirv_build_gpu_ir_shader(&module, entry, &shader) != GPU_IR_OK) ok = 0;
    const gpu_ir_shader_instruction *varying = NULL;
    for (uint32_t i = 0; ok && i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (instruction && instruction->opcode == GPU_IR_SHADER_OP_VARIABLE && instruction->result_id == 25)
            varying = instruction;
    }
    if (!varying || !(varying->decoration_flags & gpu_decoration)) ok = 0;
    if (ok) ok = gpu_ir_msl_emit_vertex(shader, &source, &source_size) == GPU_IR_UNSUPPORTED;
    free(source);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    if (ok) printf("spirv_msl: interpolation qualifier %s is retained in common IR and rejected by the smooth-only graphics emitter: ok\n", name);
    else fprintf(stderr, "spirv_msl: interpolation qualifier %s was lost or incorrectly accepted\n", name);
    return ok;
}

static int make_image_copy_shader(uint32_t *words, size_t capacity, size_t *word_count,
                                  int sampled_fetch, uint32_t source_binding, uint32_t destination_binding) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 47;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                      // Shader capability
    OP(14, 3, 0, 1);                                   // Logical GLSL450 memory model
    OP(15, 7, 5, 30, 0x6e69616du, 0, 20, 21);          // compute main with both images
    OP(16, 6, 30, 17, 1, 1, 1);                        // LocalSize 1x1x1
    OP(71, 4, 20, 34, 0);                              // source DescriptorSet 0
    OP(71, 4, 20, 33, source_binding);                 // source Binding
    if (!sampled_fetch) OP(71, 3, 20, 24);              // storage source NonWritable
    OP(71, 4, 21, 34, 0);                              // destination DescriptorSet 0
    OP(71, 4, 21, 33, destination_binding);            // destination Binding
    OP(71, 3, 21, 25);                                 // destination NonReadable
    OP(19, 2, 1);                                      // void
    OP(22, 3, 2, 32);                                 // float
    OP(21, 4, 3, 32, 1);                              // signed int
    OP(23, 4, 4, 3, 2);                               // int2 coordinates
    OP(23, 4, 5, 2, 4);                               // float4 texel
    OP(25, 9, 6, 2, 1, 0, 0, 0, sampled_fetch ? 1 : 2, sampled_fetch ? 0 : 4);
                                                       // 2D sampled or storage RGBA8 image
    if (sampled_fetch) OP(25, 9, 22, 2, 1, 0, 0, 0, 2, 4); // storage output image
    OP(32, 4, 7, 0, 6);                               // UniformConstant image pointer
    if (sampled_fetch) OP(32, 4, 23, 0, 22);         // storage output image pointer
    OP(33, 3, 8, 1);                                  // void()
    OP(43, 4, 3, 9, 0);                               // int zero
    OP(59, 4, 7, 20, 0);                              // source image
    OP(59, 4, sampled_fetch ? 23 : 7, 21, 0);         // destination image
    OP(54, 5, 1, 30, 0, 8);                           // main
    OP(248, 2, 31);                                   // entry block
    OP(57, 4, 1, 32, 40);                             // call helper
    OP(253, 1);                                       // return
    OP(56, 1);                                        // function end
    OP(54, 5, 1, 40, 0, 8);                           // helper
    OP(248, 2, 41);                                   // helper entry block
    OP(249, 2, 42);                                   // branch to image operations
    OP(248, 2, 42);                                   // second helper block
    OP(80, 5, 4, 43, 9, 9);                           // coordinate (0, 0)
    OP(61, 4, 6, 44, 20);                             // load source image
    if (sampled_fetch) OP(95, 7, 5, 45, 44, 43, 2, 9); // OpImageFetch at Lod 0
    else OP(98, 5, 5, 45, 44, 43);                   // OpImageRead
    OP(61, 4, sampled_fetch ? 22 : 6, 46, 21);        // load destination image
    OP(99, 4, 46, 43, 45);                            // OpImageWrite
    OP(253, 1);                                       // return
    OP(56, 1);                                        // function end
#undef OP
    *word_count = count;
    return 1;
}

static int make_image_size_query_shader(uint32_t *words, size_t capacity, size_t *word_count,
                                        uint32_t query_opcode, int query_storage_image) {
    if (!words || !word_count || capacity < 5 ||
        (query_opcode != 103 && query_opcode != 104 && query_opcode != 106) ||
        (query_storage_image && query_opcode != 106)) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 40;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                      // Shader capability
    OP(17, 2, 50);                                     // ImageQuery capability
    OP(14, 3, 0, 1);                                   // Logical GLSL450 memory model
    OP(15, 8, 5, 30, 0x6e69616du, 0, 20, 21, 27);     // compute main with images and GlobalInvocationId
    OP(16, 6, 30, 17, 1, 1, 1);                        // LocalSize 1x1x1
    OP(71, 4, 20, 34, 0);                              // source DescriptorSet 0
    OP(71, 4, 20, 33, 0);                              // source Binding 0
    OP(71, 4, 21, 34, 0);                              // destination DescriptorSet 0
    OP(71, 4, 21, 33, 1);                              // destination Binding 1
    OP(71, 3, 21, 25);                                 // destination NonReadable
    OP(71, 4, 27, 11, 28);                             // BuiltIn GlobalInvocationId
    OP(19, 2, 1);                                      // void
    OP(22, 3, 2, 32);                                  // float
    OP(21, 4, 3, 32, 1);                               // signed int
    OP(21, 4, 24, 32, 0);                              // unsigned int
    OP(23, 4, 4, 3, 2);                                // int2 coordinates and query result
    OP(23, 4, 5, 2, 4);                                // float4 texel
    OP(23, 4, 25, 24, 3);                              // uint3 GlobalInvocationId
    OP(25, 9, 6, 2, 1, 0, 0, 0, 1, 0);                // sampled 2D float image
    OP(25, 9, 22, 2, 1, 0, 0, 0, 2, 4);              // storage 2D RGBA8 image
    OP(32, 4, 7, 0, 6);                               // source UniformConstant image pointer
    OP(32, 4, 23, 0, 22);                             // destination UniformConstant image pointer
    OP(32, 4, 26, 1, 25);                             // Input uint3 pointer
    OP(33, 3, 8, 1);                                  // void()
    OP(43, 4, 3, 9, 0);                               // integer zero (base mip)
    OP(43, 4, 3, 10, 1);                             // integer one
    OP(59, 4, 7, 20, 0);                             // source image
    OP(59, 4, 23, 21, 0);                            // destination image
    OP(59, 4, 26, 27, 1);                            // GlobalInvocationId input
    OP(54, 5, 1, 30, 0, 8);                         // main
    OP(248, 2, 31);                                  // entry block
    OP(61, 4, 6, 32, 20);                           // load source image
    OP(61, 4, 22, 36, 21);                          // load destination image
    OP(61, 4, 25, 39, 27);                           // load GlobalInvocationId
    OP(81, 5, 24, 38, 39, 0);                        // dynamic LOD from invocation x
    if (query_opcode == 103) OP(103, 5, 4, 33, 32, 38); // OpImageQuerySizeLod with dynamic Lod
    else if (query_opcode == 106) OP(106, 4, 3, 33, query_storage_image ? 36 : 32); // OpImageQueryLevels
    else OP(104, 4, 4, 33, 32);                         // OpImageQuerySize
    if (query_opcode == 106) OP(80, 5, 4, 37, 33, 33); // duplicate the single mip count as XY
    else OP(80, 5, 4, 37, 10, 10);                    // int2(1, 1)
    if (query_opcode == 106) OP(83, 4, 4, 34, 37);  // mip count 1 selects the bottom-right texel
    else OP(130, 5, 4, 34, 33, 37);                 // bottom-right coordinate = size - 1
    OP(95, 7, 5, 35, 32, 34, 2, 9);                // OpImageFetch at Lod 0
    OP(99, 4, 36, 34, 35);                          // write only the bottom-right texel
    OP(253, 1);                                     // return
    OP(56, 1);                                      // function end
#undef OP
    *word_count = count;
    return 1;
}

static int make_image_query_samples_shader(uint32_t *words, size_t capacity, size_t *word_count,
                                           uint32_t dimension, uint32_t multisampled, uint32_t arrayed) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 19;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                      // Shader capability
    OP(17, 2, 50);                                     // ImageQuery capability
    OP(14, 3, 0, 1);                                   // Logical GLSL450 memory model
    OP(15, 7, 5, 13, 0x6e69616du, 0, 7, 18);           // compute main with image and output buffer
    OP(16, 6, 13, 17, 1, 1, 1);                        // LocalSize 1x1x1
    OP(71, 4, 7, 34, 0);                               // DescriptorSet 0
    OP(71, 4, 7, 33, 0);                               // Binding 0
    OP(71, 4, 8, 6, 4);                                // output runtime-array stride
    OP(72, 5, 9, 0, 35, 0);                           // output block member offset
    OP(71, 3, 9, 2);                                   // output block
    OP(71, 4, 18, 34, 0);                             // output DescriptorSet 0
    OP(71, 4, 18, 33, 1);                             // output Binding 1
    OP(19, 2, 1);                                      // void
    OP(22, 3, 2, 32);                                  // float
    OP(21, 4, 3, 32, 0);                              // uint32
    OP(25, 9, 4, 2, dimension, 0, arrayed, multisampled, 1, 0); // sampled image type
    OP(32, 4, 5, 0, 4);                               // UniformConstant image pointer
    OP(33, 3, 6, 1);                                  // void()
    OP(29, 3, 8, 3);                                  // output runtime array of uint
    OP(30, 3, 9, 8);                                   // output storage-buffer block
    OP(32, 4, 10, 12, 9);                             // StorageBuffer pointer to block
    OP(32, 4, 11, 12, 3);                             // StorageBuffer pointer to uint
    OP(43, 4, 3, 12, 0);                              // uint 0
    OP(59, 4, 5, 7, 0);                               // sampled image
    OP(59, 4, 10, 18, 12);                           // output buffer
    OP(54, 5, 1, 13, 0, 6);                          // main
    OP(248, 2, 14);                                  // entry block
    OP(61, 4, 4, 15, 7);                             // load multisampled image
    OP(107, 4, 3, 16, 15);                           // OpImageQuerySamples
    OP(65, 6, 11, 17, 18, 12, 12);                   // output[0]
    OP(62, 3, 17, 16);                               // store sample count
    OP(253, 1);                                      // return
    OP(56, 1);                                       // function end
#undef OP
    *word_count = count;
    return 1;
}

static int check_image_query_samples_translation(int arrayed) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!make_image_query_samples_shader(words, sizeof words / sizeof words[0], &word_count, 1, 1,
                                         arrayed ? 1u : 0u)) {
        fprintf(stderr, "spirv_msl: could not construct OpImageQuerySamples fixture\n");
        return 0;
    }
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    char *source = NULL;
    size_t source_size = 0;
    int ok = 0;
    id<MTLDevice> metal_device = nil;
    NSString *metal_source = nil;
    NSError *metal_error = nil;
    id<MTLLibrary> library = nil;
    id<MTLFunction> function = nil;
    id<MTLComputePipelineState> pipeline = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLTexture> texture = nil;
    id<MTLBuffer> output = nil;
    id<MTLBuffer> ranges = nil;
    MTLTextureDescriptor *texture_desc = nil;
    id<MTLCommandBuffer> command = nil;
    id<MTLComputeCommandEncoder> encoder = nil;
    int rc = spirv_read(words, word_count * sizeof words[0], &module);
    int read_rc = rc;
    const spirv_entry_point *entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    if (!entry && rc == SPIRV_OK) rc = GPU_IR_INVALID;
    if (entry && rc == SPIRV_OK) rc = spirv_build_gpu_ir_shader(&module, entry, &shader);
    int shader_rc = rc;
    if (rc == GPU_IR_OK) rc = spirv_build_gpu_ir_layout(&module, entry, &layout);
    int layout_rc = rc;
    if (rc == GPU_IR_OK) rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &source, &source_size);
    int msl_rc = rc;
    const char *texture_parameter = arrayed ?
        "texture2d_ms_array<float, access::read> vx_image_7 [[texture(0)]]" :
        "texture2d_ms<float, access::read> vx_image_7 [[texture(0)]]";
    if (rc != GPU_IR_OK || !source || !source_size ||
        !strstr(source, texture_parameter) ||
        !strstr(source, "uint vx_value_16 = uint(vx_image_7.get_num_samples());")) goto done;
    metal_device = MTLCreateSystemDefaultDevice();
    metal_source = [[NSString alloc] initWithBytes:source length:source_size encoding:NSUTF8StringEncoding];
    library = metal_device && metal_source ?
        [metal_device newLibraryWithSource:metal_source options:nil error:&metal_error] : nil;
    function = [library newFunctionWithName:@"vx_main"];
    pipeline = metal_device && function ?
        [metal_device newComputePipelineStateWithFunction:function error:&metal_error] : nil;
    queue = [metal_device newCommandQueue];
    texture_desc = [[MTLTextureDescriptor alloc] init];
    texture_desc.textureType = arrayed ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample;
    texture_desc.pixelFormat = MTLPixelFormatR32Float;
    texture_desc.width = texture_desc.height = 1;
    texture_desc.arrayLength = arrayed ? 2 : 1;
    texture_desc.sampleCount = 4;
    texture_desc.storageMode = MTLStorageModePrivate;
    texture_desc.usage = MTLTextureUsageShaderRead;
    texture = metal_device && [metal_device supportsTextureSampleCount:4] ?
        [metal_device newTextureWithDescriptor:texture_desc] : nil;
    output = [metal_device newBufferWithLength:sizeof(uint32_t) options:MTLResourceStorageModeShared];
    ranges = [metal_device newBufferWithLength:sizeof(uint64_t) options:MTLResourceStorageModeShared];
    if (ranges) *(uint64_t *)ranges.contents = sizeof(uint32_t);
    command = [queue commandBuffer];
    encoder = [command computeCommandEncoder];
    if (pipeline && texture && output && ranges && command && encoder) {
        [encoder setComputePipelineState:pipeline];
        [encoder setTexture:texture atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:0];
        [encoder setBuffer:ranges offset:0 atIndex:30];
        [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        ok = command.status == MTLCommandBufferStatusCompleted &&
            *(const uint32_t *)output.contents == 4;
    }
    if (!ok) fprintf(stderr, "spirv_msl: Metal OpImageQuerySamples runtime (arrayed=%d) got %u, want 4\n", arrayed,
                     output ? *(const uint32_t *)output.contents : 0);
done:
    if (!ok) fprintf(stderr, "spirv_msl: OpImageQuerySamples translation failed "
                     "(arrayed=%d read=%d shader=%d layout=%d msl=%d, %s)\n%s",
                     arrayed,
                     read_rc, shader_rc, layout_rc, msl_rc, module.error, source ? source : "");
    free(source);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int rejects_non_multisampled_image_query_samples(void) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!make_image_query_samples_shader(words, sizeof words / sizeof words[0], &word_count, 1, 0, 0)) return 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    int rc = spirv_read(words, word_count * sizeof words[0], &module);
    const spirv_entry_point *entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    if (entry && rc == SPIRV_OK) rc = spirv_build_gpu_ir_shader(&module, entry, &shader);
    int rejected = rc == GPU_IR_UNSUPPORTED;
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return rejected;
}

static int set_image_query_lod(uint32_t *words, size_t word_count, uint32_t lod_id) {
    for (size_t cursor = 5; cursor < word_count;) {
        uint16_t count = (uint16_t)(words[cursor] >> 16);
        uint16_t opcode = (uint16_t)words[cursor];
        if (!count || count > word_count - cursor) return 0;
        if (opcode == 103 && count == 5) {
            words[cursor + 4] = lod_id;
            return 1;
        }
        cursor += count;
    }
    return 0;
}

static int make_sampler_sample_shader(uint32_t *words, size_t capacity, size_t *word_count,
                                     int separate_sampler) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 40;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                      // Shader capability
    OP(14, 3, 0, 1);                                   // Logical GLSL450 memory model
    if (separate_sampler) OP(15, 8, 5, 30, 0x6e69616du, 0, 20, 24, 21);
    else OP(15, 7, 5, 30, 0x6e69616du, 0, 20, 21);     // compute main with the descriptors
    OP(16, 6, 30, 17, 1, 1, 1);                        // LocalSize 1x1x1
    OP(71, 4, 20, 34, 0);                              // source DescriptorSet 0
    OP(71, 4, 20, 33, 0);                              // source Binding 0
    OP(71, 4, 21, 34, 0);                              // destination DescriptorSet 0
    OP(71, 4, 21, 33, separate_sampler ? 2 : 1);        // destination Binding
    if (separate_sampler) {
        OP(71, 4, 24, 34, 0);                          // sampler DescriptorSet 0
        OP(71, 4, 24, 33, 1);                          // sampler Binding 1
    }
    OP(71, 3, 21, 25);                                 // destination NonReadable
    OP(19, 2, 1);                                      // void
    OP(22, 3, 2, 32);                                 // float
    OP(21, 4, 3, 32, 1);                              // signed int
    OP(23, 4, 4, 2, 2);                               // float2 coordinates
    OP(23, 4, 5, 2, 4);                               // float4 texel
    OP(23, 4, 14, 3, 2);                              // int2 output coordinates
    OP(25, 9, 6, 2, 1, 0, 0, 0, 1, 0);                // sampled 2D float image
    OP(25, 9, 22, 2, 1, 0, 0, 0, 2, 4);              // storage 2D RGBA8 output
    OP(27, 3, 7, 6);                                  // sampled image type
    if (separate_sampler) OP(26, 2, 25);              // sampler type
    OP(32, 4, 8, 0, separate_sampler ? 6 : 7);        // UniformConstant image/sampled-image pointer
    if (separate_sampler) OP(32, 4, 26, 0, 25);       // UniformConstant sampler pointer
    OP(32, 4, 23, 0, 22);                             // UniformConstant storage-image pointer
    OP(33, 3, 9, 1);                                  // void()
    OP(43, 4, 2, 10, 0x3f000000u);                    // 0.5 float
    OP(43, 4, 2, 11, 0);                              // LOD 0.0
    OP(43, 4, 3, 13, 0);                              // integer zero
    OP(59, 4, 8, 20, 0);                              // combined image sampler
    if (separate_sampler) OP(59, 4, 26, 24, 0);       // separate sampler
    OP(59, 4, 23, 21, 0);                             // output image
    OP(54, 5, 1, 30, 0, 9);                           // main
    OP(248, 2, 31);                                   // entry block
    OP(80, 5, 4, 35, 10, 10);                         // normalized sample coordinate (0.5, 0.5)
    OP(80, 5, 14, 36, 13, 13);                        // output coordinate (0, 0)
    if (separate_sampler) {
        OP(61, 4, 6, 32, 20);                         // load separate image
        OP(61, 4, 25, 37, 24);                        // load separate sampler
        OP(86, 5, 7, 38, 32, 37);                     // OpSampledImage
        OP(88, 7, 5, 33, 38, 35, 2, 11);              // explicit LOD sample at zero
    } else {
        OP(61, 4, 7, 32, 20);                         // load combined sampled image
        OP(88, 7, 5, 33, 32, 35, 2, 11);              // explicit LOD sample at zero
    }
    OP(61, 4, 22, 34, 21);                            // load output image
    OP(99, 4, 34, 36, 33);                            // write sampled texel
    OP(253, 1);                                       // return
    OP(56, 1);                                        // function end
#undef OP
    *word_count = count;
    return 1;
}

static int make_combined_image_array_shader(uint32_t *words, size_t capacity, size_t *word_count) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 40;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                      // Shader capability
    OP(14, 3, 0, 1);                                   // Logical GLSL450 memory model
    OP(15, 7, 5, 32, 0x6e69616du, 0, 20, 21);          // compute main with descriptor array and output
    OP(16, 6, 32, 17, 1, 1, 1);                        // LocalSize 1x1x1
    OP(71, 4, 20, 34, 0);                              // sampled array DescriptorSet 0
    OP(71, 4, 20, 33, 0);                              // sampled array Binding 0
    OP(71, 4, 21, 34, 0);                              // output DescriptorSet 0
    OP(71, 4, 21, 33, 1);                              // output Binding 1
    OP(71, 3, 21, 25);                                 // output NonReadable
    OP(19, 2, 1);                                      // void
    OP(22, 3, 2, 32);                                 // float
    OP(21, 4, 3, 32, 1);                              // signed int
    OP(23, 4, 4, 2, 2);                               // float2
    OP(23, 4, 5, 2, 4);                               // float4
    OP(23, 4, 14, 3, 2);                              // int2
    OP(25, 9, 6, 2, 1, 0, 0, 0, 1, 0);                // sampled 2D float image
    OP(25, 9, 8, 2, 1, 0, 0, 0, 2, 4);              // storage 2D RGBA8 output image
    OP(27, 3, 7, 6);                                  // combined image sampler type
    OP(43, 4, 3, 15, 2);                              // descriptor-array length
    OP(28, 4, 12, 7, 15);                             // array of two combined image samplers
    OP(32, 4, 16, 0, 12);                             // pointer to combined image array
    OP(32, 4, 17, 0, 7);                              // pointer to combined image sampler
    OP(32, 4, 18, 0, 8);                              // pointer to output image
    OP(33, 3, 9, 1);                                 // void()
    OP(43, 4, 2, 10, 0x3f000000u);                    // 0.5 float
    OP(43, 4, 2, 11, 0);                             // LOD 0.0
    OP(43, 4, 3, 13, 0);                             // signed integer zero
    OP(43, 4, 3, 23, 1);                             // descriptor array index one
    OP(59, 4, 16, 20, 0);                            // sampled-image descriptor array
    OP(59, 4, 18, 21, 0);                            // output image
    OP(54, 5, 1, 32, 0, 9);                          // main
    OP(248, 2, 33);                                  // entry block
    OP(80, 5, 4, 30, 10, 10);                        // sample coordinate (0.5, 0.5)
    OP(80, 5, 14, 31, 13, 13);                       // output coordinate (0, 0)
    OP(65, 5, 17, 34, 20, 23);                       // descriptor[1]
    OP(61, 4, 7, 35, 34);                           // load combined image sampler
    OP(88, 7, 5, 36, 35, 30, 2, 11);                // sample descriptor[1] at explicit LOD 0
    OP(61, 4, 8, 37, 21);                           // load output image
    OP(99, 4, 37, 31, 36);                          // store the sampled value
    OP(253, 1);                                      // return
    OP(56, 1);                                       // function end
#undef OP
    *word_count = count;
    return 1;
}

static int make_separate_image_sampler_array_shader(uint32_t *words, size_t capacity, size_t *word_count) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 41;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                      // Shader capability
    OP(14, 3, 0, 1);                                   // Logical GLSL450 memory model
    OP(15, 8, 5, 32, 0x6e69616du, 0, 20, 21, 22);      // compute main with image/sampler arrays and output
    OP(16, 6, 32, 17, 1, 1, 1);                        // LocalSize 1x1x1
    OP(71, 4, 20, 34, 0);                              // sampled image array DescriptorSet 0
    OP(71, 4, 20, 33, 0);                              // sampled image array Binding 0
    OP(71, 4, 22, 34, 0);                              // sampler array DescriptorSet 0
    OP(71, 4, 22, 33, 1);                              // sampler array Binding 1
    OP(71, 4, 21, 34, 0);                              // output DescriptorSet 0
    OP(71, 4, 21, 33, 2);                              // output Binding 2
    OP(71, 3, 21, 25);                                 // output NonReadable
    OP(19, 2, 1);                                      // void
    OP(22, 3, 2, 32);                                 // float
    OP(21, 4, 3, 32, 1);                              // signed int
    OP(23, 4, 4, 2, 2);                               // float2
    OP(23, 4, 5, 2, 4);                               // float4
    OP(23, 4, 14, 3, 2);                              // int2
    OP(25, 9, 6, 2, 1, 0, 0, 0, 1, 0);                // sampled 2D float image
    OP(25, 9, 8, 2, 1, 0, 0, 0, 2, 4);              // storage 2D RGBA8 output image
    OP(27, 3, 7, 6);                                  // sampled image type
    OP(43, 4, 3, 15, 2);                              // descriptor-array length
    OP(28, 4, 12, 6, 15);                             // array of two sampled images
    OP(26, 2, 25);                                    // sampler type
    OP(28, 4, 26, 25, 15);                            // array of two samplers
    OP(32, 4, 16, 0, 12);                             // pointer to sampled-image array
    OP(32, 4, 17, 0, 6);                              // pointer to sampled image
    OP(32, 4, 18, 0, 8);                              // pointer to output image
    OP(32, 4, 27, 0, 26);                             // pointer to sampler array
    OP(32, 4, 28, 0, 25);                             // pointer to sampler
    OP(33, 3, 9, 1);                                 // void()
    OP(43, 4, 2, 10, 0x3f000000u);                    // 0.5 float
    OP(43, 4, 2, 11, 0);                             // LOD 0.0
    OP(43, 4, 3, 13, 0);                             // signed integer zero
    OP(43, 4, 3, 23, 1);                             // descriptor array index one
    OP(59, 4, 16, 20, 0);                            // sampled-image descriptor array
    OP(59, 4, 18, 21, 0);                            // output image
    OP(59, 4, 27, 22, 0);                            // sampler descriptor array
    OP(54, 5, 1, 32, 0, 9);                          // main
    OP(248, 2, 33);                                  // entry block
    OP(80, 5, 4, 30, 10, 10);                        // sample coordinate (0.5, 0.5)
    OP(80, 5, 14, 31, 13, 13);                       // output coordinate (0, 0)
    OP(65, 5, 17, 34, 20, 23);                       // sampled image descriptor[1]
    OP(61, 4, 6, 35, 34);                           // load sampled image
    OP(65, 5, 28, 36, 22, 23);                       // sampler descriptor[1]
    OP(61, 4, 25, 37, 36);                         // load sampler
    OP(86, 5, 7, 38, 35, 37);                       // OpSampledImage
    OP(88, 7, 5, 39, 38, 30, 2, 11);                // explicit LOD sample at zero
    OP(61, 4, 8, 40, 21);                           // load output image
    OP(99, 4, 40, 31, 39);                          // write sampled texel
    OP(253, 1);                                      // return
    OP(56, 1);                                       // function end
#undef OP
    *word_count = count;
    return 1;
}

static int make_arithmetic_shader(uint32_t *words, size_t capacity, size_t *word_count,
                                 uint32_t array_length_member) {
    if (capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 420;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                      // OpCapability Shader
    OP(17, 2, 6025);                                    // OpCapability BitInstructions
    OP(14, 3, 0, 1);                                   // OpMemoryModel Logical GLSL450
    OP(11, 6, 200, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0); // GLSL.std.450
    OP(15, 13, 5, 10, 0x6e69616du, 0, 9, 11, 104, 105, 106, 122, 123, 155); // compute interface includes shared array
    OP(331, 6, 10, 38, 45, 14, 14);                    // LocalSizeId, x is a specialization constant
    OP(71, 3, 5, 2);                                    // Block
    OP(71, 4, 4, 6, 4);                                 // ArrayStride 4
    OP(72, 5, 5, 0, 35, 0);                            // member 0 Offset 0
    OP(71, 4, 9, 11, 28);                               // BuiltIn GlobalInvocationId
    OP(71, 4, 104, 11, 27);                             // BuiltIn LocalInvocationId
    OP(71, 4, 105, 11, 26);                             // BuiltIn WorkgroupId
    OP(71, 4, 106, 11, 29);                             // BuiltIn LocalInvocationIndex
    OP(71, 4, 122, 11, 24);                             // BuiltIn NumWorkgroups
    OP(71, 4, 123, 11, 25);                             // BuiltIn WorkgroupSize
    OP(71, 4, 11, 34, 0);                               // DescriptorSet 0
    OP(71, 4, 11, 33, 0);                               // Binding 0
    OP(71, 4, 45, 1, 0);                                // SpecId 0
    OP(19, 2, 1);                                       // void
    OP(21, 4, 2, 32, 0);                               // uint
    OP(23, 4, 3, 2, 3);                                 // uint3
    OP(29, 3, 4, 2);                                    // runtime array of uint
    OP(30, 3, 5, 4);                                    // block struct
    OP(21, 4, 24, 32, 1);                              // int
    OP(22, 3, 30, 32);                                 // float
    OP(20, 2, 46);                                     // bool
    OP(23, 4, 90, 46, 3);                              // bool3
    OP(23, 4, 160, 24, 3);                             // int3
    OP(23, 4, 36, 30, 4);                              // float4
    OP(23, 4, 138, 2, 4);                             // uint4
    OP(32, 4, 6, 12, 5);                               // StorageBuffer pointer to block
    OP(32, 4, 7, 1, 3);                                // Input pointer to uint3
    OP(32, 4, 8, 12, 2);                               // StorageBuffer pointer to uint
    OP(32, 4, 103, 1, 2);                              // Input pointer to uint
    OP(32, 4, 142, 7, 2);                              // Function pointer to uint
    OP(32, 4, 152, 4, 2);                              // Workgroup pointer to uint
    OP(33, 3, 12, 1);                                  // void()
    OP(43, 4, 2, 13, 0);                               // uint 0
    OP(43, 4, 2, 14, 1);                               // uint 1
    OP(43, 4, 2, 21, 2);                               // uint 2
    OP(43, 4, 2, 47, 7);                               // uint 7
    OP(43, 4, 2, 48, 999);                             // selected value
    OP(43, 4, 2, 72, 3);                               // uint 3
    OP(43, 4, 2, 157, 32);                             // uint 32: shared array length
    OP(43, 4, 2, 73, 10);                              // uint 10
    OP(43, 4, 2, 74, 100);                             // uint 100
    OP(43, 4, 2, 75, 1000);                            // uint 1000
    OP(43, 4, 2, 88, 64);                              // second output region
    OP(43, 4, 2, 127, 0x108);                          // AcquireRelease | WorkgroupMemory
    OP(43, 4, 2, 129, 3);                              // Subgroup scope
    OP(43, 4, 2, 130, 0x48);                           // AcquireRelease | UniformMemory
    OP(43, 4, 2, 131, 0x848);                          // AcquireRelease | UniformMemory | ImageMemory
    OP(41, 3, 46, 92);                                 // true
    OP(42, 3, 46, 93);                                 // false
    OP(43, 4, 30, 51, 0x7fc00000u);                    // quiet NaN
    OP(43, 4, 24, 25, 0xfffffff9u);                    // int -7
    OP(43, 4, 24, 26, 3);                              // int 3
    OP(43, 4, 24, 161, 7);                             // int 7
    OP(43, 4, 24, 162, 0xfffffffdu);                   // int -3
    OP(43, 4, 2, 300, 0x000000f3u);                   // uint bits for field extraction
    OP(43, 4, 2, 301, 4);                             // uint field width
    OP(43, 4, 2, 302, 0x000000a5u);                   // uint field insertion base
    OP(43, 4, 2, 303, 3);                             // uint field insertion value
    OP(43, 4, 24, 350, 0);                            // int zero for bit-scan edge cases
    OP(43, 4, 24, 351, 0xffffffffu);                  // int -1 for signed MSB edge case
    OP(43, 4, 24, 352, 8);                            // int 8 for least-significant bit index 3
    OP(43, 4, 30, 31, 0x3f800000u);                    // float 1.0
    OP(43, 4, 30, 32, 0x40000000u);                    // float 2.0
    OP(43, 4, 30, 140, 0x3fe00000u);                   // float 1.75
    OP(43, 4, 30, 141, 0xc0300000u);                   // float -2.75
    OP(43, 4, 30, 180, 0xbf800000u);                   // float -1.0
    OP(43, 4, 30, 254, 0x00000000u);                   // float 0.0
    OP(43, 4, 30, 255, 0x40200000u);                   // float 2.5
    OP(43, 4, 30, 390, 0x3f000000u);                   // float 0.5 for FMix
    OP(50, 4, 2, 45, 32);                              // LocalSizeId x default 32
    OP(28, 4, 146, 2, 72);                             // array of three uints
    OP(32, 4, 147, 7, 146);                            // Function pointer to local uint array
    OP(32, 4, 149, 7, 2);                              // Function pointer to local array element
    OP(28, 4, 153, 2, 157);                            // Workgroup array of one uint per invocation
    OP(32, 4, 154, 4, 153);                            // Workgroup pointer to shared array
    OP(59, 4, 7, 9, 1);                                // GlobalInvocationId
    OP(59, 4, 6, 11, 12);                              // storage buffer
    OP(59, 4, 7, 104, 1);                              // LocalInvocationId
    OP(59, 4, 7, 105, 1);                              // WorkgroupId
    OP(59, 4, 103, 106, 1);                            // LocalInvocationIndex
    OP(59, 4, 7, 122, 1);                              // NumWorkgroups
    OP(59, 4, 7, 123, 1);                              // WorkgroupSize
    OP(59, 4, 154, 155, 4);                            // module-scope Workgroup shared array
    OP(54, 5, 1, 10, 0, 12);                           // main
    OP(248, 2, 20);                                     // entry block
    OP(59, 5, 142, 143, 7, 14);                        // function-local uint initialized to 1
    OP(59, 4, 147, 148, 7);                            // function-local uint array
    OP(61, 4, 3, 15, 9);                               // load GlobalInvocationId
    OP(81, 5, 2, 16, 15, 0);                           // extract x
    OP(65, 6, 8, 17, 11, 13, 16);                      // buffer[gid.x]
    OP(61, 4, 2, 18, 17);                              // load buffer element
    OP(80, 6, 3, 39, 21, 21, 21);                     // construct uint3(2,2,2)
    OP(132, 5, 3, 40, 15, 39);                         // uint3 gid * 2
    OP(81, 5, 2, 41, 40, 0);                          // extract doubled x
    OP(128, 5, 2, 42, 41, 14);                        // add 1
    OP(170, 5, 46, 49, 16, 47);                       // gid.x == 7
    OP(182, 5, 46, 52, 51, 31);                       // ordered NaN != 1 is false
    OP(183, 5, 46, 54, 51, 31);                       // unordered NaN != 1 is true
    OP(169, 6, 2, 50, 54, 48, 42);                    // unordered comparison selects 999
    OP(169, 6, 2, 55, 52, 47, 50);                    // ordered comparison would select 7
    OP(169, 6, 2, 56, 49, 55, 42);                    // only lane 7 uses the comparison results
    OP(62, 3, 143, 56);                                 // store through a function-local pointer
    OP(61, 4, 2, 144, 143);                            // reload function-local value
    OP(62, 3, 17, 144);                                 // store reloaded value
    OP(65, 5, 149, 150, 148, 21);                      // local_array[2]
    OP(62, 3, 150, 56);                                 // store through a local array access chain
    OP(61, 4, 2, 151, 150);                            // reload local array element
    OP(62, 3, 17, 151);                                 // store reloaded array value
    OP(61, 4, 2, 158, 106);                            // load LocalInvocationIndex
    OP(65, 5, 152, 156, 155, 158);                     // shared_array[LocalInvocationIndex]
    OP(62, 3, 156, 56);                                 // each invocation writes its own slot
    OP(224, 4, 21, 21, 127);                           // synchronize shared-memory writes
    OP(61, 4, 2, 159, 156);                            // read after the workgroup barrier
    OP(62, 3, 17, 159);                                 // write shared value back to the output
    OP(128, 5, 24, 27, 25, 26);                         // signed add
    OP(130, 5, 24, 28, 25, 26);                         // signed subtract
    OP(132, 5, 24, 29, 25, 26);                         // signed multiply
    OP(129, 5, 30, 33, 31, 32);                         // float add
    OP(131, 5, 30, 34, 31, 32);                         // float subtract
    OP(133, 5, 30, 35, 31, 32);                         // float multiply
    OP(80, 7, 36, 37, 31, 32, 31, 32);               // construct float4
    OP(80, 7, 36, 181, 140, 141, 140, 141);           // float4(1.75, -2.75, 1.75, -2.75)
    OP(80, 7, 36, 182, 31, 31, 180, 180);             // float4(1, 1, -1, -1)
    OP(141, 5, 36, 183, 181, 182);                    // vector FMod matches the divisor sign
    OP(81, 5, 30, 184, 183, 0);                      // first component is 0.75
    OP(133, 5, 30, 185, 184, 32);                    // scale by two
    OP(133, 5, 30, 186, 185, 32);                    // scale by two again: 3.0
    OP(109, 4, 2, 187, 186);                          // float to uint for checked output
    OP(129, 5, 36, 43, 37, 37);                       // float4 add
    OP(81, 5, 30, 44, 43, 3);                          // extract .w from float4
    OP(142, 5, 36, 173, 37, 32);                      // float4 times scalar
    OP(81, 5, 30, 174, 173, 3);                       // scaled .w is 4.0
    OP(109, 4, 2, 175, 174);                          // float to uint for checked output
    OP(148, 5, 30, 177, 37, 37);                      // dot(float4(1, 2, 1, 2), itself) = 10
    OP(109, 4, 2, 178, 177);                          // dot result to uint for checked output
    OP(135, 5, 24, 57, 25, 26);                       // signed divide
    OP(138, 5, 24, 58, 25, 26);                       // signed remainder
    OP(139, 5, 24, 59, 25, 26);                       // signed modulo
    OP(80, 6, 160, 163, 25, 161, 25);                 // int3(-7, 7, -7)
    OP(80, 6, 160, 164, 26, 162, 162);                 // int3(3, -3, -3)
    OP(139, 5, 160, 165, 163, 164);                   // component-wise int3 signed modulo
    OP(81, 5, 24, 166, 165, 0);                       // SMod lane 0: 2
    OP(81, 5, 24, 167, 165, 1);                       // SMod lane 1: -2
    OP(81, 5, 24, 168, 165, 2);                       // SMod lane 2: -1
    OP(128, 5, 24, 169, 166, 167);                   // sum is 0
    OP(128, 5, 24, 170, 169, 168);                   // sum is -1
    OP(124, 4, 2, 171, 170);                          // preserve -1 bits as uint
    OP(200, 4, 2, 60, 47);                            // bitwise not
    OP(199, 5, 2, 61, 47, 14);                        // bitwise and
    OP(194, 5, 2, 62, 47, 14);                        // logical right shift
    OP(196, 5, 2, 63, 47, 14);                        // left shift
    OP(195, 5, 24, 64, 25, 26);                       // arithmetic right shift
    OP(136, 5, 30, 65, 31, 32);                       // float divide
    OP(140, 5, 30, 66, 31, 32);                       // float remainder
    OP(141, 5, 30, 67, 31, 32);                       // float modulo
    OP(126, 4, 24, 68, 25);                           // signed negate
    OP(127, 4, 30, 69, 31);                           // float negate
    OP(80, 6, 90, 94, 92, 93, 92);                    // bool3(true, false, true)
    OP(169, 6, 3, 260, 94, 40, 39);                    // component-wise uint3 select
    OP(81, 5, 2, 261, 260, 0);                         // selected x
    OP(81, 5, 2, 262, 260, 1);                         // selected y
    OP(81, 5, 2, 263, 260, 2);                         // selected z
    OP(132, 5, 2, 264, 261, 74);                      // weight x by 100
    OP(132, 5, 2, 265, 262, 73);                      // weight y by 10
    OP(128, 5, 2, 266, 264, 265);                     // combine weighted x and y
    OP(128, 5, 2, 267, 266, 263);                     // combine selected vector lanes
    OP(137, 5, 2, 270, 16, 72);                       // bounded dynamic component index
    OP(77, 5, 2, 271, 40, 270);                       // OpVectorExtractDynamic
    OP(78, 6, 3, 273, 40, 72, 270);                   // OpVectorInsertDynamic
    OP(81, 5, 2, 274, 273, 0);                        // sum the inserted vector's components
    OP(81, 5, 2, 275, 273, 1);
    OP(81, 5, 2, 276, 273, 2);
    OP(128, 5, 2, 277, 274, 275);
    OP(128, 5, 2, 278, 277, 276);
    OP(82, 6, 3, 280, 72, 40, 1);                    // OpCompositeInsert into lane 1
    OP(81, 5, 2, 281, 280, 0);
    OP(81, 5, 2, 282, 280, 1);
    OP(81, 5, 2, 283, 280, 2);
    OP(128, 5, 2, 284, 281, 282);
    OP(128, 5, 2, 285, 284, 283);
    OP(154, 4, 46, 95, 94);                           // any(bool3)
    OP(155, 4, 46, 96, 94);                           // all(bool3)
    OP(168, 4, 90, 100, 94);                          // logical not
    OP(167, 5, 90, 101, 94, 94);                      // logical and
    OP(166, 5, 90, 102, 94, 94);                      // logical or
    OP(134, 5, 2, 76, 47, 72);                        // unsigned divide
    OP(137, 5, 2, 77, 47, 72);                        // unsigned modulo
    OP(199, 5, 2, 78, 47, 72);                        // unsigned bitwise and
    OP(196, 5, 2, 79, 47, 14);                        // unsigned left shift
    OP(132, 5, 2, 80, 76, 75);                        // scaled quotient
    OP(132, 5, 2, 81, 77, 74);                        // scaled modulo
    OP(132, 5, 2, 82, 78, 73);                        // scaled bitwise and
    OP(128, 5, 2, 83, 80, 81);                        // combine quotient and modulo
    OP(128, 5, 2, 84, 83, 82);                        // combine bitwise result
    OP(128, 5, 2, 85, 84, 79);                        // combine shifted result
    OP(128, 5, 2, 86, 85, 16);                        // add invocation index
    OP(61, 4, 3, 111, 104);                            // load LocalInvocationId
    OP(61, 4, 3, 112, 105);                            // load WorkgroupId
    OP(61, 4, 2, 113, 106);                            // load LocalInvocationIndex
    OP(81, 5, 2, 114, 111, 0);                         // LocalInvocationId.x
    OP(81, 5, 2, 115, 112, 0);                         // WorkgroupId.x
    OP(61, 4, 3, 119, 122);                            // load NumWorkgroups
    OP(61, 4, 3, 120, 123);                            // load WorkgroupSize
    OP(81, 5, 2, 121, 120, 0);                         // WorkgroupSize.x
    OP(132, 5, 2, 116, 115, 121);                      // WorkgroupId.x * WorkgroupSize.x
    OP(128, 5, 2, 117, 116, 114);                      // global x from local and workgroup IDs
    OP(128, 5, 2, 118, 86, 113);                        // add LocalInvocationIndex to output value
    OP(81, 5, 2, 124, 119, 0);                         // NumWorkgroups.x
    OP(128, 5, 2, 126, 118, 124);                      // include the grid's x group count
    OP(68, 5, 2, 97, 11, array_length_member);          // OpArrayLength of the bound storage-buffer range
    OP(128, 5, 2, 98, 126, 97);                        // add the descriptor-relative element count
    OP(109, 4, 2, 132, 140);                            // float to unsigned integer, truncating toward zero
    OP(110, 4, 24, 133, 141);                          // float to signed integer, truncating toward zero
    OP(111, 4, 30, 134, 133);                          // signed integer to float
    OP(112, 4, 30, 135, 132);                          // unsigned integer to float
    OP(124, 4, 2, 136, 31);                            // bitcast float bits to uint
    OP(109, 4, 138, 139, 43);                          // component-wise float4 to uint4
    OP(128, 5, 2, 137, 98, 132);                       // include the converted result in the GPU output
    OP(128, 5, 2, 172, 137, 171);                     // include the vector SMod checksum
    OP(128, 5, 2, 176, 172, 175);                     // include the vector-times-scalar result
    OP(128, 5, 2, 179, 176, 178);                     // include the vector dot product
    OP(128, 5, 2, 188, 179, 187);                     // include vector FMod component
    OP(128, 5, 2, 268, 188, 267);                     // include vector OpSelect checksum
    OP(128, 5, 2, 272, 268, 271);                     // include dynamic vector extraction
    OP(128, 5, 2, 279, 272, 278);                     // include dynamic vector insertion
    OP(128, 5, 2, 87, 117, 88);                        // second output index
    OP(65, 6, 8, 89, 11, 13, 87);                     // buffer[64 + gid.x]
    OP(62, 3, 89, 188);                                // store arithmetic result plus built-ins and vector operations
    OP(12, 6, 30, 190, 200, 4, 141);                   // FAbs(-2.75)
    OP(12, 6, 30, 191, 200, 8, 140);                   // Floor(1.75)
    OP(12, 6, 30, 192, 200, 9, 140);                   // Ceil(1.75)
    OP(12, 6, 30, 193, 200, 31, 32);                   // Sqrt(2.0)
    OP(12, 6, 30, 194, 200, 32, 32);                   // InverseSqrt(2.0)
    OP(12, 7, 30, 195, 200, 37, 140, 32);              // FMin(1.75, 2.0)
    OP(12, 7, 30, 196, 200, 40, 140, 32);              // FMax(1.75, 2.0)
    OP(12, 8, 30, 197, 200, 43, 140, 31, 32);          // FClamp(1.75, 1.0, 2.0)
    OP(12, 8, 30, 198, 200, 50, 31, 32, 140);          // Fma(1.0, 2.0, 1.75)
    OP(12, 7, 30, 199, 200, 26, 32, 31);              // Pow(2.0, 1.0)
    OP(129, 5, 30, 201, 190, 191);                    // Accumulate the extended math results
    OP(129, 5, 30, 202, 201, 192);
    OP(129, 5, 30, 203, 202, 193);
    OP(129, 5, 30, 204, 203, 194);
    OP(129, 5, 30, 205, 204, 195);
    OP(129, 5, 30, 206, 205, 196);
    OP(129, 5, 30, 207, 206, 197);
    OP(129, 5, 30, 208, 207, 198);
    OP(129, 5, 30, 209, 208, 199);
    OP(109, 4, 2, 210, 209);                          // The sum truncates to 19
    OP(128, 5, 2, 286, 279, 285);                     // include static vector insertion
    OP(128, 5, 2, 211, 286, 210);                     // Include vector operations in readback
    OP(12, 6, 30, 212, 200, 3, 140);                  // Trunc(1.75)
    OP(12, 6, 30, 213, 200, 10, 140);                 // Fract(1.75)
    OP(12, 6, 30, 214, 200, 13, 254);                 // Sin(0)
    OP(12, 6, 30, 215, 200, 14, 254);                 // Cos(0)
    OP(12, 6, 30, 216, 200, 15, 254);                 // Tan(0)
    OP(12, 6, 30, 217, 200, 16, 254);                 // Asin(0)
    OP(12, 6, 30, 218, 200, 17, 31);                  // Acos(1)
    OP(12, 6, 30, 219, 200, 18, 254);                 // Atan(0)
    OP(12, 6, 30, 220, 200, 19, 254);                 // Sinh(0)
    OP(12, 6, 30, 221, 200, 20, 254);                 // Cosh(0)
    OP(12, 6, 30, 222, 200, 21, 254);                 // Tanh(0)
    OP(12, 6, 30, 223, 200, 22, 254);                 // Asinh(0)
    OP(12, 6, 30, 224, 200, 23, 31);                  // Acosh(1)
    OP(12, 6, 30, 225, 200, 24, 254);                 // Atanh(0)
    OP(12, 6, 30, 226, 200, 27, 254);                 // Exp(0)
    OP(12, 6, 30, 227, 200, 28, 31);                  // Log(1)
    OP(12, 6, 30, 228, 200, 29, 254);                 // Exp2(0)
    OP(12, 6, 30, 229, 200, 30, 31);                  // Log2(1)
    OP(12, 6, 30, 230, 200, 1, 140);                  // Round(1.75)
    OP(12, 6, 30, 231, 200, 2, 255);                  // RoundEven(2.5)
    OP(129, 5, 30, 232, 212, 213);                     // Accumulate the additional GLSL math results
    OP(129, 5, 30, 233, 232, 214);
    OP(129, 5, 30, 234, 233, 215);
    OP(129, 5, 30, 235, 234, 216);
    OP(129, 5, 30, 236, 235, 217);
    OP(129, 5, 30, 237, 236, 218);
    OP(129, 5, 30, 238, 237, 219);
    OP(129, 5, 30, 239, 238, 220);
    OP(129, 5, 30, 240, 239, 221);
    OP(129, 5, 30, 241, 240, 222);
    OP(129, 5, 30, 242, 241, 223);
    OP(129, 5, 30, 243, 242, 224);
    OP(129, 5, 30, 244, 243, 225);
    OP(129, 5, 30, 245, 244, 226);
    OP(129, 5, 30, 246, 245, 227);
    OP(129, 5, 30, 247, 246, 228);
    OP(129, 5, 30, 248, 247, 229);
    OP(129, 5, 30, 249, 248, 230);
    OP(129, 5, 30, 250, 249, 231);
    OP(12, 8, 30, 391, 200, 46, 31, 32, 390);         // FMix(1.0, 2.0, 0.5) = 1.5
    OP(129, 5, 30, 392, 250, 391);                    // Include FMix in the checked math result
    OP(109, 4, 2, 251, 392);                          // Convert the additional math checksum
    OP(205, 4, 2, 287, 25);                           // uint popcount(int(-7)) = 30
    OP(205, 4, 24, 288, 47);                          // signed result type keeps the unsigned count
    OP(124, 4, 2, 298, 288);                          // reinterpret the nonnegative signed result
    OP(205, 4, 3, 289, 39);                           // uint3 popcount(uint3(2, 2, 2)) = uint3(1)
    OP(81, 5, 2, 290, 289, 0);
    OP(81, 5, 2, 291, 289, 1);
    OP(81, 5, 2, 292, 289, 2);
    OP(128, 5, 2, 293, 290, 291);
    OP(128, 5, 2, 294, 293, 292);
    OP(128, 5, 2, 295, 287, 298);
    OP(128, 5, 2, 296, 294, 295);                     // The bit-count checks sum to 36
    OP(204, 4, 2, 299, 47);                            // uint reverse_bits(7) = 0xe0000000
    OP(203, 6, 24, 310, 25, 14, 72);                  // zero-extend bits 1..3 of int(-7): 4
    OP(202, 6, 24, 311, 25, 14, 72);                  // sign-extend bits 1..3 of int(-7): -4
    OP(201, 7, 2, 312, 302, 303, 21, 72);             // insert 3 into bits 2..4 of 0xa5: 0xad
    OP(203, 6, 3, 313, 39, 14, 14);                   // component-wise uint3 field extraction
    OP(81, 5, 2, 314, 313, 0);
    OP(81, 5, 2, 315, 313, 1);
    OP(81, 5, 2, 316, 313, 2);
    OP(128, 5, 2, 317, 314, 315);
    OP(128, 5, 2, 318, 317, 316);                     // vector extraction contributes 3
    OP(124, 4, 2, 319, 310);                          // preserve unsigned extract bits
    OP(124, 4, 2, 320, 311);                          // preserve signed extract bits
    OP(128, 5, 2, 321, 319, 320);                     // unsigned 4 plus signed -4 is zero
    OP(128, 5, 2, 322, 321, 312);
    OP(203, 6, 2, 325, 300, 21, 301);                  // unsigned bits 2..5 of 0xf3 are 12
    OP(128, 5, 2, 323, 322, 318);
    OP(128, 5, 2, 326, 323, 325);                     // bit-field checks contribute 188
    OP(128, 5, 2, 252, 211, 251);                     // Include both GLSL math checks
    OP(128, 5, 2, 297, 252, 296);
    OP(128, 5, 2, 253, 297, 299);                     // Include the bit-reverse check
    OP(128, 5, 2, 324, 253, 326);
    OP(12, 6, 160, 327, 200, 73, 163);               // FindILsb on signed int3, per component
    OP(12, 6, 160, 328, 200, 74, 163);               // FindSMsb on signed int3, per component
    OP(12, 6, 160, 329, 200, 75, 39);                // FindUMsb on uint3, per component
    OP(81, 5, 24, 330, 327, 0);
    OP(81, 5, 24, 331, 327, 1);
    OP(81, 5, 24, 332, 327, 2);
    OP(81, 5, 24, 333, 328, 0);
    OP(81, 5, 24, 334, 328, 1);
    OP(81, 5, 24, 335, 328, 2);
    OP(81, 5, 24, 336, 329, 0);
    OP(81, 5, 24, 337, 329, 1);
    OP(81, 5, 24, 338, 329, 2);
    OP(128, 5, 24, 339, 330, 331);
    OP(128, 5, 24, 340, 339, 332);                   // FindILsb sums to 0
    OP(128, 5, 24, 341, 333, 334);
    OP(128, 5, 24, 342, 341, 335);                   // FindSMsb sums to 6
    OP(128, 5, 24, 343, 336, 337);
    OP(128, 5, 24, 344, 343, 338);                   // FindUMsb sums to 3
    OP(128, 5, 24, 345, 340, 342);
    OP(128, 5, 24, 346, 345, 344);                   // All three vector scans sum to 9
    OP(124, 4, 2, 348, 346);                          // Keep the checksum's bit pattern as uint
    OP(128, 5, 2, 347, 324, 348);
    OP(80, 6, 160, 353, 350, 352, 350);              // int3(0, 8, 0)
    OP(80, 6, 160, 354, 350, 351, 350);              // int3(0, -1, 0)
    OP(80, 6, 3, 355, 13, 14, 13);                   // uint3(0, 1, 0)
    OP(12, 6, 160, 356, 200, 73, 353);               // zero FindILsb results are -1
    OP(12, 6, 160, 357, 200, 74, 354);               // FindSMsb(0, -1, 0) is (-1, -1, -1)
    OP(12, 6, 160, 358, 200, 75, 355);               // zero FindUMsb results are -1
    OP(81, 5, 24, 359, 356, 0);
    OP(81, 5, 24, 360, 356, 1);
    OP(81, 5, 24, 361, 356, 2);
    OP(81, 5, 24, 362, 357, 0);
    OP(81, 5, 24, 363, 357, 1);
    OP(81, 5, 24, 364, 357, 2);
    OP(81, 5, 24, 365, 358, 0);
    OP(81, 5, 24, 366, 358, 1);
    OP(81, 5, 24, 367, 358, 2);
    OP(128, 5, 24, 368, 359, 360);
    OP(128, 5, 24, 369, 368, 361);                   // FindILsb edge lanes sum to 1
    OP(128, 5, 24, 370, 362, 363);
    OP(128, 5, 24, 371, 370, 364);                   // FindSMsb edge lanes sum to -3
    OP(128, 5, 24, 372, 365, 366);
    OP(128, 5, 24, 373, 372, 367);                   // FindUMsb edge lanes sum to -2
    OP(128, 5, 24, 374, 369, 371);
    OP(128, 5, 24, 375, 374, 373);                   // The edge cases sum to -4
    OP(128, 5, 24, 376, 346, 375);                   // The vector scan checksum is 5
    OP(124, 4, 2, 377, 376);
    OP(128, 5, 2, 378, 324, 377);
    OP(12, 6, 24, 379, 200, 73, 25);                // Scalar FindILsb(-7) is 0
    OP(12, 6, 24, 380, 200, 74, 25);                // Scalar FindSMsb(-7) is 2
    OP(12, 6, 24, 381, 200, 75, 47);                // Scalar FindUMsb(7) is 2
    OP(128, 5, 24, 382, 379, 380);
    OP(128, 5, 24, 383, 382, 381);                  // Scalar scans sum to 4
    OP(124, 4, 2, 384, 383);
    OP(128, 5, 2, 385, 378, 384);
    OP(62, 3, 89, 385);
    OP(224, 4, 21, 21, 127);                          // OpControlBarrier: Workgroup + WorkgroupMemory
    OP(224, 4, 129, 129, 127);                        // OpControlBarrier: Subgroup + WorkgroupMemory
    OP(225, 3, 21, 127);                              // OpMemoryBarrier: Workgroup + WorkgroupMemory
    OP(225, 3, 14, 130);                              // OpMemoryBarrier: Device + UniformMemory
    OP(225, 3, 14, 131);                              // OpMemoryBarrier: Device + UniformMemory + ImageMemory
    OP(253, 1, 0);                                      // return
    OP(56, 1, 0);                                       // OpFunctionEnd
#undef OP
    *word_count = count;
    return 1;
}

static int make_loop_shader(uint32_t *words, size_t capacity, size_t *word_count) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 40;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                              // OpCapability Shader
    OP(14, 3, 0, 1);                           // OpMemoryModel Logical GLSL450
    OP(15, 5, 5, 10, 0x6e69616du, 0);          // OpEntryPoint GLCompute main
    OP(16, 6, 10, 17, 1, 1, 1);                // OpExecutionMode LocalSize 1 1 1
    OP(19, 2, 1);                              // void
    OP(21, 4, 2, 32, 0);                       // uint
    OP(20, 2, 3);                              // bool
    OP(32, 4, 4, 7, 2);                        // Function pointer to uint
    OP(33, 3, 5, 1);                           // void()
    OP(43, 4, 2, 6, 0);                        // uint 0
    OP(43, 4, 2, 7, 1);                        // uint 1
    OP(43, 4, 2, 8, 3);                        // uint 3
    OP(54, 5, 1, 10, 0, 5);                    // main
    OP(248, 2, 30);                            // entry
    OP(59, 4, 4, 20, 7);                       // function-local loop result
    OP(62, 3, 20, 6);                          // initialize to zero
    OP(249, 2, 31);                            // branch to loop header
    OP(248, 2, 31);                            // loop header
    OP(245, 7, 2, 21, 6, 30, 25, 33);          // OpPhi: swap with the second phi on the back edge
    OP(245, 7, 2, 25, 7, 30, 21, 33);          // OpPhi: cross-reference the first phi
    OP(245, 7, 2, 26, 6, 30, 23, 33);          // OpPhi: loop counter
    OP(176, 5, 3, 22, 26, 8);                  // OpULessThan counter, 3
    OP(246, 4, 34, 33, 0);                     // OpLoopMerge merge, continue
    OP(250, 4, 22, 32, 34);                    // branch to body or merge
    OP(248, 2, 32);                            // loop body
    OP(249, 2, 33);                            // branch to continue
    OP(248, 2, 33);                            // continue
    OP(128, 5, 2, 23, 26, 7);                  // increment counter phi
    OP(249, 2, 31);                            // back edge
    OP(248, 2, 34);                            // merge
    OP(62, 3, 20, 21);                         // retain final phi value
    OP(253, 1);                                // return
    OP(56, 1);                                 // OpFunctionEnd
#undef OP
    *word_count = count;
    return 1;
}

static int make_helper_shader(uint32_t *words, size_t capacity, size_t *word_count, uint32_t call_argument_id) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 32;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) { \
        fprintf(stderr, "spirv_msl: helper fixture overflow at opcode %u (count=%zu, wc=%u)\n", \
                (unsigned)(code), count, (unsigned)(n)); return 0; \
    } \
} while (0)
    OP(17, 2, 1);                              // OpCapability Shader
    OP(14, 3, 0, 1);                           // OpMemoryModel Logical GLSL450
    OP(15, 5, 5, 10, 0x6e69616du, 0);          // OpEntryPoint GLCompute main
    OP(16, 6, 10, 17, 1, 1, 1);                // OpExecutionMode LocalSize 1 1 1
    OP(19, 2, 1);                              // void
    OP(21, 4, 2, 32, 0);                       // uint
    OP(20, 2, 5);                              // bool
    OP(33, 3, 3, 1);                           // void()
    OP(33, 4, 4, 2, 2);                       // uint(uint)
    OP(43, 4, 2, 6, 1);                       // uint 1
    OP(43, 4, 2, 7, 2);                       // uint 2
    OP(41, 3, 5, 8);                          // bool true, used to test bad call typing
    OP(54, 5, 1, 10, 0, 3);                    // main
    OP(248, 2, 20);                            // entry
    OP(57, 5, 2, 21, 14, call_argument_id);   // call wrapper
    OP(57, 4, 1, 25, 18);                     // call a void helper
    OP(176, 5, 5, 22, 21, 7);                 // compare the returned value
    OP(250, 4, 22, 23, 24);                   // branch on the comparison
    OP(248, 2, 23);                            // true block
    OP(253, 1);                                // return
    OP(248, 2, 24);                            // false block
    OP(253, 1);                                // return
    OP(56, 1);                                 // OpFunctionEnd
    OP(54, 5, 2, 14, 0, 4);                   // wrapper(uint) -> uint
    OP(55, 3, 2, 15);                          // parameter
    OP(248, 2, 31);                            // wrapper entry
    OP(57, 5, 2, 16, 11, 15);                 // nested helper call
    OP(128, 5, 2, 17, 16, 6);                 // add one
    OP(254, 2, 17);                            // return value
    OP(56, 1);                                 // OpFunctionEnd
    OP(54, 5, 2, 11, 0, 4);                   // helper(uint) -> uint
    OP(55, 3, 2, 12);                          // parameter
    OP(248, 2, 30);                            // helper entry
    OP(128, 5, 2, 13, 12, 6);                  // add parameter + 1
    OP(254, 2, 13);                            // return value
    OP(56, 1);                                 // OpFunctionEnd
    OP(54, 5, 1, 18, 0, 3);                   // void helper
    OP(248, 2, 29);                            // void helper entry
    OP(253, 1);                                // return
    OP(56, 1);                                 // OpFunctionEnd
#undef OP
    *word_count = count;
    return 1;
}

static int rejects_bad_helper_argument_type(void) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!make_helper_shader(words, sizeof words / sizeof words[0], &word_count, 8)) return 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    int read_rc = spirv_read(words, word_count * sizeof words[0], &module);
    const spirv_entry_point *entry = read_rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    int lower_rc = entry ? spirv_build_gpu_ir_shader(&module, entry, &shader) : GPU_IR_INVALID;
    int ok = read_rc == SPIRV_OK && entry && lower_rc == GPU_IR_INVALID && !shader;
    if (!ok) fprintf(stderr, "spirv_msl: bad helper argument check failed (read=%d entry=%d lower=%d shader=%d)\n",
                     read_rc, entry != NULL, lower_rc, shader != NULL);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int write_helper_msl(const char *path) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!path || !make_helper_shader(words, sizeof words / sizeof words[0], &word_count, 7)) {
        fprintf(stderr, "spirv_msl: could not construct helper-call fixture\n");
        return 0;
    }
    if (!rejects_bad_helper_argument_type()) {
        fprintf(stderr, "spirv_msl: invalid helper argument was not rejected\n");
        return 0;
    }
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    char *source = NULL;
    size_t source_size = 0;
    int ok = 0;
    int reader_status = spirv_read(words, word_count * sizeof words[0], &module);
    int ir_status = GPU_IR_INVALID, msl_status = GPU_IR_INVALID;
    if (reader_status != SPIRV_OK) goto done;
    const spirv_entry_point *entry = spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE);
    if (!entry) goto done;
    ir_status = spirv_build_gpu_ir_shader(&module, entry, &shader);
    if (ir_status != GPU_IR_OK) goto done;
    msl_status = gpu_ir_msl_emit_compute(shader, &source, &source_size);
    if (msl_status != GPU_IR_OK || !source || !source_size ||
        !strstr(source, "uint vx_fn_11(uint);") || !strstr(source, "uint vx_fn_14(uint);") ||
        !strstr(source, "void vx_fn_18();") ||
        !strstr(source, "uint vx_fn_11(uint vx_arg_12) {") ||
        !strstr(source, "uint vx_fn_14(uint vx_arg_15) {") ||
        !strstr(source, "void vx_fn_18() {") || !strstr(source, "    vx_fn_18();") ||
        !strstr(source, "return vx_value_13;") ||
        !strstr(source, "uint vx_value_16 = vx_fn_11(vx_arg_15);") ||
        !strstr(source, "vx_value_21 = vx_fn_14(uint(0x00000002u));") ||
        !strstr(source, "vx_value_22 = (vx_value_21 < uint(0x00000002u));")) goto done;
    FILE *file = fopen(path, "w");
    if (!file) goto done;
    ok = fwrite(source, 1, source_size, file) == source_size && !ferror(file);
    if (fclose(file)) ok = 0;
done:
    if (!ok) fprintf(stderr, "spirv_msl: helper-call fixture failed (reader=%d ir=%d msl=%d)\n",
                     reader_status, ir_status, msl_status);
    free(source);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int make_helper_capture_shader(uint32_t *words, size_t capacity, size_t *word_count) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 64;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) { \
        fprintf(stderr, "spirv_msl: helper capture fixture overflow at opcode %u (count=%zu, wc=%u)\n", \
                (unsigned)(code), count, (unsigned)(n)); return 0; \
    } \
} while (0)
    OP(17, 2, 1);                              // OpCapability Shader
    OP(14, 3, 0, 1);                           // OpMemoryModel Logical GLSL450
    OP(15, 9, 5, 12, 0x6e69616du, 0, 30, 31, 32, 33); // buffer, invocation ID, and shared variables
    OP(16, 6, 12, 17, 1, 1, 1);                // OpExecutionMode LocalSize 1 1 1
    OP(71, 3, 5, 2);                           // Block
    OP(71, 4, 4, 6, 4);                        // ArrayStride 4
    OP(72, 5, 5, 0, 35, 0);                    // member 0 Offset 0
    OP(71, 4, 30, 34, 0);                      // buffer DescriptorSet 0
    OP(71, 4, 30, 33, 0);                      // buffer Binding 0
    OP(71, 4, 31, 11, 28);                     // GlobalInvocationId
    OP(19, 2, 1);                              // void
    OP(21, 4, 2, 32, 0);                       // uint
    OP(23, 4, 3, 2, 3);                        // uint3
    OP(29, 3, 4, 2);                           // runtime array of uint
    OP(30, 3, 5, 4);                           // storage block
    OP(32, 4, 6, 12, 5);                       // StorageBuffer pointer to block
    OP(32, 4, 7, 12, 2);                       // StorageBuffer pointer to uint
    OP(32, 4, 8, 1, 3);                        // Input pointer to uint3
    OP(32, 4, 11, 4, 2);                       // Workgroup pointer to uint
    OP(43, 4, 2, 15, 4);                       // uint 4, fixed Workgroup-array length
    OP(28, 4, 16, 2, 15);                      // fixed array of uint
    OP(32, 4, 17, 4, 16);                      // Workgroup pointer to fixed array
    OP(32, 4, 18, 4, 2);                       // Workgroup pointer to uint array element
    OP(32, 4, 19, 7, 2);                       // Function pointer parameter to uint
    OP(20, 2, 44);                             // bool for the helper branch
    OP(33, 3, 9, 1);                           // void()
    OP(33, 5, 10, 2, 19, 11);                 // uint(Function uint*, Workgroup uint*)
    OP(43, 4, 2, 14, 0);                       // uint 0
    OP(59, 4, 6, 30, 12);                     // storage buffer variable
    OP(59, 4, 8, 31, 1);                      // GlobalInvocationId variable
    OP(59, 4, 11, 32, 4);                     // shared Workgroup scalar
    OP(59, 4, 17, 33, 4);                     // shared Workgroup array
    OP(54, 5, 1, 12, 0, 9);                    // main
    OP(248, 2, 20);                            // entry
    OP(59, 4, 19, 46, 7);                     // local value passed by pointer
    OP(62, 3, 46, 14);                         // initialize local value
    OP(62, 3, 32, 14);                         // initialize shared scalar
    OP(65, 5, 18, 34, 33, 14);                // shared array[0]
    OP(62, 3, 34, 14);                         // initialize shared array element
    OP(57, 6, 2, 21, 13, 46, 32);             // call helper(&local, &workgroup)
    OP(65, 6, 7, 27, 30, 14, 14);             // output buffer[0]
    OP(62, 3, 27, 21);                        // store the helper result
    OP(253, 1);                                // return
    OP(56, 1);                                 // OpFunctionEnd
    OP(54, 5, 2, 13, 0, 10);                  // helper(Function uint*, Workgroup uint*) -> uint
    OP(55, 3, 19, 47);                         // helper's pointer argument
    OP(55, 3, 11, 50);                         // helper's Workgroup pointer argument
    OP(248, 2, 22);                            // helper entry
    OP(61, 4, 3, 23, 31);                     // load GlobalInvocationId
    OP(81, 5, 2, 24, 23, 0);                  // get invocation X
    OP(65, 6, 7, 25, 30, 14, 24);             // buffer[GlobalInvocationId.x]
    OP(61, 4, 2, 26, 25);                     // load the buffer element
    OP(61, 4, 2, 28, 32);                     // capture and load shared Workgroup scalar
    OP(128, 5, 2, 29, 26, 28);                // buffer value + shared scalar
    OP(170, 5, 44, 41, 24, 14);               // invocation X equals zero
    OP(247, 3, 40, 0);                         // selection merge
    OP(250, 4, 41, 38, 39);                   // branch to helper blocks
    OP(248, 2, 38);                            // helper true block
    OP(249, 2, 40);                            // merge
    OP(248, 2, 39);                            // helper false block
    OP(249, 2, 40);                            // merge
    OP(248, 2, 40);                            // helper merge block
    OP(245, 7, 2, 42, 26, 38, 29, 39);        // choose buffer or scalar-adjusted value
    OP(65, 5, 18, 35, 33, 14);                // capture and index shared Workgroup array
    OP(61, 4, 2, 36, 35);                     // load shared array element
    OP(128, 5, 2, 37, 42, 36);                // add shared array element to the phi value
    OP(61, 4, 2, 48, 47);                     // load through the function pointer parameter
    OP(128, 5, 2, 45, 37, 48);                // add the helper argument
    OP(61, 4, 2, 52, 50);                     // load through the Workgroup pointer parameter
    OP(128, 5, 2, 53, 45, 52);                // add the Workgroup pointer value
    OP(254, 2, 53);                            // return value
    OP(56, 1);                                 // OpFunctionEnd
#undef OP
    *word_count = count;
    return 1;
}

static int write_helper_capture_msl(const char *path) {
    uint32_t words[512] = { 0 };
    size_t word_count = 0;
    if (!path || !make_helper_capture_shader(words, sizeof words / sizeof words[0], &word_count)) return 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    char *source = NULL;
    size_t source_size = 0;
    int ok = 0;
    int reader_status = spirv_read(words, word_count * sizeof words[0], &module);
    int ir_status = GPU_IR_INVALID, msl_status = GPU_IR_INVALID;
    if (reader_status != SPIRV_OK) goto done;
    const spirv_entry_point *entry = spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE);
    if (!entry) goto done;
    ir_status = spirv_build_gpu_ir_shader(&module, entry, &shader);
    if (ir_status != GPU_IR_OK) goto done;
    msl_status = gpu_ir_msl_emit_compute(shader, &source, &source_size);
    if (msl_status != GPU_IR_OK || !source || !source_size ||
        !strstr(source, "uint vx_fn_13(thread uint* vx_arg_47, threadgroup uint* vx_arg_50, device uint* vx_capture_30, uint3 vx_capture_31, threadgroup uint* vx_capture_32, threadgroup array<uint, 4>* vx_capture_33, constant ulong* vx_buffer_ranges)") ||
        !strstr(source, "uint3 vx_builtin_31 [[thread_position_in_grid]]") ||
        !strstr(source, "threadgroup uint vx_workgroup_32;") ||
        !strstr(source, "threadgroup array<uint, 4> vx_workgroup_33;") ||
        !strstr(source, "vx_value_21 = vx_fn_13(&vx_local_46, &vx_workgroup_32, vx_buffer_30, vx_builtin_31, &vx_workgroup_32, &vx_workgroup_33, vx_buffer_ranges);") ||
        !strstr(source, "if (vx_value_41) { vx_pred = 22u; vx_block = 38u; } else { vx_pred = 22u; vx_block = 39u; }") ||
        !strstr(source, "if (vx_pred == 38u) vx_phi_in_42 = vx_value_26;") ||
        !strstr(source, "vx_buffer_ranges[0]") || !strstr(source, "vx_capture_30[vx_value_24]") ||
        !strstr(source, "vx_value_28 = (*vx_capture_32);") ||
        !strstr(source, "vx_value_48 = (*vx_arg_47);") ||
        !strstr(source, "vx_value_52 = (*vx_arg_50);") ||
        !strstr(source, "vx_value_36 = (*vx_capture_33)[uint(0x00000000u)];") ||
        !strstr(source, "vx_buffer_30[uint(0x00000000u)] = vx_value_21")) {
        goto done;
    }
    FILE *file = fopen(path, "w");
    if (!file) goto done;
    ok = fwrite(source, 1, source_size, file) == source_size && !ferror(file);
    if (fclose(file)) ok = 0;
done:
    if (!ok) fprintf(stderr, "spirv_msl: helper capture fixture failed (reader=%d ir=%d msl=%d)\n",
                     reader_status, ir_status, msl_status);
    free(source);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int write_loop_msl(const char *path) {
    uint32_t words[128] = { 0 };
    size_t word_count = 0;
    if (!path || !make_loop_shader(words, sizeof words / sizeof words[0], &word_count)) return 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    char *source = NULL;
    size_t source_size = 0;
    int ok = 0;
    if (spirv_read(words, word_count * sizeof words[0], &module) != SPIRV_OK) goto done;
    const spirv_entry_point *entry = spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE);
    if (!entry || spirv_build_gpu_ir_shader(&module, entry, &shader) != GPU_IR_OK ||
        gpu_ir_msl_emit_compute(shader, &source, &source_size) != GPU_IR_OK || !source || !source_size ||
        !strstr(source, "while (true)") || !strstr(source, "vx_pred == 33u") ||
        !strstr(source, "vx_block = 31u")) goto done;
    const char *phi_a_in = strstr(source, "vx_phi_in_21 = vx_value_25");
    const char *phi_b_in = strstr(source, "vx_phi_in_25 = vx_value_21");
    const char *phi_a_commit = strstr(source, "vx_value_21 = vx_phi_in_21");
    const char *phi_b_commit = strstr(source, "vx_value_25 = vx_phi_in_25");
    if (!phi_a_in || !phi_b_in || !phi_a_commit || !phi_b_commit ||
        phi_a_in >= phi_a_commit || phi_b_in >= phi_a_commit || phi_a_commit >= phi_b_commit) goto done;
    FILE *file = fopen(path, "w");
    if (!file) goto done;
    ok = fwrite(source, 1, source_size, file) == source_size && !ferror(file);
    if (fclose(file)) ok = 0;
done:
    if (!ok) fprintf(stderr, "spirv_msl: failed to lower the loop/phi fixture\n");
    free(source);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int make_vector_classification_shader(uint32_t *words, size_t capacity, size_t *word_count) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 40;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) { \
        fprintf(stderr, "spirv_msl: could not append vector fixture opcode %u\n", (unsigned)(code)); \
        return 0; \
    } \
} while (0)
    OP(17, 2, 1);                                     // Shader capability
    OP(14, 3, 0, 1);                                  // Logical GLSL450 memory model
    OP(15, 5, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 16, 0x6e69616du, 0); // Entry point: main
    OP(16, 6, 16, 17, 1, 1, 1);                      // LocalSize 1x1x1
    OP(19, 2, 1);                                     // void
    OP(20, 2, 2);                                     // bool
    OP(22, 3, 3, 32);                                 // float32
    OP(23, 4, 4, 3, 2);                               // float2
    OP(23, 4, 5, 2, 2);                               // bool2
    OP(33, 3, 6, 1);                                  // void()
    OP(43, 4, 3, 7, 0x7fc00000u);                    // quiet NaN
    OP(43, 4, 3, 8, 0x3f800000u);                    // 1.0
    OP(43, 4, 3, 9, 0);                               // 0.0
    OP(54, 5, 1, 16, 0, 6);                           // main()
    OP(248, 2, 18);                                   // entry block
    OP(80, 5, 4, 10, 7, 8);                           // float2(NaN, 1)
    OP(80, 5, 4, 11, 8, 9);                           // float2(1, 0)
    static const uint32_t unary_compare_opcodes[] = { 156, 157, 158, 159 };
    static const uint32_t binary_compare_opcodes[] = {
        162, 163, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191
    };
    uint32_t result_id = 19;
    for (size_t i = 0; i < sizeof unary_compare_opcodes / sizeof unary_compare_opcodes[0]; i++)
        OP(unary_compare_opcodes[i], 4, 5, result_id++, 10);
    for (size_t i = 0; i < sizeof binary_compare_opcodes / sizeof binary_compare_opcodes[0]; i++)
        OP(binary_compare_opcodes[i], 5, 5, result_id++, 10, 11);
    OP(253, 1);                                       // return
    OP(56, 1);                                        // function end
#undef OP
    *word_count = count;
    return 1;
}

static int write_vector_classification_msl(const char *path) {
    uint32_t words[192] = { 0 };
    size_t word_count = 0;
    if (!path || !make_vector_classification_shader(words, 192, &word_count)) return 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    char *source = NULL;
    size_t source_size = 0;
    int ok = 0;
    int rc = spirv_read(words, word_count * sizeof words[0], &module);
    if (rc != SPIRV_OK) { fprintf(stderr, "spirv_msl: vector fixture read failed: %d %s\n", rc, module.error); goto done; }
    const spirv_entry_point *entry = spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE);
    if (!entry) { fprintf(stderr, "spirv_msl: vector fixture entry point missing\n"); goto done; }
    rc = spirv_build_gpu_ir_shader(&module, entry, &shader);
    if (rc != GPU_IR_OK) { fprintf(stderr, "spirv_msl: vector fixture IR build failed: %d\n", rc); goto done; }
    rc = gpu_ir_msl_emit_compute(shader, &source, &source_size);
    if (rc != GPU_IR_OK || !source || !source_size) {
        fprintf(stderr, "spirv_msl: vector fixture MSL emission failed: %d\n", rc);
        for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
            const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
            fprintf(stderr, "  op=%u operation=%u type=%u operands=%u\n", instruction->opcode,
                    instruction->operation, instruction->result_type_id, instruction->operand_count);
        }
        goto done;
    }
    for (uint32_t result_id = 19; result_id < 37; result_id++) {
        char declaration[48];
        snprintf(declaration, sizeof declaration, "bool2 vx_value_%u = ", result_id);
        if (!strstr(source, declaration)) {
            fprintf(stderr, "spirv_msl: vector comparison result %u was not emitted:\n%s\n", result_id, source);
            goto done;
        }
    }
    if (!strstr(source, "bool2 vx_value_19 = isnan(") ||
        !strstr(source, " | isnan(") || !strstr(source, " & !isnan(") ||
        !strstr(source, " | (")) {
        fprintf(stderr, "spirv_msl: vector fixture MSL mismatch:\n%s\n", source);
        goto done;
    }
    FILE *file = fopen(path, "w");
    if (!file) goto done;
    ok = fwrite(source, 1, source_size, file) == source_size && !ferror(file);
    if (fclose(file)) ok = 0;
done:
    free(source);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int write_fixture(const char *path) {
    uint32_t words[2048] = { 0 };
    size_t word_count = 0;
    if (!make_arithmetic_shader(words, sizeof words / sizeof words[0], &word_count, 0)) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int write_graphics_stage_fixture(const char *path, int vertex) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    int built = vertex ? make_graphics_vertex_shader(words, sizeof words / sizeof words[0], &word_count,
                                                     0, 1, 0) :
                         make_graphics_fragment_shader(words, sizeof words / sizeof words[0], &word_count);
    if (!built) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int write_graphics_sampled_image_fixture(const char *path, int implicit_lod) {
    uint32_t words[128] = { 0 };
    size_t word_count = 0;
    if (!path || !make_graphics_sampled_image_fragment_shader(words,
            sizeof words / sizeof words[0], &word_count, implicit_lod)) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int write_graphics_separate_image_sampler_fixture(const char *path, int implicit_lod) {
    uint32_t words[160] = { 0 };
    size_t word_count = 0;
    if (!path || !make_graphics_separate_image_sampler_fragment_shader(words,
            sizeof words / sizeof words[0], &word_count, implicit_lod)) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int write_graphics_integer_vertex_fixture(const char *path, int signed_input) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!make_graphics_vertex_shader(words, sizeof words / sizeof words[0], &word_count,
                                     0, signed_input ? 3 : 2, 0)) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int write_image_fixture(const char *path, int sampled_fetch) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!make_image_copy_shader(words, sizeof words / sizeof words[0], &word_count, sampled_fetch, 0, 1)) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int write_storage_image_fixture(const char *path) {
    return write_image_fixture(path, 0);
}

static int write_sampled_image_fixture(const char *path) {
    return write_image_fixture(path, 1);
}

static int write_image_size_query_fixture(const char *path, uint32_t query_opcode, int query_storage_image) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!path || !make_image_size_query_shader(words, sizeof words / sizeof words[0],
                                                &word_count, query_opcode, query_storage_image)) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int write_image_size_lod1_fixture(const char *path) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!path || !make_image_size_query_shader(words, sizeof words / sizeof words[0],
                                                &word_count, 103, 0) ||
        !set_image_query_lod(words, word_count, 10)) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int write_high_texture_fixture(const char *path) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!path || !make_image_copy_shader(words, sizeof words / sizeof words[0], &word_count, 1, 15, 19)) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int write_sampler_fixture(const char *path) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!path || !make_sampler_sample_shader(words, sizeof words / sizeof words[0], &word_count, 0)) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int write_separate_sampler_fixture(const char *path) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!path || !make_sampler_sample_shader(words, sizeof words / sizeof words[0], &word_count, 1)) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int write_image_sampler_array_fixture(const char *path, int separate) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    int made = separate ? make_separate_image_sampler_array_shader(words,
        sizeof words / sizeof words[0], &word_count) :
        make_combined_image_array_shader(words, sizeof words / sizeof words[0], &word_count);
    if (!path || !made) return 0;
    spirv_module module = { 0 };
    int read_result = spirv_read(words, word_count * sizeof words[0], &module);
    if (read_result != SPIRV_OK) {
        fprintf(stderr, "spirv_msl: image/sampler-array fixture rejected: %s (%d)\n",
                module.error, read_result);
        spirv_free(&module);
        return 0;
    }
    spirv_free(&module);
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int write_combined_image_array_fixture(const char *path) {
    return write_image_sampler_array_fixture(path, 0);
}

static int write_separate_image_sampler_array_fixture(const char *path) {
    return write_image_sampler_array_fixture(path, 1);
}

static int check_sampler_sample_translation(void) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!make_sampler_sample_shader(words, sizeof words / sizeof words[0], &word_count, 0)) return 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    char *source = NULL;
    size_t source_size = 0;
    int ok = 0;
    int rc = spirv_read(words, word_count * sizeof words[0], &module);
    const spirv_entry_point *entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    if (entry) rc = spirv_build_gpu_ir_shader(&module, entry, &shader);
    if (rc == GPU_IR_OK) rc = spirv_build_gpu_ir_layout(&module, entry, &layout);
    if (rc == GPU_IR_OK) rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &source, &source_size);
    ok = rc == GPU_IR_OK && source && source_size &&
        strstr(source, "texture2d<float, access::sample> vx_image_20 [[texture(0)]]") &&
        strstr(source, "sampler vx_sampler_20 [[sampler(0)]]") &&
        strstr(source, "vx_image_20.sample(vx_sampler_20,") && strstr(source, "level(as_type<float>");
    if (!ok) fprintf(stderr, "spirv_msl: sampler sampling translation failed (%d)\n%s",
                     rc, source ? source : "");
    free(source);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int check_image_size_query_translation(uint32_t query_opcode, int query_storage_image) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!make_image_size_query_shader(words, sizeof words / sizeof words[0], &word_count,
                                      query_opcode, query_storage_image)) return 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    char *source = NULL;
    size_t source_size = 0;
    int rc = spirv_read(words, word_count * sizeof words[0], &module);
    int read_rc = rc;
    const spirv_entry_point *entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    if (entry) rc = spirv_build_gpu_ir_shader(&module, entry, &shader);
    int shader_rc = rc;
    if (rc == GPU_IR_OK) rc = spirv_build_gpu_ir_layout(&module, entry, &layout);
    int layout_rc = rc;
    if (rc == GPU_IR_OK) rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &source, &source_size);
    int msl_rc = rc;
    const char *query_image_name = query_storage_image ? "vx_image_21" : "vx_image_20";
    char levels_query[112];
    snprintf(levels_query, sizeof levels_query, "int vx_value_33 = int(%s.get_num_mip_levels());", query_image_name);
    int query_emitted = query_opcode == 106 ?
        source && strstr(source, levels_query) : query_opcode == 103 ?
        source && strstr(source, "get_width(uint(") && strstr(source, "get_height(uint(") :
        source && strstr(source, "int2(int(vx_image_20.get_width()), int(vx_image_20.get_height()))");
    int coordinate_emitted = query_opcode == 106 ?
        source && strstr(source, "vx_value_34 = vx_value_37;") :
        source && strstr(source, "vx_value_34 = vx_value_33 - vx_value_37;");
    int ok = rc == GPU_IR_OK && source && source_size && query_emitted &&
        strstr(source, "int2 vx_value_37 = int2(") && coordinate_emitted &&
        strstr(source, "vx_image_20.read(uint2(vx_value_34))") &&
        strstr(source, "vx_image_21.write(vx_value_35, uint2(vx_value_34))");
    if (!ok) fprintf(stderr, "spirv_msl: OpImageQuerySize%s translation failed (read=%d shader=%d layout=%d msl=%d, %s)\n%s",
        query_opcode == 103 ? "Lod" : query_opcode == 106 ? "Levels" : "",
        read_rc, shader_rc, layout_rc, msl_rc, module.error, source ? source : "");
    free(source);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int make_image_array_size_query_shader(uint32_t *words, size_t capacity, size_t *word_count) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 13;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                      // Shader capability
    OP(17, 2, 50);                                     // ImageQuery capability
    OP(14, 3, 0, 1);                                   // Logical GLSL450 memory model
    OP(15, 6, 5, 11, 0x6e69616du, 0, 8);              // compute main with one sampled image
    OP(16, 6, 11, 17, 1, 1, 1);                       // LocalSize 1x1x1
    OP(71, 4, 8, 34, 0);                              // DescriptorSet 0
    OP(71, 4, 8, 33, 0);                              // Binding 0
    OP(19, 2, 1);                                      // void
    OP(22, 3, 2, 32);                                 // float
    OP(21, 4, 3, 32, 1);                              // signed int
    OP(23, 4, 4, 3, 3);                               // int3 query result
    OP(25, 9, 5, 2, 1, 0, 1, 0, 1, 0);               // sampled 2D float array image
    OP(32, 4, 6, 0, 5);                               // UniformConstant image pointer
    OP(33, 3, 7, 1);                                 // void()
    OP(59, 4, 6, 8, 0);                              // sampled image
    OP(54, 5, 1, 11, 0, 7);                          // main function
    OP(248, 2, 12);                                  // entry block
    OP(61, 4, 5, 9, 8);                             // load image
    OP(104, 4, 4, 10, 9);                           // OpImageQuerySize returns width/height/layers
    OP(253, 1);                                     // return
    OP(56, 1);                                      // function end
#undef OP
    *word_count = count;
    return 1;
}

static int write_image_array_size_query_fixture(const char *path) {
    uint32_t words[128] = { 0 };
    size_t word_count = 0;
    if (!path || !make_image_array_size_query_shader(words, sizeof words / sizeof words[0], &word_count))
        return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int check_image_array_size_query_translation(void) {
    uint32_t words[128] = { 0 };
    size_t word_count = 0;
    if (!make_image_array_size_query_shader(words, sizeof words / sizeof words[0], &word_count)) return 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    char *source = NULL;
    size_t source_size = 0;
    int ok = 0;
    id<MTLDevice> metal_device = nil;
    NSString *metal_source = nil;
    NSError *metal_error = nil;
    id<MTLLibrary> library = nil;
    int rc = spirv_read(words, word_count * sizeof words[0], &module);
    int read_rc = rc;
    const spirv_entry_point *entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    if (!entry && rc == SPIRV_OK) rc = GPU_IR_INVALID;
    if (entry && rc == SPIRV_OK) rc = spirv_build_gpu_ir_shader(&module, entry, &shader);
    int shader_rc = rc;
    if (rc == GPU_IR_OK) rc = spirv_build_gpu_ir_layout(&module, entry, &layout);
    int layout_rc = rc;
    if (rc == GPU_IR_OK) rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &source, &source_size);
    int msl_rc = rc;
    if (rc != GPU_IR_OK || !source || !source_size ||
        !strstr(source, "texture2d_array<float, access::read> vx_image_8 [[texture(0)]]") ||
        !strstr(source, "int3 vx_value_10 = int3(") ||
        !strstr(source, "int3(int(vx_image_8.get_width()), int(vx_image_8.get_height()), int(vx_image_8.get_array_size()))"))
        goto done;
    metal_device = MTLCreateSystemDefaultDevice();
    metal_source = [[NSString alloc] initWithBytes:source length:source_size encoding:NSUTF8StringEncoding];
    library = metal_device && metal_source ?
        [metal_device newLibraryWithSource:metal_source options:nil error:&metal_error] : nil;
    ok = library != nil;
done:
    if (!ok) fprintf(stderr, "spirv_msl: array-image OpImageQuerySize translation failed "
                     "(read=%d shader=%d layout=%d msl=%d, %s)\n%s",
                     read_rc, shader_rc, layout_rc, msl_rc, module.error, source ? source : "");
    free(source);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int check_combined_image_array_translation(void) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!make_combined_image_array_shader(words, sizeof words / sizeof words[0], &word_count)) return 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    char *source = NULL;
    size_t source_size = 0;
    int ok = 0;
    id<MTLDevice> metal_device = nil;
    NSString *metal_source = nil;
    NSError *metal_error = nil;
    id<MTLLibrary> library = nil;
    id<MTLFunction> function = nil;
    int rc = spirv_read(words, word_count * sizeof words[0], &module);
    int read_rc = rc;
    const spirv_entry_point *entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    if (!entry && rc == SPIRV_OK) rc = GPU_IR_INVALID;
    if (entry && rc == SPIRV_OK) rc = spirv_build_gpu_ir_shader(&module, entry, &shader);
    int shader_rc = rc;
    if (rc == GPU_IR_OK) rc = spirv_build_gpu_ir_layout(&module, entry, &layout);
    int layout_rc = rc;
    if (rc == GPU_IR_OK) rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &source, &source_size);
    int msl_rc = rc;
    if (rc != GPU_IR_OK || !source || !source_size ||
        !strstr(source, "texture2d<float, access::sample> vx_image_20_0 [[texture(0)]]") ||
        !strstr(source, "sampler vx_sampler_20_0 [[sampler(0)]]") ||
        !strstr(source, "texture2d<float, access::sample> vx_image_20_1 [[texture(1)]]") ||
        !strstr(source, "sampler vx_sampler_20_1 [[sampler(1)]]") ||
        !strstr(source, "texture2d<float, access::write> vx_image_21 [[texture(2)]]") ||
        !strstr(source, "vx_image_20_1.sample(vx_sampler_20_1,")) goto done;

    metal_device = MTLCreateSystemDefaultDevice();
    metal_source = [[NSString alloc] initWithBytes:source length:source_size encoding:NSUTF8StringEncoding];
    library = metal_device && metal_source ?
        [metal_device newLibraryWithSource:metal_source options:nil error:&metal_error] : nil;
    function = [library newFunctionWithName:@"vx_main"];
    ok = function != nil;
done:
    if (!ok) fprintf(stderr, "spirv_msl: combined image-array translation failed (read=%d shader=%d layout=%d msl=%d, %s)\n%s",
                     read_rc, shader_rc, layout_rc, msl_rc, module.error, source ? source : "");
    free(source);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int check_separate_image_sampler_array_translation(void) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!make_separate_image_sampler_array_shader(words, sizeof words / sizeof words[0], &word_count)) return 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    char *source = NULL;
    size_t source_size = 0;
    int ok = 0;
    id<MTLDevice> metal_device = nil;
    NSString *metal_source = nil;
    NSError *metal_error = nil;
    id<MTLLibrary> library = nil;
    id<MTLFunction> function = nil;
    int rc = spirv_read(words, word_count * sizeof words[0], &module);
    int read_rc = rc;
    const spirv_entry_point *entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    if (!entry && rc == SPIRV_OK) rc = GPU_IR_INVALID;
    if (entry && rc == SPIRV_OK) rc = spirv_build_gpu_ir_shader(&module, entry, &shader);
    int shader_rc = rc;
    if (rc == GPU_IR_OK) rc = spirv_build_gpu_ir_layout(&module, entry, &layout);
    int layout_rc = rc;
    if (rc == GPU_IR_OK) rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &source, &source_size);
    int msl_rc = rc;
    if (rc != GPU_IR_OK || !source || !source_size ||
        !strstr(source, "texture2d<float, access::sample> vx_image_20_0 [[texture(0)]]") ||
        !strstr(source, "texture2d<float, access::sample> vx_image_20_1 [[texture(1)]]") ||
        !strstr(source, "sampler vx_sampler_22_0 [[sampler(0)]]") ||
        !strstr(source, "sampler vx_sampler_22_1 [[sampler(1)]]") ||
        !strstr(source, "texture2d<float, access::write> vx_image_21 [[texture(2)]]") ||
        !strstr(source, "vx_image_20_1.sample(vx_sampler_22_1,")) goto done;

    metal_device = MTLCreateSystemDefaultDevice();
    metal_source = [[NSString alloc] initWithBytes:source length:source_size encoding:NSUTF8StringEncoding];
    library = metal_device && metal_source ?
        [metal_device newLibraryWithSource:metal_source options:nil error:&metal_error] : nil;
    function = [library newFunctionWithName:@"vx_main"];
    ok = function != nil;
done:
    if (!ok) fprintf(stderr, "spirv_msl: separate image/sampler-array translation failed "
                     "(read=%d shader=%d layout=%d msl=%d, %s)\n%s",
                     read_rc, shader_rc, layout_rc, msl_rc, module.error, source ? source : "");
    free(source);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int check_dynamic_sampled_image_array_translation(int separate_sampler) {
    uint32_t words[512] = { 0 };
    size_t word_count = 0;
    if (!make_dynamic_sampled_image_array_shader(words, sizeof words / sizeof words[0],
                                                  &word_count, separate_sampler, 26)) return 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    char *source = NULL;
    size_t source_size = 0;
    int ok = 0;
    id<MTLDevice> metal_device = nil;
    NSString *metal_source = nil;
    NSError *metal_error = nil;
    id<MTLLibrary> library = nil;
    id<MTLFunction> function = nil;
    int rc = spirv_read(words, word_count * sizeof words[0], &module);
    int read_rc = rc;
    const spirv_entry_point *entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    if (!entry && rc == SPIRV_OK) rc = GPU_IR_INVALID;
    if (entry && rc == SPIRV_OK) rc = spirv_build_gpu_ir_shader(&module, entry, &shader);
    int shader_rc = rc;
    if (rc == GPU_IR_OK) rc = spirv_build_gpu_ir_layout(&module, entry, &layout);
    int layout_rc = rc;
    if (rc == GPU_IR_OK) rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &source, &source_size);
    int msl_rc = rc;
    if (rc != GPU_IR_OK || !source || !source_size ||
        !strstr(source, "uint(vx_value_42) == 0u ? vx_image_30_0 : vx_image_30_1") ||
        !strstr(source, "texture2d<float, access::write> vx_image_31 [[texture(2)]]")) goto done;
    if (separate_sampler) {
        if (!strstr(source, "uint(vx_value_43) == 0u ? vx_sampler_33_0 : vx_sampler_33_1") ||
            !strstr(source, "uint(vx_value_42) < 2u && uint(vx_value_43) < 2u")) goto done;
    } else if (!strstr(source, "uint(vx_value_42) == 0u ? vx_sampler_30_0 : vx_sampler_30_1") ||
               !strstr(source, "uint(vx_value_42) < 2u")) goto done;

    metal_device = MTLCreateSystemDefaultDevice();
    metal_source = [[NSString alloc] initWithBytes:source length:source_size encoding:NSUTF8StringEncoding];
    library = metal_device && metal_source ?
        [metal_device newLibraryWithSource:metal_source options:nil error:&metal_error] : nil;
    function = [library newFunctionWithName:@"vx_main"];
    ok = function != nil;
done:
    if (!ok) fprintf(stderr, "spirv_msl: dynamic sampled-image/sampler-array translation failed "
                     "(separate=%d read=%d shader=%d layout=%d msl=%d, %s)\n%s",
                     separate_sampler, read_rc, shader_rc, layout_rc, msl_rc, module.error,
                     source ? source : "");
    free(source);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int check_dynamic_storage_image_array_translation(void) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!make_dynamic_storage_image_array_shader(words, sizeof words / sizeof words[0], &word_count)) return 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    char *source = NULL;
    size_t source_size = 0;
    int ok = 0;
    id<MTLDevice> metal_device = nil;
    NSString *metal_source = nil;
    NSError *metal_error = nil;
    id<MTLLibrary> library = nil;
    id<MTLFunction> function = nil;
    int rc = spirv_read(words, word_count * sizeof words[0], &module);
    int read_rc = rc;
    const spirv_entry_point *entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    if (!entry && rc == SPIRV_OK) rc = GPU_IR_INVALID;
    if (entry && rc == SPIRV_OK) rc = spirv_build_gpu_ir_shader(&module, entry, &shader);
    int shader_rc = rc;
    if (rc == GPU_IR_OK) rc = spirv_build_gpu_ir_layout(&module, entry, &layout);
    int layout_rc = rc;
    if (rc == GPU_IR_OK) rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &source, &source_size);
    int msl_rc = rc;
    if (rc != GPU_IR_OK || !source || !source_size ||
        !strstr(source, "texture2d<float, access::write> vx_image_20_0 [[texture(0)]]") ||
        !strstr(source, "texture2d<float, access::write> vx_image_20_1 [[texture(1)]]") ||
        !strstr(source, "(uint(vx_value_33) == 0u ? vx_image_20_0 : vx_image_20_1)") ||
        !strstr(source, "if (uint(vx_value_33) < 2u)")) goto done;

    metal_device = MTLCreateSystemDefaultDevice();
    metal_source = [[NSString alloc] initWithBytes:source length:source_size encoding:NSUTF8StringEncoding];
    library = metal_device && metal_source ?
        [metal_device newLibraryWithSource:metal_source options:nil error:&metal_error] : nil;
    function = [library newFunctionWithName:@"vx_main"];
    ok = function != nil;
done:
    if (!ok) fprintf(stderr, "spirv_msl: dynamic storage-image array translation failed "
                     "(read=%d shader=%d layout=%d msl=%d, %s; %s)\n%s",
                     read_rc, shader_rc, layout_rc, msl_rc,
                     metal_error.localizedDescription.UTF8String ?: "no Metal compiler diagnostic",
                     module.error, source ? source : "");
    free(source);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int make_descriptor_array_shader(uint32_t *words, size_t capacity, size_t *word_count) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 33;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                         // Shader capability
    OP(14, 3, 0, 1);                      // Logical GLSL450 memory model
    OP(15, 6, 5, 30, 0x6e69616du, 0, 20); // main, with the descriptor variable in its interface
    OP(16, 6, 30, 17, 1, 1, 1);           // LocalSize 1,1,1
    OP(71, 4, 3, 6, 4);                    // runtime-array ArrayStride 4
    OP(71, 4, 20, 34, 0);                 // DescriptorSet 0
    OP(71, 4, 20, 33, 0);                 // Binding 0
    OP(72, 5, 4, 0, 35, 0);               // block member Offset 0
    OP(71, 3, 4, 2);                       // storage block
    OP(19, 2, 1);                          // void
    OP(21, 4, 2, 32, 0);                  // uint
    OP(29, 3, 3, 2);                       // runtime array of uint
    OP(30, 3, 4, 3);                       // storage block struct
    OP(33, 3, 8, 1);                       // void()
    OP(43, 4, 2, 9, 2);                   // descriptor-array length
    OP(28, 4, 5, 4, 9);                   // two storage-buffer blocks
    OP(32, 4, 6, 12, 5);                  // pointer to descriptor array
    OP(32, 4, 7, 12, 2);                  // pointer to storage-buffer uint
    OP(43, 4, 2, 10, 1);                  // descriptor element one
    OP(43, 4, 2, 11, 0);                  // block member zero
    OP(43, 4, 2, 12, 0);                  // runtime-array element zero
    OP(43, 4, 2, 13, 123);                // value written by the shader
    OP(59, 4, 6, 20, 12);                 // storage-buffer descriptor array
    OP(54, 5, 1, 30, 0, 8);               // function main
    OP(248, 2, 31);                        // entry block
    OP(65, 7, 7, 32, 20, 10, 11, 12);     // descriptor[1].data[0]
    OP(62, 3, 32, 13);                    // store 123
    OP(253, 1, 0);
    OP(56, 1, 0);
#undef OP
    *word_count = count;
    return 1;
}

static int write_descriptor_array_fixture(const char *path) {
    uint32_t words[128] = { 0 };
    size_t word_count = 0;
    if (!path || !make_descriptor_array_shader(words, sizeof words / sizeof words[0], &word_count)) return 0;
    spirv_module module = { 0 };
    int read_result = spirv_read(words, word_count * sizeof words[0], &module);
    if (read_result != SPIRV_OK) {
        fprintf(stderr, "spirv_msl: descriptor-array fixture rejected: %s (%d)\n", module.error, read_result);
        spirv_free(&module);
        return 0;
    }
    spirv_free(&module);
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int make_dynamic_descriptor_array_shader(uint32_t *words, size_t capacity, size_t *word_count) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 41;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                          // Shader capability
    OP(14, 3, 0, 1);                       // Logical GLSL450 memory model
    OP(15, 7, 5, 30, 0x6e69616du, 0, 20, 40); // main, descriptor array and GlobalInvocationId
    OP(16, 6, 30, 17, 1, 1, 1);            // LocalSize 1,1,1
    OP(71, 4, 3, 6, 4);                     // runtime-array ArrayStride 4
    OP(71, 4, 20, 34, 0);                   // DescriptorSet 0
    OP(71, 4, 20, 33, 0);                   // Binding 0
    OP(71, 4, 40, 11, 28);                  // BuiltIn GlobalInvocationId
    OP(72, 5, 4, 0, 35, 0);                 // block member Offset 0
    OP(71, 3, 4, 2);                         // storage block
    OP(19, 2, 1);                            // void
    OP(21, 4, 2, 32, 0);                    // uint
    OP(29, 3, 3, 2);                         // runtime array of uint
    OP(30, 3, 4, 3);                         // storage block struct
    OP(33, 3, 8, 1);                         // void()
    OP(43, 4, 2, 9, 2);                      // descriptor-array length
    OP(28, 4, 5, 4, 9);                      // two storage-buffer blocks
    OP(32, 4, 6, 12, 5);                     // pointer to descriptor array
    OP(32, 4, 7, 12, 2);                     // pointer to storage-buffer uint
    OP(43, 4, 2, 10, 0);                     // block member zero / runtime-array element zero
    OP(43, 4, 2, 11, 123);                   // base value written by the shader
    OP(23, 4, 13, 2, 3);                     // uint3
    OP(32, 4, 14, 1, 13);                    // pointer to input uint3
    OP(59, 4, 6, 20, 12);                    // storage-buffer descriptor array
    OP(59, 4, 14, 40, 1);                    // GlobalInvocationId input
    OP(54, 5, 1, 30, 0, 8);                  // function main
    OP(248, 2, 31);                           // entry block
    OP(61, 4, 13, 33, 40);                   // load GlobalInvocationId
    OP(81, 5, 2, 34, 33, 0);                  // extract x as the descriptor index
    OP(128, 5, 2, 35, 34, 11);                // value = 123 + descriptor index
    OP(65, 7, 7, 32, 20, 34, 10, 10);         // descriptor[index].data[0]
    OP(62, 3, 32, 35);                        // store value
    OP(253, 1, 0);
    OP(56, 1, 0);
#undef OP
    *word_count = count;
    return 1;
}

static int write_dynamic_descriptor_array_fixture(const char *path) {
    uint32_t words[160] = { 0 };
    size_t word_count = 0;
    if (!path || !make_dynamic_descriptor_array_shader(words,
        sizeof words / sizeof words[0], &word_count)) return 0;
    spirv_module module = { 0 };
    int read_result = spirv_read(words, word_count * sizeof words[0], &module);
    if (read_result != SPIRV_OK) {
        fprintf(stderr, "spirv_msl: dynamic descriptor-array fixture rejected: %s (%d)\n",
                module.error, read_result);
        spirv_free(&module);
        return 0;
    }
    spirv_free(&module);
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int make_dynamic_storage_image_array_shader(uint32_t *words, size_t capacity, size_t *word_count) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 41;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                      // Shader capability
    OP(14, 3, 0, 1);                                   // Logical GLSL450 memory model
    OP(15, 7, 5, 30, 0x6e69616du, 0, 20, 21);          // compute main, image array and WorkgroupId
    OP(16, 6, 30, 17, 1, 1, 1);                        // LocalSize 1x1x1
    OP(71, 4, 20, 34, 0);                              // DescriptorSet 0
    OP(71, 4, 20, 33, 0);                              // Binding 0
    OP(71, 3, 20, 25);                                 // storage images are write-only
    OP(71, 4, 21, 11, 26);                             // BuiltIn WorkgroupId
    OP(19, 2, 1);                                      // void
    OP(22, 3, 2, 32);                                  // float
    OP(21, 4, 3, 32, 1);                              // signed int
    OP(21, 4, 4, 32, 0);                              // uint
    OP(23, 4, 5, 2, 4);                               // float4 texel
    OP(23, 4, 6, 3, 2);                               // int2 coordinate
    OP(25, 9, 7, 2, 1, 0, 0, 0, 2, 4);                // 2D storage RGBA8 image
    OP(43, 4, 4, 15, 2);                              // descriptor-array length 2
    OP(28, 4, 12, 7, 15);                             // array of two storage images
    OP(32, 4, 16, 0, 12);                             // UniformConstant pointer to array
    OP(32, 4, 17, 0, 7);                              // UniformConstant pointer to storage image
    OP(23, 4, 18, 4, 3);                              // uint3 WorkgroupId
    OP(32, 4, 19, 1, 18);                             // Input pointer to uint3
    OP(33, 3, 9, 1);                                  // void()
    OP(43, 4, 3, 10, 0);                              // signed int zero
    OP(43, 4, 2, 11, 0x3e800000u);                    // 0.25f
    OP(43, 4, 2, 13, 0x3f000000u);                    // 0.5f
    OP(43, 4, 2, 14, 0x3f400000u);                    // 0.75f
    OP(43, 4, 2, 22, 0x3f800000u);                    // 1.0f
    OP(59, 4, 16, 20, 0);                            // storage-image descriptor array
    OP(59, 4, 19, 21, 1);                            // WorkgroupId input
    OP(54, 5, 1, 30, 0, 9);                           // main
    OP(248, 2, 31);                                   // entry block
    OP(61, 4, 18, 32, 21);                           // load WorkgroupId
    OP(81, 5, 4, 33, 32, 0);                         // x is dynamically uniform per workgroup
    OP(65, 5, 17, 34, 20, 33);                       // storage-image array[WorkgroupId.x]
    OP(61, 4, 7, 35, 34);                            // load storage image
    OP(80, 5, 6, 36, 10, 10);                        // coordinate (0, 0)
    OP(80, 7, 5, 37, 11, 13, 14, 22);               // texel (0.25, 0.5, 0.75, 1)
    OP(99, 4, 35, 36, 37);                           // write selected image
    OP(253, 1);                                       // return
    OP(56, 1);                                        // function end
#undef OP
    *word_count = count;
    return 1;
}

static int write_dynamic_storage_image_array_fixture(const char *path) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!path || !make_dynamic_storage_image_array_shader(words,
        sizeof words / sizeof words[0], &word_count)) return 0;
    spirv_module module = { 0 };
    int read_result = spirv_read(words, word_count * sizeof words[0], &module);
    if (read_result != SPIRV_OK) {
        fprintf(stderr, "spirv_msl: dynamic storage-image array fixture rejected: %s (%d)\n",
                module.error, read_result);
        spirv_free(&module);
        return 0;
    }
    spirv_free(&module);
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int make_dynamic_sampled_image_array_shader(uint32_t *words, size_t capacity,
                                                   size_t *word_count, int separate_sampler,
                                                   uint32_t index_builtin) {
    if (!words || !word_count || capacity < 5) return 0;
    if (index_builtin != 26 && index_builtin != 28) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 64;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                      // Shader capability
    OP(14, 3, 0, 1);                                   // Logical GLSL450 memory model
    if (separate_sampler)
        OP(15, 9, 5, 40, 0x6e69616du, 0, 30, 31, 32, 33); // image, output, WorkgroupId, sampler
    else
        OP(15, 8, 5, 40, 0x6e69616du, 0, 30, 31, 32);    // combined image array, output, WorkgroupId
    OP(16, 6, 40, 17, 1, 1, 1);                        // LocalSize 1x1x1
    OP(71, 4, 30, 34, 0);                              // descriptor array DescriptorSet 0
    OP(71, 4, 30, 33, 0);                              // sampled image array Binding 0
    OP(71, 4, 31, 34, 0);                              // output DescriptorSet 0
    OP(71, 4, 31, 33, separate_sampler ? 2 : 1);       // output Binding
    OP(71, 3, 31, 25);                                 // output image is write-only
    OP(71, 4, 32, 11, index_builtin);                  // BuiltIn WorkgroupId or GlobalInvocationId
    if (separate_sampler) {
        OP(71, 4, 33, 34, 0);                          // sampler array DescriptorSet 0
        OP(71, 4, 33, 33, 1);                          // sampler array Binding 1
    }
    OP(19, 2, 1);                                      // void
    OP(22, 3, 2, 32);                                // float
    OP(21, 4, 3, 32, 0);                              // uint
    OP(21, 4, 4, 32, 1);                              // int
    OP(23, 4, 5, 2, 2);                               // float2
    OP(23, 4, 6, 2, 4);                               // float4
    OP(23, 4, 7, 4, 2);                               // int2
    OP(23, 4, 28, 3, 2);                              // uint2 output coordinates
    OP(25, 9, 8, 2, 1, 0, 0, 0, 1, 0);                // sampled 2D float image
    OP(25, 9, 9, 2, 1, 0, 0, 0, 2, 4);                // storage 2D RGBA8 output image
    OP(27, 3, 10, 8);                                 // combined image-sampler type
    OP(43, 4, 3, 23, 2);                              // array length 2
    OP(28, 4, 12, separate_sampler ? 8 : 10, 23);     // sampled image or combined array
    OP(32, 4, 13, 0, 12);                             // UniformConstant pointer to array
    OP(32, 4, 14, 0, separate_sampler ? 8 : 10);      // UniformConstant pointer to array element
    OP(32, 4, 15, 0, 9);                              // UniformConstant pointer to output image
    OP(23, 4, 16, 3, 3);                              // uint3 WorkgroupId
    OP(32, 4, 17, 1, 16);                             // Input pointer to uint3
    OP(33, 3, 18, 1);                                 // void()
    if (separate_sampler) {
        OP(26, 2, 19);                                // sampler type
        OP(28, 4, 20, 19, 23);                        // sampler descriptor array
        OP(32, 4, 21, 0, 20);                         // UniformConstant pointer to sampler array
        OP(32, 4, 22, 0, 19);                         // UniformConstant pointer to sampler element
    }
    OP(43, 4, 2, 24, 0x3f000000u);                   // sample coordinate component 0.5
    OP(43, 4, 2, 25, 0);                             // explicit LOD 0.0
    OP(43, 4, 4, 26, 0);                             // image-write coordinate component 0
    OP(43, 4, 3, 27, 1);                             // uint one for reverse sampler selection
    OP(43, 4, 3, 29, 0);                             // uint zero output coordinate component
    OP(59, 4, 13, 30, 0);                            // sampled descriptor array
    OP(59, 4, 15, 31, 0);                            // output image
    OP(59, 4, 17, 32, 1);                            // WorkgroupId input
    if (separate_sampler) OP(59, 4, 21, 33, 0);      // separate sampler array
    OP(54, 5, 1, 40, 0, 18);                         // main
    OP(248, 2, 60);                                  // entry block
    OP(61, 4, 16, 41, 32);                          // load WorkgroupId
    OP(81, 5, 3, 42, 41, 0);                        // extract WorkgroupId.x (uniform within workgroup)
    if (separate_sampler) OP(130, 5, 3, 43, 27, 42); // sampler index = 1 - WorkgroupId.x
    OP(65, 5, 14, 44, 30, 42);                      // sampled image descriptor[WorkgroupId.x]
    OP(61, 4, separate_sampler ? 8 : 10, 45, 44);    // load sampled or combined image
    if (separate_sampler) {
        OP(65, 5, 22, 46, 33, 43);                  // sampler descriptor[1 - WorkgroupId.x]
        OP(61, 4, 19, 47, 46);                       // load sampler
        OP(86, 5, 10, 48, 45, 47);                   // OpSampledImage
    }
    OP(80, 5, 5, 49, 24, 24);                       // normalized sample coordinate (0.5, 0.5)
    OP(80, 5, 28, 50, 42, 29);                      // image-write coordinate (WorkgroupId.x, 0)
    OP(88, 7, 6, 51, separate_sampler ? 48 : 45, 49, 2, 25); // explicit LOD sample
    OP(61, 4, 9, 52, 31);                           // load output image
    OP(99, 4, 52, 50, 51);                          // store sampled texel
    OP(253, 1);                                      // return
    OP(56, 1);                                       // function end
#undef OP
    *word_count = count;
    return 1;
}

static int write_dynamic_sampled_image_array_fixture(const char *path, int separate_sampler,
                                                      uint32_t index_builtin) {
    uint32_t words[512] = { 0 };
    size_t word_count = 0;
    if (!path || !make_dynamic_sampled_image_array_shader(words,
        sizeof words / sizeof words[0], &word_count, separate_sampler, index_builtin)) return 0;
    spirv_module module = { 0 };
    int read_result = spirv_read(words, word_count * sizeof words[0], &module);
    if (read_result != SPIRV_OK) {
        fprintf(stderr, "spirv_msl: dynamic sampled-image-array fixture rejected: %s (%d)\n",
                module.error, read_result);
        spirv_free(&module);
        return 0;
    }
    spirv_free(&module);
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

enum { STORAGE_SCALAR_UINT, STORAGE_SCALAR_SINT, STORAGE_SCALAR_FLOAT };

static int make_scalar_storage_shader(uint32_t *words, size_t capacity, size_t *word_count,
                                     uint32_t scalar_kind, uint32_t array_stride) {
    if (!words || !word_count || capacity < 5 || scalar_kind > STORAGE_SCALAR_FLOAT) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 32;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                      // Shader capability
    OP(14, 3, 0, 1);                                   // Logical GLSL450 memory model
    OP(15, 6, 5, 10, 0x6e69616du, 0, 20);             // main, with the storage buffer in its interface
    OP(16, 6, 10, 17, 1, 1, 1);                       // LocalSize 1,1,1
    OP(71, 4, 4, 6, array_stride);                     // runtime-array ArrayStride
    OP(72, 5, 5, 0, 35, 0);                           // block member Offset 0
    OP(71, 3, 5, 2);                                  // Block
    OP(71, 4, 20, 34, 0);                             // DescriptorSet 0
    OP(71, 4, 20, 33, 0);                             // Binding 0
    OP(19, 2, 1);                                      // void
    OP(21, 4, 2, 32, 0);                             // uint index type
    if (scalar_kind == STORAGE_SCALAR_SINT) OP(21, 4, 3, 32, 1); // int element type
    else if (scalar_kind == STORAGE_SCALAR_FLOAT) OP(22, 3, 3, 32); // float element type
    uint32_t element_type = scalar_kind == STORAGE_SCALAR_UINT ? 2u : 3u;
    OP(29, 3, 4, element_type);                       // runtime array of scalar elements
    OP(30, 3, 5, 4);                                  // storage-buffer block
    OP(32, 4, 6, 12, 5);                             // StorageBuffer pointer to block
    OP(32, 4, 7, 12, element_type);                  // StorageBuffer pointer to scalar
    OP(33, 3, 8, 1);                                 // void()
    OP(43, 4, 2, 9, 0);                              // uint 0: block member and first element
    OP(43, 4, 2, 12, 1);                             // uint 1: second element
    if (scalar_kind == STORAGE_SCALAR_UINT) OP(43, 4, 2, 13, 1); // uint 1
    else if (scalar_kind == STORAGE_SCALAR_SINT) OP(43, 4, 3, 13, 1); // int 1
    else OP(43, 4, 3, 13, 0x3f800000u);              // float 1.0
    OP(59, 4, 6, 20, 12);                            // storage-buffer variable
    OP(54, 5, 1, 10, 0, 8);                          // main
    OP(248, 2, 11);                                  // entry block
    OP(65, 6, 7, 14, 20, 9, 9);                     // data[0]
    OP(61, 4, element_type, 15, 14);                 // load element
    OP(scalar_kind == STORAGE_SCALAR_FLOAT ? 129 : 128, 5, element_type, 16, 15, 13);
    OP(65, 6, 7, 17, 20, 9, 12);                    // data[1]
    OP(62, 3, 17, 16);                               // store the incremented value
    OP(253, 1);                                      // return
    OP(56, 1);                                       // OpFunctionEnd
#undef OP
    *word_count = count;
    return 1;
}

static int make_atomic_add_shader(uint32_t *words, size_t capacity, size_t *word_count,
                                  uint32_t scope, uint32_t semantics) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 23;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                      // OpCapability Shader
    OP(14, 3, 0, 1);                                   // Logical GLSL450 memory model
    OP(15, 6, 5, 10, 0x6e69616du, 0, 20);             // main and storage-buffer interface
    OP(16, 6, 10, 17, 64, 1, 1);                      // 64 invocations contend on one counter
    OP(71, 4, 4, 6, 4);                                // runtime-array ArrayStride 4
    OP(72, 5, 5, 0, 35, 0);                           // block member Offset 0
    OP(71, 3, 5, 2);                                   // Block
    OP(71, 4, 20, 34, 0);                             // DescriptorSet 0
    OP(71, 4, 20, 33, 0);                             // Binding 0
    OP(19, 2, 1);                                      // void
    OP(21, 4, 2, 32, 0);                             // uint32
    OP(29, 3, 4, 2);                                  // runtime array of uint32
    OP(30, 3, 5, 4);                                  // storage-buffer block
    OP(32, 4, 6, 12, 5);                             // StorageBuffer pointer to block
    OP(32, 4, 7, 12, 2);                             // StorageBuffer pointer to uint32
    OP(33, 3, 8, 1);                                  // void()
    OP(43, 4, 2, 9, 0);                              // member zero and array index zero
    OP(43, 4, 2, 12, scope);                         // memory scope
    OP(43, 4, 2, 13, semantics);                     // memory semantics
    OP(43, 4, 2, 14, 1);                             // add one per invocation
    OP(59, 4, 6, 20, 12);                            // storage-buffer variable
    OP(54, 5, 1, 10, 0, 8);                          // main
    OP(248, 2, 11);                                  // entry block
    OP(65, 6, 7, 21, 20, 9, 9);                     // counter = data[0]
    OP(234, 7, 2, 22, 21, 12, 13, 14);              // OpAtomicIAdd; result is unused
    OP(253, 1);                                      // return
    OP(56, 1);                                       // OpFunctionEnd
#undef OP
    *word_count = count;
    return 1;
}

static int make_atomic_rmw_shader(uint32_t *words, size_t capacity, size_t *word_count, uint32_t opcode,
                                 uint32_t scope, uint32_t semantics) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 23;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                      // OpCapability Shader
    OP(14, 3, 0, 1);                                   // Logical GLSL450 memory model
    OP(15, 6, 5, 10, 0x6e69616du, 0, 20);             // main and storage-buffer interface
    OP(16, 6, 10, 17, 1, 1, 1);                       // one invocation for translation checks
    OP(71, 4, 4, 6, 4);                                // runtime-array ArrayStride 4
    OP(72, 5, 5, 0, 35, 0);                           // block member Offset 0
    OP(71, 3, 5, 2);                                   // Block
    OP(71, 4, 20, 34, 0);                             // DescriptorSet 0
    OP(71, 4, 20, 33, 0);                             // Binding 0
    OP(19, 2, 1);                                      // void
    OP(21, 4, 2, 32, 0);                             // uint32
    OP(29, 3, 4, 2);                                  // runtime array of uint32
    OP(30, 3, 5, 4);                                  // storage-buffer block
    OP(32, 4, 6, 12, 5);                             // StorageBuffer pointer to block
    OP(32, 4, 7, 12, 2);                             // StorageBuffer pointer to uint32
    OP(33, 3, 8, 1);                                  // void()
    OP(43, 4, 2, 9, 0);                              // data[0]
    OP(43, 4, 2, 12, scope);                         // memory scope
    OP(43, 4, 2, 13, semantics);                     // memory semantics
    OP(43, 4, 2, 14, 1);                             // operation value
    OP(59, 4, 6, 20, 12);                            // storage-buffer variable
    OP(54, 5, 1, 10, 0, 8);                          // main
    OP(248, 2, 11);                                  // entry block
    OP(65, 6, 7, 21, 20, 9, 9);                     // pointer to data[0]
    if (opcode == 227)
        OP(opcode, 6, 2, 22, 21, 12, 13);             // OpAtomicLoad
    else if (opcode == 228)
        OP(opcode, 5, 21, 12, 13, 14);                // OpAtomicStore
    else if (opcode == 232 || opcode == 233)
        OP(opcode, 6, 2, 22, 21, 12, 13);            // OpAtomicIIncrement / OpAtomicIDecrement
    else
        OP(opcode, 7, 2, 22, 21, 12, 13, 14);       // selected OpAtomic* read-modify-write
    OP(253, 1);                                      // return
    OP(56, 1);                                       // OpFunctionEnd
#undef OP
    *word_count = count;
    return 1;
}

static int decorate_atomic_resource_nonwritable(uint32_t *words, size_t capacity, size_t *word_count,
                                                uint32_t variable_id) {
    if (!words || !word_count || *word_count > capacity || *word_count + 3u > capacity) return 0;
    for (size_t cursor = 5; cursor < *word_count;) {
        uint32_t instruction = words[cursor];
        uint32_t instruction_words = instruction >> 16;
        uint32_t opcode = instruction & 0xffffu;
        if (!instruction_words || instruction_words > *word_count - cursor) return 0;
        if (opcode == 19) {
            memmove(words + cursor + 3, words + cursor, (*word_count - cursor) * sizeof words[0]);
            words[cursor] = (3u << 16) | 71u; // OpDecorate
            words[cursor + 1] = variable_id;
            words[cursor + 2] = 24;           // NonWritable
            *word_count += 3;
            return 1;
        }
        cursor += instruction_words;
    }
    return 0;
}

static int check_atomic_load_readonly_pipeline(void) {
    uint32_t words[160] = { 0 };
    size_t word_count = 0;
    if (!make_atomic_rmw_shader(words, sizeof words / sizeof words[0], &word_count, 227, 1, 0x42) ||
        !decorate_atomic_resource_nonwritable(words, sizeof words / sizeof words[0], &word_count, 20))
        return 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    gpu_ir_pipeline *pipeline = NULL;
    gpu_ir_metal_device *device = NULL;
    char *source = NULL;
    size_t source_size = 0;
    char error[512] = { 0 };
    int rc = spirv_read(words, word_count * sizeof words[0], &module);
    const spirv_entry_point *entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    if (!entry && rc == SPIRV_OK) rc = GPU_IR_INVALID;
    if (rc == GPU_IR_OK) rc = spirv_build_gpu_ir_shader(&module, entry, &shader);
    if (rc == GPU_IR_OK) rc = spirv_build_gpu_ir_layout(&module, entry, &layout);
    if (rc == GPU_IR_OK) rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &source, &source_size);
    int ok = rc == GPU_IR_OK && source && source_size &&
        strstr(source, "device const uint* vx_buffer_20") &&
        strstr(source, "atomic_load_explicit(reinterpret_cast<device const atomic_uint*>") &&
        strstr(source, "memory_order_relaxed") &&
        strstr(source, "atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device)");
    if (ok) {
        device = gpu_ir_metal_device_create();
        pipeline = device ? gpu_ir_metal_compute_pipeline_create(device, shader, layout, error, sizeof error) : NULL;
        ok = pipeline != NULL;
    }
    if (!ok) fprintf(stderr, "spirv_msl: atomic load from NonWritable StorageBuffer failed compilation "
                     "(rc=%d, %s, %s)\n%s", rc, module.error, error, source ? source : "");
    else puts("spirv_msl: Device-scope Acquire OpAtomicLoad compiles for a NonWritable StorageBuffer: ok");
    free(source);
    gpu_ir_pipeline_release(pipeline);
    gpu_ir_metal_device_release(device);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int check_atomic_rmw_translation(uint32_t opcode, uint32_t expected_operation,
                                        const char *expected_call, uint32_t scope,
                                        uint32_t semantics, int expect_supported) {
    uint32_t words[128] = { 0 };
    size_t word_count = 0;
    if (!make_atomic_rmw_shader(words, sizeof words / sizeof words[0], &word_count, opcode,
                                scope, semantics)) return 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    char *source = NULL;
    size_t source_size = 0;
    int rc = spirv_read(words, word_count * sizeof words[0], &module);
    const spirv_entry_point *entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    if (!entry && rc == SPIRV_OK) rc = GPU_IR_INVALID;
    if (rc == SPIRV_OK) rc = spirv_build_gpu_ir_shader(&module, entry, &shader);
    if (expect_supported && rc == GPU_IR_OK)
        rc = spirv_build_gpu_ir_layout(&module, entry, &layout);
    if (expect_supported && rc == GPU_IR_OK)
        rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &source, &source_size);
    int found_operation = 0;
    if (rc == GPU_IR_OK && shader) for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (instruction && instruction->opcode == GPU_IR_SHADER_OP_ATOMIC &&
            instruction->operation == expected_operation &&
            instruction->operand_count ==
                ((opcode == 227 || opcode == 232 || opcode == 233) ? 3u : 4u))
            found_operation++;
    }
    const char *expected_order = semantics == 0 ? "memory_order_relaxed" :
        (semantics == 0x42 || semantics == 0x44 || semantics == 0x48) ?
        "atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device)" : NULL;
    int ok = expect_supported ? rc == GPU_IR_OK && found_operation == 1 && source && source_size &&
        expected_call && expected_order && strstr(source, expected_call) && strstr(source, expected_order) &&
        strstr(source, "vx_buffer_ranges[0]") : rc == GPU_IR_UNSUPPORTED;
    if (!ok) fprintf(stderr, "spirv_msl: atomic RMW opcode=%u scope=%u semantics=%u failed "
                     "(rc=%d, operation=%d, %s)\n%s", opcode, scope, semantics, rc,
                     found_operation, module.error, source ? source : "");
    free(source);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int make_atomic_rmw_runtime_shader(uint32_t *words, size_t capacity, size_t *word_count);

static int check_atomic_rmw_translations(void) {
    static const struct {
        uint32_t opcode;
        uint32_t operation;
        const char *call;
    } cases[] = {
        { 229, GPU_IR_SHADER_ATOMIC_INTEGER_EXCHANGE,
          "atomic_exchange_explicit(reinterpret_cast<device atomic_uint*>" },
        { 234, GPU_IR_SHADER_ATOMIC_INTEGER_ADD,
          "atomic_fetch_add_explicit(reinterpret_cast<device atomic_uint*>" },
        { 235, GPU_IR_SHADER_ATOMIC_INTEGER_SUBTRACT,
          "atomic_fetch_sub_explicit(reinterpret_cast<device atomic_uint*>" },
        { 236, GPU_IR_SHADER_ATOMIC_INTEGER_SIGNED_MINIMUM,
          "atomic_fetch_min_explicit(reinterpret_cast<device atomic_int*>" },
        { 237, GPU_IR_SHADER_ATOMIC_INTEGER_UNSIGNED_MINIMUM,
          "atomic_fetch_min_explicit(reinterpret_cast<device atomic_uint*>" },
        { 238, GPU_IR_SHADER_ATOMIC_INTEGER_SIGNED_MAXIMUM,
          "atomic_fetch_max_explicit(reinterpret_cast<device atomic_int*>" },
        { 239, GPU_IR_SHADER_ATOMIC_INTEGER_UNSIGNED_MAXIMUM,
          "atomic_fetch_max_explicit(reinterpret_cast<device atomic_uint*>" },
        { 240, GPU_IR_SHADER_ATOMIC_INTEGER_AND,
          "atomic_fetch_and_explicit(reinterpret_cast<device atomic_uint*>" },
        { 241, GPU_IR_SHADER_ATOMIC_INTEGER_OR,
          "atomic_fetch_or_explicit(reinterpret_cast<device atomic_uint*>" },
        { 242, GPU_IR_SHADER_ATOMIC_INTEGER_XOR,
          "atomic_fetch_xor_explicit(reinterpret_cast<device atomic_uint*>" },
        { 232, GPU_IR_SHADER_ATOMIC_INTEGER_INCREMENT,
          "vx_value_22 = as_type<uint>(atomic_fetch_add_explicit(reinterpret_cast<device atomic_uint*>" },
        { 233, GPU_IR_SHADER_ATOMIC_INTEGER_DECREMENT,
          "vx_value_22 = as_type<uint>(atomic_fetch_sub_explicit(reinterpret_cast<device atomic_uint*>" },
        { 227, GPU_IR_SHADER_ATOMIC_INTEGER_LOAD,
          "vx_value_22 = as_type<uint>(atomic_load_explicit(reinterpret_cast<device const atomic_uint*>" },
        { 228, GPU_IR_SHADER_ATOMIC_INTEGER_STORE,
          "atomic_store_explicit(reinterpret_cast<device atomic_uint*>" }
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        if (!check_atomic_rmw_translation(cases[i].opcode, cases[i].operation, cases[i].call, 1, 0, 1))
            return 0;
    if (!check_atomic_rmw_translation(229, GPU_IR_SHADER_ATOMIC_INTEGER_EXCHANGE, NULL, 2, 0, 0) ||
        !check_atomic_rmw_translation(234, GPU_IR_SHADER_ATOMIC_INTEGER_ADD, NULL, 1, 2, 0) ||
        !check_atomic_rmw_translation(232, GPU_IR_SHADER_ATOMIC_INTEGER_INCREMENT, NULL, 2, 0, 0) ||
        !check_atomic_rmw_translation(233, GPU_IR_SHADER_ATOMIC_INTEGER_DECREMENT, NULL, 1, 2, 0) ||
        !check_atomic_rmw_translation(227, GPU_IR_SHADER_ATOMIC_INTEGER_LOAD, NULL, 2, 0, 0) ||
        !check_atomic_rmw_translation(227, GPU_IR_SHADER_ATOMIC_INTEGER_LOAD, NULL, 1, 0x44, 0) ||
        !check_atomic_rmw_translation(228, GPU_IR_SHADER_ATOMIC_INTEGER_STORE, NULL, 1, 2, 0) ||
        !check_atomic_rmw_translation(228, GPU_IR_SHADER_ATOMIC_INTEGER_STORE, NULL, 1, 0x42, 0)) return 0;
    if (!check_atomic_rmw_translation(234, GPU_IR_SHADER_ATOMIC_INTEGER_ADD,
            "atomic_fetch_add_explicit(reinterpret_cast<device atomic_uint*>", 1, 0x42, 1) ||
        !check_atomic_rmw_translation(234, GPU_IR_SHADER_ATOMIC_INTEGER_ADD,
            "atomic_fetch_add_explicit(reinterpret_cast<device atomic_uint*>", 1, 0x44, 1) ||
        !check_atomic_rmw_translation(234, GPU_IR_SHADER_ATOMIC_INTEGER_ADD,
            "atomic_fetch_add_explicit(reinterpret_cast<device atomic_uint*>", 1, 0x48, 1)) return 0;
    if (!check_atomic_rmw_translation(227, GPU_IR_SHADER_ATOMIC_INTEGER_LOAD,
            "atomic_load_explicit(reinterpret_cast<device const atomic_uint*>", 1, 0x42, 1) ||
        !check_atomic_rmw_translation(228, GPU_IR_SHADER_ATOMIC_INTEGER_STORE,
            "atomic_store_explicit(reinterpret_cast<device atomic_uint*>", 1, 0x44, 1)) return 0;
    if (!check_atomic_load_readonly_pipeline()) return 0;

    uint32_t words[384] = { 0 };
    size_t word_count = 0;
    if (!make_atomic_rmw_runtime_shader(words, sizeof words / sizeof words[0], &word_count)) return 0;
    spirv_module module = { 0 };
    gpu_ir_shader *unresolved = NULL, *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    char *source = NULL;
    size_t source_size = 0;
    const char *failed_stage = "reader";
    int rc = spirv_read(words, word_count * sizeof words[0], &module);
    const spirv_entry_point *entry = NULL;
    if (rc == SPIRV_OK) {
        failed_stage = "entry point";
        entry = spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE);
        if (!entry) rc = GPU_IR_INVALID;
    }
    if (rc == GPU_IR_OK) {
        failed_stage = "shader lowering";
        rc = spirv_build_gpu_ir_shader(&module, entry, &unresolved);
    }
    if (rc == GPU_IR_OK) {
        failed_stage = "specialization";
        rc = gpu_ir_shader_specialize(unresolved, NULL, 0, entry->local_size, &shader);
    }
    if (rc == GPU_IR_OK) {
        failed_stage = "layout lowering";
        rc = spirv_build_gpu_ir_layout(&module, entry, &layout);
    }
    if (rc == GPU_IR_OK) {
        failed_stage = "MSL emission";
        rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &source, &source_size);
    }
    uint32_t atomics = 0;
    if (rc == GPU_IR_OK && shader) for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (instruction && instruction->opcode == GPU_IR_SHADER_OP_ATOMIC) atomics++;
    }
    int ok = rc == GPU_IR_OK && atomics == 13 && source && source_size;
    if (!ok) fprintf(stderr, "spirv_msl: atomic RMW runtime fixture lowering failed at %s (rc=%d, atomics=%u, %s)\n",
                     failed_stage, rc, atomics, module.error);
    else puts("spirv_msl: contention fixture lowers 11 integer OpAtomic RMW operations plus load/store: ok");
    free(source);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    gpu_ir_shader_release(unresolved);
    spirv_free(&module);
    return ok;
}

static int make_atomic_rmw_runtime_shader(uint32_t *words, size_t capacity, size_t *word_count) {
    static const uint32_t opcodes[] = { 229, 235, 236, 237, 238, 239, 240, 241, 242, 232, 233 };
    static const uint32_t values[] = { 7, 1, 17, 17, 17, 17, 0x0ff0, 0x5a00, 0x00ff, 0, 0 };
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 128;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                      // OpCapability Shader
    OP(14, 3, 0, 1);                                   // Logical GLSL450 memory model
    OP(15, 6, 5, 40, 0x6e69616du, 0, 20);             // main and storage-buffer interface
    OP(16, 6, 40, 17, 63, 1, 1);                      // 63 invocations exercise odd-count XOR
    OP(71, 4, 4, 6, 4);                                // runtime-array ArrayStride 4
    OP(72, 5, 5, 0, 35, 0);                           // block member Offset 0
    OP(71, 3, 5, 2);                                   // Block
    OP(71, 4, 20, 34, 0);                             // DescriptorSet 0
    OP(71, 4, 20, 33, 0);                             // Binding 0
    OP(19, 2, 1);                                      // void
    OP(21, 4, 2, 32, 0);                             // uint32
    OP(29, 3, 4, 2);                                  // runtime array of uint32
    OP(30, 3, 5, 4);                                  // storage-buffer block
    OP(32, 4, 6, 12, 5);                             // StorageBuffer pointer to block
    OP(32, 4, 7, 12, 2);                             // StorageBuffer pointer to uint32
    OP(33, 3, 8, 1);                                  // void()
    OP(43, 4, 2, 9, 0);                              // struct member index zero
    OP(43, 4, 2, 12, 1);                             // Device scope
    OP(43, 4, 2, 13, 0x48);                          // UniformMemory | AcquireRelease for integer RMW operations
    OP(43, 4, 2, 63, 0x42);                          // UniformMemory | Acquire for atomic load
    OP(43, 4, 2, 64, 0x44);                          // UniformMemory | Release for atomic store
    OP(43, 4, 2, 61, 11);                            // atomic-load source element
    OP(43, 4, 2, 62, 12);                            // atomic-store destination element
    for (uint32_t i = 0; i < 11; i++) {
        OP(43, 4, 2, 50 + i, i);                     // array index
        if (i < 9) OP(43, 4, 2, 70 + i, values[i]); // RMW operand
    }
    OP(59, 4, 6, 20, 12);                            // storage-buffer variable
    OP(54, 5, 1, 40, 0, 8);                         // main
    OP(248, 2, 41);                                 // entry block
    for (uint32_t i = 0; i < 11; i++) {
        OP(65, 6, 7, 90 + i, 20, 9, 50 + i);        // pointer to data[i]
        if (opcodes[i] == 232 || opcodes[i] == 233)
            OP(opcodes[i], 6, 2, 110 + i, 90 + i, 12, 13);
        else
            OP(opcodes[i], 7, 2, 110 + i, 90 + i, 12, 13, 70 + i);
    }
    OP(65, 6, 7, 101, 20, 9, 61);                  // pointer to data[11]
    OP(227, 6, 2, 121, 101, 12, 63);               // acquire-load old value
    OP(65, 6, 7, 102, 20, 9, 62);                  // pointer to data[12]
    OP(228, 5, 102, 12, 64, 121);                  // release-store loaded value
    OP(253, 1);                                      // return
    OP(56, 1);                                       // OpFunctionEnd
#undef OP
    *word_count = count;
    return 1;
}

static int write_atomic_rmw_fixture(const char *path) {
    uint32_t words[384] = { 0 };
    size_t word_count = 0;
    if (!make_atomic_rmw_runtime_shader(words, sizeof words / sizeof words[0], &word_count)) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int check_atomic_add_translation(uint32_t scope, uint32_t semantics, int expect_supported) {
    uint32_t words[128] = { 0 };
    size_t word_count = 0;
    if (!make_atomic_add_shader(words, sizeof words / sizeof words[0], &word_count, scope, semantics)) return 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    char *source = NULL;
    size_t source_size = 0;
    int rc = spirv_read(words, word_count * sizeof words[0], &module);
    const spirv_entry_point *entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    if (!entry && rc == SPIRV_OK) rc = GPU_IR_INVALID;
    if (rc == SPIRV_OK) rc = spirv_build_gpu_ir_shader(&module, entry, &shader);
    if (expect_supported && rc == GPU_IR_OK)
        rc = spirv_build_gpu_ir_layout(&module, entry, &layout);
    if (expect_supported && rc == GPU_IR_OK)
        rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &source, &source_size);
    int ok;
    if (expect_supported)
        ok = rc == GPU_IR_OK && source && source_size &&
            strstr(source, "atomic_fetch_add_explicit(reinterpret_cast<device atomic_uint*>") &&
            strstr(source, "memory_order_relaxed") && strstr(source, "vx_buffer_ranges[0]");
    else
        ok = rc == GPU_IR_UNSUPPORTED;
    if (!ok) fprintf(stderr, "spirv_msl: atomic add scope=%u semantics=%u failed (rc=%d, %s)\n%s",
                     scope, semantics, rc, module.error, source ? source : "");
    free(source);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int write_atomic_add_fixture(const char *path) {
    uint32_t words[128] = { 0 };
    size_t word_count = 0;
    if (!make_atomic_add_shader(words, sizeof words / sizeof words[0], &word_count, 1, 0)) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int make_atomic_compare_exchange_shader(uint32_t *words, size_t capacity, size_t *word_count,
                                              uint32_t scope, uint32_t equal_semantics,
                                              uint32_t unequal_semantics) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 25;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                      // OpCapability Shader
    OP(14, 3, 0, 1);                                   // Logical GLSL450 memory model
    OP(15, 6, 5, 10, 0x6e69616du, 0, 20);             // main and storage-buffer interface
    OP(16, 6, 10, 17, 64, 1, 1);                      // 64 invocations contend on two words
    OP(71, 4, 4, 6, 4);                                // runtime-array ArrayStride 4
    OP(72, 5, 5, 0, 35, 0);                           // block member Offset 0
    OP(71, 3, 5, 2);                                   // Block
    OP(71, 4, 20, 34, 0);                             // DescriptorSet 0
    OP(71, 4, 20, 33, 0);                             // Binding 0
    OP(19, 2, 1);                                      // void
    OP(21, 4, 2, 32, 0);                             // uint32
    OP(29, 3, 4, 2);                                  // runtime array of uint32
    OP(30, 3, 5, 4);                                  // storage-buffer block
    OP(32, 4, 6, 12, 5);                             // StorageBuffer pointer to block
    OP(32, 4, 7, 12, 2);                             // StorageBuffer pointer to uint32
    OP(33, 3, 8, 1);                                  // void()
    OP(43, 4, 2, 9, 0);                              // data[0] index
    OP(43, 4, 2, 12, scope);                         // memory scope
    OP(43, 4, 2, 13, equal_semantics);               // equal memory semantics
    OP(43, 4, 2, 14, unequal_semantics);             // unequal memory semantics
    OP(43, 4, 2, 15, 1);                             // first replacement value
    OP(43, 4, 2, 16, 41);                            // first comparator
    OP(43, 4, 2, 17, 1);                             // data[1] index
    OP(59, 4, 6, 20, 12);                            // storage-buffer variable
    OP(54, 5, 1, 10, 0, 8);                          // main
    OP(248, 2, 11);                                  // entry block
    OP(65, 6, 7, 21, 20, 9, 9);                     // pointer to data[0]
    OP(230, 9, 2, 22, 21, 12, 13, 14, 15, 16);      // CAS 41 -> 1; result is the old value
    OP(65, 6, 7, 23, 20, 9, 17);                    // pointer to data[1]
    OP(230, 9, 2, 24, 23, 12, 13, 14, 22, 9);      // CAS 0 -> first old value
    OP(253, 1);                                      // return
    OP(56, 1);                                       // OpFunctionEnd
#undef OP
    *word_count = count;
    return 1;
}

static int check_atomic_compare_exchange_translation(uint32_t scope, uint32_t equal_semantics,
                                                      uint32_t unequal_semantics, int expect_supported) {
    uint32_t words[160] = { 0 };
    size_t word_count = 0;
    if (!make_atomic_compare_exchange_shader(words, sizeof words / sizeof words[0], &word_count,
                                               scope, equal_semantics, unequal_semantics)) return 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    char *source = NULL;
    size_t source_size = 0;
    int rc = spirv_read(words, word_count * sizeof words[0], &module);
    const spirv_entry_point *entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    if (!entry && rc == SPIRV_OK) rc = GPU_IR_INVALID;
    if (rc == SPIRV_OK) rc = spirv_build_gpu_ir_shader(&module, entry, &shader);
    if (expect_supported && rc == GPU_IR_OK)
        rc = spirv_build_gpu_ir_layout(&module, entry, &layout);
    if (expect_supported && rc == GPU_IR_OK)
        rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &source, &source_size);
    int valid_atomic = 0;
    if (rc == GPU_IR_OK && shader) for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (instruction && instruction->opcode == GPU_IR_SHADER_OP_ATOMIC &&
            instruction->operation == GPU_IR_SHADER_ATOMIC_INTEGER_COMPARE_EXCHANGE &&
            instruction->operand_count == 6) valid_atomic++;
    }
    int has_ordering_fence = equal_semantics != 0 || unequal_semantics != 0;
    int ok;
    if (expect_supported)
        ok = rc == GPU_IR_OK && valid_atomic == 2 && source && source_size &&
            strstr(source, "atomic_compare_exchange_weak_explicit(") &&
            strstr(source, "while (vx_expected_") &&
            (has_ordering_fence ? strstr(source,
                "atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device)") :
                strstr(source, "memory_order_relaxed, memory_order_relaxed")) &&
            strstr(source, "vx_buffer_ranges[0]");
    else
        ok = rc == GPU_IR_UNSUPPORTED;
    if (!ok) fprintf(stderr, "spirv_msl: compare-exchange scope=%u equal=%u unequal=%u failed "
                     "(rc=%d, valid=%d, %s)\n%s", scope, equal_semantics, unequal_semantics,
                     rc, valid_atomic, module.error, source ? source : "");
    free(source);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int write_atomic_compare_exchange_fixture(const char *path) {
    uint32_t words[160] = { 0 };
    size_t word_count = 0;
    if (!make_atomic_compare_exchange_shader(words, sizeof words / sizeof words[0], &word_count,
                                               1, 0x48, 0x42))
        return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int prepare_scalar_storage_shader(uint32_t scalar_kind, uint32_t array_stride,
                                         spirv_module *module, gpu_ir_shader **shader,
                                         gpu_ir_pipeline_layout **layout) {
    uint32_t words[128] = { 0 };
    size_t word_count = 0;
    if (!module || !shader || !layout ||
        !make_scalar_storage_shader(words, sizeof words / sizeof words[0], &word_count,
                                    scalar_kind, array_stride)) return GPU_IR_INVALID;
    int rc = spirv_read(words, word_count * sizeof words[0], module);
    if (rc != SPIRV_OK) return GPU_IR_INVALID;
    const spirv_entry_point *entry = spirv_find_entry_point(module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE);
    gpu_ir_shader *unresolved = NULL;
    rc = entry ? spirv_build_gpu_ir_shader(module, entry, &unresolved) : GPU_IR_NOT_FOUND;
    uint32_t threadgroup[3] = { 1, 1, 1 };
    if (rc == GPU_IR_OK) rc = gpu_ir_shader_specialize(unresolved, NULL, 0, threadgroup, shader);
    gpu_ir_shader_release(unresolved);
    if (rc == GPU_IR_OK) rc = spirv_build_gpu_ir_layout(module, entry, layout);
    if (rc != GPU_IR_OK) {
        gpu_ir_shader_release(*shader);
        *shader = NULL;
        gpu_ir_pipeline_layout_release(*layout);
        *layout = NULL;
    }
    return rc;
}

static int make_uniform_buffer_shader(uint32_t *words, size_t capacity, size_t *word_count) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 48;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) return 0; \
} while (0)
    OP(17, 2, 1);                                      // Shader capability
    OP(14, 3, 0, 1);                                   // Logical GLSL450 memory model
    OP(15, 8, 5, 14, 0x6e69616du, 0, 20, 21, 22);     // main, storage buffer, UBO and GlobalInvocationId
    OP(16, 6, 14, 17, 1, 1, 1);                       // LocalSize 1,1,1
    OP(71, 3, 5, 2);                                   // UBO Block
    OP(72, 5, 5, 0, 35, 0);                           // UBO member Offset 0
    OP(72, 5, 5, 1, 35, 16);                          // UBO member Offset 16
    OP(72, 5, 5, 2, 35, 20);                          // UBO signed member Offset 20
    OP(72, 5, 5, 3, 35, 24);                          // UBO float member Offset 24
    OP(71, 4, 8, 6, 4);                               // storage runtime-array ArrayStride 4
    OP(72, 5, 9, 0, 35, 0);                           // storage block member Offset 0
    OP(71, 3, 9, 2);                                   // storage Block
    OP(71, 4, 20, 34, 0);                             // storage DescriptorSet 0
    OP(71, 4, 20, 33, 0);                             // storage Binding 0
    OP(71, 4, 21, 34, 0);                             // UBO DescriptorSet 0
    OP(71, 4, 21, 33, 1);                             // UBO Binding 1
    OP(71, 4, 22, 11, 28);                            // GlobalInvocationId
    OP(19, 2, 1);                                      // void
    OP(21, 4, 2, 32, 0);                             // uint
    OP(21, 4, 33, 32, 1);                            // int
    OP(22, 3, 34, 32);                               // float
    OP(23, 4, 3, 2, 3);                              // uint3
    OP(30, 6, 5, 2, 2, 33, 34);                    // UBO block struct
    OP(29, 3, 8, 2);                                 // runtime array of uint
    OP(30, 3, 9, 8);                                 // storage block struct
    OP(32, 4, 10, 12, 9);                            // StorageBuffer pointer to block
    OP(32, 4, 11, 12, 2);                            // StorageBuffer pointer to uint
    OP(32, 4, 12, 2, 5);                             // Uniform pointer to block
    OP(32, 4, 13, 2, 2);                             // Uniform pointer to uint
    OP(32, 4, 35, 2, 33);                            // Uniform pointer to int
    OP(32, 4, 36, 2, 34);                            // Uniform pointer to float
    OP(32, 4, 16, 1, 3);                             // Input pointer to uint3
    OP(33, 3, 17, 1);                                // void()
    OP(43, 4, 2, 18, 0);                             // uint zero
    OP(43, 4, 2, 28, 1);                             // uint one
    OP(43, 4, 2, 37, 2);                             // uint two
    OP(43, 4, 2, 38, 3);                             // uint three
    OP(59, 4, 10, 20, 12);                           // storage buffer variable
    OP(59, 4, 12, 21, 2);                            // uniform buffer variable
    OP(59, 4, 16, 22, 1);                            // GlobalInvocationId variable
    OP(54, 5, 1, 14, 0, 17);                         // main
    OP(248, 2, 15);                                  // entry block
    OP(61, 4, 3, 23, 22);                            // load GlobalInvocationId
    OP(81, 5, 2, 24, 23, 0);                         // gid.x
    OP(65, 5, 13, 25, 21, 18);                      // UBO member zero
    OP(61, 4, 2, 26, 25);                            // load UBO value
    OP(65, 5, 13, 29, 21, 28);                      // UBO member one
    OP(61, 4, 2, 30, 29);                            // load second UBO value
    OP(65, 5, 35, 39, 21, 37);                      // UBO signed member two
    OP(61, 4, 33, 40, 39);                           // load signed UBO value
    OP(65, 5, 36, 41, 21, 38);                      // UBO float member three
    OP(61, 4, 34, 42, 41);                           // load float UBO value
    OP(128, 5, 2, 31, 26, 30);                      // sum the two UBO fields
    OP(65, 6, 11, 27, 20, 18, 24);                 // output[gid.x]
    OP(62, 3, 27, 31);                               // output = sum of UBO fields
    OP(253, 1);                                      // return
    OP(56, 1);                                      // OpFunctionEnd
#undef OP
    *word_count = count;
    return 1;
}

static int write_uniform_buffer_fixture(const char *path) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!path || !make_uniform_buffer_shader(words, sizeof words / sizeof words[0], &word_count)) return 0;
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    char *source = NULL;
    size_t source_size = 0;
    int ok = 0;
    int read_result = spirv_read(words, word_count * sizeof words[0], &module);
    if (read_result != SPIRV_OK) {
        fprintf(stderr, "spirv_msl: uniform-buffer SPIR-V rejected: %s (%d)\n", module.error, read_result);
        goto done;
    }
    const spirv_entry_point *entry = spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE);
    int shader_result = entry ? spirv_build_gpu_ir_shader(&module, entry, &shader) : GPU_IR_NOT_FOUND;
    if (shader_result != GPU_IR_OK) {
        fprintf(stderr, "spirv_msl: uniform-buffer shader lowering failed (%d)\n", shader_result);
        goto done;
    }
    int layout_result = spirv_build_gpu_ir_layout(&module, entry, &layout);
    if (layout_result != GPU_IR_OK) {
        fprintf(stderr, "spirv_msl: uniform-buffer layout reflection failed (%d)\n", layout_result);
        goto done;
    }
    int emit_result = gpu_ir_msl_emit_compute_with_layout(shader, layout, &source, &source_size);
    if (emit_result != GPU_IR_OK) {
        fprintf(stderr, "spirv_msl: uniform-buffer MSL emission failed (%d)\n", emit_result);
        goto done;
    }
    if (
        !source || !source_size || !strstr(source, "constant uint* vx_uniform_21 [[buffer(1)]]") ||
        !strstr(source, "vx_value_26 = (*vx_uniform_21);") ||
        !strstr(source, "reinterpret_cast<constant uint*>(vx_uniform_21 + 4)") ||
        !strstr(source, "vx_value_30 = (*reinterpret_cast<constant uint*>(vx_uniform_21 + 4));") ||
        !strstr(source, "reinterpret_cast<constant int*>(vx_uniform_21 + 5)") ||
        !strstr(source, "vx_value_40 = (*reinterpret_cast<constant int*>(vx_uniform_21 + 5));") ||
        !strstr(source, "reinterpret_cast<constant float*>(vx_uniform_21 + 6)") ||
        !strstr(source, "vx_value_42 = (*reinterpret_cast<constant float*>(vx_uniform_21 + 6));") ||
        !strstr(source, "device uint* vx_buffer_20 [[buffer(0)]]")) goto done;
    FILE *file = fopen(path, "wb");
    if (!file) goto done;
    ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
done:
    if (!ok) fprintf(stderr, "spirv_msl: uniform-buffer fixture failed\n%s",
                     source ? source : "");
    free(source);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int make_push_constant_shader(uint32_t *words, size_t capacity, size_t *word_count) {
    if (!words || !word_count || capacity < 5) return 0;
    words[0] = SPIRV_MAGIC;
    words[1] = 0x00010600u;
    words[2] = 0;
    words[3] = 38;
    words[4] = 0;
    size_t count = 5;
#define OP(code, n, ...) do { \
    const uint32_t operands_[] = { __VA_ARGS__ }; \
    if (!append_instruction(words, capacity, &count, code, n, operands_)) { \
        fprintf(stderr, "spirv_msl: could not append push-constant opcode %u (%u words) at %zu\n", \
                (unsigned)(code), (unsigned)(n), count); \
        return 0; \
    } \
} while (0)
    OP(17, 2, 1);                                      // Shader capability
    OP(14, 3, 0, 1);                                   // Logical GLSL450 memory model
    OP(15, 7, 5, 30, 0x6e69616du, 0, 20, 21);         // main, storage buffer and push constants
    OP(16, 6, 30, 17, 1, 1, 1);                       // LocalSize 1,1,1
    OP(71, 4, 3, 6, 4);                               // storage runtime-array ArrayStride 4
    OP(71, 3, 4, 2);                                  // storage block
    OP(72, 5, 4, 0, 35, 0);                          // storage block member Offset 0
    OP(71, 3, 5, 2);                                  // push-constant block
    OP(72, 5, 5, 0, 35, 0);                          // push value a at byte 0
    OP(72, 5, 5, 1, 35, 4);                          // push value b at byte 4
    OP(71, 4, 20, 34, 0);                            // DescriptorSet 0
    OP(71, 4, 20, 33, 0);                            // Binding 0
    OP(19, 2, 1);                                     // void
    OP(21, 4, 2, 32, 0);                            // uint
    OP(29, 3, 3, 2);                                 // runtime array of uint
    OP(30, 3, 4, 3);                                 // storage block struct
    OP(30, 4, 5, 2, 2);                             // push block { uint, uint }
    OP(32, 4, 6, 12, 4);                            // StorageBuffer pointer to block
    OP(32, 4, 7, 12, 2);                            // StorageBuffer pointer to uint
    OP(32, 4, 8, 9, 5);                             // PushConstant pointer to block
    OP(32, 4, 9, 9, 2);                             // PushConstant pointer to uint
    OP(33, 3, 10, 1);                               // void()
    OP(43, 4, 2, 11, 0);                            // uint zero
    OP(43, 4, 2, 12, 1);                            // uint one
    OP(59, 4, 6, 20, 12);                          // storage buffer variable
    OP(59, 4, 8, 21, 9);                           // push-constant variable
    OP(54, 5, 1, 30, 0, 10);                       // main function
    OP(248, 2, 31);                                 // entry block
    OP(65, 5, 9, 32, 21, 11);                     // push_constants.a
    OP(61, 4, 2, 33, 32);                          // load a
    OP(65, 5, 9, 34, 21, 12);                     // push_constants.b
    OP(61, 4, 2, 35, 34);                          // load b
    OP(128, 5, 2, 36, 33, 35);                    // sum = a + b
    OP(65, 6, 7, 37, 20, 11, 11);                // storage.values[0]
    OP(62, 3, 37, 36);                             // store sum
    OP(253, 1);                                    // return
    OP(56, 1);                                     // OpFunctionEnd
#undef OP
    *word_count = count;
    return 1;
}

static int write_push_constant_fixture(const char *path) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!path) return 0;
    if (!make_push_constant_shader(words, sizeof words / sizeof words[0], &word_count)) {
        fprintf(stderr, "spirv_msl: could not construct push-constant SPIR-V fixture\n");
        return 0;
    }
    spirv_module module = { 0 };
    int read_result = spirv_read(words, word_count * sizeof words[0], &module);
    if (read_result != SPIRV_OK) {
        fprintf(stderr, "spirv_msl: push-constant fixture rejected: %s (%d)\n", module.error, read_result);
        spirv_free(&module);
        return 0;
    }
    const spirv_entry_point *entry = spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE);
    gpu_ir_pipeline_layout *layout = NULL;
    int layout_result = entry ? spirv_build_gpu_ir_layout(&module, entry, &layout) : GPU_IR_NOT_FOUND;
    gpu_ir_push_constant_range range = { 0 };
    int ok = layout_result == GPU_IR_OK && layout &&
        gpu_ir_pipeline_layout_push_constant_count(layout) == 1 &&
        gpu_ir_pipeline_layout_push_constant_at(layout, 0, &range) == GPU_IR_OK &&
        range.offset == 0 && range.size == 8 && range.stage_mask == GPU_IR_STAGE_COMPUTE;
    if (!ok) fprintf(stderr, "spirv_msl: push-constant range reflection failed (%d)\n", layout_result);
    gpu_ir_pipeline_layout_release(layout);
    spirv_free(&module);
    if (!ok) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    ok = fwrite(words, sizeof words[0], word_count, file) == word_count && !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int execute_on_metal(gpu_ir_metal_device *device, const gpu_ir_shader *shader,
                            gpu_ir_pipeline_layout *layout, const uint32_t threadgroup[3],
                            uint64_t descriptor_range) {
    enum { ELEMENTS = 64 };
    uint32_t values[ELEMENTS * 2];
    for (uint32_t i = 0; i < ELEMENTS * 2; i++) values[i] = 0x100u + i;
    gpu_ir_resource *buffer = gpu_ir_metal_buffer_create(device, sizeof values, values);
    if (!buffer || !descriptor_range || descriptor_range > sizeof values) {
        fprintf(stderr, "spirv_msl: could not allocate a shared-IR Metal buffer or invalid test range\n");
        gpu_ir_resource_release(buffer);
        return 0;
    }

    char error[512];
    gpu_ir_pipeline *pipeline = gpu_ir_metal_compute_pipeline_create(device, shader, layout, error, sizeof error);
    if (!pipeline) {
        fprintf(stderr, "spirv_msl: shared-IR Metal pipeline failed: %s\n", error);
        gpu_ir_resource_release(buffer);
        return 0;
    }
    gpu_ir_descriptor_value value = {
        .set = 0, .binding_namespace = 0, .binding = 0, .array_element = 0,
        .kind = GPU_IR_DESCRIPTOR_STORAGE_BUFFER, .stage_mask = GPU_IR_STAGE_COMPUTE,
        .access_mask = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE,
        .resource = buffer, .range = descriptor_range
    };
    gpu_ir_descriptor_state_desc state_description = { .values = &value, .value_count = 1 };
    gpu_ir_descriptor_state *state = gpu_ir_descriptor_state_create(layout, &state_description);
    if (!state) {
        fprintf(stderr, "spirv_msl: common descriptor snapshot creation failed\n");
        gpu_ir_pipeline_release(pipeline);
        gpu_ir_resource_release(buffer);
        return 0;
    }

    gpu_ir_stream stream;
    gpu_ir_stream_init(&stream);
    uint32_t index = GPU_IR_NO_COMMAND;
    int rc = gpu_ir_record_bind_descriptor_state(&stream, state, &index);
    if (rc == GPU_IR_OK) rc = gpu_ir_record_dispatch_with_accesses(&stream, pipeline,
        ELEMENTS / threadgroup[0], 1, 1,
        &(gpu_ir_resource_access){ buffer, GPU_IR_STAGE_COMPUTE,
            GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE }, 1, &index);
    if (rc == GPU_IR_OK) rc = gpu_ir_metal_execute_stream(device, &stream);
    if (rc != GPU_IR_OK) fprintf(stderr, "spirv_msl: common GPU IR stream execution failed (%d)\n", rc);

    const uint32_t *result = gpu_ir_metal_buffer_contents(buffer);
    int ok = rc == GPU_IR_OK && result;
    uint64_t array_length = descriptor_range / sizeof(uint32_t);
    for (uint32_t i = 0; ok && i < ELEMENTS * 2; i++) {
        uint32_t expected = 0x100u + i;
        if (i < ELEMENTS) expected = i == 7 ? 999u : i * 2u + 1u;
        else if (i < array_length) {
            uint32_t invocation = i - ELEMENTS;
            expected = 0xe000097au + (uint32_t)array_length + (i - ELEMENTS) +
                (invocation < threadgroup[0] ? invocation : invocation % threadgroup[0]) +
                200u * invocation + 26u + 4u * invocation;
        }
        if (result[i] != expected) {
            fprintf(stderr, "spirv_msl: value %u was %u; expected %u\n", i, result[i], expected);
            ok = 0;
        }
    }
    gpu_ir_stream_destroy(&stream);
    gpu_ir_descriptor_state_release(state);
    gpu_ir_pipeline_release(pipeline);
    gpu_ir_resource_release(buffer);
    return ok;
}

static int make_robust_load_shader(uint32_t *words, size_t capacity, size_t *word_count) {
    if (!make_arithmetic_shader(words, capacity, word_count, 0) || capacity - *word_count < 6) return 0;
    size_t insert_offset = 0;
    int local_size_found = 0, load_access_found = 0, store_found = 0;
    for (size_t cursor = 5; cursor < *word_count;) {
        uint32_t instruction = words[cursor];
        uint16_t count = (uint16_t)(instruction >> 16);
        uint16_t opcode = (uint16_t)instruction;
        if (!count || count > *word_count - cursor) return 0;
        if (opcode == 50 && count == 4 && words[cursor + 2] == 45) {
            words[cursor + 3] = 1;
            local_size_found = 1;
        } else if (opcode == 65 && count == 6 && words[cursor + 2] == 17) {
            // Make the load index element one, outside the descriptor's one-element range.
            words[cursor + 5] = 14;
            insert_offset = cursor + count;
            load_access_found = 1;
        } else if (opcode == 62 && count == 3 && words[cursor + 1] == 17) {
            // Store the loaded value to element zero so the out-of-range load's zero is observable.
            words[cursor + 1] = 128;
            words[cursor + 2] = 18;
            store_found = 1;
        }
        cursor += count;
    }
    if (!local_size_found || !load_access_found || !store_found) return 0;
    const uint32_t store_access[] = { (6u << 16) | 65u, 8, 128, 11, 13, 13 };
    memmove(words + insert_offset + 6, words + insert_offset,
            (*word_count - insert_offset) * sizeof *words);
    memcpy(words + insert_offset, store_access, sizeof store_access);
    *word_count += 6;
    words[3] = 420; // Preserve the bound for the GLSL import and extended bit-scan operations.
    return 1;
}

static int execute_robust_load_on_metal(gpu_ir_metal_device *device) {
    uint32_t words[2048] = { 0 };
    size_t word_count = 0;
    if (!device || !make_robust_load_shader(words, sizeof words / sizeof words[0], &word_count)) return 0;

    spirv_module module = { 0 };
    gpu_ir_shader *unresolved_shader = NULL, *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    gpu_ir_resource *buffer = NULL;
    gpu_ir_pipeline *pipeline = NULL;
    gpu_ir_descriptor_state *state = NULL;
    gpu_ir_stream stream;
    int stream_initialized = 0, ok = 0;

    int read_rc = spirv_read(words, word_count * sizeof words[0], &module);
    if (read_rc != SPIRV_OK) {
        fprintf(stderr, "spirv_msl: robust-load fixture reader failed (%d: %s)\n", read_rc, module.error);
        goto done;
    }
    const spirv_entry_point *entry = spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE);
    int build_rc = entry ? spirv_build_gpu_ir_shader(&module, entry, &unresolved_shader) : GPU_IR_INVALID;
    if (!entry || build_rc != GPU_IR_OK) {
        fprintf(stderr, "spirv_msl: robust-load fixture lowering failed (%d)\n", build_rc);
        goto done;
    }
    uint32_t default_threads[3] = { 0, 0, 0 };
    if (!entry->has_local_size_default) goto done;
    for (uint32_t axis = 0; axis < 3; axis++) default_threads[axis] = (uint32_t)entry->local_size_default[axis];
    int specialize_rc = gpu_ir_shader_specialize(unresolved_shader, NULL, 0, default_threads, &shader);
    if (specialize_rc != GPU_IR_OK) {
        fprintf(stderr, "spirv_msl: robust-load fixture specialization failed (%d)\n", specialize_rc);
        goto done;
    }
    gpu_ir_shader_threadgroup_size(shader, default_threads);
    if (default_threads[0] != 1 || default_threads[1] != 1 || default_threads[2] != 1 ||
        spirv_build_gpu_ir_layout(&module, entry, &layout) != GPU_IR_OK) {
        fprintf(stderr, "spirv_msl: robust-load fixture layout failed\n");
        goto done;
    }

    enum { ELEMENTS = 16 };
    uint32_t values[ELEMENTS];
    for (uint32_t i = 0; i < ELEMENTS; i++) values[i] = 0x6a09e667u + i;
    buffer = gpu_ir_metal_buffer_create(device, sizeof values, values);
    if (!buffer) {
        fprintf(stderr, "spirv_msl: robust-load Metal buffer allocation failed\n");
        goto done;
    }
    char error[512];
    pipeline = gpu_ir_metal_compute_pipeline_create(device, shader, layout, error, sizeof error);
    if (!pipeline) {
        fprintf(stderr, "spirv_msl: robust-load Metal pipeline failed: %s\n", error);
        goto done;
    }
    gpu_ir_descriptor_value descriptor = {
        .set = 0, .binding_namespace = 0, .binding = 0, .array_element = 0,
        .kind = GPU_IR_DESCRIPTOR_STORAGE_BUFFER, .stage_mask = GPU_IR_STAGE_COMPUTE,
        .access_mask = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE,
        .resource = buffer, .range = sizeof(uint32_t)
    };
    gpu_ir_descriptor_state_desc state_description = { .values = &descriptor, .value_count = 1 };
    state = gpu_ir_descriptor_state_create(layout, &state_description);
    if (!state) {
        fprintf(stderr, "spirv_msl: robust-load descriptor snapshot failed\n");
        goto done;
    }
    gpu_ir_stream_init(&stream);
    stream_initialized = 1;
    int rc = gpu_ir_record_bind_descriptor_state(&stream, state, NULL);
    if (rc == GPU_IR_OK) rc = gpu_ir_record_dispatch_with_accesses(&stream, pipeline, 1, 1, 1,
        &(gpu_ir_resource_access){ buffer, GPU_IR_STAGE_COMPUTE,
            GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE }, 1, NULL);
    int execute_rc = rc == GPU_IR_OK ? gpu_ir_metal_execute_stream(device, &stream) : rc;
    if (execute_rc != GPU_IR_OK) {
        fprintf(stderr, "spirv_msl: robust-load stream execution failed (%d)\n", execute_rc);
        goto done;
    }
    const uint32_t *result = gpu_ir_metal_buffer_contents(buffer);
    ok = result && result[0] == 0;
    for (uint32_t i = 1; ok && i < ELEMENTS; i++)
        if (result[i] != values[i]) ok = 0;
    if (!ok) fprintf(stderr, "spirv_msl: out-of-range load did not produce zero without corrupting adjacent data\n");

done:
    if (stream_initialized) gpu_ir_stream_destroy(&stream);
    gpu_ir_descriptor_state_release(state);
    gpu_ir_pipeline_release(pipeline);
    gpu_ir_resource_release(buffer);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    gpu_ir_shader_release(unresolved_shader);
    spirv_free(&module);
    return ok;
}

static int execute_scalar_storage_on_metal(gpu_ir_metal_device *device, uint32_t scalar_kind) {
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    gpu_ir_resource *buffer = NULL;
    gpu_ir_pipeline *pipeline = NULL;
    gpu_ir_descriptor_state *state = NULL;
    gpu_ir_stream stream;
    int stream_initialized = 0, ok = 0;
    char *msl = NULL;
    size_t msl_size = 0;

    int rc = prepare_scalar_storage_shader(scalar_kind, 4, &module, &shader, &layout);
    if (rc != GPU_IR_OK) {
        fprintf(stderr, "spirv_msl: scalar storage-buffer lowering failed (%u, %d)\n", scalar_kind, rc);
        goto done;
    }
    rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &msl, &msl_size);
    const char *scalar_name = scalar_kind == STORAGE_SCALAR_FLOAT ? "float" :
        scalar_kind == STORAGE_SCALAR_SINT ? "int" : "uint";
    char expected_parameter[96];
    snprintf(expected_parameter, sizeof expected_parameter, "device %s* vx_buffer_20 [[buffer(0)]]", scalar_name);
    if (rc != GPU_IR_OK || !msl || !msl_size || !strstr(msl, expected_parameter) ||
        !strstr(msl, "vx_buffer_ranges[0]")) {
        fprintf(stderr, "spirv_msl: scalar storage-buffer MSL did not preserve typed access (%u, %d)\n",
                scalar_kind, rc);
        goto done;
    }
    free(msl);
    msl = NULL;

    uint32_t initial = scalar_kind == STORAGE_SCALAR_FLOAT ? 0x40000000u :
        scalar_kind == STORAGE_SCALAR_SINT ? (uint32_t)-40 : 41u;
    uint32_t expected = scalar_kind == STORAGE_SCALAR_FLOAT ? 0x40400000u :
        scalar_kind == STORAGE_SCALAR_SINT ? (uint32_t)-39 : 42u;
    uint32_t values[2] = { initial, 0xa5a5a5a5u };
    buffer = gpu_ir_metal_buffer_create(device, sizeof values, values);
    if (!buffer) {
        fprintf(stderr, "spirv_msl: scalar storage-buffer allocation failed (%u)\n", scalar_kind);
        goto done;
    }
    char error[512];
    pipeline = gpu_ir_metal_compute_pipeline_create(device, shader, layout, error, sizeof error);
    if (!pipeline) {
        fprintf(stderr, "spirv_msl: scalar storage-buffer Metal pipeline failed (%u): %s\n",
                scalar_kind, error);
        goto done;
    }
    gpu_ir_descriptor_value descriptor = {
        .set = 0, .binding_namespace = 0, .binding = 0, .array_element = 0,
        .kind = GPU_IR_DESCRIPTOR_STORAGE_BUFFER, .stage_mask = GPU_IR_STAGE_COMPUTE,
        .access_mask = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE,
        .resource = buffer, .range = sizeof values
    };
    gpu_ir_descriptor_state_desc state_description = { .values = &descriptor, .value_count = 1 };
    state = gpu_ir_descriptor_state_create(layout, &state_description);
    if (!state) {
        fprintf(stderr, "spirv_msl: scalar storage-buffer descriptor snapshot failed (%u)\n", scalar_kind);
        goto done;
    }
    gpu_ir_stream_init(&stream);
    stream_initialized = 1;
    rc = gpu_ir_record_bind_descriptor_state(&stream, state, NULL);
    if (rc == GPU_IR_OK) rc = gpu_ir_record_dispatch_with_accesses(&stream, pipeline, 1, 1, 1,
        &(gpu_ir_resource_access){ buffer, GPU_IR_STAGE_COMPUTE,
            GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE }, 1, NULL);
    if (rc == GPU_IR_OK) rc = gpu_ir_metal_execute_stream(device, &stream);
    const uint32_t *result = gpu_ir_metal_buffer_contents(buffer);
    ok = rc == GPU_IR_OK && result && result[0] == initial && result[1] == expected;
    if (!ok) fprintf(stderr, "spirv_msl: scalar storage-buffer result mismatch (%u, rc=%d, got=%08x)\n",
                     scalar_kind, rc, result ? result[1] : 0);

done:
    free(msl);
    if (stream_initialized) gpu_ir_stream_destroy(&stream);
    gpu_ir_descriptor_state_release(state);
    gpu_ir_pipeline_release(pipeline);
    gpu_ir_resource_release(buffer);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok;
}

static int rejects_padded_scalar_storage_stride(void) {
    spirv_module module = { 0 };
    gpu_ir_shader *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    char *msl = NULL;
    size_t msl_size = 0;
    int rc = prepare_scalar_storage_shader(STORAGE_SCALAR_FLOAT, 8, &module, &shader, &layout);
    if (rc == GPU_IR_OK) rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &msl, &msl_size);
    int rejected = rc == GPU_IR_UNSUPPORTED && !msl && !msl_size;
    if (!rejected) fprintf(stderr, "spirv_msl: unsupported padded storage-buffer stride was accepted (%d)\n", rc);
    free(msl);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return rejected;
}

static int rejects_bad_array_length_member(void) {
    uint32_t words[2048] = { 0 };
    size_t word_count = 0;
    if (!make_arithmetic_shader(words, sizeof words / sizeof words[0], &word_count, 1)) return 0;
    spirv_module module = { 0 };
    int read_rc = spirv_read(words, word_count * sizeof words[0], &module);
    const spirv_entry_point *entry = read_rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    int has_entry = entry != NULL;
    gpu_ir_shader *shader = NULL;
    int lower_rc = entry ? spirv_build_gpu_ir_shader(&module, entry, &shader) : GPU_IR_INVALID;
    int shader_is_null = shader == NULL;
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    int ok = read_rc == SPIRV_OK && has_entry && lower_rc == GPU_IR_INVALID && shader_is_null;
    if (!ok) fprintf(stderr, "spirv_msl: bad array-length member check read=%d entry=%d lower=%d shader_null=%d\n",
                     read_rc, has_entry, lower_rc, shader_is_null);
    return ok;
}

static int rejects_bad_vector_math_type(uint16_t target_opcode) {
    uint32_t words[2048] = { 0 };
    size_t word_count = 0;
    if (!make_arithmetic_shader(words, sizeof words / sizeof words[0], &word_count, 0)) return 0;
    int found = 0;
    for (size_t cursor = 5; cursor < word_count;) {
        uint16_t count = (uint16_t)(words[cursor] >> 16);
        uint16_t opcode = (uint16_t)words[cursor];
        if (!count || count > word_count - cursor) return 0;
        if (opcode == target_opcode && count == 5) {
            // Replace a required vector/scalar input with a valid but differently typed value.
            words[cursor + 4] = target_opcode == 142 ? 25u : 32u;
            found = 1;
            break;
        }
        cursor += count;
    }
    if (!found) return 0;
    spirv_module module = { 0 };
    int read_rc = spirv_read(words, word_count * sizeof words[0], &module);
    const spirv_entry_point *entry = read_rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    gpu_ir_shader *shader = NULL;
    int lower_rc = entry ? spirv_build_gpu_ir_shader(&module, entry, &shader) : GPU_IR_INVALID;
    int rejected = read_rc == SPIRV_OK && entry && lower_rc == GPU_IR_INVALID && !shader;
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return rejected;
}

static int rejects_bad_bit_count_type(void) {
    uint32_t words[2048] = { 0 };
    size_t word_count = 0;
    if (!make_arithmetic_shader(words, sizeof words / sizeof words[0], &word_count, 0)) return 0;
    int found = 0;
    for (size_t cursor = 5; cursor < word_count;) {
        uint16_t count = (uint16_t)(words[cursor] >> 16);
        uint16_t opcode = (uint16_t)words[cursor];
        if (!count || count > word_count - cursor) return 0;
        if (opcode == 205 && count == 4) {
            words[cursor + 3] = 31; // OpBitCount requires an integer scalar or vector base.
            found = 1;
            break;
        }
        cursor += count;
    }
    if (!found) return 0;
    spirv_module module = { 0 };
    int read_rc = spirv_read(words, word_count * sizeof words[0], &module);
    const spirv_entry_point *entry = read_rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    gpu_ir_shader *shader = NULL;
    int lower_rc = entry ? spirv_build_gpu_ir_shader(&module, entry, &shader) : GPU_IR_INVALID;
    int rejected = read_rc == SPIRV_OK && entry && lower_rc == GPU_IR_INVALID && !shader;
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return rejected;
}

static int rejects_bad_bit_reverse_type(void) {
    uint32_t words[2048] = { 0 };
    size_t word_count = 0;
    if (!make_arithmetic_shader(words, sizeof words / sizeof words[0], &word_count, 0)) return 0;
    int found = 0;
    for (size_t cursor = 5; cursor < word_count;) {
        uint16_t count = (uint16_t)(words[cursor] >> 16);
        uint16_t opcode = (uint16_t)words[cursor];
        if (!count || count > word_count - cursor) return 0;
        if (opcode == 204 && count == 4) {
            words[cursor + 3] = 25; // OpBitReverse requires Base to have exactly the result type.
            found = 1;
            break;
        }
        cursor += count;
    }
    if (!found) return 0;
    spirv_module module = { 0 };
    int read_rc = spirv_read(words, word_count * sizeof words[0], &module);
    const spirv_entry_point *entry = read_rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    gpu_ir_shader *shader = NULL;
    int lower_rc = entry ? spirv_build_gpu_ir_shader(&module, entry, &shader) : GPU_IR_INVALID;
    int rejected = read_rc == SPIRV_OK && entry && lower_rc == GPU_IR_INVALID && !shader;
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return rejected;
}

static int rejects_bad_bit_field_type(void) {
    uint32_t words[2048] = { 0 };
    size_t word_count = 0;
    if (!make_arithmetic_shader(words, sizeof words / sizeof words[0], &word_count, 0)) return 0;
    int found = 0;
    for (size_t cursor = 5; cursor < word_count;) {
        uint16_t count = (uint16_t)(words[cursor] >> 16);
        uint16_t opcode = (uint16_t)words[cursor];
        if (!count || count > word_count - cursor) return 0;
        if (opcode == 203 && count == 6) {
            words[cursor + 5] = 39; // OpBitFieldUExtract requires a scalar Count operand.
            found = 1;
            break;
        }
        cursor += count;
    }
    if (!found) return 0;
    spirv_module module = { 0 };
    int read_rc = spirv_read(words, word_count * sizeof words[0], &module);
    const spirv_entry_point *entry = read_rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    gpu_ir_shader *shader = NULL;
    int lower_rc = entry ? spirv_build_gpu_ir_shader(&module, entry, &shader) : GPU_IR_INVALID;
    int rejected = read_rc == SPIRV_OK && entry && lower_rc == GPU_IR_INVALID && !shader;
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return rejected;
}

static int rejects_nonzero_sampled_fetch_lod(void) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!make_image_copy_shader(words, sizeof words / sizeof words[0], &word_count, 1, 0, 1)) return 0;
    int found = 0;
    for (size_t cursor = 5; cursor < word_count;) {
        uint16_t count = (uint16_t)(words[cursor] >> 16);
        uint16_t opcode = (uint16_t)words[cursor];
        if (!count || count > word_count - cursor) return 0;
        if (opcode == 43 && count == 4 && words[cursor + 2] == 9) {
            words[cursor + 3] = 1;
            found = 1;
            break;
        }
        cursor += count;
    }
    if (!found) return 0;
    spirv_module module = { 0 };
    int read_rc = spirv_read(words, word_count * sizeof words[0], &module);
    const spirv_entry_point *entry = read_rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    gpu_ir_shader *shader = NULL;
    int lower_rc = entry ? spirv_build_gpu_ir_shader(&module, entry, &shader) : GPU_IR_INVALID;
    int rejected = read_rc == SPIRV_OK && entry && lower_rc == GPU_IR_UNSUPPORTED && !shader;
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return rejected;
}

static int accepts_nonzero_image_query_lod(void) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!make_image_size_query_shader(words, sizeof words / sizeof words[0], &word_count, 103, 0)) return 0;
    if (!set_image_query_lod(words, word_count, 10)) return 0;
    spirv_module module = { 0 };
    int read_rc = spirv_read(words, word_count * sizeof words[0], &module);
    const spirv_entry_point *entry = read_rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    gpu_ir_shader *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    char *source = NULL;
    size_t source_size = 0;
    int rc = entry ? spirv_build_gpu_ir_shader(&module, entry, &shader) : GPU_IR_INVALID;
    if (rc == GPU_IR_OK) rc = spirv_build_gpu_ir_layout(&module, entry, &layout);
    if (rc == GPU_IR_OK) rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &source, &source_size);
    int accepted = read_rc == SPIRV_OK && entry && rc == GPU_IR_OK && source && source_size &&
        strstr(source, "get_width(uint(") && strstr(source, "get_height(uint(");
    if (!accepted) fprintf(stderr, "spirv_msl: nonzero OpImageQuerySizeLod was not preserved (read=%d rc=%d)\n%s",
                           read_rc, rc, source ? source : "");
    free(source);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return accepted;
}

static int rejects_bad_image_query_levels_result_type(void) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!make_image_size_query_shader(words, sizeof words / sizeof words[0], &word_count, 106, 0)) return 0;
    int found = 0;
    for (size_t cursor = 5; cursor < word_count;) {
        uint16_t count = (uint16_t)(words[cursor] >> 16);
        uint16_t opcode = (uint16_t)words[cursor];
        if (!count || count > word_count - cursor) return 0;
        if (opcode == 106 && count == 4) {
            words[cursor + 1] = 4; // OpImageQueryLevels requires a scalar integer result.
            found = 1;
            break;
        }
        cursor += count;
    }
    if (!found) return 0;
    spirv_module module = { 0 };
    int read_rc = spirv_read(words, word_count * sizeof words[0], &module);
    const spirv_entry_point *entry = read_rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    gpu_ir_shader *shader = NULL;
    int lower_rc = entry ? spirv_build_gpu_ir_shader(&module, entry, &shader) : GPU_IR_INVALID;
    int rejected = read_rc == SPIRV_OK && entry && lower_rc == GPU_IR_UNSUPPORTED && !shader;
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return rejected;
}

static void release_test_metal_object(void *object) {
    if (object) CFRelease(object);
}

static int execute_image_copy_on_metal(gpu_ir_metal_device *backend, int sampled_fetch) {
    uint32_t words[256] = { 0 };
    size_t word_count = 0;
    if (!backend || !make_image_copy_shader(words, sizeof words / sizeof words[0], &word_count,
                                             sampled_fetch, 0, 1))
        return 0;

    spirv_module module = { 0 };
    gpu_ir_shader *unresolved_shader = NULL, *shader = NULL;
    gpu_ir_pipeline_layout *layout = NULL;
    gpu_ir_resource *source_resource = NULL, *destination_resource = NULL;
    gpu_ir_pipeline *pipeline = NULL;
    gpu_ir_descriptor_state *state = NULL;
    gpu_ir_stream stream;
    int stream_initialized = 0, ok = 0;
    char *msl = NULL;
    size_t msl_size = 0;
    id<MTLDevice> metal_device = MTLCreateSystemDefaultDevice();
    MTLTextureDescriptor *source_description = nil, *destination_description = nil;
    id<MTLTexture> source_texture = nil, destination_texture = nil;
    MTLRegion pixel_region = MTLRegionMake2D(0, 0, 1, 1);
    const uint8_t expected[4] = { 37, 99, 171, 255 };
    const uint8_t empty[4] = { 0, 0, 0, 0 };
    if (!metal_device) goto done;

    int rc = spirv_read(words, word_count * sizeof words[0], &module);
    const spirv_entry_point *entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    rc = entry ? spirv_build_gpu_ir_shader(&module, entry, &unresolved_shader) : GPU_IR_INVALID;
    if (rc != GPU_IR_OK || !unresolved_shader) {
        fprintf(stderr, "spirv_msl: image shader lowering failed (%d: %s)\n", rc, module.error);
        goto done;
    }
    uint32_t default_threads[3] = { 1, 1, 1 };
    rc = gpu_ir_shader_specialize(unresolved_shader, NULL, 0, default_threads, &shader);
    if (rc == GPU_IR_OK) rc = spirv_build_gpu_ir_layout(&module, entry, &layout);
    if (rc != GPU_IR_OK || !shader || !layout) {
        fprintf(stderr, "spirv_msl: image specialization or layout failed (%d)\n", rc);
        goto done;
    }
    rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &msl, &msl_size);
    if (rc != GPU_IR_OK || !msl || !msl_size ||
        !strstr(msl, "texture2d<float, access::read> vx_image_20 [[texture(0)]]") ||
        !strstr(msl, "texture2d<float, access::write> vx_image_21 [[texture(1)]]") ||
        !strstr(msl, "vx_value_45 = vx_capture_20.read(uint2(vx_value_43));") ||
        !strstr(msl, "vx_capture_21.write(vx_value_45, uint2(vx_value_43));")) {
        fprintf(stderr, "spirv_msl: image MSL emission failed (%d)\n%s", rc, msl ? msl : "");
        goto done;
    }

    source_description = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    source_description.storageMode = MTLStorageModeShared;
    source_description.usage = MTLTextureUsageShaderRead;
    destination_description = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    destination_description.storageMode = MTLStorageModeShared;
    destination_description.usage = MTLTextureUsageShaderWrite;
    source_texture = [metal_device newTextureWithDescriptor:source_description];
    destination_texture = [metal_device newTextureWithDescriptor:destination_description];
    if (!source_texture || !destination_texture) {
        fprintf(stderr, "spirv_msl: storage-image Metal texture allocation failed\n");
        goto done;
    }
    [source_texture replaceRegion:pixel_region mipmapLevel:0 withBytes:expected bytesPerRow:4];
    [destination_texture replaceRegion:pixel_region mipmapLevel:0 withBytes:empty bytesPerRow:4];

    gpu_ir_texture_desc texture_description = {
        .width = 1, .height = 1, .depth = 1, .array_layers = 1, .mip_levels = 1,
        .bytes_per_texel = 4, .sample_count = 1, .block_width = 1, .block_height = 1,
        .bytes_per_block = 4, .format = GPU_IR_FORMAT_RGBA8_UNORM
    };
    void *owned_source = (__bridge_retained void *)source_texture;
    void *owned_destination = (__bridge_retained void *)destination_texture;
    source_resource = gpu_ir_texture_create(&texture_description, owned_source, release_test_metal_object);
    if (!source_resource) release_test_metal_object(owned_source);
    destination_resource = gpu_ir_texture_create(&texture_description, owned_destination,
                                                   release_test_metal_object);
    if (!destination_resource) release_test_metal_object(owned_destination);
    if (!source_resource || !destination_resource) {
        fprintf(stderr, "spirv_msl: storage-image common texture resources failed\n");
        goto done;
    }

    char error[512];
    pipeline = gpu_ir_metal_compute_pipeline_create(backend, shader, layout, error, sizeof error);
    if (!pipeline) {
        fprintf(stderr, "spirv_msl: storage-image Metal pipeline failed: %s\n", error);
        goto done;
    }
    gpu_ir_descriptor_value values[] = {
        { .set = 0, .binding_namespace = 0, .binding = 0, .array_element = 0,
          .kind = sampled_fetch ? GPU_IR_DESCRIPTOR_SAMPLED_IMAGE : GPU_IR_DESCRIPTOR_STORAGE_IMAGE,
          .stage_mask = GPU_IR_STAGE_COMPUTE,
          .access_mask = GPU_IR_ACCESS_SHADER_READ, .resource = source_resource },
        { .set = 0, .binding_namespace = 0, .binding = 1, .array_element = 0,
          .kind = GPU_IR_DESCRIPTOR_STORAGE_IMAGE, .stage_mask = GPU_IR_STAGE_COMPUTE,
          .access_mask = GPU_IR_ACCESS_SHADER_WRITE, .resource = destination_resource }
    };
    gpu_ir_descriptor_state_desc state_description = { .values = values, .value_count = 2 };
    state = gpu_ir_descriptor_state_create(layout, &state_description);
    if (!state) {
        fprintf(stderr, "spirv_msl: storage-image descriptor snapshot failed\n");
        goto done;
    }
    gpu_ir_stream_init(&stream);
    stream_initialized = 1;
    rc = gpu_ir_record_bind_descriptor_state(&stream, state, NULL);
    gpu_ir_resource_access accesses[] = {
        { source_resource, GPU_IR_STAGE_COMPUTE, GPU_IR_ACCESS_SHADER_READ },
        { destination_resource, GPU_IR_STAGE_COMPUTE, GPU_IR_ACCESS_SHADER_WRITE }
    };
    if (rc == GPU_IR_OK)
        rc = gpu_ir_record_dispatch_with_accesses(&stream, pipeline, 1, 1, 1, accesses, 2, NULL);
    if (rc == GPU_IR_OK) rc = gpu_ir_metal_execute_stream(backend, &stream);
    if (rc != GPU_IR_OK) {
        fprintf(stderr, "spirv_msl: image Metal dispatch failed (%d)\n", rc);
        goto done;
    }
    uint8_t actual[4] = { 0 };
    [destination_texture getBytes:actual bytesPerRow:4 fromRegion:pixel_region mipmapLevel:0];
    ok = memcmp(actual, expected, sizeof expected) == 0;
    if (!ok) fprintf(stderr, "spirv_msl: storage-image read/write mismatch (%u, %u, %u, %u)\n",
                     actual[0], actual[1], actual[2], actual[3]);

done:
    free(msl);
    if (stream_initialized) gpu_ir_stream_destroy(&stream);
    gpu_ir_descriptor_state_release(state);
    gpu_ir_pipeline_release(pipeline);
    gpu_ir_resource_release(source_resource);
    gpu_ir_resource_release(destination_resource);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    gpu_ir_shader_release(unresolved_shader);
    spirv_free(&module);
    return ok;
}

static int attachment_clear_on_metal(gpu_ir_metal_device *backend) {
    enum { WIDTH = 16, HEIGHT = 8 };
    id<MTLDevice> metal_device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [metal_device newCommandQueue];
    MTLTextureDescriptor *texture_descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:WIDTH height:HEIGHT mipmapped:NO];
    texture_descriptor.storageMode = MTLStorageModePrivate;
    texture_descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [metal_device newTextureWithDescriptor:texture_descriptor];
    id<MTLBuffer> readback = [metal_device newBufferWithLength:WIDTH * HEIGHT * 4
        options:MTLResourceStorageModeShared];
    if (!backend || !metal_device || !queue || !texture || !readback) {
        fprintf(stderr, "spirv_msl: could not create Metal attachment-clear resources\n");
        return 0;
    }

    gpu_ir_texture_desc description = {
        .width = WIDTH, .height = HEIGHT, .depth = 1, .array_layers = 1, .mip_levels = 1,
        .bytes_per_texel = 4, .sample_count = 1, .block_width = 1, .block_height = 1,
        .bytes_per_block = 4, .format = GPU_IR_FORMAT_RGBA8_UNORM
    };
    void *owned_texture = (__bridge_retained void *)texture;
    gpu_ir_resource *resource = gpu_ir_texture_create(&description, owned_texture, release_test_metal_object);
    if (!resource) {
        release_test_metal_object(owned_texture);
        fprintf(stderr, "spirv_msl: common IR rejected the Metal color attachment\n");
        return 0;
    }

    const float blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    const gpu_ir_rect rectangles[] = { { -3, 1, 6, 5 }, { 4, 3, 12, HEIGHT } };
    gpu_ir_stream stream;
    gpu_ir_stream_init(&stream);
    uint32_t full_index = GPU_IR_NO_COMMAND, rect_index = GPU_IR_NO_COMMAND;
    int rc = gpu_ir_record_clear_color(&stream, resource, blue, NULL, 0, &full_index);
    if (rc == GPU_IR_OK)
        rc = gpu_ir_record_clear_color(&stream, resource, red, rectangles,
            sizeof rectangles / sizeof rectangles[0], &rect_index);
    gpu_ir_resource_release(resource);
    if (rc != GPU_IR_OK || full_index != 0 || rect_index != 1 ||
        gpu_ir_metal_execute_stream(backend, &stream) != GPU_IR_OK) {
        fprintf(stderr, "spirv_msl: Metal backend did not execute common full and rectangular clears\n");
        gpu_ir_stream_destroy(&stream);
        return 0;
    }
    gpu_ir_stream_destroy(&stream);

    id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    if (!blit) {
        fprintf(stderr, "spirv_msl: could not create a Metal attachment readback encoder\n");
        return 0;
    }
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
        sourceSize:MTLSizeMake(WIDTH, HEIGHT, 1) toBuffer:readback destinationOffset:0
        destinationBytesPerRow:WIDTH * 4 destinationBytesPerImage:WIDTH * HEIGHT * 4];
    [blit endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
        fprintf(stderr, "spirv_msl: Metal attachment clear readback failed: %s\n",
                command_buffer.error.localizedDescription.UTF8String ?: "unknown Metal error");
        return 0;
    }

    const uint8_t *pixels = readback.contents;
    uint32_t wrong = 0;
    for (uint32_t y = 0; y < HEIGHT; y++) for (uint32_t x = 0; x < WIDTH; x++) {
        int cleared = (x < 6 && y >= 1 && y < 5) || (x >= 4 && x < 12 && y >= 3);
        const uint8_t *pixel = pixels + ((size_t)y * WIDTH + x) * 4;
        uint8_t expected[4] = { cleared ? 255 : 0, 0, cleared ? 0 : 255, 255 };
        if (memcmp(pixel, expected, sizeof expected)) wrong++;
    }
    if (wrong) fprintf(stderr, "spirv_msl: common Metal attachment clears produced %u wrong pixels\n", wrong);
    else puts("gpu_ir_metal: full and clipped overlapping color attachment clears, 0 wrong pixels: ok");
    return wrong == 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--check-graphics-stage"))
        return check_graphics_stage_msl() ? 0 : 1;
    if (argc == 2 && !strcmp(argv[1], "--check-fragment-termination"))
        return check_fragment_termination_shader(NULL) ? 0 : 1;
    if (argc == 3 && !strcmp(argv[1], "--write-fragment-termination"))
        return check_fragment_termination_shader(argv[2]) ? 0 : 1;
    if (argc == 2 && !strcmp(argv[1], "--check-graphics-sampled-image-layout"))
        return check_graphics_sampled_image_layout() ? 0 : 1;
    if (argc == 3 && !strcmp(argv[1], "--emit-graphics-sampled-image-layout"))
        return write_graphics_sampled_image_layout(argv[2], 0) ? 0 : 1;
    if (argc == 3 && !strcmp(argv[1], "--emit-graphics-sampled-image-implicit-layout"))
        return write_graphics_sampled_image_layout(argv[2], 1) ? 0 : 1;
    if (argc == 3 && !strcmp(argv[1], "--emit-graphics-separate-image-sampler-layout"))
        return write_graphics_separate_image_sampler_layout(argv[2], 0) ? 0 : 1;
    if (argc == 3 && !strcmp(argv[1], "--emit-graphics-separate-image-sampler-implicit-layout"))
        return write_graphics_separate_image_sampler_layout(argv[2], 1) ? 0 : 1;
    if (argc == 3 && !strcmp(argv[1], "--emit-vertex-input"))
        return write_graphics_vertex_input_msl(argv[2]) ? 0 : 1;
    if (argc == 3 && (!strcmp(argv[1], "--emit-uint-vertex-input") ||
                      !strcmp(argv[1], "--emit-sint-vertex-input"))) {
        int signed_input = !strcmp(argv[1], "--emit-sint-vertex-input");
        return write_graphics_integer_vertex_input_msl(argv[2], signed_input) ? 0 : 1;
    }
    if (argc == 2 && !strcmp(argv[1], "--check-integer-vertex-inputs"))
        return check_graphics_integer_vertex_attribute_msl() ? 0 : 1;
    if (argc == 3 && !strcmp(argv[1], "--write-fixture")) {
        if (!write_fixture(argv[2])) {
            fprintf(stderr, "spirv_msl: could not write SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && (!strcmp(argv[1], "--write-graphics-vertex-fixture") ||
                      !strcmp(argv[1], "--write-graphics-fragment-fixture"))) {
        int vertex = !strcmp(argv[1], "--write-graphics-vertex-fixture");
        if (!write_graphics_stage_fixture(argv[2], vertex)) {
            fprintf(stderr, "spirv_msl: could not write graphics SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-graphics-sampled-image-fixture")) {
        if (!write_graphics_sampled_image_fixture(argv[2], 0)) {
            fprintf(stderr, "spirv_msl: could not write graphics sampled-image fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-graphics-sampled-image-implicit-fixture")) {
        if (!write_graphics_sampled_image_fixture(argv[2], 1)) {
            fprintf(stderr, "spirv_msl: could not write graphics implicit-LOD sampled-image fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-graphics-separate-image-sampler-fixture")) {
        if (!write_graphics_separate_image_sampler_fixture(argv[2], 0)) {
            fprintf(stderr, "spirv_msl: could not write graphics separate image/sampler fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-graphics-separate-image-sampler-implicit-fixture")) {
        if (!write_graphics_separate_image_sampler_fixture(argv[2], 1)) {
            fprintf(stderr, "spirv_msl: could not write graphics implicit-LOD separate image/sampler fixture: %s\n",
                    argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && (!strcmp(argv[1], "--write-graphics-uint-vertex-fixture") ||
                      !strcmp(argv[1], "--write-graphics-sint-vertex-fixture"))) {
        int signed_input = !strcmp(argv[1], "--write-graphics-sint-vertex-fixture");
        if (!write_graphics_integer_vertex_fixture(argv[2], signed_input)) {
            fprintf(stderr, "spirv_msl: could not write integer vertex SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-atomic-add-fixture")) {
        if (!write_atomic_add_fixture(argv[2])) {
            fprintf(stderr, "spirv_msl: could not write atomic-add SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-atomic-compare-exchange-fixture")) {
        if (!write_atomic_compare_exchange_fixture(argv[2])) {
            fprintf(stderr, "spirv_msl: could not write atomic compare-exchange SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-atomic-rmw-fixture")) {
        if (!write_atomic_rmw_fixture(argv[2])) {
            fprintf(stderr, "spirv_msl: could not write atomic RMW SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-storage-image-fixture")) {
        if (!write_storage_image_fixture(argv[2])) {
            fprintf(stderr, "spirv_msl: could not write storage-image SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-sampled-image-fixture")) {
        if (!write_sampled_image_fixture(argv[2])) {
            fprintf(stderr, "spirv_msl: could not write sampled-image SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-image-size-fixture")) {
        if (!write_image_size_query_fixture(argv[2], 104, 0)) {
            fprintf(stderr, "spirv_msl: could not write OpImageQuerySize SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-image-size-lod-fixture")) {
        if (!write_image_size_query_fixture(argv[2], 103, 0)) {
            fprintf(stderr, "spirv_msl: could not write OpImageQuerySizeLod SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-image-size-lod1-fixture")) {
        if (!write_image_size_lod1_fixture(argv[2])) {
            fprintf(stderr, "spirv_msl: could not write nonzero OpImageQuerySizeLod SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-image-array-size-fixture")) {
        if (!write_image_array_size_query_fixture(argv[2])) {
            fprintf(stderr, "spirv_msl: could not write array-image OpImageQuerySize fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-image-levels-fixture")) {
        if (!write_image_size_query_fixture(argv[2], 106, 0)) {
            fprintf(stderr, "spirv_msl: could not write OpImageQueryLevels SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-image-levels-storage-fixture")) {
        if (!write_image_size_query_fixture(argv[2], 106, 1)) {
            fprintf(stderr, "spirv_msl: could not write storage-image OpImageQueryLevels SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-sampler-fixture")) {
        if (!write_sampler_fixture(argv[2])) {
            fprintf(stderr, "spirv_msl: could not write sampler SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-separate-sampler-fixture")) {
        if (!write_separate_sampler_fixture(argv[2])) {
            fprintf(stderr, "spirv_msl: could not write separate-sampler SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-combined-image-array-fixture")) {
        if (!write_combined_image_array_fixture(argv[2])) {
            fprintf(stderr, "spirv_msl: could not write combined image-array SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-separate-image-sampler-array-fixture")) {
        if (!write_separate_image_sampler_array_fixture(argv[2])) {
            fprintf(stderr, "spirv_msl: could not write separate image/sampler-array SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-high-texture-fixture")) {
        if (!write_high_texture_fixture(argv[2])) {
            fprintf(stderr, "spirv_msl: could not write high-texture-slot SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-array-fixture")) {
        if (!write_descriptor_array_fixture(argv[2])) {
            fprintf(stderr, "spirv_msl: could not write descriptor-array SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-dynamic-array-fixture")) {
        if (!write_dynamic_descriptor_array_fixture(argv[2])) {
            fprintf(stderr, "spirv_msl: could not write dynamic descriptor-array SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-dynamic-storage-image-array-fixture")) {
        if (!write_dynamic_storage_image_array_fixture(argv[2])) {
            fprintf(stderr, "spirv_msl: could not write dynamic storage-image-array SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-dynamic-sampled-image-array-fixture")) {
        if (!write_dynamic_sampled_image_array_fixture(argv[2], 0, 26)) {
            fprintf(stderr, "spirv_msl: could not write dynamic sampled-image-array SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-dynamic-separate-image-sampler-array-fixture")) {
        if (!write_dynamic_sampled_image_array_fixture(argv[2], 1, 26)) {
            fprintf(stderr, "spirv_msl: could not write dynamic separate image/sampler-array SPIR-V fixture: %s\n",
                    argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-nonuniform-sampled-image-array-fixture")) {
        if (!write_dynamic_sampled_image_array_fixture(argv[2], 0, 28)) {
            fprintf(stderr, "spirv_msl: could not write nonuniform sampled-image-array SPIR-V fixture: %s\n",
                    argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-uniform-fixture")) {
        if (!write_uniform_buffer_fixture(argv[2])) {
            fprintf(stderr, "spirv_msl: could not write uniform-buffer SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write-push-fixture")) {
        if (!write_push_constant_fixture(argv[2])) {
            fprintf(stderr, "spirv_msl: could not write push-constant SPIR-V fixture: %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--emit-loop"))
        return write_loop_msl(argv[2]) ? 0 : 1;
    if (argc == 3 && !strcmp(argv[1], "--emit-helper"))
        return write_helper_msl(argv[2]) ? 0 : 1;
    if (argc == 3 && !strcmp(argv[1], "--emit-helper-capture"))
        return write_helper_capture_msl(argv[2]) ? 0 : 1;
    if (argc == 3 && !strcmp(argv[1], "--emit-vector-classification"))
        return write_vector_classification_msl(argv[2]) ? 0 : 1;
    if (argc != 1) {
        fprintf(stderr, "usage: spirv_msl_test [--write-fixture path]\n");
        return 1;
    }
    if (!check_graphics_vertex_attribute_msl() || !check_graphics_integer_vertex_attribute_msl() ||
        !check_graphics_stage_msl() ||
        !check_graphics_sparse_location_link() ||
        !check_graphics_interpolation_rejected(13, GPU_IR_SHADER_DECORATION_NO_PERSPECTIVE, "NoPerspective") ||
        !check_graphics_interpolation_rejected(14, GPU_IR_SHADER_DECORATION_FLAT, "Flat") ||
        !check_graphics_interpolation_rejected(16, GPU_IR_SHADER_DECORATION_CENTROID, "Centroid") ||
        !check_graphics_interpolation_rejected(17, GPU_IR_SHADER_DECORATION_SAMPLE, "Sample") ||
        !check_graphics_interpolation_rejected(18, GPU_IR_SHADER_DECORATION_INVARIANT, "Invariant")) return 1;
    if (!check_atomic_rmw_translations() ||
        !check_atomic_add_translation(1, 0, 1) || !check_atomic_add_translation(2, 0, 0) ||
        !check_atomic_add_translation(1, 2, 0) ||
        !check_atomic_compare_exchange_translation(1, 0, 0, 1) ||
        !check_atomic_compare_exchange_translation(2, 0, 0, 0) ||
        !check_atomic_compare_exchange_translation(1, 2, 0, 0) ||
        !check_atomic_compare_exchange_translation(1, 0x48, 0x42, 1) ||
        !check_atomic_compare_exchange_translation(1, 0x44, 0, 1) ||
        !check_atomic_compare_exchange_translation(1, 0x42, 0x44, 0) ||
        !check_atomic_compare_exchange_translation(1, 0, 2, 0) ||
        !check_sampler_sample_translation() || !check_combined_image_array_translation() ||
        !check_image_size_query_translation(104, 0) || !check_image_size_query_translation(103, 0) ||
        !check_image_array_size_query_translation() ||
        !check_image_size_query_translation(106, 0) || !check_image_size_query_translation(106, 1) ||
        !check_image_query_samples_translation(0) || !check_image_query_samples_translation(1) ||
        !rejects_non_multisampled_image_query_samples() ||
        !check_separate_image_sampler_array_translation() ||
        !check_dynamic_sampled_image_array_translation(0) ||
        !check_dynamic_sampled_image_array_translation(1) ||
        !check_dynamic_storage_image_array_translation()) return 1;
    puts("spirv_msl: image sample-count query lowers to 2D and array MSAA MSL and rejects non-MS images: ok");
    uint32_t words[2048] = { 0 };
    size_t word_count = 0;
    if (!make_arithmetic_shader(words, sizeof words / sizeof words[0], &word_count, 0)) {
        fprintf(stderr, "spirv_msl: failed to construct fixture\n");
        return 1;
    }
    if (!rejects_bad_array_length_member()) {
        fprintf(stderr, "spirv_msl: invalid OpArrayLength member was not rejected\n");
        return 1;
    }
    if (!rejects_bad_vector_math_type(142) || !rejects_bad_vector_math_type(148) ||
        !rejects_bad_bit_count_type() || !rejects_bad_bit_reverse_type() ||
        !rejects_bad_bit_field_type() || !rejects_nonzero_sampled_fetch_lod() ||
        !accepts_nonzero_image_query_lod() || !rejects_bad_image_query_levels_result_type()) {
        fprintf(stderr, "spirv_msl: an arithmetic input type mismatch was not rejected\n");
        return 1;
    }
    if (!rejects_padded_scalar_storage_stride()) return 1;
    spirv_module module = { 0 };
    int rc = spirv_read(words, word_count * sizeof words[0], &module);
    const spirv_entry_point *entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    gpu_ir_shader *unresolved_shader = NULL, *shader = NULL;
    int lower_rc = entry ? spirv_build_gpu_ir_shader(&module, entry, &unresolved_shader) : GPU_IR_INVALID;
    uint32_t default_threads[3] = { 0, 0, 0 };
    if (entry && entry->has_local_size_default)
        for (uint32_t axis = 0; axis < 3; axis++) default_threads[axis] = (uint32_t)entry->local_size_default[axis];
    if (lower_rc == GPU_IR_OK) lower_rc = gpu_ir_shader_specialize(unresolved_shader, NULL, 0,
        default_threads, &shader);
    gpu_ir_shader_release(unresolved_shader);
    if (lower_rc != GPU_IR_OK || !shader) {
        fprintf(stderr, "spirv_msl: SPIR-V lowering failed (reader %d: %s, lowerer %d)\n",
            rc, module.error, lower_rc);
        spirv_free(&module);
        return 1;
    }
    gpu_ir_pipeline_layout *layout = NULL;
    int layout_rc = spirv_build_gpu_ir_layout(&module, entry, &layout);
    if (layout_rc != GPU_IR_OK || !layout) {
        fprintf(stderr, "spirv_msl: common descriptor layout lowering failed (%d)\n", layout_rc);
        gpu_ir_shader_release(shader);
        spirv_free(&module);
        return 1;
    }
    char *msl = NULL;
    size_t msl_size = 0;
    int msl_rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &msl, &msl_size);
    int arithmetic_lowered = msl_rc == GPU_IR_OK && msl &&
        strstr(msl, "int vx_value_57 = int(-7ll) / int(3ll);") &&
        strstr(msl, "int vx_value_58 = int(-7ll) % int(3ll);") &&
        strstr(msl, "int vx_remainder_59 = int(-7ll) % int(3ll);") &&
        strstr(msl, "int3 vx_remainder_165 = vx_value_163 % vx_value_164;") &&
        strstr(msl, "select(vx_remainder_165, vx_remainder_165 + vx_value_164, "
                    "(vx_remainder_165 != int3(0)) & ((vx_remainder_165 < int3(0)) != "
                    "(vx_value_164 < int3(0))));") &&
        strstr(msl, "float4 vx_value_173 = vx_value_37 * as_type<float>(0x40000000u);") &&
        strstr(msl, "float vx_value_177 = dot(vx_value_37, vx_value_37);") &&
        strstr(msl, "float4 vx_value_183 = (vx_value_181 - (vx_value_182 * floor(vx_value_181 / vx_value_182)));") &&
        strstr(msl, "float vx_value_190 = fabs(as_type<float>(0xc0300000u));") &&
        strstr(msl, "float vx_value_191 = floor(as_type<float>(0x3fe00000u));") &&
        strstr(msl, "float vx_value_192 = ceil(as_type<float>(0x3fe00000u));") &&
        strstr(msl, "float vx_value_193 = sqrt(as_type<float>(0x40000000u));") &&
        strstr(msl, "float vx_value_194 = rsqrt(as_type<float>(0x40000000u));") &&
        strstr(msl, "float vx_value_195 = fmin(as_type<float>(0x3fe00000u), as_type<float>(0x40000000u));") &&
        strstr(msl, "float vx_value_196 = fmax(as_type<float>(0x3fe00000u), as_type<float>(0x40000000u));") &&
        strstr(msl, "float vx_value_197 = clamp(as_type<float>(0x3fe00000u), as_type<float>(0x3f800000u), as_type<float>(0x40000000u));") &&
        strstr(msl, "float vx_value_198 = fma(as_type<float>(0x3f800000u), as_type<float>(0x40000000u), as_type<float>(0x3fe00000u));") &&
        strstr(msl, "float vx_value_199 = pow(as_type<float>(0x40000000u), as_type<float>(0x3f800000u));") &&
        strstr(msl, "float vx_value_391 = as_type<float>(0x3f800000u) * (float(1.0f) - as_type<float>(0x3f000000u)) + as_type<float>(0x40000000u) * as_type<float>(0x3f000000u);") &&
        strstr(msl, "float vx_value_392 = vx_value_250 + vx_value_391;") &&
        strstr(msl, "uint vx_value_210 = uint(vx_value_209);") &&
        strstr(msl, "uint3 vx_value_273 = vx_value_40;") &&
        strstr(msl, "vx_value_273[vx_value_270] = uint(0x00000003u);") &&
        strstr(msl, "uint vx_value_279 = vx_value_272 + vx_value_278;") &&
        strstr(msl, "uint3 vx_value_280 = vx_value_40;") &&
        strstr(msl, "vx_value_280[1u] = uint(0x00000003u);") &&
        strstr(msl, "uint vx_value_285 = vx_value_284 + vx_value_283;") &&
        strstr(msl, "uint vx_value_286 = vx_value_279 + vx_value_285;") &&
        strstr(msl, "uint vx_value_211 = vx_value_286 + vx_value_210;") &&
        strstr(msl, "float vx_value_212 = trunc(as_type<float>(0x3fe00000u));") &&
        strstr(msl, "float vx_value_213 = fract(as_type<float>(0x3fe00000u));") &&
        strstr(msl, "float vx_value_214 = sin(as_type<float>(0x00000000u));") &&
        strstr(msl, "float vx_value_218 = acos(as_type<float>(0x3f800000u));") &&
        strstr(msl, "float vx_value_223 = asinh(as_type<float>(0x00000000u));") &&
        strstr(msl, "float vx_value_226 = exp(as_type<float>(0x00000000u));") &&
        strstr(msl, "float vx_value_229 = log2(as_type<float>(0x3f800000u));") &&
        strstr(msl, "float vx_value_230 = round(as_type<float>(0x3fe00000u));") &&
        strstr(msl, "float vx_value_231 = rint(as_type<float>(0x40200000u));") &&
        strstr(msl, "uint vx_value_251 = uint(vx_value_392);") &&
        strstr(msl, "uint vx_value_252 = vx_value_211 + vx_value_251;") &&
        strstr(msl, "uint vx_value_287 = uint(popcount(uint(int(-7ll))));") &&
        strstr(msl, "int vx_value_288 = int(popcount(uint(0x00000007u)));") &&
        strstr(msl, "uint vx_value_298 = as_type<uint>(vx_value_288);") &&
        strstr(msl, "uint3 vx_value_289 = uint3(popcount(vx_value_39));") &&
        strstr(msl, "uint vx_value_299 = reverse_bits(uint(0x00000007u));") &&
        strstr(msl, "int vx_value_310 = as_type<int>(extract_bits(as_type<uint>(int(-7ll)), "
                    "uint(0x00000001u), uint(0x00000003u)));") &&
        strstr(msl, "int vx_value_311 = extract_bits(int(-7ll), uint(0x00000001u), uint(0x00000003u));") &&
        strstr(msl, "uint vx_value_312 = insert_bits(uint(0x000000a5u), uint(0x00000003u), "
                    "uint(0x00000002u), uint(0x00000003u));") &&
        strstr(msl, "uint3 vx_value_313 = extract_bits(vx_value_39, uint(0x00000001u), uint(0x00000001u));") &&
        strstr(msl, "uint vx_value_325 = extract_bits(uint(0x000000f3u), "
                    "uint(0x00000002u), uint(0x00000004u));") &&
        strstr(msl, "uint vx_value_296 = vx_value_294 + vx_value_295;") &&
        strstr(msl, "uint vx_value_297 = vx_value_252 + vx_value_296;") &&
        strstr(msl, "uint vx_value_253 = vx_value_297 + vx_value_299;") &&
        strstr(msl, "uint vx_value_324 = vx_value_253 + vx_value_326;") &&
        strstr(msl, "int3 vx_value_327 = int3(as_type<int>(vx_find_result_327_0), "
                    "as_type<int>(vx_find_result_327_1), as_type<int>(vx_find_result_327_2));") &&
        strstr(msl, "int3 vx_value_328 = int3(as_type<int>(vx_find_result_328_0), "
                    "as_type<int>(vx_find_result_328_1), as_type<int>(vx_find_result_328_2));") &&
        strstr(msl, "int3 vx_value_329 = int3(as_type<int>(vx_find_result_329_0), "
                    "as_type<int>(vx_find_result_329_1), as_type<int>(vx_find_result_329_2));") &&
        strstr(msl, "uint vx_value_348 = as_type<uint>(vx_value_346);") &&
        strstr(msl, "uint vx_value_347 = vx_value_324 + vx_value_348;") &&
        strstr(msl, "int vx_value_379 = as_type<int>(vx_find_result_379_0);") &&
        strstr(msl, "int vx_value_380 = as_type<int>(vx_find_result_380_0);") &&
        strstr(msl, "int vx_value_381 = as_type<int>(vx_find_result_381_0);") &&
        strstr(msl, "uint vx_value_385 = vx_value_378 + vx_value_384;") &&
        strstr(msl, "uint vx_value_60 = ~uint(0x00000007u);") &&
        strstr(msl, "uint vx_value_61 = uint(0x00000007u) & uint(0x00000001u);") &&
        strstr(msl, "uint vx_value_62 = uint(0x00000007u) >> uint(0x00000001u);") &&
        strstr(msl, "uint vx_value_63 = uint(0x00000007u) << uint(0x00000001u);") &&
        strstr(msl, "int vx_value_64 = int(-7ll) >> int(3ll);") &&
        strstr(msl, "float vx_value_65 = as_type<float>(0x3f800000u) / as_type<float>(0x40000000u);") &&
        strstr(msl, "fmod(as_type<float>(0x3f800000u), as_type<float>(0x40000000u))") &&
        strstr(msl, "floor(as_type<float>(0x3f800000u) / as_type<float>(0x40000000u))") &&
        strstr(msl, "int vx_value_68 = -int(-7ll);") &&
        strstr(msl, "float vx_value_69 = -as_type<float>(0x3f800000u);") &&
        strstr(msl, "bool3 vx_value_94 = bool3(true, false, true);") &&
        strstr(msl, "uint3 vx_value_260 = uint3((vx_value_94.x ? vx_value_40.x : vx_value_39.x), "
                    "(vx_value_94.y ? vx_value_40.y : vx_value_39.y), "
                    "(vx_value_94.z ? vx_value_40.z : vx_value_39.z));") &&
        strstr(msl, "uint vx_value_271 = (vx_value_40)[vx_value_270];") &&
        strstr(msl, "bool vx_value_95 = any(vx_value_94);") &&
        strstr(msl, "bool vx_value_96 = all(vx_value_94);") &&
        strstr(msl, "bool3 vx_value_100 = !vx_value_94;") &&
        strstr(msl, "bool3 vx_value_101 = vx_value_94 & vx_value_94;") &&
        strstr(msl, "bool3 vx_value_102 = vx_value_94 | vx_value_94;") &&
        strstr(msl, "uint vx_local_143 = uint(0x00000001u);") &&
        strstr(msl, "vx_local_143 = vx_value_56;") &&
        strstr(msl, "uint vx_value_144 = vx_local_143;") &&
        strstr(msl, "array<uint, 3> vx_local_148;") &&
        strstr(msl, "vx_local_148[uint(0x00000002u)] = vx_value_56;") &&
        strstr(msl, "uint vx_value_151 = vx_local_148[uint(0x00000002u)];") &&
        strstr(msl, "threadgroup array<uint, 32> vx_workgroup_155;") &&
        strstr(msl, "vx_workgroup_155[vx_value_158] = vx_value_56;") &&
        strstr(msl, "uint vx_value_159 = vx_workgroup_155[vx_value_158];") &&
        strstr(msl, "threadgroup_barrier(mem_flags::mem_threadgroup);") &&
        strstr(msl, "simdgroup_barrier(mem_flags::mem_threadgroup);") &&
        strstr(msl, "atomic_thread_fence(mem_flags::mem_threadgroup, memory_order_seq_cst, thread_scope_threadgroup);") &&
        strstr(msl, "atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device);") &&
        strstr(msl, "atomic_thread_fence(mem_flags::mem_device | mem_flags::mem_texture, memory_order_seq_cst, thread_scope_device);");
    int conversions_lowered = msl_rc == GPU_IR_OK && msl &&
        strstr(msl, "uint vx_value_132 = uint(as_type<float>(0x3fe00000u));") &&
        strstr(msl, "int vx_value_133 = int(as_type<float>(0xc0300000u));") &&
        strstr(msl, "float vx_value_134 = float(vx_value_133);") &&
        strstr(msl, "float vx_value_135 = float(vx_value_132);") &&
        strstr(msl, "uint vx_value_136 = as_type<uint>(as_type<float>(0x3f800000u));") &&
        strstr(msl, "uint4 vx_value_139 = uint4(vx_value_43);");
    int guarded_access = msl_rc == GPU_IR_OK && msl && msl_size &&
        strstr(msl, "constant ulong* vx_buffer_ranges [[buffer(30)]]") &&
        strstr(msl, "vx_buffer_ranges[0]") && strstr(msl, "= 0u;\n    if (") &&
        strstr(msl, ") { vx_buffer_") &&
        strstr(msl, "uint vx_value_97 = uint(vx_buffer_ranges[0] / 4ul);") && arithmetic_lowered &&
        conversions_lowered;
    if (!guarded_access) {
        fprintf(stderr, "spirv_msl: storage-buffer range guards, arithmetic or conversions were not emitted "
                        "(emitter %d, arithmetic %d, conversions %d, %zu bytes)\n%s",
                msl_rc, arithmetic_lowered, conversions_lowered, msl_size, msl ? msl : "<no MSL generated>\n");
        free(msl);
        gpu_ir_pipeline_layout_release(layout);
        gpu_ir_shader_release(shader);
        spirv_free(&module);
        return 1;
    }
    free(msl);
    uint32_t threadgroup[3];
    gpu_ir_shader_threadgroup_size(shader, threadgroup);
    gpu_ir_metal_device *device = gpu_ir_metal_device_create();
    if (!device) fprintf(stderr, "spirv_msl: Metal device is unavailable\n");
    int ok = execute_image_copy_on_metal(device, 0) && execute_image_copy_on_metal(device, 1) &&
        attachment_clear_on_metal(device) &&
        execute_on_metal(device, shader, layout, threadgroup, 128 * sizeof(uint32_t)) &&
        execute_on_metal(device, shader, layout, threadgroup, 80 * sizeof(uint32_t) + 2) &&
        execute_robust_load_on_metal(device) &&
        execute_scalar_storage_on_metal(device, STORAGE_SCALAR_UINT) &&
        execute_scalar_storage_on_metal(device, STORAGE_SCALAR_SINT) &&
        execute_scalar_storage_on_metal(device, STORAGE_SCALAR_FLOAT);
    if (ok) puts("spirv_msl: storage-image read/write, combined and separate image/sampler descriptor arrays, descriptor snapshots, dispatch, scalar uint/int/float storage-buffer loads and stores, Device-scope Relaxed/Acquire/Release/AcquireRelease 32-bit storage-buffer atomics, compute built-ins, GLSL.std.450 math and integer bit scans, arithmetic/conversions/bit operations including bit-field insert/extract, vector OpSelect/extract/insert, OpArrayLength, workgroup/subgroup and scoped memory barriers, and bool-vector logic ran through common IR and Metal: ok");
    gpu_ir_metal_device_release(device);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    spirv_free(&module);
    return ok ? 0 : 1;
}
