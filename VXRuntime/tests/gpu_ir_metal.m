#import <Foundation/Foundation.h>
#include "gpu_ir_metal.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "gpu_ir_metal raster check failed at %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void) {
    enum { WIDTH = 4, HEIGHT = 4, BYTES_PER_PIXEL = 4 };
    gpu_ir_metal_device *device = gpu_ir_metal_device_create();
    CHECK(device != NULL);
    uint64_t row_pitch = gpu_ir_metal_linear_texture_row_pitch(device, GPU_IR_FORMAT_RGBA8_UNORM, WIDTH);
    uint64_t alignment = gpu_ir_metal_linear_texture_alignment(device, GPU_IR_FORMAT_RGBA8_UNORM);
    CHECK(row_pitch >= WIDTH * BYTES_PER_PIXEL && alignment && row_pitch % alignment == 0);
    gpu_ir_resource *backing = gpu_ir_metal_buffer_create(device, row_pitch * HEIGHT, NULL);
    CHECK(backing != NULL);
    gpu_ir_texture_desc texture_description = {
        .width = WIDTH, .height = HEIGHT, .depth = 1,
        .array_layers = 1, .mip_levels = 1, .bytes_per_texel = BYTES_PER_PIXEL, .sample_count = 1,
        .block_width = 1, .block_height = 1, .bytes_per_block = BYTES_PER_PIXEL,
        .format = GPU_IR_FORMAT_RGBA8_UNORM
    };
    gpu_ir_resource *texture = gpu_ir_metal_texture_create_from_buffer(device, backing, 0, row_pitch,
        &texture_description, 0, 0);
    CHECK(texture != NULL);

    static const uint8_t vertex_positions[3][4] = {
        { 0, 0, 0, 255 }, { 255, 0, 0, 255 }, { 0, 255, 0, 255 }
    };
    gpu_ir_resource *vertex_buffer = gpu_ir_metal_buffer_create(device, sizeof vertex_positions,
                                                                 (void *)vertex_positions);
    CHECK(vertex_buffer != NULL);
    static const char vertex_source[] =
        "struct VxVertexInput { float4 position [[attribute(0)]]; }; "
        "vertex float4 vx_vertex(VxVertexInput input [[stage_in]]) { "
        "return float4(input.position.xy * 4.0f - 1.0f, 0.0f, 1.0f); }";
    static const char fragment_source[] =
        "fragment float4 vx_fragment() { return float4(0.0f, 1.0f, 0.0f, 1.0f); }";
    gpu_ir_vertex_input_desc vertex_input = {
        .binding_count = 1,
        .attribute_count = 1,
        .bindings = { { .binding = 0, .stride = sizeof vertex_positions[0],
                        .input_rate = GPU_IR_VERTEX_INPUT_RATE_VERTEX, .step_rate = 1 } },
        .attributes = { { .location = 0, .binding = 0, .format = GPU_IR_VERTEX_FORMAT_UNORM8X4, .offset = 0 } }
    };
    char pipeline_error[512];
    gpu_ir_pipeline *pipeline = gpu_ir_metal_graphics_pipeline_create_with_vertex_input(device, vertex_source,
        fragment_source, GPU_IR_FORMAT_RGBA8_UNORM, &vertex_input, pipeline_error, sizeof pipeline_error);
    if (!pipeline) fprintf(stderr, "gpu_ir_metal raster: %s\n", pipeline_error);
    CHECK(pipeline != NULL);

    gpu_ir_stream stream;
    gpu_ir_stream_init(&stream);
    static const uint16_t triangle_indices[] = { 0, 1, 2 };
    gpu_ir_resource *index_buffer = gpu_ir_metal_buffer_create(device, sizeof triangle_indices,
                                                               (void *)triangle_indices);
    CHECK(index_buffer != NULL);
    const float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    CHECK(gpu_ir_record_clear_color(&stream, texture, clear_color, NULL, 0, NULL) == GPU_IR_OK);
    gpu_ir_render_pass_desc pass = {
        .mip_level = 0,
        .array_layer = 0,
        .render_area = { 1, 1, 3, 3 },
        .load_action = GPU_IR_RENDER_PASS_CLEAR,
        .store_action = GPU_IR_RENDER_PASS_STORE,
        .clear_value_type = GPU_IR_CLEAR_COLOR_VALUE_FLOAT,
        .clear_value = { .color = { 0.0f, 1.0f, 0.0f, 1.0f } }
    };
    CHECK(gpu_ir_record_end_render_pass(&stream, NULL) == GPU_IR_INVALID);
    gpu_ir_render_pass_desc invalid_pass = pass;
    invalid_pass.render_area.right = WIDTH + 1;
    CHECK(gpu_ir_record_begin_render_pass(&stream, texture, &invalid_pass, NULL) == GPU_IR_RANGE);
    invalid_pass = pass;
    invalid_pass.store_action = GPU_IR_RENDER_PASS_STORE_DONT_CARE + 1;
    CHECK(gpu_ir_record_begin_render_pass(&stream, texture, &invalid_pass, NULL) == GPU_IR_INVALID);
    invalid_pass = pass;
    invalid_pass.clear_value_type = (gpu_ir_clear_color_value_type)99;
    CHECK(gpu_ir_record_begin_render_pass(&stream, texture, &invalid_pass, NULL) == GPU_IR_INVALID);
    CHECK(gpu_ir_record_begin_render_pass(&stream, texture, &pass, NULL) == GPU_IR_OK);
    CHECK(gpu_ir_record_begin_render_pass(&stream, texture, &pass, NULL) == GPU_IR_INVALID);
    gpu_ir_draw_info draw = {
        .topology = GPU_IR_TOPOLOGY_TRIANGLE_LIST,
        .vertex_count = 3,
        .instance_count = 1,
        .vertex_buffer_count = 1,
        .vertex_buffers = { { .binding = 0, .resource = vertex_buffer, .offset = 0,
                             .size = sizeof vertex_positions } }
    };
    gpu_ir_draw_info indexed_draw = {
        .topology = GPU_IR_TOPOLOGY_TRIANGLE_LIST,
        .indexed = 1,
        .index_count = 3,
        .instance_count = 1,
        .index_type = GPU_IR_INDEX_TYPE_UINT16,
        .index_buffer = index_buffer,
        .index_buffer_size = sizeof triangle_indices,
        .vertex_buffer_count = 1,
        .vertex_buffers = { { .binding = 0, .resource = vertex_buffer, .offset = 0,
                             .size = sizeof vertex_positions } }
    };
    CHECK(gpu_ir_record_draw(&stream, pipeline, &draw, NULL) == GPU_IR_OK);
    CHECK(gpu_ir_record_draw(&stream, pipeline, &indexed_draw, NULL) == GPU_IR_OK);
    gpu_ir_resource_release(vertex_buffer);
    vertex_buffer = NULL;
    CHECK(gpu_ir_record_end_render_pass(&stream, NULL) == GPU_IR_OK);
    CHECK(gpu_ir_command_kind(&stream, 1) == GPU_IR_COMMAND_BEGIN_RENDER_PASS &&
          gpu_ir_command_kind(&stream, 4) == GPU_IR_COMMAND_END_RENDER_PASS);
    CHECK(gpu_ir_metal_execute_stream(device, &stream) == GPU_IR_OK);

    const uint8_t *pixels = gpu_ir_metal_buffer_contents(backing);
    CHECK(pixels != NULL);
    const uint8_t black[4] = { 0, 0, 0, 255 }, green[4] = { 0, 255, 0, 255 };
    for (uint32_t y = 0; y < HEIGHT; y++) {
        for (uint32_t x = 0; x < WIDTH; x++) {
            const uint8_t *expected = (x >= 1 && x < 3 && y >= 1 && y < 3) ? green : black;
            CHECK(memcmp(pixels + y * row_pitch + x * BYTES_PER_PIXEL, expected, BYTES_PER_PIXEL) == 0);
        }
    }

    gpu_ir_stream_destroy(&stream);
    gpu_ir_resource_release(index_buffer);
    gpu_ir_resource_release(vertex_buffer);
    gpu_ir_pipeline_release(pipeline);
    gpu_ir_resource_release(texture);
    gpu_ir_resource_release(backing);
    gpu_ir_metal_device_release(device);
    puts("gpu_ir_metal: common render pass, vertex-buffer binding lifetime, clipped indexed/non-indexed triangle draws and shared-buffer readback: ok");
    return 0;
}
