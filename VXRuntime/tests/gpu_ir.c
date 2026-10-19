#include "gpu_ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t id; uint32_t *destroyed; } fake_resource;
typedef struct {
    uint32_t calls, resource_calls, texture_calls, src_id, dst_id;
    uint32_t resolve_calls, src_mip, src_slice, dst_mip, dst_slice, resolve_mode;
    gpu_ir_resolve_region resolve_region;
    uint32_t barrier_calls, barrier_type, barrier_flags, barrier_subresource;
    uint32_t barrier_before_access, barrier_after_access;
    gpu_ir_barrier_desc barrier_description;
    uint32_t dispatch_calls, dispatch_indirect_calls, dispatch_indirect_range_calls;
    uint32_t group_count[3], threadgroup_size[3];
    uint32_t access_count, access_resource_id, access_stage_mask, access_mask;
    uint32_t draw_calls, draw_pipeline_id, draw_indirect_calls, draw_indirect_pipeline_id;
    uint32_t draw_indirect_arguments_id, draw_indirect_count_id;
    uint32_t dispatch_range_arguments_id, dispatch_range_count_id;
    uint32_t draw_access_counts[2], draw_access_resource_ids[2];
    uint32_t descriptor_state_calls, descriptor_state_value_count;
    uint32_t descriptor_kinds[8], descriptor_sets[8], descriptor_bindings[8], descriptor_elements[8];
    uint32_t descriptor_resource_ids[8], descriptor_backend_ids[8], descriptor_flags[8];
    uint64_t descriptor_ranges[8];
    gpu_ir_draw_info draws[2];
    gpu_ir_indirect_draw_info indirect_draw;
    gpu_ir_indirect_dispatch_info indirect_dispatch_range;
    uint64_t src_offset, dst_offset, size, indirect_offset;
    uint32_t update_calls, update_words[4];
    uint32_t fill_calls, fill_value, fill_pattern_calls, fill_pattern_size;
    uint8_t fill_pattern[GPU_IR_MAX_FILL_PATTERN_SIZE];
    uint32_t clear_color_calls, clear_depth_stencil_calls, clear_texture_id;
    uint32_t clear_aspects, clear_stencil, clear_rectangle_count;
    float clear_color[4], clear_depth;
    gpu_ir_rect clear_rectangles[4];
    gpu_ir_texture_location src_location, dst_location;
    uint32_t width, height, depth;
} fake_backend;

static int fail(const char *what) { fprintf(stderr, "gpu_ir: %s\n", what); return 1; }

static void release_fake(void *p) {
    fake_resource *b = p;
    (*b->destroyed)++;
    free(b);
}

static int copy_buffer(void *context, const gpu_ir_resource *src, uint64_t src_offset,
                       const gpu_ir_resource *dst, uint64_t dst_offset, uint64_t size) {
    fake_backend *b = context;
    fake_resource *s = gpu_ir_backend_object(src), *d = gpu_ir_backend_object(dst);
    if (!s || !d || !size || src_offset + size > gpu_ir_buffer_size(src) || dst_offset + size > gpu_ir_buffer_size(dst))
        return GPU_IR_RANGE;
    b->calls++;
    b->src_id = s->id;
    b->dst_id = d->id;
    b->src_offset = src_offset;
    b->dst_offset = dst_offset;
    b->size = size;
    return GPU_IR_OK;
}

static int update_buffer(void *context, const gpu_ir_resource *dst, uint64_t dst_offset,
                         const void *data, uint64_t size) {
    fake_backend *b = context;
    fake_resource *d = gpu_ir_backend_object(dst);
    if (!d || !data || size != sizeof b->update_words || dst_offset > gpu_ir_buffer_size(dst) ||
        size > gpu_ir_buffer_size(dst) - dst_offset) return GPU_IR_RANGE;
    b->update_calls++;
    b->dst_id = d->id;
    b->dst_offset = dst_offset;
    b->size = size;
    memcpy(b->update_words, data, sizeof b->update_words);
    return GPU_IR_OK;
}

static int fill_buffer(void *context, const gpu_ir_resource *dst, uint64_t dst_offset,
                       uint64_t size, uint32_t value) {
    fake_backend *b = context;
    fake_resource *d = gpu_ir_backend_object(dst);
    if (!d || !size || ((dst_offset | size) & 3u) || dst_offset > gpu_ir_buffer_size(dst) ||
        size > gpu_ir_buffer_size(dst) - dst_offset) return GPU_IR_RANGE;
    b->fill_calls++;
    b->dst_id = d->id;
    b->dst_offset = dst_offset;
    b->size = size;
    b->fill_value = value;
    return GPU_IR_OK;
}

static int fill_buffer_pattern(void *context, const gpu_ir_resource *dst, uint64_t dst_offset,
                               uint64_t size, const void *pattern, uint32_t pattern_size) {
    fake_backend *b = context;
    if (!pattern || !pattern_size || pattern_size > GPU_IR_MAX_FILL_PATTERN_SIZE ||
        dst_offset % pattern_size || size % pattern_size) return GPU_IR_RANGE;
    b->fill_pattern_calls++;
    b->fill_pattern_size = pattern_size;
    memcpy(b->fill_pattern, pattern, pattern_size);
    if (pattern_size == sizeof(uint32_t)) {
        uint32_t value;
        memcpy(&value, pattern, sizeof value);
        return fill_buffer(context, dst, dst_offset, size, value);
    }
    fake_resource *resource = gpu_ir_backend_object(dst);
    if (!resource || !size || dst_offset > gpu_ir_buffer_size(dst) ||
        size > gpu_ir_buffer_size(dst) - dst_offset) return GPU_IR_RANGE;
    b->fill_calls++;
    b->dst_id = resource->id;
    b->dst_offset = dst_offset;
    b->size = size;
    return GPU_IR_OK;
}

static int copy_texture(void *context, const gpu_ir_resource *src, const gpu_ir_texture_location *src_location,
                       const gpu_ir_resource *dst, const gpu_ir_texture_location *dst_location,
                       uint32_t width, uint32_t height, uint32_t depth) {
    fake_backend *b = context;
    fake_resource *s = gpu_ir_backend_object(src), *d = gpu_ir_backend_object(dst);
    if (!s || !d || !width || !height || !depth) return GPU_IR_INVALID;
    b->texture_calls++;
    b->src_id = s->id;
    b->dst_id = d->id;
    b->src_location = *src_location;
    b->dst_location = *dst_location;
    b->width = width;
    b->height = height;
    b->depth = depth;
    return GPU_IR_OK;
}

static int copy_resource(void *context, const gpu_ir_resource *src, const gpu_ir_resource *dst) {
    fake_backend *b = context;
    fake_resource *s = gpu_ir_backend_object(src), *d = gpu_ir_backend_object(dst);
    if (!s || !d || gpu_ir_resource_is_buffer(src) != gpu_ir_resource_is_buffer(dst)) return GPU_IR_INVALID;
    b->resource_calls++;
    b->src_id = s->id;
    b->dst_id = d->id;
    return GPU_IR_OK;
}

static int barrier(void *context, const gpu_ir_barrier_desc *desc) {
    fake_backend *b = context;
    fake_resource *src = gpu_ir_backend_object(desc->resource);
    fake_resource *dst = gpu_ir_backend_object(desc->resource_after);
    b->barrier_calls++;
    b->barrier_type = desc->type;
    b->barrier_flags = desc->flags;
    b->barrier_subresource = desc->subresource;
    b->barrier_before_access = desc->before_access;
    b->barrier_after_access = desc->after_access;
    b->barrier_description = *desc;
    b->src_id = src ? src->id : 0;
    b->dst_id = dst ? dst->id : 0;
    return GPU_IR_OK;
}

static int resolve_texture(void *context, const gpu_ir_resource *src, const gpu_ir_resolve_location *src_location,
                           const gpu_ir_resource *dst, const gpu_ir_resolve_location *dst_location,
                           const gpu_ir_resolve_region *region, uint32_t mode) {
    fake_backend *b = context;
    fake_resource *s = gpu_ir_backend_object(src), *d = gpu_ir_backend_object(dst);
    if (!s || !d || !region) return GPU_IR_INVALID;
    b->resolve_calls++;
    b->src_id = s->id;
    b->dst_id = d->id;
    b->src_mip = src_location->mip_level;
    b->src_slice = src_location->array_slice;
    b->dst_mip = dst_location->mip_level;
    b->dst_slice = dst_location->array_slice;
    b->resolve_region = *region;
    b->resolve_mode = mode;
    return GPU_IR_OK;
}

static int clear_color(void *context, const gpu_ir_resource *texture, const gpu_ir_clear_color_info *clear) {
    fake_backend *b = context;
    fake_resource *resource = gpu_ir_backend_object(texture);
    if (!resource || !gpu_ir_texture_description(texture) || !clear ||
        clear->rectangle_count > sizeof b->clear_rectangles / sizeof b->clear_rectangles[0] ||
        (clear->rectangle_count && !clear->rectangles)) return GPU_IR_INVALID;
    b->clear_color_calls++;
    b->clear_texture_id = resource->id;
    memcpy(b->clear_color, clear->color, sizeof b->clear_color);
    b->clear_rectangle_count = clear->rectangle_count;
    if (clear->rectangle_count)
        memcpy(b->clear_rectangles, clear->rectangles, clear->rectangle_count * sizeof *clear->rectangles);
    return GPU_IR_OK;
}

static int clear_depth_stencil(void *context, const gpu_ir_resource *texture,
                               const gpu_ir_clear_depth_stencil_info *clear) {
    fake_backend *b = context;
    fake_resource *resource = gpu_ir_backend_object(texture);
    if (!resource || !gpu_ir_texture_description(texture) || !clear || !clear->aspects ||
        (clear->aspects & ~GPU_IR_CLEAR_DEPTH_STENCIL_ALL) ||
        clear->rectangle_count > sizeof b->clear_rectangles / sizeof b->clear_rectangles[0] ||
        (clear->rectangle_count && !clear->rectangles)) return GPU_IR_INVALID;
    b->clear_depth_stencil_calls++;
    b->clear_texture_id = resource->id;
    b->clear_aspects = clear->aspects;
    b->clear_depth = clear->depth;
    b->clear_stencil = clear->stencil;
    b->clear_rectangle_count = clear->rectangle_count;
    if (clear->rectangle_count)
        memcpy(b->clear_rectangles, clear->rectangles, clear->rectangle_count * sizeof *clear->rectangles);
    return GPU_IR_OK;
}

static int dispatch(void *context, const gpu_ir_pipeline *pipeline, uint32_t x, uint32_t y, uint32_t z,
                    const gpu_ir_resource_access *accesses, uint32_t access_count) {
    fake_backend *b = context;
    const gpu_ir_pipeline_info *info = gpu_ir_pipeline_description(pipeline);
    fake_resource *native = gpu_ir_pipeline_backend_object(pipeline);
    if (!info || info->kind != GPU_IR_PIPELINE_COMPUTE || !native) return GPU_IR_INVALID;
    b->dispatch_calls++;
    b->src_id = native->id;
    b->group_count[0] = x; b->group_count[1] = y; b->group_count[2] = z;
    b->threadgroup_size[0] = info->threadgroup_size[0];
    b->threadgroup_size[1] = info->threadgroup_size[1];
    b->threadgroup_size[2] = info->threadgroup_size[2];
    b->access_count = access_count;
    if (access_count) {
        fake_resource *resource = gpu_ir_backend_object(accesses[0].resource);
        if (!resource) return GPU_IR_INVALID;
        b->access_resource_id = resource->id;
        b->access_stage_mask = accesses[0].stage_mask;
        b->access_mask = accesses[0].access_mask;
    }
    return GPU_IR_OK;
}

static int dispatch_indirect(void *context, const gpu_ir_pipeline *pipeline,
                             const gpu_ir_resource *arguments, uint64_t offset,
                             const gpu_ir_resource_access *accesses, uint32_t access_count) {
    fake_backend *b = context;
    const gpu_ir_pipeline_info *info = gpu_ir_pipeline_description(pipeline);
    fake_resource *native = gpu_ir_pipeline_backend_object(pipeline);
    fake_resource *args = gpu_ir_backend_object(arguments);
    if (!info || info->kind != GPU_IR_PIPELINE_COMPUTE || !native || !args || offset > UINT32_MAX)
        return GPU_IR_INVALID;
    b->dispatch_indirect_calls++;
    b->src_id = native->id;
    b->dst_id = args->id;
    b->indirect_offset = offset;
    b->access_count = access_count;
    if (access_count) {
        fake_resource *resource = gpu_ir_backend_object(accesses[0].resource);
        if (!resource) return GPU_IR_INVALID;
        b->access_resource_id = resource->id;
        b->access_stage_mask = accesses[0].stage_mask;
        b->access_mask = accesses[0].access_mask;
    }
    return GPU_IR_OK;
}

static int dispatch_indirect_range(void *context, const gpu_ir_pipeline *pipeline,
                                   const gpu_ir_resource *arguments, const gpu_ir_resource *count_arguments,
                                   const gpu_ir_indirect_dispatch_info *dispatch,
                                   const gpu_ir_resource_access *accesses, uint32_t access_count) {
    fake_backend *b = context;
    const gpu_ir_pipeline_info *info = gpu_ir_pipeline_description(pipeline);
    fake_resource *native = gpu_ir_pipeline_backend_object(pipeline);
    fake_resource *args = gpu_ir_backend_object(arguments);
    fake_resource *count = count_arguments ? gpu_ir_backend_object(count_arguments) : NULL;
    if (!info || info->kind != GPU_IR_PIPELINE_COMPUTE || !native || !args || !dispatch ||
        (count_arguments && !count)) return GPU_IR_INVALID;
    b->dispatch_indirect_range_calls++;
    b->src_id = native->id;
    b->dispatch_range_arguments_id = args->id;
    b->dispatch_range_count_id = count ? count->id : 0;
    b->indirect_dispatch_range = *dispatch;
    b->access_count = access_count;
    if (access_count) {
        fake_resource *resource = gpu_ir_backend_object(accesses[0].resource);
        if (!resource) return GPU_IR_INVALID;
        b->access_resource_id = resource->id;
        b->access_stage_mask = accesses[0].stage_mask;
        b->access_mask = accesses[0].access_mask;
    }
    return GPU_IR_OK;
}

static int bind_descriptor_state(void *context, const gpu_ir_descriptor_state *state) {
    fake_backend *b = context;
    uint32_t count = gpu_ir_descriptor_state_value_count(state);
    if (count > 8) return GPU_IR_RANGE;
    b->descriptor_state_calls++;
    b->descriptor_state_value_count = count;
    for (uint32_t i = 0; i < count; i++) {
        gpu_ir_descriptor_value value;
        if (gpu_ir_descriptor_state_value_at(state, i, &value) != GPU_IR_OK) return GPU_IR_INVALID;
        fake_resource *resource = gpu_ir_backend_object(value.resource ? value.resource :
            (gpu_ir_resource *)gpu_ir_buffer_view_resource(value.buffer_view));
        fake_resource *native = value.backend_object;
        if (!native && value.buffer_view) native = gpu_ir_buffer_view_backend_object(value.buffer_view);
        b->descriptor_kinds[i] = value.kind;
        b->descriptor_sets[i] = value.set;
        b->descriptor_bindings[i] = value.binding;
        b->descriptor_elements[i] = value.array_element;
        b->descriptor_resource_ids[i] = resource ? resource->id : 0;
        b->descriptor_backend_ids[i] = native ? native->id : 0;
        b->descriptor_flags[i] = value.flags;
        b->descriptor_ranges[i] = value.range;
    }
    return GPU_IR_OK;
}

static int draw(void *context, const gpu_ir_pipeline *pipeline, const gpu_ir_draw_info *info,
                const gpu_ir_resource_access *accesses, uint32_t access_count) {
    fake_backend *b = context;
    const gpu_ir_pipeline_info *desc = gpu_ir_pipeline_description(pipeline);
    fake_resource *native = gpu_ir_pipeline_backend_object(pipeline);
    if (!info || !desc || desc->kind != GPU_IR_PIPELINE_GRAPHICS || !native || b->draw_calls >= 2)
        return GPU_IR_INVALID;
    b->draw_pipeline_id = native->id;
    uint32_t draw_index = b->draw_calls++;
    b->draws[draw_index] = *info;
    b->draw_access_counts[draw_index] = access_count;
    b->access_count = access_count;
    if (access_count) {
        fake_resource *resource = gpu_ir_backend_object(accesses[0].resource);
        if (!resource) return GPU_IR_INVALID;
        b->draw_access_resource_ids[draw_index] = resource->id;
        b->access_resource_id = resource->id;
        b->access_stage_mask = accesses[0].stage_mask;
        b->access_mask = accesses[0].access_mask;
    }
    return GPU_IR_OK;
}

static int draw_indirect(void *context, const gpu_ir_pipeline *pipeline, const gpu_ir_resource *arguments,
                         const gpu_ir_resource *count_arguments,
                         const gpu_ir_indirect_draw_info *info,
                         const gpu_ir_resource_access *accesses, uint32_t access_count) {
    fake_backend *b = context;
    const gpu_ir_pipeline_info *desc = gpu_ir_pipeline_description(pipeline);
    fake_resource *native = gpu_ir_pipeline_backend_object(pipeline);
    fake_resource *args = gpu_ir_backend_object(arguments);
    fake_resource *count = gpu_ir_backend_object(count_arguments);
    if (!info || !desc || desc->kind != GPU_IR_PIPELINE_GRAPHICS || !native || !args)
        return GPU_IR_INVALID;
    b->draw_indirect_calls++;
    b->draw_indirect_pipeline_id = native->id;
    b->draw_indirect_arguments_id = args->id;
    b->draw_indirect_count_id = count ? count->id : 0;
    b->indirect_draw = *info;
    b->access_count = access_count;
    if (access_count) {
        fake_resource *resource = gpu_ir_backend_object(accesses[0].resource);
        if (!resource) return GPU_IR_INVALID;
        b->access_resource_id = resource->id;
        b->access_stage_mask = accesses[0].stage_mask;
        b->access_mask = accesses[0].access_mask;
    }
    return GPU_IR_OK;
}

static gpu_ir_resource *new_buffer(uint32_t id, uint64_t size, uint32_t *destroyed) {
    fake_resource *b = malloc(sizeof *b);
    if (!b) return NULL;
    *b = (fake_resource){ id, destroyed };
    gpu_ir_resource *r = gpu_ir_buffer_create(size, b, release_fake);
    if (!r) free(b);
    return r;
}

static gpu_ir_resource *new_texture(uint32_t id, const gpu_ir_texture_desc *desc, uint32_t *destroyed) {
    fake_resource *t = malloc(sizeof *t);
    if (!t) return NULL;
    *t = (fake_resource){ id, destroyed };
    gpu_ir_resource *r = gpu_ir_texture_create(desc, t, release_fake);
    if (!r) free(t);
    return r;
}

static int shader_model_test(void) {
    gpu_ir_shader_type types[] = {
        { .id = 1, .kind = GPU_IR_SHADER_TYPE_VOID },
        { .id = 2, .kind = GPU_IR_SHADER_TYPE_INT, .bit_width = 32, .signedness = 0 },
        { .id = 11, .kind = GPU_IR_SHADER_TYPE_FUNCTION, .element_type_id = 1,
          .first_member = 0, .member_count = 1 }
    };
    gpu_ir_shader_member type_members[] = { { .type_id = 2 } };
    gpu_ir_shader_function functions[] = {
        { .id = 10, .result_type_id = 1, .function_type_id = 11,
          .first_block = 0, .block_count = 1, .first_instruction = 1, .instruction_count = 3,
          .parameter_count = 1 }
    };
    gpu_ir_shader_block blocks[] = {
        { .function_index = 0, .label_id = 12, .first_instruction = 2, .instruction_count = 2,
          .terminator_opcode = GPU_IR_SHADER_OP_RETURN_VALUE }
    };
    gpu_ir_shader_operand operands[] = {
        { GPU_IR_SHADER_OPERAND_ID, 20 }, { GPU_IR_SHADER_OPERAND_ID, 22 },
        { GPU_IR_SHADER_OPERAND_ID, 21 }
    };
    uint32_t literals[] = { 2 };
    gpu_ir_shader_instruction instructions[] = {
        { .source_api = GPU_IR_SHADER_SOURCE_SPIRV, .opcode = GPU_IR_SHADER_OP_CONSTANT,
          .operation = GPU_IR_SHADER_CONSTANT_SCALAR, .source_opcode = 43,
          .result_id = 22, .result_type_id = 2,
          .function_index = UINT32_MAX, .block_index = UINT32_MAX,
          .first_literal = 0, .literal_word_count = 1 },
        { .source_api = GPU_IR_SHADER_SOURCE_SPIRV, .opcode = GPU_IR_SHADER_OP_PARAMETER,
          .source_opcode = 55, .result_id = 20, .result_type_id = 2,
          .function_index = 0, .block_index = UINT32_MAX },
        { .source_api = GPU_IR_SHADER_SOURCE_SPIRV, .opcode = GPU_IR_SHADER_OP_ARITHMETIC,
          .operation = GPU_IR_SHADER_ARITH_ADD, .source_opcode = 128,
          .result_id = 21, .result_type_id = 2,
          .function_index = 0, .block_index = 0, .first_operand = 0, .operand_count = 2 },
        { .source_api = GPU_IR_SHADER_SOURCE_SPIRV, .opcode = GPU_IR_SHADER_OP_RETURN_VALUE,
          .source_opcode = 254, .function_index = 0, .block_index = 0,
          .first_operand = 2, .operand_count = 1 }
    };
    gpu_ir_shader_desc desc = {
        .source_api = GPU_IR_SHADER_SOURCE_SPIRV, .stage_mask = GPU_IR_STAGE_COMPUTE,
        .entry_function_id = 10, .threadgroup_size = { 8, 4, 2 }, .types = types, .type_count = 3,
        .type_members = type_members, .type_member_count = 1,
        .functions = functions, .function_count = 1, .blocks = blocks, .block_count = 1,
        .instructions = instructions, .instruction_count = 4,
        .operands = operands, .operand_count = 3, .literal_words = literals, .literal_word_count = 1
    };
    gpu_ir_shader *shader = gpu_ir_shader_create(&desc);
    if (!shader) return fail("typed shader IR rejected a valid compute function");
    uint32_t threadgroup_size[3] = { 0, 0, 0 };
    gpu_ir_shader_threadgroup_size(shader, threadgroup_size);
    types[1].bit_width = 16;
    operands[0].value = 99;
    literals[0] = 3;
    const gpu_ir_shader_instruction *addition = gpu_ir_shader_instruction_at(shader, 2);
    if (gpu_ir_shader_source_api(shader) != GPU_IR_SHADER_SOURCE_SPIRV ||
        gpu_ir_shader_stage(shader) != GPU_IR_STAGE_COMPUTE || gpu_ir_shader_entry_function(shader) != 10 ||
        threadgroup_size[0] != 8 || threadgroup_size[1] != 4 || threadgroup_size[2] != 2 ||
        gpu_ir_shader_type_count(shader) != 3 || gpu_ir_shader_find_type(shader, 2)->bit_width != 32 ||
        gpu_ir_shader_function_count(shader) != 1 || gpu_ir_shader_find_function(shader, 10)->parameter_count != 1 ||
        gpu_ir_shader_block_count(shader) != 1 || gpu_ir_shader_block_at(shader, 0)->label_id != 12 ||
        !addition || addition->opcode != GPU_IR_SHADER_OP_ARITHMETIC || addition->source_opcode != 128 ||
        gpu_ir_shader_operand_at(shader, 0)->value != 20 || gpu_ir_shader_literal_word_at(shader, 0) != 2) {
        gpu_ir_shader_release(shader);
        return fail("typed shader IR lost source identity, structure or owned operand data");
    }
    gpu_ir_shader_retain(shader);
    gpu_ir_shader_release(shader);
    if (gpu_ir_shader_instruction_count(shader) != 4) {
        gpu_ir_shader_release(shader);
        return fail("typed shader IR did not retain its arrays across a shared reference");
    }
    gpu_ir_shader_release(shader);

    types[1].bit_width = 32;
    operands[0].value = 20;
    literals[0] = 2;
    gpu_ir_shader_instruction specialization_instructions[4];
    memcpy(specialization_instructions, instructions, sizeof specialization_instructions);
    specialization_instructions[0].flags = GPU_IR_SHADER_FLAG_SPECIALIZATION;
    gpu_ir_shader_desc specialization_desc = desc;
    specialization_desc.instructions = specialization_instructions;
    gpu_ir_shader *generic_shader = gpu_ir_shader_create(&specialization_desc);
    uint32_t override_value = 9, specialized_threads[3] = { 4, 2, 1 };
    gpu_ir_shader_constant_override override = { 22, &override_value, 1 };
    gpu_ir_shader *specialized_shader = NULL;
    if (!generic_shader || gpu_ir_shader_specialize(generic_shader, &override, 1, specialized_threads,
                                                     &specialized_shader) != GPU_IR_OK || !specialized_shader) {
        gpu_ir_shader_release(generic_shader);
        gpu_ir_shader_release(specialized_shader);
        return fail("typed shader specialization failed to produce an immutable pipeline variant");
    }
    uint32_t specialized_size[3] = { 0 };
    gpu_ir_shader_threadgroup_size(specialized_shader, specialized_size);
    const gpu_ir_shader_instruction *generic_constant = gpu_ir_shader_instruction_at(generic_shader, 0);
    const gpu_ir_shader_instruction *specialized_constant = gpu_ir_shader_instruction_at(specialized_shader, 0);
    if (!generic_constant || !specialized_constant ||
        !(generic_constant->flags & GPU_IR_SHADER_FLAG_SPECIALIZATION) ||
        (specialized_constant->flags & GPU_IR_SHADER_FLAG_SPECIALIZATION) ||
        gpu_ir_shader_literal_word_at(generic_shader, 0) != 2 ||
        gpu_ir_shader_literal_word_at(specialized_shader, 0) != 9 ||
        specialized_size[0] != 4 || specialized_size[1] != 2 || specialized_size[2] != 1) {
        gpu_ir_shader_release(generic_shader);
        gpu_ir_shader_release(specialized_shader);
        return fail("specialization changed the source shader or lost effective constants/workgroup size");
    }
    gpu_ir_shader *default_shader = NULL;
    if (gpu_ir_shader_specialize(generic_shader, NULL, 0, NULL, &default_shader) != GPU_IR_OK || !default_shader ||
        (gpu_ir_shader_instruction_at(default_shader, 0)->flags & GPU_IR_SHADER_FLAG_SPECIALIZATION) ||
        gpu_ir_shader_literal_word_at(default_shader, 0) != 2) {
        gpu_ir_shader_release(default_shader);
        gpu_ir_shader_release(generic_shader);
        gpu_ir_shader_release(specialized_shader);
        return fail("specialization did not materialize an unmodified default constant");
    }
    gpu_ir_shader_release(default_shader);
    gpu_ir_shader_release(generic_shader);
    gpu_ir_shader_release(specialized_shader);
    puts("gpu_ir: specialization constants, defaults, overrides and immutable compute workgroup variants: ok");

    gpu_ir_shader_desc invalid_size = desc;
    invalid_size.threadgroup_size[1] = 0;
    if (gpu_ir_shader_create(&invalid_size)) return fail("typed compute shader IR accepted a partial local size");

    operands[0].value = 20;
    gpu_ir_shader_instruction invalid_instructions[4];
    memcpy(invalid_instructions, instructions, sizeof invalid_instructions);
    invalid_instructions[0].result_id = invalid_instructions[2].result_id;
    gpu_ir_shader_desc invalid = desc;
    invalid.instructions = invalid_instructions;
    if (gpu_ir_shader_create(&invalid)) return fail("typed shader IR accepted duplicate result IDs");
    invalid_instructions[0].result_id = 22;
    invalid_instructions[2].first_operand = 3;
    if (gpu_ir_shader_create(&invalid)) return fail("typed shader IR accepted an out-of-range operand span");
    memcpy(invalid_instructions, instructions, sizeof invalid_instructions);
    invalid_instructions[2].operation = UINT32_MAX;
    invalid.instructions = invalid_instructions;
    if (gpu_ir_shader_create(&invalid)) return fail("typed shader IR accepted an unknown normalized operation");
    operands[0].value = 99;
    invalid = desc;
    if (gpu_ir_shader_create(&invalid)) return fail("typed shader IR accepted an unresolved ID operand");
    operands[0].value = 20;
    invalid = desc;
    invalid.stage_mask = GPU_IR_STAGE_COMPUTE | GPU_IR_STAGE_VERTEX;
    if (gpu_ir_shader_create(&invalid)) return fail("typed shader IR accepted multiple stages for one shader");
    puts("gpu_ir: immutable typed shader modules, source metadata, SSA identity, spans and lifetime: ok");
    return 0;
}

int main(void) {
    if (shader_model_test()) return 1;
    uint32_t destroyed = 0, index = 0;
    gpu_ir_resource *src = new_buffer(3, 128, &destroyed);
    gpu_ir_resource *dst = new_buffer(7, 256, &destroyed);
    if (!src || !dst) return fail("buffer allocation failed");

    gpu_ir_stream stream;
    gpu_ir_stream_init(&stream);
    if (gpu_ir_record_copy_buffer(&stream, src, 8, dst, 32, 64, &index) != GPU_IR_OK || index != 0)
        return fail("valid copy was not recorded");
    if (gpu_ir_stream_count(&stream) != 1) return fail("recorded command count is wrong");
    if (gpu_ir_record_copy_buffer(&stream, src, 65, dst, 0, 64, NULL) != GPU_IR_RANGE || gpu_ir_stream_count(&stream) != 1)
        return fail("out-of-range copy was recorded");
    uint32_t update_data[4] = { 0x12345678u, 0x87654321u, 0x10203040u, 0xfedcba98u };
    if (gpu_ir_record_update_buffer(&stream, dst, 64, update_data, sizeof update_data, &index) != GPU_IR_OK ||
        index != 1 || gpu_ir_command_kind(&stream, index) != GPU_IR_COMMAND_UPDATE_BUFFER)
        return fail("valid buffer update was not recorded");
    memset(update_data, 0, sizeof update_data);
    if (gpu_ir_record_fill_buffer(&stream, dst, 80, 32, 0xa1b2c3d4u, &index) != GPU_IR_OK ||
        index != 2 || gpu_ir_command_kind(&stream, index) != GPU_IR_COMMAND_FILL_BUFFER)
        return fail("valid repeated-pattern buffer fill was not recorded");
    if (gpu_ir_record_fill_buffer(&stream, dst, 1, 4, 0, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_fill_buffer(&stream, dst, 240, 32, 0, NULL) != GPU_IR_RANGE ||
        gpu_ir_stream_count(&stream) != 3)
        return fail("invalid buffer fill alignment or range was recorded");
    if (gpu_ir_record_update_buffer(&stream, dst, 252, update_data, sizeof update_data, NULL) != GPU_IR_RANGE ||
        gpu_ir_stream_count(&stream) != 3)
        return fail("out-of-range buffer update was recorded");
    index = 0;
    if (gpu_ir_record_copy_buffer(&stream, src, 0, dst, 0, 0, &index) != GPU_IR_OK || index != GPU_IR_NO_COMMAND)
        return fail("zero-length copy was not a no-op");
    uint8_t element_pattern[2] = { 0x78, 0x56 };
    if (gpu_ir_record_fill_buffer_pattern(&stream, dst, 96, 8, element_pattern, sizeof element_pattern, &index) != GPU_IR_OK ||
        index != 3 || gpu_ir_command_kind(&stream, index) != GPU_IR_COMMAND_FILL_BUFFER)
        return fail("valid narrow element pattern was not recorded");
    memset(element_pattern, 0, sizeof element_pattern);
    if (gpu_ir_record_fill_buffer_pattern(&stream, dst, 97, 4, element_pattern, sizeof element_pattern, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_fill_buffer_pattern(&stream, dst, 96, 3, element_pattern, sizeof element_pattern, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_fill_buffer_pattern(&stream, dst, 96, 4, element_pattern, 3, NULL) != GPU_IR_INVALID ||
        gpu_ir_stream_count(&stream) != 4)
        return fail("invalid repeated element pattern was recorded");

    gpu_ir_resource_release(src);
    gpu_ir_resource_release(dst);
    fake_backend state = { 0 };
    gpu_ir_backend backend = { .context = &state, .copy_buffer = copy_buffer, .update_buffer = update_buffer,
                               .fill_buffer = fill_buffer, .fill_buffer_pattern = fill_buffer_pattern };
    if (gpu_ir_execute(&stream, 0, &backend) != GPU_IR_OK || state.calls != 1 || state.src_id != 3 || state.dst_id != 7 ||
        state.src_offset != 8 || state.dst_offset != 32 || state.size != 64)
        return fail("execution did not preserve resources and range");
    if (gpu_ir_execute(&stream, 4, &backend) != GPU_IR_INVALID) return fail("invalid command index was accepted");
    if (gpu_ir_execute(&stream, 1, &backend) != GPU_IR_OK || state.update_calls != 1 || state.dst_id != 7 ||
        state.dst_offset != 64 || state.size != 16 || state.update_words[0] != 0x12345678u ||
        state.update_words[1] != 0x87654321u || state.update_words[2] != 0x10203040u ||
        state.update_words[3] != 0xfedcba98u)
        return fail("recorded buffer update did not retain its destination range and source bytes");
    if (gpu_ir_execute(&stream, 2, &backend) != GPU_IR_OK || state.fill_calls != 1 || state.dst_id != 7 ||
        state.dst_offset != 80 || state.size != 32 || state.fill_value != 0xa1b2c3d4u)
        return fail("recorded repeated-pattern fill changed its destination, range or value");
    if (gpu_ir_execute(&stream, 3, &backend) != GPU_IR_OK || state.fill_calls != 2 ||
        state.fill_pattern_calls != 2 || state.fill_pattern_size != 2 || state.dst_offset != 96 ||
        state.size != 8 || state.fill_pattern[0] != 0x78 || state.fill_pattern[1] != 0x56)
        return fail("recorded narrow buffer pattern did not snapshot and preserve its range");
    if (destroyed != 0) return fail("recorded resources were released too early");
    gpu_ir_stream_clear(&stream);
    if (destroyed != 2 || gpu_ir_stream_count(&stream) != 0) return fail("clear did not release retained resources");
    gpu_ir_stream_destroy(&stream);
    puts("gpu_ir: buffer copy/update and repeated 1/2/4-byte fill patterns, bounds, snapshots and lifetime: ok");

    gpu_ir_texture_desc td = { .width = 4, .height = 4, .depth = 1, .array_layers = 2,
        .mip_levels = 2, .bytes_per_texel = 4, .sample_count = 1 };
    gpu_ir_resource *tex = new_texture(11, &td, &destroyed);
    gpu_ir_resource *upload = new_buffer(13, 64, &destroyed);
    gpu_ir_stream_init(&stream);
    gpu_ir_texture_location buffer_loc = { .row_pitch = 16, .image_pitch = 64,
        .buffer_width = 4, .buffer_height = 4, .buffer_depth = 1, .origin_x = 1, .origin_y = 1 };
    gpu_ir_texture_location texture_loc = { .mip_level = 1, .array_slice = 1 };
    if (!tex || !upload || gpu_ir_record_copy_texture(&stream, upload, &buffer_loc, tex, &texture_loc, 2, 2, 1, &index) != GPU_IR_OK || index != 0)
        return fail("buffer-to-texture copy was not recorded");
    texture_loc.origin_x = 1;
    if (gpu_ir_record_copy_texture(&stream, upload, &buffer_loc, tex, &texture_loc, 2, 2, 1, NULL) != GPU_IR_RANGE ||
        gpu_ir_stream_count(&stream) != 1) return fail("out-of-range texture copy was recorded");
    texture_loc.origin_x = 0;
    texture_loc.array_slice = 2;
    if (gpu_ir_record_copy_texture(&stream, upload, &buffer_loc, tex, &texture_loc, 1, 1, 1, NULL) != GPU_IR_RANGE)
        return fail("invalid texture array slice was accepted");
    texture_loc.array_slice = 1;
    buffer_loc.row_pitch = 4;
    if (gpu_ir_record_copy_texture(&stream, upload, &buffer_loc, tex, &texture_loc, 2, 2, 1, NULL) != GPU_IR_RANGE)
        return fail("undersized texture row pitch was accepted");
    buffer_loc.row_pitch = 16;
    gpu_ir_texture_desc copy_msaa_desc = td;
    copy_msaa_desc.sample_count = 4;
    gpu_ir_resource *copy_msaa = new_texture(14, &copy_msaa_desc, &destroyed);
    if (!copy_msaa || gpu_ir_record_copy_texture(&stream, upload, &buffer_loc, copy_msaa, &texture_loc,
                                                  1, 1, 1, NULL) != GPU_IR_RANGE || gpu_ir_stream_count(&stream) != 1)
        return fail("texture footprint copy accepted a multisampled texture");
    gpu_ir_resource_release(copy_msaa);
    gpu_ir_backend texture_backend = { .context = &state, .copy_texture = copy_texture, .copy_resource = copy_resource };
    if (gpu_ir_execute(&stream, 0, &texture_backend) != GPU_IR_OK || state.texture_calls != 1 ||
        state.src_id != 13 || state.dst_id != 11 || state.width != 2 || state.height != 2 || state.depth != 1 ||
        state.src_location.origin_x != 1 || state.src_location.origin_y != 1 || state.dst_location.mip_level != 1 ||
        state.dst_location.array_slice != 1) return fail("texture execution did not preserve location and extent");
    gpu_ir_resource_release(tex);
    gpu_ir_resource_release(upload);
    gpu_ir_stream_destroy(&stream);
    if (destroyed != 5) return fail("texture copy did not release retained resources");
    puts("gpu_ir: texture copy locations, mip/slice bounds, pitch validation and lifetime: ok");

    gpu_ir_texture_desc bc_desc = { .width = 6, .height = 6, .depth = 1, .array_layers = 1, .mip_levels = 1,
        .sample_count = 1, .block_width = 4, .block_height = 4, .bytes_per_block = 8 };
    uint32_t bc_destroyed = 0;
    gpu_ir_resource *bc_texture = new_texture(33, &bc_desc, &bc_destroyed);
    gpu_ir_resource *bc_buffer = new_buffer(35, 512, &bc_destroyed);
    gpu_ir_texture_location bc_buffer_loc = { .row_pitch = 256, .image_pitch = 512,
        .buffer_width = 8, .buffer_height = 8, .buffer_depth = 1 };
    gpu_ir_texture_location bc_texture_loc = { 0 };
    gpu_ir_stream_init(&stream);
    if (!bc_texture || !bc_buffer ||
        gpu_ir_record_copy_texture(&stream, bc_buffer, &bc_buffer_loc, bc_texture, &bc_texture_loc, 6, 6, 1, &index) != GPU_IR_OK || index != 0)
        return fail("edge-sized BC texture upload was rejected");
    bc_texture_loc.origin_x = 1;
    if (gpu_ir_record_copy_texture(&stream, bc_buffer, &bc_buffer_loc, bc_texture, &bc_texture_loc, 4, 4, 1, NULL) != GPU_IR_RANGE)
        return fail("misaligned BC texture origin was accepted");
    bc_texture_loc.origin_x = 0;
    gpu_ir_resource_release(bc_texture);
    gpu_ir_resource_release(bc_buffer);
    if (gpu_ir_execute(&stream, 0, &texture_backend) != GPU_IR_OK || state.texture_calls != 2 ||
        state.src_id != 35 || state.dst_id != 33 || state.width != 6 || state.height != 6)
        return fail("BC texture copy execution lost its edge extent");
    gpu_ir_stream_destroy(&stream);
    if (bc_destroyed != 2) return fail("BC texture copy did not retain and release resources");
    puts("gpu_ir: block-compressed texture layout, edge regions and block alignment: ok");

    gpu_ir_texture_desc other_desc = td;
    other_desc.width = 8;
    gpu_ir_resource *copy_src = new_texture(17, &td, &destroyed);
    gpu_ir_resource *copy_dst = new_texture(19, &td, &destroyed);
    gpu_ir_resource *copy_bad = new_texture(23, &other_desc, &destroyed);
    gpu_ir_stream_init(&stream);
    if (!copy_src || !copy_dst || !copy_bad) return fail("texture resource allocation failed");
    if (gpu_ir_record_copy_resource(&stream, copy_src, copy_bad, NULL) != GPU_IR_RANGE ||
        gpu_ir_stream_count(&stream) != 0) return fail("resource copy accepted mismatched texture dimensions");
    if (gpu_ir_record_copy_resource(&stream, copy_src, copy_dst, &index) != GPU_IR_OK || index != 0)
        return fail("compatible full texture copy was not recorded");
    gpu_ir_resource_release(copy_src);
    gpu_ir_resource_release(copy_dst);
    gpu_ir_resource_release(copy_bad);
    if (gpu_ir_execute(&stream, 0, &texture_backend) != GPU_IR_OK || state.resource_calls != 1 ||
        state.src_id != 17 || state.dst_id != 19) return fail("full texture copy did not reach the backend");
    gpu_ir_stream_destroy(&stream);
    if (destroyed != 8) return fail("full texture copy did not release retained resources");
    puts("gpu_ir: full resource copy compatibility, execution and lifetime: ok");

    gpu_ir_texture_desc msaa_desc = td;
    msaa_desc.sample_count = 4;
    gpu_ir_texture_desc bad_msaa_desc = td;
    bad_msaa_desc.sample_count = 2;
    gpu_ir_resource *msaa = new_texture(25, &msaa_desc, &destroyed);
    gpu_ir_resource *resolve_dst = new_texture(27, &td, &destroyed);
    gpu_ir_resource *bad_resolve_dst = new_texture(28, &bad_msaa_desc, &destroyed);
    gpu_ir_stream_init(&stream);
    gpu_ir_resolve_location src_resolve = { .mip_level = 1, .array_slice = 1 };
    gpu_ir_resolve_location dst_resolve = { .mip_level = 1, .array_slice = 1 };
    if (!msaa || !resolve_dst || !bad_resolve_dst ||
        gpu_ir_record_resolve_texture(&stream, msaa, &src_resolve, bad_resolve_dst, &dst_resolve,
                                      GPU_IR_RESOLVE_MODE_AVERAGE, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_resolve_texture(&stream, msaa, &src_resolve, resolve_dst, &dst_resolve, 0, NULL) != GPU_IR_INVALID ||
        gpu_ir_stream_count(&stream) != 0) return fail("resolve accepted a multisampled destination");
    gpu_ir_resolve_region invalid_region = { .destination_x = 1, .width = 2, .height = 1 };
    if (gpu_ir_record_resolve_texture_region(&stream, msaa, &src_resolve, resolve_dst, &dst_resolve,
                                             &invalid_region, GPU_IR_RESOLVE_MODE_AVERAGE, NULL) != GPU_IR_RANGE ||
        gpu_ir_stream_count(&stream) != 0) return fail("resolve accepted a region outside the destination mip");
    if (gpu_ir_record_resolve_texture(&stream, msaa, &src_resolve, resolve_dst, &dst_resolve,
                                      GPU_IR_RESOLVE_MODE_AVERAGE, &index) != GPU_IR_OK ||
        index != 0 || gpu_ir_command_kind(&stream, index) != GPU_IR_COMMAND_RESOLVE_TEXTURE)
        return fail("valid multisample resolve was not recorded");
    if (gpu_ir_record_resolve_texture(&stream, msaa, &src_resolve, resolve_dst, &dst_resolve,
                                      GPU_IR_RESOLVE_MODE_MIN, &index) != GPU_IR_OK || index != 1 ||
        gpu_ir_record_resolve_texture(&stream, msaa, &src_resolve, resolve_dst, &dst_resolve,
                                      GPU_IR_RESOLVE_MODE_MAX, &index) != GPU_IR_OK || index != 2 ||
        gpu_ir_stream_count(&stream) != 3) return fail("min/max resolve modes were not recorded");
    gpu_ir_resolve_region partial_region = {
        .source_x = 0, .source_y = 0, .destination_x = 1, .destination_y = 1, .width = 1, .height = 1
    };
    if (gpu_ir_record_resolve_texture_region(&stream, msaa, &src_resolve, resolve_dst, &dst_resolve,
                                             &partial_region, GPU_IR_RESOLVE_MODE_AVERAGE, &index) != GPU_IR_OK ||
        index != 3 || gpu_ir_stream_count(&stream) != 4)
        return fail("valid partial multisample resolve was not recorded");
    gpu_ir_resource_release(msaa);
    gpu_ir_resource_release(resolve_dst);
    gpu_ir_resource_release(bad_resolve_dst);
    gpu_ir_backend resolve_backend = { .context = &state, .resolve_texture = resolve_texture };
    if (gpu_ir_execute(&stream, 0, &resolve_backend) != GPU_IR_OK || state.resolve_calls != 1 ||
        state.src_id != 25 || state.dst_id != 27 || state.src_mip != 1 || state.src_slice != 1 ||
        state.dst_mip != 1 || state.dst_slice != 1 || state.resolve_mode != GPU_IR_RESOLVE_MODE_AVERAGE ||
        state.resolve_region.source_x != 0 || state.resolve_region.source_y != 0 ||
        state.resolve_region.destination_x != 0 || state.resolve_region.destination_y != 0 ||
        state.resolve_region.width != 2 || state.resolve_region.height != 2)
        return fail("resolve execution lost subresource selection or mode");
    if (gpu_ir_execute(&stream, 1, &resolve_backend) != GPU_IR_OK || state.resolve_calls != 2 ||
        state.resolve_mode != GPU_IR_RESOLVE_MODE_MIN ||
        gpu_ir_execute(&stream, 2, &resolve_backend) != GPU_IR_OK || state.resolve_calls != 3 ||
        state.resolve_mode != GPU_IR_RESOLVE_MODE_MAX ||
        gpu_ir_execute(&stream, 3, &resolve_backend) != GPU_IR_OK || state.resolve_calls != 4 ||
        state.resolve_region.source_x != 0 || state.resolve_region.source_y != 0 ||
        state.resolve_region.destination_x != 1 || state.resolve_region.destination_y != 1 ||
        state.resolve_region.width != 1 || state.resolve_region.height != 1)
        return fail("resolve execution lost min/max mode or its partial region");
    gpu_ir_stream_destroy(&stream);
    if (destroyed != 11) return fail("resolve did not retain and release its textures");
    puts("gpu_ir: multisample resolve validation, execution and lifetime: ok");

    uint32_t clear_destroyed = 0;
    gpu_ir_texture_desc clear_desc = { .width = 32, .height = 24, .depth = 1, .array_layers = 1,
        .mip_levels = 1, .bytes_per_texel = 4, .sample_count = 1 };
    gpu_ir_resource *color_target = new_texture(61, &clear_desc, &clear_destroyed);
    gpu_ir_resource *depth_target = new_texture(63, &clear_desc, &clear_destroyed);
    gpu_ir_rect clear_rects[2] = { { -2, 1, 12, 18 }, { 8, 5, 30, 24 } };
    const float clear_rgba[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    gpu_ir_stream_init(&stream);
    if (!color_target || !depth_target ||
        gpu_ir_record_clear_color(&stream, color_target, clear_rgba, clear_rects, 2, &index) != GPU_IR_OK ||
        index != 0 || gpu_ir_command_kind(&stream, index) != GPU_IR_COMMAND_CLEAR_COLOR ||
        gpu_ir_record_clear_depth_stencil(&stream, depth_target, GPU_IR_CLEAR_DEPTH_STENCIL_ALL,
            0.625f, 0xa5u, clear_rects, 1, &index) != GPU_IR_OK ||
        index != 1 || gpu_ir_command_kind(&stream, index) != GPU_IR_COMMAND_CLEAR_DEPTH_STENCIL ||
        gpu_ir_record_clear_depth_stencil(&stream, depth_target, 0, 0, 0, NULL, 0, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_clear_color(&stream, color_target, clear_rgba, NULL, 1, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_clear_color(&stream, color_target, clear_rgba, NULL, 0, &index) != GPU_IR_OK ||
        index != 2 || gpu_ir_stream_count(&stream) != 3)
        return fail("attachment clear recording accepted invalid inputs or lost command order");
    clear_rects[0] = (gpu_ir_rect){ 99, 99, 100, 100 };
    gpu_ir_resource_release(color_target);
    gpu_ir_resource_release(depth_target);
    gpu_ir_backend clear_backend = { .context = &state, .clear_color = clear_color,
                                     .clear_depth_stencil = clear_depth_stencil };
    if (gpu_ir_execute(&stream, 0, &clear_backend) != GPU_IR_OK || state.clear_color_calls != 1 ||
        state.clear_texture_id != 61 || state.clear_rectangle_count != 2 ||
        state.clear_rectangles[0].left != -2 || state.clear_rectangles[0].right != 12 ||
        state.clear_rectangles[1].top != 5 || state.clear_rectangles[1].bottom != 24 ||
        memcmp(state.clear_color, clear_rgba, sizeof clear_rgba))
        return fail("color clear execution lost its copied attachment, values or rectangle list");
    if (gpu_ir_execute(&stream, 1, &clear_backend) != GPU_IR_OK || state.clear_depth_stencil_calls != 1 ||
        state.clear_texture_id != 63 || state.clear_aspects != GPU_IR_CLEAR_DEPTH_STENCIL_ALL ||
        state.clear_depth != 0.625f || state.clear_stencil != 0xa5u || state.clear_rectangle_count != 1 ||
        state.clear_rectangles[0].left != -2 || state.clear_rectangles[0].bottom != 18)
        return fail("depth-stencil clear execution lost its aspects, values or copied rectangle");
    if (gpu_ir_execute(&stream, 2, &clear_backend) != GPU_IR_OK || state.clear_color_calls != 2 ||
        state.clear_texture_id != 61 || state.clear_rectangle_count != 0 ||
        memcmp(state.clear_color, clear_rgba, sizeof clear_rgba))
        return fail("full attachment clear did not preserve an empty rectangle list");
    gpu_ir_stream_destroy(&stream);
    if (clear_destroyed != 2) return fail("attachment clears did not retain and release their textures");
    puts("gpu_ir: color and depth-stencil attachment clears, rectangle snapshots, execution and lifetime: ok");

    gpu_ir_resource *before = new_buffer(29, 64, &destroyed);
    gpu_ir_resource *after = new_buffer(31, 64, &destroyed);
    gpu_ir_resource *barrier_buffer = new_buffer(37, 64, &destroyed);
    gpu_ir_resource *barrier_texture = new_texture(41, &td, &destroyed);
    if (!before || !after || !barrier_buffer || !barrier_texture) return fail("barrier resource allocation failed");
    gpu_ir_stream_init(&stream);
    gpu_ir_barrier_desc transition = {
        .type = GPU_IR_BARRIER_TRANSITION,
        .flags = GPU_IR_BARRIER_BEGIN_ONLY,
        .subresource = UINT32_MAX,
        .before_access = GPU_IR_ACCESS_COPY_WRITE,
        .after_access = GPU_IR_ACCESS_SHADER_READ,
        .resource = before
    };
    if (gpu_ir_record_barrier(&stream, &transition, &index) != GPU_IR_OK || index != 0 ||
        gpu_ir_command_kind(&stream, index) != GPU_IR_COMMAND_BARRIER)
        return fail("transition barrier was not recorded as a common command");
    gpu_ir_barrier_desc aliasing = {
        .type = GPU_IR_BARRIER_ALIASING,
        .resource = before,
        .resource_after = after
    };
    if (gpu_ir_record_barrier(&stream, &aliasing, &index) != GPU_IR_OK || index != 1)
        return fail("aliasing barrier order was not preserved");
    gpu_ir_barrier_desc global = { .type = GPU_IR_BARRIER_GLOBAL };
    if (gpu_ir_record_barrier(&stream, &global, &index) != GPU_IR_OK || index != 2)
        return fail("global barrier was not recorded");
    gpu_ir_barrier_desc memory_dependency = {
        .type = GPU_IR_BARRIER_MEMORY_DEPENDENCY,
        .source_stage_scope = GPU_IR_SYNC_STAGE_TOP_OF_PIPE,
        .destination_stage_scope = GPU_IR_SYNC_STAGE_ALL_GRAPHICS,
        .dependency_flags = GPU_IR_BARRIER_DEPENDENCY_BY_REGION,
        .source_queue_family = GPU_IR_QUEUE_FAMILY_IGNORED,
        .destination_queue_family = GPU_IR_QUEUE_FAMILY_IGNORED
    };
    if (gpu_ir_record_barrier(&stream, &memory_dependency, &index) != GPU_IR_OK || index != 3)
        return fail("scoped memory dependency was not recorded");
    gpu_ir_barrier_desc buffer_dependency = {
        .type = GPU_IR_BARRIER_BUFFER_DEPENDENCY,
        .before_access = GPU_IR_ACCESS_COPY_WRITE,
        .after_access = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE,
        .resource = barrier_buffer,
        .source_stage_scope = GPU_IR_SYNC_STAGE_TRANSFER,
        .destination_stage_scope = GPU_IR_SYNC_STAGE_COMPUTE_SHADER,
        .source_queue_family = 2,
        .destination_queue_family = 4,
        .buffer_offset = 16,
        .buffer_size = 32
    };
    if (gpu_ir_record_barrier(&stream, &buffer_dependency, &index) != GPU_IR_OK || index != 4)
        return fail("scoped buffer dependency was not recorded");
    gpu_ir_barrier_desc image_dependency = {
        .type = GPU_IR_BARRIER_IMAGE_DEPENDENCY,
        .before_access = GPU_IR_ACCESS_COPY_WRITE,
        .after_access = GPU_IR_ACCESS_SHADER_READ,
        .resource = barrier_texture,
        .source_stage_scope = GPU_IR_SYNC_STAGE_COPY,
        .destination_stage_scope = GPU_IR_SYNC_STAGE_FRAGMENT_SHADER,
        .old_layout = GPU_IR_IMAGE_LAYOUT_TRANSFER_DESTINATION,
        .new_layout = GPU_IR_IMAGE_LAYOUT_SHADER_READ_ONLY,
        .source_queue_family = GPU_IR_QUEUE_FAMILY_IGNORED,
        .destination_queue_family = GPU_IR_QUEUE_FAMILY_IGNORED,
        .image_aspects = GPU_IR_IMAGE_ASPECT_COLOR,
        .base_mip_level = 1,
        .mip_level_count = 1,
        .base_array_layer = 1,
        .array_layer_count = 1
    };
    if (gpu_ir_record_barrier(&stream, &image_dependency, &index) != GPU_IR_OK || index != 5)
        return fail("scoped image dependency was not recorded");
    gpu_ir_barrier_desc execution_only = {
        .type = GPU_IR_BARRIER_MEMORY_DEPENDENCY,
        .source_stage_scope = GPU_IR_SYNC_STAGE_TRANSFER,
        .destination_stage_scope = GPU_IR_SYNC_STAGE_COMPUTE_SHADER,
        .source_queue_family = GPU_IR_QUEUE_FAMILY_IGNORED,
        .destination_queue_family = GPU_IR_QUEUE_FAMILY_IGNORED
    };
    if (gpu_ir_record_barrier(&stream, &execution_only, &index) != GPU_IR_OK || index != 6)
        return fail("execution-only dependency was not recorded");
    gpu_ir_barrier_desc invalid = transition;
    invalid.flags = GPU_IR_BARRIER_BEGIN_ONLY | GPU_IR_BARRIER_END_ONLY;
    gpu_ir_barrier_desc invalid_buffer = buffer_dependency;
    invalid_buffer.buffer_offset = 60;
    gpu_ir_barrier_desc invalid_image = image_dependency;
    invalid_image.base_mip_level = 2;
    gpu_ir_barrier_desc invalid_scope = memory_dependency;
    invalid_scope.source_stage_scope = UINT64_C(1) << 63;
    gpu_ir_barrier_desc invalid_queue_family = image_dependency;
    invalid_queue_family.source_queue_family = GPU_IR_QUEUE_FAMILY_IGNORED;
    invalid_queue_family.destination_queue_family = 2;
    if (gpu_ir_record_barrier(&stream, &invalid, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_barrier(&stream, &invalid_buffer, NULL) != GPU_IR_RANGE ||
        gpu_ir_record_barrier(&stream, &invalid_image, NULL) != GPU_IR_RANGE ||
        gpu_ir_record_barrier(&stream, &invalid_scope, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_barrier(&stream, &invalid_queue_family, NULL) != GPU_IR_INVALID ||
        gpu_ir_stream_count(&stream) != 7)
        return fail("invalid split barrier flags were accepted");
    gpu_ir_resource_release(before);
    gpu_ir_resource_release(after);
    gpu_ir_resource_release(barrier_buffer);
    gpu_ir_resource_release(barrier_texture);
    gpu_ir_backend barrier_backend = { .context = &state, .barrier = barrier };
    if (gpu_ir_execute(&stream, 0, &barrier_backend) != GPU_IR_OK || state.barrier_calls != 1 ||
        state.barrier_type != GPU_IR_BARRIER_TRANSITION || state.barrier_flags != GPU_IR_BARRIER_BEGIN_ONLY ||
        state.barrier_subresource != UINT32_MAX || state.barrier_before_access != GPU_IR_ACCESS_COPY_WRITE ||
        state.barrier_after_access != GPU_IR_ACCESS_SHADER_READ || state.src_id != 29)
        return fail("transition barrier fields or retained resource were not preserved");
    if (gpu_ir_execute(&stream, 1, &barrier_backend) != GPU_IR_OK || state.barrier_calls != 2 ||
        state.barrier_type != GPU_IR_BARRIER_ALIASING || state.src_id != 29 || state.dst_id != 31)
        return fail("aliasing barrier fields or order were not preserved");
    if (gpu_ir_execute(&stream, 2, &barrier_backend) != GPU_IR_OK || state.barrier_calls != 3 ||
        state.barrier_type != GPU_IR_BARRIER_GLOBAL || state.src_id || state.dst_id)
        return fail("global barrier was not dispatched");
    if (gpu_ir_execute(&stream, 3, &barrier_backend) != GPU_IR_OK || state.barrier_calls != 4 ||
        state.barrier_type != GPU_IR_BARRIER_MEMORY_DEPENDENCY || state.src_id || state.dst_id ||
        state.barrier_description.source_stage_scope != GPU_IR_SYNC_STAGE_TOP_OF_PIPE ||
        state.barrier_description.destination_stage_scope != GPU_IR_SYNC_STAGE_ALL_GRAPHICS ||
        state.barrier_description.dependency_flags != GPU_IR_BARRIER_DEPENDENCY_BY_REGION)
        return fail("scoped memory dependency lost its execution and dependency scopes");
    if (gpu_ir_execute(&stream, 4, &barrier_backend) != GPU_IR_OK || state.barrier_calls != 5 ||
        state.barrier_type != GPU_IR_BARRIER_BUFFER_DEPENDENCY || state.src_id != 37 || state.dst_id ||
        state.barrier_description.buffer_offset != 16 || state.barrier_description.buffer_size != 32 ||
        state.barrier_description.source_queue_family != 2 || state.barrier_description.destination_queue_family != 4 ||
        state.barrier_description.before_access != GPU_IR_ACCESS_COPY_WRITE ||
        state.barrier_description.after_access != (GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE))
        return fail("buffer dependency lost its range, access scope, or queue-family transfer");
    if (gpu_ir_execute(&stream, 5, &barrier_backend) != GPU_IR_OK || state.barrier_calls != 6 ||
        state.barrier_type != GPU_IR_BARRIER_IMAGE_DEPENDENCY || state.src_id != 41 || state.dst_id ||
        state.barrier_description.old_layout != GPU_IR_IMAGE_LAYOUT_TRANSFER_DESTINATION ||
        state.barrier_description.new_layout != GPU_IR_IMAGE_LAYOUT_SHADER_READ_ONLY ||
        state.barrier_description.image_aspects != GPU_IR_IMAGE_ASPECT_COLOR ||
        state.barrier_description.base_mip_level != 1 || state.barrier_description.mip_level_count != 1 ||
        state.barrier_description.base_array_layer != 1 || state.barrier_description.array_layer_count != 1)
        return fail("image dependency lost its layout transition or subresource range");
    if (gpu_ir_execute(&stream, 6, &barrier_backend) != GPU_IR_OK || state.barrier_calls != 7 ||
        state.barrier_type != GPU_IR_BARRIER_MEMORY_DEPENDENCY || state.src_id || state.dst_id ||
        state.barrier_description.before_access || state.barrier_description.after_access ||
        state.barrier_description.source_stage_scope != GPU_IR_SYNC_STAGE_TRANSFER ||
        state.barrier_description.destination_stage_scope != GPU_IR_SYNC_STAGE_COMPUTE_SHADER)
        return fail("execution-only dependency scopes were not preserved");
    gpu_ir_stream_destroy(&stream);
    if (destroyed != 15) return fail("barriers did not retain and release their resource references");
    puts("gpu_ir: legacy barriers and scoped memory, buffer and image dependencies: ok");

    gpu_ir_descriptor_binding bindings[] = {
        { 1, 3, GPU_IR_DESCRIPTOR_STORAGE_BUFFER, 4, GPU_IR_STAGE_COMPUTE,
          GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE, 0 },
        { 0, 2, GPU_IR_DESCRIPTOR_UNIFORM_BUFFER, 1, GPU_IR_STAGE_VERTEX, GPU_IR_ACCESS_UNIFORM_READ, 0 },
        { 0, 0, GPU_IR_DESCRIPTOR_SAMPLED_IMAGE, 8, GPU_IR_STAGE_FRAGMENT, GPU_IR_ACCESS_SHADER_READ, 0 },
        { 0, 1, GPU_IR_DESCRIPTOR_SAMPLER, 1, GPU_IR_STAGE_FRAGMENT, 0, 0 },
        { 2, 0, GPU_IR_DESCRIPTOR_STORAGE_IMAGE, GPU_IR_DESCRIPTOR_COUNT_UNBOUNDED, GPU_IR_STAGE_COMPUTE,
          GPU_IR_ACCESS_SHADER_WRITE, 0 }
    };
    gpu_ir_push_constant_range push_constants[] = {
        { 16, 16, GPU_IR_STAGE_VERTEX },
        { 20, 8, GPU_IR_STAGE_FRAGMENT }
    };
    gpu_ir_pipeline_layout_desc layout_desc = { bindings, 5, push_constants, 2 };
    gpu_ir_pipeline_layout *layout = gpu_ir_pipeline_layout_create(&layout_desc);
    if (!layout || gpu_ir_pipeline_layout_binding_count(layout) != 5 ||
        gpu_ir_pipeline_layout_push_constant_count(layout) != 2) return fail("valid pipeline layout was rejected");
    bindings[0].binding = 99;
    gpu_ir_descriptor_binding binding;
    if (gpu_ir_pipeline_layout_find_binding(layout, 1, 3, &binding) != GPU_IR_OK ||
        binding.kind != GPU_IR_DESCRIPTOR_STORAGE_BUFFER || binding.count != 4 ||
        binding.access_mask != (GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE))
        return fail("layout did not own and normalize descriptor bindings");
    if (gpu_ir_pipeline_layout_binding_at(layout, 0, &binding) != GPU_IR_OK ||
        binding.set != 0 || binding.binding != 0 || binding.kind != GPU_IR_DESCRIPTOR_SAMPLED_IMAGE)
        return fail("descriptor bindings were not sorted by set and binding");
    if (gpu_ir_pipeline_layout_find_binding(layout, 9, 7, &binding) != GPU_IR_NOT_FOUND ||
        gpu_ir_pipeline_layout_binding_at(layout, 5, &binding) != GPU_IR_RANGE)
        return fail("layout lookup accepted a missing or out-of-range binding");
    gpu_ir_push_constant_range push;
    if (gpu_ir_pipeline_layout_push_constant_at(layout, 0, &push) != GPU_IR_OK ||
        push.offset != 16 || push.size != 16 || push.stage_mask != GPU_IR_STAGE_VERTEX)
        return fail("push-constant ranges were not retained in normalized order");
    gpu_ir_pipeline_layout_retain(layout);
    gpu_ir_pipeline_layout_release(layout);
    if (gpu_ir_pipeline_layout_binding_count(layout) != 5) return fail("retained pipeline layout was released early");
    gpu_ir_pipeline_layout_release(layout);
    puts("gpu_ir: descriptor layouts, stage/access validation, push-constant ranges and lifetime: ok");

    gpu_ir_descriptor_binding state_bindings[] = {
        { .set = 0, .binding = 0, .kind = GPU_IR_DESCRIPTOR_SAMPLED_IMAGE, .count = 8,
          .stage_mask = GPU_IR_STAGE_FRAGMENT, .access_mask = GPU_IR_ACCESS_SHADER_READ },
        { .set = 0, .binding = 1, .kind = GPU_IR_DESCRIPTOR_SAMPLER, .count = 1,
          .stage_mask = GPU_IR_STAGE_FRAGMENT },
        { .set = 0, .binding = 2, .kind = GPU_IR_DESCRIPTOR_UNIFORM_BUFFER, .count = 1,
          .stage_mask = GPU_IR_STAGE_VERTEX, .access_mask = GPU_IR_ACCESS_UNIFORM_READ },
        { .set = 1, .binding = 3, .kind = GPU_IR_DESCRIPTOR_STORAGE_BUFFER, .count = 4,
          .stage_mask = GPU_IR_STAGE_COMPUTE, .access_mask = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE },
        { .set = 2, .binding = 0, .kind = GPU_IR_DESCRIPTOR_STORAGE_IMAGE, .count = GPU_IR_DESCRIPTOR_COUNT_UNBOUNDED,
          .stage_mask = GPU_IR_STAGE_COMPUTE, .access_mask = GPU_IR_ACCESS_SHADER_WRITE }
    };
    gpu_ir_pipeline_layout_desc state_layout_desc = { .bindings = state_bindings, .binding_count = 5 };
    gpu_ir_pipeline_layout *state_layout = gpu_ir_pipeline_layout_create(&state_layout_desc);
    uint32_t descriptor_destroyed = 0, descriptor_index = GPU_IR_NO_COMMAND;
    gpu_ir_resource *descriptor_buffer = new_buffer(51, 128, &descriptor_destroyed);
    gpu_ir_resource *descriptor_aux_buffer = new_buffer(56, 64, &descriptor_destroyed);
    gpu_ir_resource *descriptor_texture = new_texture(52, &td, &descriptor_destroyed);
    fake_resource *image_view = malloc(sizeof *image_view), *sampler_state = malloc(sizeof *sampler_state),
                  *typed_view_object = malloc(sizeof *typed_view_object);
    if (!state_layout || !descriptor_buffer || !descriptor_aux_buffer || !descriptor_texture || !image_view || !sampler_state || !typed_view_object)
        return fail("descriptor-state setup failed");
    *image_view = (fake_resource){ 53, &descriptor_destroyed };
    *sampler_state = (fake_resource){ 54, &descriptor_destroyed };
    *typed_view_object = (fake_resource){ 55, &descriptor_destroyed };
    gpu_ir_buffer_view_desc state_view_desc = {
        .kind = GPU_IR_BUFFER_VIEW_TYPED,
        .access_mask = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE,
        .byte_offset = 16, .byte_size = 16, .element_count = 4, .element_size = 4
    };
    gpu_ir_buffer_view *descriptor_view = gpu_ir_buffer_view_create(descriptor_buffer, &state_view_desc,
                                                                    typed_view_object, release_fake);
    if (!descriptor_view) return fail("typed descriptor view setup failed");
    gpu_ir_descriptor_value descriptor_values[] = {
        { .set = 0, .binding = 0, .array_element = 2, .kind = GPU_IR_DESCRIPTOR_SAMPLED_IMAGE,
          .stage_mask = GPU_IR_STAGE_FRAGMENT, .access_mask = GPU_IR_ACCESS_SHADER_READ,
          .resource = descriptor_texture, .backend_object = image_view, .backend_release = release_fake },
        { .set = 0, .binding = 1, .kind = GPU_IR_DESCRIPTOR_SAMPLER, .stage_mask = GPU_IR_STAGE_FRAGMENT,
          .backend_object = sampler_state, .backend_release = release_fake },
        { .set = 0, .binding = 2, .kind = GPU_IR_DESCRIPTOR_UNIFORM_BUFFER, .stage_mask = GPU_IR_STAGE_VERTEX,
          .access_mask = GPU_IR_ACCESS_UNIFORM_READ, .resource = descriptor_buffer, .offset = 16 },
        { .set = 1, .binding = 3, .array_element = 2, .kind = GPU_IR_DESCRIPTOR_STORAGE_BUFFER,
          .stage_mask = GPU_IR_STAGE_COMPUTE, .access_mask = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE,
          .buffer_view = descriptor_view, .auxiliary_resource = descriptor_aux_buffer,
          .auxiliary_access_mask = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE,
          .auxiliary_offset = 16, .auxiliary_range = 4 },
        { .set = 2, .binding = 0, .kind = GPU_IR_DESCRIPTOR_STORAGE_IMAGE, .stage_mask = GPU_IR_STAGE_COMPUTE,
          .access_mask = GPU_IR_ACCESS_SHADER_WRITE, .flags = GPU_IR_DESCRIPTOR_VALUE_NULL }
    };
    gpu_ir_descriptor_state_desc state_desc = { .values = descriptor_values, .value_count = 5 };
    gpu_ir_descriptor_value invalid_value = descriptor_values[0];
    invalid_value.array_element = 8;
    gpu_ir_descriptor_state_desc invalid_state_desc = { .values = &invalid_value, .value_count = 1 };
    if (!state_layout || gpu_ir_descriptor_state_create(state_layout, &invalid_state_desc))
        return fail("descriptor value outside its declared array was accepted");
    gpu_ir_descriptor_value invalid_aux_value = descriptor_values[0];
    invalid_aux_value.auxiliary_resource = descriptor_aux_buffer;
    invalid_aux_value.auxiliary_access_mask = GPU_IR_ACCESS_SHADER_READ;
    invalid_aux_value.auxiliary_range = 4;
    gpu_ir_descriptor_state_desc invalid_aux_desc = { .values = &invalid_aux_value, .value_count = 1 };
    if (gpu_ir_descriptor_state_create(state_layout, &invalid_aux_desc))
        return fail("an auxiliary buffer access on a non-buffer descriptor was accepted");
    gpu_ir_descriptor_state *descriptor_state = gpu_ir_descriptor_state_create(state_layout, &state_desc);
    gpu_ir_descriptor_value auxiliary_value;
    if (!descriptor_state || gpu_ir_descriptor_state_value_count(descriptor_state) != 5 ||
        gpu_ir_descriptor_state_find_for_stage(descriptor_state, 0, 0, 2, 0, GPU_IR_STAGE_VERTEX, &invalid_value) != GPU_IR_OK ||
        invalid_value.range != 112 || gpu_ir_descriptor_state_find(descriptor_state, 2, 0, 0, 0, &invalid_value) != GPU_IR_OK ||
        invalid_value.flags != GPU_IR_DESCRIPTOR_VALUE_NULL ||
        gpu_ir_descriptor_state_find(descriptor_state, 1, 0, 3, 2, &auxiliary_value) != GPU_IR_OK ||
        auxiliary_value.auxiliary_resource != descriptor_aux_buffer || auxiliary_value.auxiliary_offset != 16 ||
        auxiliary_value.auxiliary_range != 4 ||
        auxiliary_value.auxiliary_access_mask != (GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE))
        return fail("descriptor snapshot did not normalize ranges, stages, or null values");
    gpu_ir_stream_init(&stream);
    if (gpu_ir_record_bind_descriptor_state(&stream, descriptor_state, &descriptor_index) != GPU_IR_OK ||
        descriptor_index != 0 || gpu_ir_command_kind(&stream, descriptor_index) != GPU_IR_COMMAND_BIND_DESCRIPTOR_STATE)
        return fail("descriptor snapshot bind was not recorded as an ordered command");
    gpu_ir_pipeline_layout_release(state_layout);
    gpu_ir_descriptor_state_release(descriptor_state);
    gpu_ir_resource_release(descriptor_buffer);
    gpu_ir_resource_release(descriptor_aux_buffer);
    gpu_ir_resource_release(descriptor_texture);
    gpu_ir_buffer_view_release(descriptor_view);
    fake_backend descriptor_backend_state = { 0 };
    gpu_ir_backend descriptor_backend = { .context = &descriptor_backend_state,
        .bind_descriptor_state = bind_descriptor_state };
    if (gpu_ir_execute(&stream, descriptor_index, &descriptor_backend) != GPU_IR_OK ||
        descriptor_backend_state.descriptor_state_calls != 1 || descriptor_backend_state.descriptor_state_value_count != 5 ||
        descriptor_backend_state.descriptor_kinds[0] != GPU_IR_DESCRIPTOR_SAMPLED_IMAGE ||
        descriptor_backend_state.descriptor_elements[0] != 2 || descriptor_backend_state.descriptor_resource_ids[0] != 52 ||
        descriptor_backend_state.descriptor_backend_ids[0] != 53 ||
        descriptor_backend_state.descriptor_kinds[2] != GPU_IR_DESCRIPTOR_UNIFORM_BUFFER ||
        descriptor_backend_state.descriptor_resource_ids[2] != 51 || descriptor_backend_state.descriptor_ranges[2] != 112 ||
        descriptor_backend_state.descriptor_backend_ids[3] != 55 ||
        descriptor_backend_state.descriptor_flags[4] != GPU_IR_DESCRIPTOR_VALUE_NULL)
        return fail("descriptor bind callback lost layout keys, resources, views, ranges, or null state");
    gpu_ir_stream_destroy(&stream);
    if (descriptor_destroyed != 6) return fail("descriptor snapshot did not retain and release all referenced objects");
    puts("gpu_ir: descriptor snapshots, auxiliary accesses, sparse/null entries, stage aliases, backend binding and lifetime: ok");

    gpu_ir_descriptor_binding duplicate_bindings[] = {
        { 0, 4, GPU_IR_DESCRIPTOR_SAMPLER, 1, GPU_IR_STAGE_FRAGMENT, 0, 0 },
        { 0, 4, GPU_IR_DESCRIPTOR_SAMPLED_IMAGE, 1, GPU_IR_STAGE_FRAGMENT, GPU_IR_ACCESS_SHADER_READ, 0 }
    };
    gpu_ir_pipeline_layout_desc invalid_layout = { duplicate_bindings, 2, NULL, 0 };
    if (gpu_ir_pipeline_layout_create(&invalid_layout)) return fail("duplicate set/binding pair was accepted");
    duplicate_bindings[1] = (gpu_ir_descriptor_binding){ 0, 5, GPU_IR_DESCRIPTOR_UNIFORM_BUFFER, 1,
                                                         GPU_IR_STAGE_VERTEX, GPU_IR_ACCESS_SHADER_WRITE, 0 };
    if (gpu_ir_pipeline_layout_create(&invalid_layout)) return fail("invalid descriptor access was accepted");
    gpu_ir_push_constant_range overlapping_push[] = {
        { 0, 16, GPU_IR_STAGE_VERTEX }, { 8, 8, GPU_IR_STAGE_VERTEX }
    };
    invalid_layout = (gpu_ir_pipeline_layout_desc){ NULL, 0, overlapping_push, 2 };
    if (gpu_ir_pipeline_layout_create(&invalid_layout)) return fail("overlapping push constants for one stage were accepted");
    puts("gpu_ir: duplicate descriptors, invalid access and overlapping push constants rejected: ok");

    gpu_ir_descriptor_binding namespaced[] = {
        { .set = 2, .binding = 5, .kind = GPU_IR_DESCRIPTOR_READ_ONLY_RESOURCE, .count = 4,
          .stage_mask = GPU_IR_STAGE_VERTEX, .access_mask = GPU_IR_ACCESS_SHADER_READ, .binding_namespace = 0 },
        { .set = 2, .binding = 5, .kind = GPU_IR_DESCRIPTOR_STORAGE_RESOURCE, .count = 1,
          .stage_mask = GPU_IR_STAGE_VERTEX, .access_mask = GPU_IR_ACCESS_SHADER_WRITE, .binding_namespace = 1 },
        { .set = 2, .binding = 5, .kind = GPU_IR_DESCRIPTOR_UNIFORM_BUFFER, .count = 1,
          .stage_mask = GPU_IR_STAGE_VERTEX, .access_mask = GPU_IR_ACCESS_UNIFORM_READ, .binding_namespace = 2 },
        { .set = 2, .binding = 5, .kind = GPU_IR_DESCRIPTOR_SAMPLER, .count = 1,
          .stage_mask = GPU_IR_STAGE_VERTEX, .access_mask = 0, .binding_namespace = 3 },
        { .set = 1, .binding = 7, .kind = GPU_IR_DESCRIPTOR_READ_ONLY_RESOURCE, .count = 2,
          .stage_mask = GPU_IR_STAGE_VERTEX, .access_mask = GPU_IR_ACCESS_SHADER_READ, .binding_namespace = 0 },
        { .set = 1, .binding = 7, .kind = GPU_IR_DESCRIPTOR_READ_ONLY_RESOURCE, .count = 6,
          .stage_mask = GPU_IR_STAGE_FRAGMENT, .access_mask = GPU_IR_ACCESS_SHADER_READ, .binding_namespace = 0 }
    };
    gpu_ir_pipeline_layout_desc namespaced_desc = { .bindings = namespaced, .binding_count = 6 };
    layout = gpu_ir_pipeline_layout_create(&namespaced_desc);
    if (!layout || gpu_ir_pipeline_layout_binding_count(layout) != 6 ||
        gpu_ir_pipeline_layout_find_binding_in_namespace(layout, 2, 1, 5, &binding) != GPU_IR_OK ||
        binding.kind != GPU_IR_DESCRIPTOR_STORAGE_RESOURCE || binding.access_mask != GPU_IR_ACCESS_SHADER_WRITE ||
        gpu_ir_pipeline_layout_find_binding_for_stage(layout, 1, 0, 7, GPU_IR_STAGE_FRAGMENT, &binding) != GPU_IR_OK ||
        binding.count != 6 || binding.stage_mask != GPU_IR_STAGE_FRAGMENT ||
        gpu_ir_pipeline_layout_find_binding_for_stage(layout, 1, 0, 7, GPU_IR_STAGE_COMPUTE, &binding) != GPU_IR_NOT_FOUND ||
        gpu_ir_pipeline_layout_find_binding_in_namespace(layout, 1, 0, 7, &binding) != GPU_IR_INVALID) {
        gpu_ir_pipeline_layout_release(layout);
        return fail("namespaced or stage-specific descriptor bindings were not preserved");
    }
    uint32_t alias_destroyed = 0;
    gpu_ir_resource *alias_buffer = new_buffer(61, 64, &alias_destroyed);
    gpu_ir_descriptor_value alias_values[] = {
        { .set = 1, .binding = 7, .kind = GPU_IR_DESCRIPTOR_READ_ONLY_RESOURCE,
          .stage_mask = GPU_IR_STAGE_VERTEX, .access_mask = GPU_IR_ACCESS_SHADER_READ, .resource = alias_buffer },
        { .set = 1, .binding = 7, .kind = GPU_IR_DESCRIPTOR_READ_ONLY_RESOURCE,
          .stage_mask = GPU_IR_STAGE_FRAGMENT, .access_mask = GPU_IR_ACCESS_SHADER_READ, .resource = alias_buffer }
    };
    gpu_ir_descriptor_state_desc alias_desc = { .values = alias_values, .value_count = 2 };
    gpu_ir_descriptor_state *alias_state = gpu_ir_descriptor_state_create(layout, &alias_desc);
    gpu_ir_descriptor_value alias_value;
    if (!alias_buffer || !alias_state ||
        gpu_ir_descriptor_state_find_for_stage(alias_state, 1, 0, 7, 0, GPU_IR_STAGE_VERTEX, &alias_value) != GPU_IR_OK ||
        gpu_ir_descriptor_state_find_for_stage(alias_state, 1, 0, 7, 0, GPU_IR_STAGE_FRAGMENT, &alias_value) != GPU_IR_OK ||
        gpu_ir_descriptor_state_find(alias_state, 1, 0, 7, 0, &alias_value) != GPU_IR_INVALID) {
        gpu_ir_descriptor_state_release(alias_state);
        gpu_ir_resource_release(alias_buffer);
        gpu_ir_pipeline_layout_release(layout);
        return fail("stage-specific descriptor aliases were not kept distinct");
    }
    gpu_ir_descriptor_state_release(alias_state);
    gpu_ir_resource_release(alias_buffer);
    if (alias_destroyed != 1) { gpu_ir_pipeline_layout_release(layout); return fail("stage aliases did not release their resource"); }
    gpu_ir_pipeline_layout_release(layout);
    namespaced[5].stage_mask = GPU_IR_STAGE_VERTEX;
    if (gpu_ir_pipeline_layout_create(&namespaced_desc)) return fail("overlapping stage-specific register bindings were accepted");
    puts("gpu_ir: independent register namespaces and stage-specific overlapping bindings: ok");

    uint32_t view_destroyed = 0;
    gpu_ir_resource *view_buffer = new_buffer(41, 64, &view_destroyed);
    fake_resource *backend_view = malloc(sizeof *backend_view);
    if (!view_buffer || !backend_view) return fail("buffer-view setup failed");
    *backend_view = (fake_resource){ 42, &view_destroyed };
    gpu_ir_buffer_view_desc view_desc = {
        .kind = GPU_IR_BUFFER_VIEW_TYPED, .access_mask = GPU_IR_ACCESS_SHADER_READ,
        .byte_offset = 16, .byte_size = 16, .element_count = 4, .element_size = 4
    };
    gpu_ir_buffer_view *buffer_view = gpu_ir_buffer_view_create(view_buffer, &view_desc, backend_view, release_fake);
    if (!buffer_view || gpu_ir_buffer_view_resource(buffer_view) != view_buffer ||
        gpu_ir_buffer_view_backend_object(buffer_view) != backend_view ||
        gpu_ir_buffer_view_description(buffer_view)->byte_offset != 16 ||
        gpu_ir_buffer_view_description(buffer_view)->element_count != 4)
        return fail("typed buffer view did not retain its resource, range, or backend view");
    gpu_ir_buffer_view_desc invalid_view = view_desc;
    invalid_view.byte_offset = 60;
    if (gpu_ir_buffer_view_create(view_buffer, &invalid_view, NULL, NULL))
        return fail("out-of-range typed buffer view was accepted");
    invalid_view = view_desc;
    invalid_view.access_mask = GPU_IR_ACCESS_COPY_READ;
    if (gpu_ir_buffer_view_create(view_buffer, &invalid_view, NULL, NULL))
        return fail("buffer view with a non-shader access was accepted");
    invalid_view = (gpu_ir_buffer_view_desc){
        .kind = GPU_IR_BUFFER_VIEW_RAW, .access_mask = GPU_IR_ACCESS_SHADER_READ,
        .byte_offset = 2, .byte_size = 8, .element_count = 2
    };
    if (gpu_ir_buffer_view_create(view_buffer, &invalid_view, NULL, NULL))
        return fail("misaligned raw buffer view was accepted");
    gpu_ir_buffer_view_desc structured = {
        .kind = GPU_IR_BUFFER_VIEW_STRUCTURED, .access_mask = GPU_IR_ACCESS_SHADER_READ,
        .byte_offset = 32, .byte_size = 32, .element_count = 4, .structure_stride = 8
    };
    gpu_ir_buffer_view *structured_view = gpu_ir_buffer_view_create(view_buffer, &structured, NULL, NULL);
    if (!structured_view) return fail("valid structured buffer view was rejected");
    gpu_ir_buffer_view_release(structured_view);
    gpu_ir_buffer_view_retain(buffer_view);
    gpu_ir_resource_release(view_buffer);
    gpu_ir_buffer_view_release(buffer_view);
    if (view_destroyed != 0) return fail("buffer view did not retain its resource and backend view");
    gpu_ir_buffer_view_release(buffer_view);
    if (view_destroyed != 2) return fail("buffer view released its retained resource or backend view incorrectly");
    puts("gpu_ir: typed, structured and raw buffer view ranges, access and lifetime: ok");

    uint32_t pipeline_destroyed = 0, pipeline_index = GPU_IR_NO_COMMAND;
    fake_resource *compute_native = malloc(sizeof *compute_native);
    if (!compute_native) return fail("compute pipeline backend setup failed");
    *compute_native = (fake_resource){ 71, &pipeline_destroyed };
    gpu_ir_pipeline_desc compute_desc = {
        .info = { .kind = GPU_IR_PIPELINE_COMPUTE, .stage_mask = GPU_IR_STAGE_COMPUTE,
                  .threadgroup_size = { 8, 4, 2 } },
        .backend_object = compute_native,
        .release = release_fake
    };
    gpu_ir_pipeline *compute_pipeline = gpu_ir_pipeline_create(&compute_desc);
    if (!compute_pipeline) { free(compute_native); return fail("valid compute pipeline was rejected"); }
    gpu_ir_pipeline_desc invalid_compute = compute_desc;
    invalid_compute.info.stage_mask = GPU_IR_STAGE_FRAGMENT;
    if (gpu_ir_pipeline_create(&invalid_compute)) return fail("compute pipeline with non-compute stages was accepted");

    fake_resource *graphics_native = malloc(sizeof *graphics_native);
    if (!graphics_native) return fail("graphics pipeline backend setup failed");
    *graphics_native = (fake_resource){ 72, &pipeline_destroyed };
    gpu_ir_pipeline_desc graphics_desc = {
        .info = { .kind = GPU_IR_PIPELINE_GRAPHICS, .stage_mask = GPU_IR_STAGE_VERTEX },
        .backend_object = graphics_native,
        .release = release_fake
    };
    gpu_ir_pipeline *graphics_pipeline = gpu_ir_pipeline_create(&graphics_desc);
    if (!graphics_pipeline) { free(graphics_native); return fail("valid graphics pipeline was rejected"); }
    uint32_t access_destroyed = 0;
    gpu_ir_resource *access_buffer = new_buffer(75, 256, &access_destroyed);
    if (!access_buffer) return fail("command access resource setup failed");
    gpu_ir_resource_access shader_access = { access_buffer, GPU_IR_STAGE_COMPUTE,
        GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE };
    gpu_ir_resource_access invalid_access = shader_access;
    invalid_access.stage_mask = 0;
    gpu_ir_resource_access read_access = { access_buffer, GPU_IR_STAGE_COMPUTE, GPU_IR_ACCESS_SHADER_READ };
    gpu_ir_resource_access host_write_access = { access_buffer, GPU_IR_STAGE_COMPUTE, GPU_IR_ACCESS_HOST_WRITE };
    gpu_ir_resource_access generic_write_access = { access_buffer, GPU_IR_STAGE_COMPUTE, GPU_IR_ACCESS_MEMORY_WRITE };
    if (!gpu_ir_resource_access_conflicts(&shader_access, &read_access) ||
        !gpu_ir_resource_access_conflicts(&host_write_access, &read_access) ||
        !gpu_ir_resource_access_conflicts(&generic_write_access, &read_access) ||
        gpu_ir_resource_access_conflicts(&read_access, &read_access))
        return fail("resource access conflict classification was incorrect");
    gpu_ir_stream_init(&stream);
    if (gpu_ir_record_dispatch(&stream, graphics_pipeline, 1, 1, 1, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_dispatch_with_accesses(&stream, compute_pipeline, 2, 3, 4, &invalid_access, 1, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_dispatch_with_accesses(&stream, compute_pipeline, 2, 3, 4, &shader_access, 1, &pipeline_index) != GPU_IR_OK ||
        pipeline_index != 0 || gpu_ir_command_kind(&stream, pipeline_index) != GPU_IR_COMMAND_DISPATCH ||
        gpu_ir_record_dispatch(&stream, compute_pipeline, 0, 3, 4, &pipeline_index) != GPU_IR_OK ||
        pipeline_index != GPU_IR_NO_COMMAND || gpu_ir_stream_count(&stream) != 1)
        return fail("dispatch pipeline type, no-op group count, or recording order was invalid");
    gpu_ir_resource_access retained_access;
    if (gpu_ir_command_access_count(&stream, 0) != 1 ||
        gpu_ir_command_access_at(&stream, 0, 0, &retained_access) != GPU_IR_OK ||
        retained_access.stage_mask != GPU_IR_STAGE_COMPUTE ||
        retained_access.access_mask != (GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE) ||
        gpu_ir_command_access_at(&stream, 0, 1, &retained_access) != GPU_IR_RANGE)
        return fail("recorded resource access metadata was not retained");
    gpu_ir_resource_release(access_buffer);
    if (access_destroyed) return fail("dispatch command did not retain its accessed resource");
    gpu_ir_pipeline_release(graphics_pipeline);
    gpu_ir_pipeline_release(compute_pipeline);
    if (pipeline_destroyed != 1) return fail("dispatch command did not retain its compute pipeline");
    fake_backend dispatch_state = { 0 };
    gpu_ir_backend dispatch_backend = { .context = &dispatch_state, .dispatch = dispatch };
    if (gpu_ir_execute(&stream, 0, &dispatch_backend) != GPU_IR_OK || dispatch_state.dispatch_calls != 1 ||
        dispatch_state.src_id != 71 || dispatch_state.group_count[0] != 2 || dispatch_state.group_count[1] != 3 ||
        dispatch_state.group_count[2] != 4 || dispatch_state.threadgroup_size[0] != 8 ||
        dispatch_state.threadgroup_size[1] != 4 || dispatch_state.threadgroup_size[2] != 2 ||
        dispatch_state.access_count != 1 || dispatch_state.access_resource_id != 75 ||
        dispatch_state.access_stage_mask != GPU_IR_STAGE_COMPUTE ||
        dispatch_state.access_mask != (GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE))
        return fail("dispatch did not preserve the pipeline and dimensions");
    if (gpu_ir_execute_with_accesses(&stream, 0, &dispatch_backend, &read_access, 1) != GPU_IR_OK ||
        dispatch_state.dispatch_calls != 2 || dispatch_state.access_count != 1 ||
        dispatch_state.access_mask != GPU_IR_ACCESS_SHADER_READ)
        return fail("execute-time resource access override was not applied");
    gpu_ir_stream_destroy(&stream);
    if (pipeline_destroyed != 2 || access_destroyed != 1) return fail("dispatch stream did not release retained pipeline and accesses");
    puts("gpu_ir: dispatch access snapshots/overrides, pipeline validation, no-op and lifetime: ok");

    uint32_t indirect_destroyed = 0;
    gpu_ir_resource *indirect_args = new_buffer(73, 24, &indirect_destroyed);
    uint32_t indirect_use_destroyed = 0;
    gpu_ir_resource *indirect_use = new_buffer(76, 64, &indirect_use_destroyed);
    compute_native = malloc(sizeof *compute_native);
    if (!indirect_args || !indirect_use || !compute_native) return fail("indirect dispatch setup failed");
    *compute_native = (fake_resource){ 74, &pipeline_destroyed };
    compute_desc.backend_object = compute_native;
    compute_pipeline = gpu_ir_pipeline_create(&compute_desc);
    if (!compute_pipeline) { free(compute_native); return fail("indirect compute pipeline setup failed"); }
    gpu_ir_resource_access indirect_access = { indirect_use, GPU_IR_STAGE_COMPUTE, GPU_IR_ACCESS_SHADER_READ };
    gpu_ir_stream_init(&stream);
    if (gpu_ir_record_dispatch_indirect(&stream, NULL, indirect_args, 0, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_dispatch_indirect(&stream, compute_pipeline, indirect_args, 2, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_dispatch_indirect(&stream, compute_pipeline, indirect_args, 16, NULL) != GPU_IR_RANGE ||
        gpu_ir_record_dispatch_indirect_with_accesses(&stream, compute_pipeline, indirect_args, 4,
                                                      &indirect_access, 1, &pipeline_index) != GPU_IR_OK ||
        pipeline_index != 0 || gpu_ir_command_kind(&stream, pipeline_index) != GPU_IR_COMMAND_DISPATCH_INDIRECT)
        return fail("indirect dispatch type, alignment, range or recording kind was invalid");
    gpu_ir_resource_release(indirect_args);
    gpu_ir_resource_release(indirect_use);
    gpu_ir_pipeline_release(compute_pipeline);
    if (indirect_destroyed || indirect_use_destroyed || pipeline_destroyed != 2)
        return fail("indirect dispatch did not retain its argument/access resources and pipeline");
    fake_backend indirect_state = { 0 };
    gpu_ir_backend indirect_backend = { .context = &indirect_state, .dispatch_indirect = dispatch_indirect };
    if (gpu_ir_execute(&stream, 0, &indirect_backend) != GPU_IR_OK || indirect_state.dispatch_indirect_calls != 1 ||
        indirect_state.src_id != 74 || indirect_state.dst_id != 73 || indirect_state.indirect_offset != 4 ||
        indirect_state.access_count != 1 || indirect_state.access_resource_id != 76 ||
        indirect_state.access_stage_mask != GPU_IR_STAGE_COMPUTE ||
        indirect_state.access_mask != GPU_IR_ACCESS_SHADER_READ)
        return fail("indirect dispatch did not preserve its pipeline, argument buffer and offset");
    gpu_ir_stream_destroy(&stream);
    if (indirect_destroyed != 1 || indirect_use_destroyed != 1 || pipeline_destroyed != 3)
        return fail("indirect dispatch stream did not release its retained objects");
    puts("gpu_ir: indirect dispatch access propagation, bounds, backend execution and lifetime: ok");

    uint32_t range_pipeline_destroyed = 0;
    fake_resource *range_pipeline_native = malloc(sizeof *range_pipeline_native);
    if (!range_pipeline_native) return fail("indirect dispatch range pipeline setup failed");
    *range_pipeline_native = (fake_resource){ 85, &range_pipeline_destroyed };
    compute_desc.backend_object = range_pipeline_native;
    compute_pipeline = gpu_ir_pipeline_create(&compute_desc);
    if (!compute_pipeline) { free(range_pipeline_native); return fail("indirect dispatch range pipeline was rejected"); }
    uint32_t range_args_destroyed = 0, range_count_destroyed = 0, range_access_destroyed = 0;
    gpu_ir_resource *range_args = new_buffer(86, 40, &range_args_destroyed);
    gpu_ir_resource *range_count = new_buffer(87, 8, &range_count_destroyed);
    gpu_ir_resource *range_access_buffer = new_buffer(88, 16, &range_access_destroyed);
    if (!range_args || !range_count || !range_access_buffer) return fail("indirect dispatch range resources failed");
    gpu_ir_resource_access range_access = { range_access_buffer, GPU_IR_STAGE_COMPUTE, GPU_IR_ACCESS_SHADER_WRITE };
    gpu_ir_indirect_dispatch_info range_info = {
        .dispatch_count = 2, .stride = 16, .argument_offset = 8, .count_offset = 4
    };
    gpu_ir_resource_access wrong_range_access = {
        range_access_buffer, GPU_IR_STAGE_VERTEX, GPU_IR_ACCESS_VERTEX_READ
    };
    gpu_ir_stream_init(&stream);
    if (gpu_ir_record_dispatch_indirect_range(NULL, compute_pipeline, range_args, &range_info, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_dispatch_indirect_range(&stream, NULL, range_args, &range_info, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_dispatch_indirect_range_counted(&stream, compute_pipeline, range_args, range_count,
            &(gpu_ir_indirect_dispatch_info){ .dispatch_count = 2, .stride = 8 }, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_dispatch_indirect_range_counted(&stream, compute_pipeline, range_args, range_count,
            &(gpu_ir_indirect_dispatch_info){ .dispatch_count = 2, .stride = 16, .argument_offset = 20 }, NULL) != GPU_IR_RANGE ||
        gpu_ir_record_dispatch_indirect_range_counted(&stream, compute_pipeline, range_args, range_count,
            &(gpu_ir_indirect_dispatch_info){ .dispatch_count = 2, .stride = 16, .count_offset = 8 }, NULL) != GPU_IR_RANGE ||
        gpu_ir_record_dispatch_indirect_range_counted_with_accesses(&stream, compute_pipeline, range_args, range_count,
            &range_info, &wrong_range_access, 1, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_dispatch_indirect_range_counted_with_accesses(&stream, compute_pipeline, range_args, range_count,
            &range_info, &range_access, 1, &pipeline_index) != GPU_IR_OK || pipeline_index != 0 ||
        gpu_ir_command_kind(&stream, pipeline_index) != GPU_IR_COMMAND_DISPATCH_INDIRECT_RANGE) {
        return fail("indirect dispatch range validation or recording failed");
    }
    gpu_ir_resource_release(range_args);
    gpu_ir_resource_release(range_count);
    gpu_ir_resource_release(range_access_buffer);
    gpu_ir_pipeline_release(compute_pipeline);
    if (range_args_destroyed || range_count_destroyed || range_access_destroyed || range_pipeline_destroyed)
        return fail("indirect dispatch range did not retain its GPU resources and pipeline");
    fake_backend range_state = { 0 };
    gpu_ir_backend range_backend = { .context = &range_state, .dispatch_indirect_range = dispatch_indirect_range };
    if (gpu_ir_execute(&stream, 0, &range_backend) != GPU_IR_OK ||
        gpu_ir_execute_with_accesses(&stream, 0, &range_backend, &range_access, 1) != GPU_IR_OK ||
        range_state.dispatch_indirect_range_calls != 2 || range_state.src_id != 85 ||
        range_state.dispatch_range_arguments_id != 86 || range_state.dispatch_range_count_id != 87 ||
        range_state.indirect_dispatch_range.dispatch_count != 2 || range_state.indirect_dispatch_range.stride != 16 ||
        range_state.indirect_dispatch_range.argument_offset != 8 || range_state.indirect_dispatch_range.count_offset != 4 ||
        range_state.access_count != 1 || range_state.access_resource_id != 88 ||
        range_state.access_stage_mask != GPU_IR_STAGE_COMPUTE || range_state.access_mask != GPU_IR_ACCESS_SHADER_WRITE)
        return fail("indirect dispatch range backend execution or access override failed");
    gpu_ir_stream_destroy(&stream);
    if (range_args_destroyed != 1 || range_count_destroyed != 1 || range_access_destroyed != 1 ||
        range_pipeline_destroyed != 1) return fail("indirect dispatch range did not release retained objects");
    puts("gpu_ir: indirect dispatch range count, bounds, access overrides and retained lifetimes: ok");

    uint32_t draw_destroyed = 0;
    fake_resource *draw_native = malloc(sizeof *draw_native);
    if (!draw_native) return fail("draw pipeline backend setup failed");
    *draw_native = (fake_resource){ 81, &draw_destroyed };
    graphics_desc.backend_object = draw_native;
    graphics_desc.release = release_fake;
    gpu_ir_pipeline *draw_pipeline = gpu_ir_pipeline_create(&graphics_desc);
    if (!draw_pipeline) { free(draw_native); return fail("draw graphics pipeline was rejected"); }
    gpu_ir_draw_info draw_info = { .topology = GPU_IR_TOPOLOGY_TRIANGLE_STRIP,
        .vertex_count = 4, .instance_count = 2, .first_vertex = 3, .first_instance = 5 };
    gpu_ir_draw_info indexed_info = { .topology = GPU_IR_TOPOLOGY_LINE_LIST, .indexed = 1,
        .index_count = 6, .instance_count = 3, .first_index = 7, .first_instance = 9, .vertex_offset = -2 };
    uint32_t draw_access_destroyed = 0;
    gpu_ir_resource *draw_access_buffer = new_buffer(82, 128, &draw_access_destroyed);
    if (!draw_access_buffer) return fail("draw resource access setup failed");
    gpu_ir_resource_access vertex_access = { draw_access_buffer, GPU_IR_STAGE_VERTEX, GPU_IR_ACCESS_VERTEX_READ };
    gpu_ir_resource_access wrong_draw_access = { draw_access_buffer, GPU_IR_STAGE_FRAGMENT, GPU_IR_ACCESS_SHADER_READ };
    uint32_t indirect_draw_destroyed = 0;
    gpu_ir_resource *indirect_draw_args = new_buffer(83, 48, &indirect_draw_destroyed);
    uint32_t indirect_draw_count_destroyed = 0;
    gpu_ir_resource *indirect_draw_count = new_buffer(84, 8, &indirect_draw_count_destroyed);
    gpu_ir_stream_init(&stream);
    gpu_ir_indirect_draw_info indirect_draw_info = { .topology = GPU_IR_TOPOLOGY_TRIANGLE_LIST,
        .draw_count = 2, .stride = 24, .argument_offset = 12 };
    if (!indirect_draw_args || !indirect_draw_count ||
        gpu_ir_record_draw_indirect(&stream, NULL, indirect_draw_args, &indirect_draw_info, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_draw_indirect(&stream, draw_pipeline, indirect_draw_args, &indirect_draw_info, NULL) != GPU_IR_RANGE ||
        gpu_ir_record_draw_with_accesses(&stream, draw_pipeline, &draw_info,
                                         &wrong_draw_access, 1, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_draw_with_accesses(&stream, draw_pipeline, &draw_info,
                                         &vertex_access, 1, &pipeline_index) != GPU_IR_OK || pipeline_index != 0 ||
        gpu_ir_command_kind(&stream, pipeline_index) != GPU_IR_COMMAND_DRAW ||
        gpu_ir_record_draw(&stream, draw_pipeline, &indexed_info, &pipeline_index) != GPU_IR_OK || pipeline_index != 1) {
        return fail("invalid indirect draw pipeline or range, or direct draw setup was rejected");
    }
    indirect_draw_info.argument_offset = 8;
    if (gpu_ir_record_draw_indirect_with_accesses(&stream, draw_pipeline, indirect_draw_args, &indirect_draw_info,
                                                   &wrong_draw_access, 1, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_draw_with_accesses(&stream, draw_pipeline, &draw_info,
                                         &wrong_draw_access, 1, NULL) != GPU_IR_INVALID) {
        return fail("draw resource stage access was not validated");
    }
    if (gpu_ir_record_draw_indirect_with_accesses(&stream, draw_pipeline, indirect_draw_args, &indirect_draw_info,
                                                   &vertex_access, 1, &pipeline_index) != GPU_IR_OK || pipeline_index != 2 ||
        gpu_ir_command_kind(&stream, pipeline_index) != GPU_IR_COMMAND_DRAW_INDIRECT) {
        return fail("valid indirect draw or its resource access was not recorded");
    }
    indirect_draw_info.count_offset = 4;
    if (gpu_ir_record_draw_indirect_counted(&stream, draw_pipeline, indirect_draw_args, indirect_draw_count,
                                             &indirect_draw_info, &pipeline_index) != GPU_IR_OK || pipeline_index != 3 ||
        gpu_ir_record_draw_indirect_counted(&stream, draw_pipeline, indirect_draw_args, indirect_draw_count,
                                             &(gpu_ir_indirect_draw_info){ .topology = GPU_IR_TOPOLOGY_TRIANGLE_LIST,
                                                 .draw_count = 2, .stride = 24, .argument_offset = 8,
                                                 .count_offset = 8 }, NULL) != GPU_IR_RANGE ||
        gpu_ir_record_draw_indirect_counted(&stream, draw_pipeline, indirect_draw_args, indirect_draw_count,
                                             &(gpu_ir_indirect_draw_info){ .topology = GPU_IR_TOPOLOGY_TRIANGLE_LIST,
                                                 .draw_count = 2, .stride = 24, .argument_offset = 8,
                                                 .count_offset = 1 }, NULL) != GPU_IR_INVALID) {
        return fail("counted indirect draw did not validate and retain its count buffer");
    }
    gpu_ir_resource_release(draw_access_buffer);
    gpu_ir_resource_release(indirect_draw_args);
    gpu_ir_resource_release(indirect_draw_count);
    if (draw_access_destroyed) return fail("draw command did not retain its accessed resource");
    if (indirect_draw_destroyed) return fail("indirect draw did not retain its argument buffer");
    if (indirect_draw_count_destroyed) return fail("counted indirect draw did not retain its count buffer");
    gpu_ir_draw_info invalid_draw = draw_info;
    invalid_draw.index_count = 1;
    gpu_ir_indirect_draw_info invalid_indirect_draw = indirect_draw_info;
    invalid_indirect_draw.stride = 8;
    if (gpu_ir_record_draw(&stream, draw_pipeline, &invalid_draw, NULL) != GPU_IR_INVALID ||
        gpu_ir_record_draw_indirect(&stream, draw_pipeline, indirect_draw_args, &invalid_indirect_draw, NULL) != GPU_IR_INVALID ||
        gpu_ir_stream_count(&stream) != 4) return fail("inconsistent draw arguments were accepted");
    gpu_ir_pipeline_release(draw_pipeline);
    if (draw_destroyed) return fail("draw stream did not retain its graphics pipeline");
    fake_backend draw_state = { 0 };
    gpu_ir_backend draw_backend = { .context = &draw_state, .draw = draw, .draw_indirect = draw_indirect };
    if (gpu_ir_execute(&stream, 0, &draw_backend) != GPU_IR_OK ||
        gpu_ir_execute(&stream, 1, &draw_backend) != GPU_IR_OK ||
        gpu_ir_execute_with_accesses(&stream, 2, &draw_backend, &vertex_access, 1) != GPU_IR_OK ||
        gpu_ir_execute(&stream, 3, &draw_backend) != GPU_IR_OK ||
        draw_state.draw_calls != 2 || draw_state.draw_indirect_calls != 2 ||
        draw_state.draw_pipeline_id != 81 || draw_state.draws[0].topology != GPU_IR_TOPOLOGY_TRIANGLE_STRIP ||
        draw_state.draws[0].vertex_count != 4 || draw_state.draws[0].instance_count != 2 ||
        draw_state.draws[0].first_vertex != 3 || draw_state.draws[0].first_instance != 5 ||
        draw_state.draw_access_counts[0] != 1 || draw_state.draw_access_resource_ids[0] != 82 ||
        !draw_state.draws[1].indexed || draw_state.draws[1].index_count != 6 ||
        draw_state.draws[1].first_index != 7 || draw_state.draws[1].instance_count != 3 ||
        draw_state.draws[1].vertex_offset != -2 || draw_state.draws[1].first_instance != 9 ||
        draw_state.draw_access_counts[1] != 0 || draw_state.draw_indirect_pipeline_id != 81 ||
        draw_state.draw_indirect_arguments_id != 83 || draw_state.draw_indirect_count_id != 84 ||
        draw_state.indirect_draw.topology != GPU_IR_TOPOLOGY_TRIANGLE_LIST || draw_state.indirect_draw.indexed ||
        draw_state.indirect_draw.draw_count != 2 || draw_state.indirect_draw.stride != 24 ||
        draw_state.indirect_draw.argument_offset != 8 || draw_state.indirect_draw.count_offset != 4 ||
        draw_state.access_resource_id != 82 ||
        draw_state.access_stage_mask != GPU_IR_STAGE_VERTEX || draw_state.access_mask != GPU_IR_ACCESS_VERTEX_READ)
        return fail("draw execution changed graphics pipeline or draw arguments");
    gpu_ir_stream_destroy(&stream);
    if (draw_destroyed != 1 || draw_access_destroyed != 1 || indirect_draw_destroyed != 1 ||
        indirect_draw_count_destroyed != 1)
        return fail("draw stream did not release its retained pipeline and accesses");
    puts("gpu_ir: draw access propagation, indexed arguments, execution and lifetime: ok");
    return 0;
}
