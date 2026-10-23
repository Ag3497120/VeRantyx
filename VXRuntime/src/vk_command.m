// Vulkan command buffers: recorded into a compact list (Vulkan command buffers are reusable and recorded on any
// thread), encoded into a Metal command buffer when submitted (vk_encode below). Render passes and dynamic
// rendering both record one "pass" description per Metal render pass; barriers need no command, because every Metal
// encoder waits for the one before it on its queue.
#include "vk_impl.h"
#include "vk_encode.h"
#include <stdlib.h>
#include <string.h>
#include <mach/mach_time.h>

void vk_push_set_write(struct VkDescriptorSet_T *s, uint32_t n, const VkWriteDescriptorSet *writes);
void vk_apply_template(struct VkDescriptorSet_T *s, VkDescriptorUpdateTemplate t, const void *data);
struct VkDescriptorSet_T *vk_push_set_new(VkDescriptorSetLayout l, uint64_t *words, uint64_t size);
void vk_push_set_free(struct VkDescriptorSet_T *s);
id<MTLDepthStencilState> vk_depth_stencil_state(VkDevice dev, const VkPipelineDepthStencilStateCreateInfo *ds, int has_depth, int has_stencil);
#define TRACE(...) do { if (vk_trace()) vx_log("vulkan: " __VA_ARGS__); } while (0)

// ---- the command list --------------------------------------------------------------------------------------------
enum {
    C_BIND_PIPELINE = 1, C_BIND_SETS, C_PUSH_CONSTANTS, C_BIND_VERTEX, C_BIND_INDEX, C_VIEWPORT, C_SCISSOR, C_STATE,
    C_DRAW, C_DRAW_INDEXED, C_DRAW_INDIRECT, C_DISPATCH, C_DISPATCH_INDIRECT, C_BEGIN_PASS, C_END_PASS,
    C_COPY_BUFFER, C_COPY_BUFFER_IMAGE, C_COPY_IMAGE, C_BLIT_IMAGE, C_RESOLVE_IMAGE, C_CLEAR_IMAGE, C_CLEAR_ATTACHMENTS,
    C_FILL_BUFFER, C_UPDATE_BUFFER, C_BARRIER, C_SET_EVENT, C_BEGIN_QUERY, C_END_QUERY, C_RESET_QUERIES, C_TIMESTAMP,
    C_COPY_QUERIES, C_EXECUTE, C_PUSH_SET, C_LABEL_BEGIN, C_LABEL_END
};
typedef struct { uint32_t op, size; } cmd;

typedef struct { cmd h; VkPipelineBindPoint point; VkPipeline pipeline; } c_bind_pipeline;
typedef struct { cmd h; VkPipelineBindPoint point; VkPipelineLayout layout; uint32_t first, count, dyn_count; } c_bind_sets;   // + sets[], offsets[]
typedef struct { cmd h; uint32_t offset, size; } c_push;                       // + data
typedef struct { cmd h; uint32_t first, count; int has_strides; } c_bind_vertex;   // + buffers[], offsets[], strides[]
typedef struct { cmd h; VkBuffer buffer; uint64_t offset; VkIndexType type; } c_bind_index;
typedef struct { cmd h; uint32_t first, count, with_count; } c_viewport;      // + VkViewport[] / VkRect2D[]
typedef struct { cmd h; uint32_t which; float f[4]; uint32_t u[4]; } c_state;
typedef struct { cmd h; uint32_t count, instances, first, first_instance; int32_t vertex_offset; } c_draw;
typedef struct { cmd h; VkBuffer buffer; uint64_t offset; uint32_t draws, stride; int indexed; VkBuffer count_buffer; uint64_t count_offset; } c_draw_indirect;
typedef struct { cmd h; uint32_t x, y, z, bx, by, bz; } c_dispatch;
typedef struct { cmd h; VkBuffer buffer; uint64_t offset; } c_dispatch_indirect;
typedef struct { cmd h; pass_desc pass; } c_begin_pass;
typedef struct { cmd h; VkBuffer src, dst; uint32_t count; } c_copy_buffer;   // + VkBufferCopy2[]
typedef struct { cmd h; VkBuffer buffer; VkImage image; int to_image; uint32_t count; } c_copy_buffer_image;   // + VkBufferImageCopy2[]
typedef struct { cmd h; VkImage src, dst; uint32_t count; } c_copy_image;      // + VkImageCopy2[]
typedef struct { cmd h; VkImage src, dst; VkFilter filter; uint32_t count; } c_blit_image;   // + VkImageBlit2[]
typedef struct { cmd h; VkImage src, dst; uint32_t count; } c_resolve_image;  // + VkImageResolve2[]
typedef struct { cmd h; VkImage image; int depth; VkClearValue value; uint32_t count; } c_clear_image;   // + VkImageSubresourceRange[]
typedef struct { cmd h; uint32_t attachments, rects; } c_clear_attachments;   // + VkClearAttachment[], VkClearRect[]
typedef struct { cmd h; VkBuffer buffer; uint64_t offset, size; uint32_t data; } c_fill;
typedef struct { cmd h; VkBuffer buffer; uint64_t offset, size; } c_update;   // + data
typedef struct { cmd h; VkEvent event; int set; } c_event;
typedef struct { cmd h; VkQueryPool pool; uint32_t query, count; VkQueryControlFlags flags; } c_query;
typedef struct { cmd h; VkQueryPool pool; uint32_t first, count; VkBuffer dst; uint64_t offset, stride; VkQueryResultFlags flags; } c_copy_queries;
typedef struct { cmd h; uint32_t count; } c_execute;                          // + VkCommandBuffer[]
typedef struct { cmd h; VkPipelineBindPoint point; uint32_t set; struct VkDescriptorSet_T *data; } c_push_set;

static void *record(VkCommandBuffer cb, uint32_t op, size_t size) {
    size = (size + 7) & ~(size_t)7;
    if (!cb->last || cb->last->used + size > cb->last->size) {
        size_t chunk = size > 65536 ? size : 65536;
        vk_chunk *c = malloc(sizeof *c + chunk);
        if (!c) return NULL;
        c->next = NULL; c->used = 0; c->size = chunk;
        if (cb->last) cb->last->next = c; else cb->first = c;
        cb->last = c;
    }
    cmd *h = (cmd *)(cb->last->data + cb->last->used);
    memset(h, 0, size);
    h->op = op;
    h->size = (uint32_t)size;
    cb->last->used += size;
    cb->count++;
    return h;
}

static void free_list(VkCommandBuffer cb) {
    for (vk_chunk *c = cb->first; c; c = c->next)
        for (size_t pos = 0; pos < c->used;) {
            cmd *h = (cmd *)(c->data + pos);
            if (h->op == C_PUSH_SET) { free(((c_push_set *)h)->data->words); vk_push_set_free(((c_push_set *)h)->data); }
            pos += h->size;
        }
    for (vk_chunk *c = cb->first, *n; c; c = n) { n = c->next; free(c); }
    cb->first = cb->last = NULL;
    cb->count = 0;
}

// ---- pools and buffers -------------------------------------------------------------------------------------------
VKFN VkResult VKAPI_CALL vkCreateCommandPool(VkDevice dev, const VkCommandPoolCreateInfo *info, const VkAllocationCallbacks *alloc, VkCommandPool *out) {
    (void)dev; (void)alloc;
    struct VkCommandPool_T *p = calloc(1, sizeof *p);
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->family = info->queueFamilyIndex;
    p->flags = info->flags;
    pthread_mutex_init(&p->lock, NULL);
    *out = p;
    return VK_SUCCESS;
}
static void buffer_unlink(VkCommandPool p, VkCommandBuffer cb) {
    if (cb->prev) cb->prev->next = cb->next; else p->buffers = cb->next;
    if (cb->next) cb->next->prev = cb->prev;
}
VKFN void VKAPI_CALL vkDestroyCommandPool(VkDevice dev, VkCommandPool p, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    if (!p) return;
    for (VkCommandBuffer cb = p->buffers, n; cb; cb = n) { n = cb->next; free_list(cb); free(cb); }
    pthread_mutex_destroy(&p->lock);
    free(p);
}
VKFN VkResult VKAPI_CALL vkResetCommandPool(VkDevice dev, VkCommandPool p, VkCommandPoolResetFlags flags) {
    (void)dev; (void)flags;
    pthread_mutex_lock(&p->lock);
    for (VkCommandBuffer cb = p->buffers; cb; cb = cb->next) { free_list(cb); cb->recording = 0; }
    pthread_mutex_unlock(&p->lock);
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkTrimCommandPool(VkDevice dev, VkCommandPool p, VkCommandPoolTrimFlags flags) { (void)dev; (void)p; (void)flags; }

VKFN VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice dev, const VkCommandBufferAllocateInfo *info, VkCommandBuffer *out) {
    VkCommandPool p = info->commandPool;
    pthread_mutex_lock(&p->lock);
    for (uint32_t i = 0; i < info->commandBufferCount; i++) {
        struct VkCommandBuffer_T *cb = calloc(1, sizeof *cb);
        cb->device = dev;
        cb->pool = p;
        cb->level = info->level;
        cb->next = p->buffers;
        if (p->buffers) p->buffers->prev = cb;
        p->buffers = cb;
        out[i] = cb;
    }
    pthread_mutex_unlock(&p->lock);
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkFreeCommandBuffers(VkDevice dev, VkCommandPool p, uint32_t n, const VkCommandBuffer *cbs) {
    (void)dev;
    pthread_mutex_lock(&p->lock);
    for (uint32_t i = 0; i < n; i++) {
        if (!cbs[i]) continue;
        buffer_unlink(p, cbs[i]);
        free_list(cbs[i]);
        free(cbs[i]);
    }
    pthread_mutex_unlock(&p->lock);
}
VKFN VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *info) {
    free_list(cb);
    cb->usage = info->flags;
    cb->recording = 1;
    return VK_SUCCESS;
}
VKFN VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer cb) { cb->recording = 0; return VK_SUCCESS; }
VKFN VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
    (void)flags;
    free_list(cb);
    cb->recording = 0;
    return VK_SUCCESS;
}

// ---- recording ---------------------------------------------------------------------------------------------------
VKFN void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline p) {
    c_bind_pipeline *c = record(cb, C_BIND_PIPELINE, sizeof *c);
    c->point = point; c->pipeline = p;
}
VKFN void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first,
                                             uint32_t count, const VkDescriptorSet *sets, uint32_t dyn_count, const uint32_t *dyn) {
    c_bind_sets *c = record(cb, C_BIND_SETS, sizeof *c + count * sizeof(VkDescriptorSet) + dyn_count * 4);
    c->point = point; c->layout = layout; c->first = first; c->count = count; c->dyn_count = dyn_count;
    memcpy(c + 1, sets, count * sizeof *sets);
    memcpy((uint8_t *)(c + 1) + count * sizeof *sets, dyn, dyn_count * 4);
}
VKFN void VKAPI_CALL vkCmdBindDescriptorSets2KHR(VkCommandBuffer cb, const VkBindDescriptorSetsInfoKHR *info) {
    if (info->stageFlags & VK_SHADER_STAGE_ALL_GRAPHICS)
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, info->firstSet, info->descriptorSetCount,
                                info->pDescriptorSets, info->dynamicOffsetCount, info->pDynamicOffsets);
    if (info->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, info->layout, info->firstSet, info->descriptorSetCount,
                                info->pDescriptorSets, info->dynamicOffsetCount, info->pDynamicOffsets);
}
VKFN void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset,
                                        uint32_t size, const void *data) {
    (void)layout; (void)stages;
    c_push *c = record(cb, C_PUSH_CONSTANTS, sizeof *c + size);
    c->offset = offset; c->size = size;
    memcpy(c + 1, data, size);
}
VKFN void VKAPI_CALL vkCmdPushConstants2KHR(VkCommandBuffer cb, const VkPushConstantsInfoKHR *info) {
    vkCmdPushConstants(cb, info->layout, info->stageFlags, info->offset, info->size, info->pValues);
}
static void push_set(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t set,
                     uint32_t n, const VkWriteDescriptorSet *writes, VkDescriptorUpdateTemplate tmpl, const void *data) {
    VkDescriptorSetLayout sl = layout->sets[set];
    uint64_t size = ((uint64_t)sl->words * 8 + 15) & ~15ull;
    if (!size) size = 16;
    uint64_t *words = calloc(1, size);
    struct VkDescriptorSet_T *s = vk_push_set_new(sl, words, size);
    if (tmpl) vk_apply_template(s, tmpl, data);
    else vk_push_set_write(s, n, writes);
    c_push_set *c = record(cb, C_PUSH_SET, sizeof *c);
    c->point = point; c->set = set; c->data = s;
}
VKFN void VKAPI_CALL vkCmdPushDescriptorSetKHR(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t set,
                                               uint32_t n, const VkWriteDescriptorSet *writes) {
    push_set(cb, point, layout, set, n, writes, NULL, NULL);
}
VKFN void VKAPI_CALL vkCmdPushDescriptorSetWithTemplateKHR(VkCommandBuffer cb, VkDescriptorUpdateTemplate t, VkPipelineLayout layout,
                                                           uint32_t set, const void *data) {
    push_set(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, set, 0, NULL, t, data);
}
VKFN void VKAPI_CALL vkCmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBuffer *bufs,
                                             const VkDeviceSize *offsets, const VkDeviceSize *sizes, const VkDeviceSize *strides) {
    (void)sizes;
    c_bind_vertex *c = record(cb, C_BIND_VERTEX, sizeof *c + count * (sizeof(VkBuffer) + 16));
    c->first = first; c->count = count; c->has_strides = strides != NULL;
    VkBuffer *b = (VkBuffer *)(c + 1);
    uint64_t *o = (uint64_t *)(b + count), *s = o + count;
    memcpy(b, bufs, count * sizeof *bufs);
    memcpy(o, offsets, count * 8);
    if (strides) memcpy(s, strides, count * 8);
}
VKFN void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBuffer *bufs, const VkDeviceSize *offsets) {
    vkCmdBindVertexBuffers2(cb, first, count, bufs, offsets, NULL, NULL);
}
VKFN void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
    c_bind_index *c = record(cb, C_BIND_INDEX, sizeof *c);
    c->buffer = buffer; c->offset = offset; c->type = type;
}
VKFN void VKAPI_CALL vkCmdBindIndexBuffer2KHR(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, VkIndexType type) {
    (void)size;
    vkCmdBindIndexBuffer(cb, buffer, offset, type);
}
VKFN void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkViewport *v) {
    c_viewport *c = record(cb, C_VIEWPORT, sizeof *c + count * sizeof *v);
    c->first = first; c->count = count;
    memcpy(c + 1, v, count * sizeof *v);
}
VKFN void VKAPI_CALL vkCmdSetViewportWithCount(VkCommandBuffer cb, uint32_t count, const VkViewport *v) {
    c_viewport *c = record(cb, C_VIEWPORT, sizeof *c + count * sizeof *v);
    c->first = 0; c->count = count; c->with_count = 1;
    memcpy(c + 1, v, count * sizeof *v);
}
VKFN void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkRect2D *r) {
    c_viewport *c = record(cb, C_SCISSOR, sizeof *c + count * sizeof *r);
    c->first = first; c->count = count;
    memcpy(c + 1, r, count * sizeof *r);
}
VKFN void VKAPI_CALL vkCmdSetScissorWithCount(VkCommandBuffer cb, uint32_t count, const VkRect2D *r) {
    c_viewport *c = record(cb, C_SCISSOR, sizeof *c + count * sizeof *r);
    c->first = 0; c->count = count; c->with_count = 1;
    memcpy(c + 1, r, count * sizeof *r);
}

// Dynamic states that are a few numbers: one command kind.
enum { S_LINE_WIDTH = 1, S_DEPTH_BIAS, S_BLEND_CONSTANTS, S_DEPTH_BOUNDS, S_STENCIL_COMPARE, S_STENCIL_WRITE, S_STENCIL_REF,
       S_CULL, S_FRONT_FACE, S_TOPOLOGY, S_DEPTH_TEST, S_DEPTH_WRITE, S_DEPTH_COMPARE, S_DEPTH_BOUNDS_TEST, S_STENCIL_TEST,
       S_STENCIL_OP, S_DEPTH_BIAS_ENABLE, S_PRIMITIVE_RESTART, S_RASTERIZER_DISCARD };
static void state(VkCommandBuffer cb, uint32_t which, float a, float b, float c2, float d, uint32_t u0, uint32_t u1, uint32_t u2, uint32_t u3) {
    c_state *c = record(cb, C_STATE, sizeof *c);
    c->which = which;
    c->f[0] = a; c->f[1] = b; c->f[2] = c2; c->f[3] = d;
    c->u[0] = u0; c->u[1] = u1; c->u[2] = u2; c->u[3] = u3;
}
VKFN void VKAPI_CALL vkCmdSetLineWidth(VkCommandBuffer cb, float w) { state(cb, S_LINE_WIDTH, w, 0, 0, 0, 0, 0, 0, 0); }
VKFN void VKAPI_CALL vkCmdSetDepthBias(VkCommandBuffer cb, float c, float clamp, float slope) { state(cb, S_DEPTH_BIAS, c, slope, clamp, 0, 0, 0, 0, 0); }
VKFN void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer cb, const float k[4]) { state(cb, S_BLEND_CONSTANTS, k[0], k[1], k[2], k[3], 0, 0, 0, 0); }
VKFN void VKAPI_CALL vkCmdSetDepthBounds(VkCommandBuffer cb, float lo, float hi) { state(cb, S_DEPTH_BOUNDS, lo, hi, 0, 0, 0, 0, 0, 0); }
VKFN void VKAPI_CALL vkCmdSetStencilCompareMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m) { state(cb, S_STENCIL_COMPARE, 0, 0, 0, 0, f, m, 0, 0); }
VKFN void VKAPI_CALL vkCmdSetStencilWriteMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m) { state(cb, S_STENCIL_WRITE, 0, 0, 0, 0, f, m, 0, 0); }
VKFN void VKAPI_CALL vkCmdSetStencilReference(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t r) { state(cb, S_STENCIL_REF, 0, 0, 0, 0, f, r, 0, 0); }
VKFN void VKAPI_CALL vkCmdSetCullMode(VkCommandBuffer cb, VkCullModeFlags m) { state(cb, S_CULL, 0, 0, 0, 0, m, 0, 0, 0); }
VKFN void VKAPI_CALL vkCmdSetFrontFace(VkCommandBuffer cb, VkFrontFace f) { state(cb, S_FRONT_FACE, 0, 0, 0, 0, f, 0, 0, 0); }
VKFN void VKAPI_CALL vkCmdSetPrimitiveTopology(VkCommandBuffer cb, VkPrimitiveTopology t) { state(cb, S_TOPOLOGY, 0, 0, 0, 0, t, 0, 0, 0); }
VKFN void VKAPI_CALL vkCmdSetDepthTestEnable(VkCommandBuffer cb, VkBool32 e) { state(cb, S_DEPTH_TEST, 0, 0, 0, 0, e, 0, 0, 0); }
VKFN void VKAPI_CALL vkCmdSetDepthWriteEnable(VkCommandBuffer cb, VkBool32 e) { state(cb, S_DEPTH_WRITE, 0, 0, 0, 0, e, 0, 0, 0); }
VKFN void VKAPI_CALL vkCmdSetDepthCompareOp(VkCommandBuffer cb, VkCompareOp op) { state(cb, S_DEPTH_COMPARE, 0, 0, 0, 0, op, 0, 0, 0); }
VKFN void VKAPI_CALL vkCmdSetDepthBoundsTestEnable(VkCommandBuffer cb, VkBool32 e) { state(cb, S_DEPTH_BOUNDS_TEST, 0, 0, 0, 0, e, 0, 0, 0); }
VKFN void VKAPI_CALL vkCmdSetStencilTestEnable(VkCommandBuffer cb, VkBool32 e) { state(cb, S_STENCIL_TEST, 0, 0, 0, 0, e, 0, 0, 0); }
VKFN void VKAPI_CALL vkCmdSetStencilOp(VkCommandBuffer cb, VkStencilFaceFlags f, VkStencilOp fail, VkStencilOp pass, VkStencilOp dfail, VkCompareOp cmp) {
    c_state *c = record(cb, C_STATE, sizeof *c);
    c->which = S_STENCIL_OP;
    c->u[0] = f; c->u[1] = fail; c->u[2] = pass; c->u[3] = dfail;
    c->f[0] = (float)cmp;
}
VKFN void VKAPI_CALL vkCmdSetDepthBiasEnable(VkCommandBuffer cb, VkBool32 e) { state(cb, S_DEPTH_BIAS_ENABLE, 0, 0, 0, 0, e, 0, 0, 0); }
VKFN void VKAPI_CALL vkCmdSetPrimitiveRestartEnable(VkCommandBuffer cb, VkBool32 e) { state(cb, S_PRIMITIVE_RESTART, 0, 0, 0, 0, e, 0, 0, 0); }
VKFN void VKAPI_CALL vkCmdSetRasterizerDiscardEnable(VkCommandBuffer cb, VkBool32 e) { state(cb, S_RASTERIZER_DISCARD, 0, 0, 0, 0, e, 0, 0, 0); }

VKFN void VKAPI_CALL vkCmdDraw(VkCommandBuffer cb, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t first_instance) {
    c_draw *c = record(cb, C_DRAW, sizeof *c);
    c->count = vertices; c->instances = instances; c->first = first; c->first_instance = first_instance;
}
VKFN void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t indices, uint32_t instances, uint32_t first, int32_t vertex_offset,
                                      uint32_t first_instance) {
    c_draw *c = record(cb, C_DRAW_INDEXED, sizeof *c);
    c->count = indices; c->instances = instances; c->first = first; c->vertex_offset = vertex_offset; c->first_instance = first_instance;
}
static void draw_indirect(VkCommandBuffer cb, VkBuffer b, uint64_t offset, uint32_t draws, uint32_t stride, int indexed,
                          VkBuffer count_buffer, uint64_t count_offset) {
    c_draw_indirect *c = record(cb, C_DRAW_INDIRECT, sizeof *c);
    c->buffer = b; c->offset = offset; c->draws = draws; c->stride = stride; c->indexed = indexed;
    c->count_buffer = count_buffer; c->count_offset = count_offset;
}
VKFN void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t stride) { draw_indirect(cb, b, o, n, stride, 0, NULL, 0); }
VKFN void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t stride) { draw_indirect(cb, b, o, n, stride, 1, NULL, 0); }
VKFN void VKAPI_CALL vkCmdDrawIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t max, uint32_t stride) {
    draw_indirect(cb, b, o, max, stride, 0, cbuf, co);
}
VKFN void VKAPI_CALL vkCmdDrawIndexedIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t max, uint32_t stride) {
    draw_indirect(cb, b, o, max, stride, 1, cbuf, co);
}
VKFN void VKAPI_CALL vkCmdDispatchBase(VkCommandBuffer cb, uint32_t bx, uint32_t by, uint32_t bz, uint32_t x, uint32_t y, uint32_t z) {
    c_dispatch *c = record(cb, C_DISPATCH, sizeof *c);
    c->x = x; c->y = y; c->z = z; c->bx = bx; c->by = by; c->bz = bz;
}
VKFN void VKAPI_CALL vkCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) { vkCmdDispatchBase(cb, 0, 0, 0, x, y, z); }
VKFN void VKAPI_CALL vkCmdDispatchIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize offset) {
    c_dispatch_indirect *c = record(cb, C_DISPATCH_INDIRECT, sizeof *c);
    c->buffer = b; c->offset = offset;
}

// Render passes: each subpass becomes one recorded pass with its load/store actions worked out.
static uint8_t load_of(VkAttachmentLoadOp op) { return op == VK_ATTACHMENT_LOAD_OP_CLEAR ? 1 : op == VK_ATTACHMENT_LOAD_OP_LOAD ? 0 : 2; }
static uint8_t store_of(VkAttachmentStoreOp op) { return op == VK_ATTACHMENT_STORE_OP_STORE ? 0 : 1; }

static void record_subpass(VkCommandBuffer cb, VkRenderPass rp, VkFramebuffer fb, const VkImageView *views, const VkClearValue *clears,
                           uint32_t nclears, VkRect2D area, uint32_t index) {
    const vk_subpass *sp = &rp->subpasses[index];
    c_begin_pass *c = record(cb, C_BEGIN_PASS, sizeof *c);
    pass_desc *p = &c->pass;
    p->area = area;
    p->layers = fb ? fb->layers : 1;
    p->view_mask = sp->view_mask;
    p->color_count = sp->color_count;
    for (uint32_t i = 0; i < sp->color_count; i++) {
        uint32_t a = sp->colors[i];
        if (a == VK_ATTACHMENT_UNUSED) continue;
        const VkAttachmentDescription2 *d = &rp->attachments[a];
        pass_attachment *pa = &p->colors[i];
        pa->view = views[a];
        pa->load = rp->first_use[a] == index ? load_of(d->loadOp) : 0;
        pa->store = rp->last_use[a] == index ? store_of(d->storeOp) : 0;
        if (a < nclears) pa->clear = clears[a];
        if (sp->resolves[i] != VK_ATTACHMENT_UNUSED) pa->resolve = views[sp->resolves[i]];
    }
    if (sp->depth != VK_ATTACHMENT_UNUSED) {
        uint32_t a = sp->depth;
        const VkAttachmentDescription2 *d = &rp->attachments[a];
        const vk_format_info *f = vk_format(d->format);
        pass_attachment *pa = &p->depth;
        pa->view = views[a];
        pa->load = rp->first_use[a] == index ? load_of(d->loadOp) : 0;
        pa->store = rp->last_use[a] == index ? store_of(d->storeOp) : 0;
        pa->sload = rp->first_use[a] == index ? load_of(d->stencilLoadOp) : 0;
        pa->sstore = rp->last_use[a] == index ? store_of(d->stencilStoreOp) : 0;
        if (a < nclears) pa->clear = clears[a];
        p->has_depth = f && (f->aspects & VKF_DEPTH);
        p->has_stencil = f && (f->aspects & VKF_STENCIL);
        if (sp->depth_resolve != VK_ATTACHMENT_UNUSED) { pa->resolve = views[sp->depth_resolve]; pa->resolve_mode = (uint8_t)sp->depth_resolve_mode; }
    }
}

// The begun render pass (for NextSubpass): kept in the command buffer between the begin and end calls.
typedef struct { VkRenderPass rp; VkFramebuffer fb; VkImageView views[32]; VkClearValue clears[32]; uint32_t nclears; VkRect2D area; uint32_t subpass; } begun_pass;
static begun_pass *begun(VkCommandBuffer cb) {
    static __thread begun_pass b;   // (recording a render pass on a thread happens between its begin and end calls)
    (void)cb;
    return &b;
}

VKFN void VKAPI_CALL vkCmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo *info, const VkSubpassBeginInfo *sub) {
    (void)sub;
    begun_pass *b = begun(cb);
    b->rp = info->renderPass;
    b->fb = info->framebuffer;
    b->area = info->renderArea;
    b->subpass = 0;
    b->nclears = info->clearValueCount < 32 ? info->clearValueCount : 32;
    memcpy(b->clears, info->pClearValues, b->nclears * sizeof *b->clears);
    const VkRenderPassAttachmentBeginInfo *ab = vk_find_struct(info->pNext, VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO);
    for (uint32_t i = 0; i < b->rp->attachment_count && i < 32; i++)
        b->views[i] = ab && i < ab->attachmentCount ? ab->pAttachments[i] : b->fb->views[i];
    record_subpass(cb, b->rp, b->fb, b->views, b->clears, b->nclears, b->area, 0);
}
VKFN void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *info, VkSubpassContents contents) {
    (void)contents;
    vkCmdBeginRenderPass2(cb, info, NULL);
}
VKFN void VKAPI_CALL vkCmdNextSubpass2(VkCommandBuffer cb, const VkSubpassBeginInfo *bi, const VkSubpassEndInfo *ei) {
    (void)bi; (void)ei;
    begun_pass *b = begun(cb);
    record(cb, C_END_PASS, sizeof(cmd));
    b->subpass++;
    if (b->subpass < b->rp->subpass_count) record_subpass(cb, b->rp, b->fb, b->views, b->clears, b->nclears, b->area, b->subpass);
}
VKFN void VKAPI_CALL vkCmdNextSubpass(VkCommandBuffer cb, VkSubpassContents contents) { (void)contents; vkCmdNextSubpass2(cb, NULL, NULL); }
VKFN void VKAPI_CALL vkCmdEndRenderPass2(VkCommandBuffer cb, const VkSubpassEndInfo *ei) { (void)ei; record(cb, C_END_PASS, sizeof(cmd)); }
VKFN void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer cb) { record(cb, C_END_PASS, sizeof(cmd)); }

static void rendering_attachment(pass_attachment *pa, const VkRenderingAttachmentInfo *a, int resuming, int suspending) {
    if (!a || !a->imageView) return;
    pa->view = a->imageView;
    pa->load = resuming ? 0 : load_of(a->loadOp);
    pa->store = suspending ? 0 : store_of(a->storeOp);
    pa->clear = a->clearValue;
    if (a->resolveMode != VK_RESOLVE_MODE_NONE && a->resolveImageView) { pa->resolve = a->resolveImageView; pa->resolve_mode = (uint8_t)a->resolveMode; }
}
VKFN void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *info) {
    c_begin_pass *c = record(cb, C_BEGIN_PASS, sizeof *c);
    pass_desc *p = &c->pass;
    int resuming = (info->flags & VK_RENDERING_RESUMING_BIT) != 0, suspending = (info->flags & VK_RENDERING_SUSPENDING_BIT) != 0;
    p->area = info->renderArea;
    p->layers = info->layerCount ? info->layerCount : 1;
    p->view_mask = info->viewMask;
    p->color_count = info->colorAttachmentCount < 8 ? info->colorAttachmentCount : 8;
    for (uint32_t i = 0; i < p->color_count; i++) rendering_attachment(&p->colors[i], &info->pColorAttachments[i], resuming, suspending);
    if (info->pDepthAttachment && info->pDepthAttachment->imageView) {
        rendering_attachment(&p->depth, info->pDepthAttachment, resuming, suspending);
        p->has_depth = 1;
    }
    if (info->pStencilAttachment && info->pStencilAttachment->imageView) {
        const VkRenderingAttachmentInfo *s = info->pStencilAttachment;
        if (!p->depth.view) { rendering_attachment(&p->depth, s, resuming, suspending); p->depth.load = 2; p->depth.store = 1; }
        p->depth.sload = resuming ? 0 : load_of(s->loadOp);
        p->depth.sstore = suspending ? 0 : store_of(s->storeOp);
        p->depth.clear.depthStencil.stencil = s->clearValue.depthStencil.stencil;
        p->has_stencil = 1;
    } else if (p->has_depth) { p->depth.sload = 2; p->depth.sstore = 1; }
}
VKFN void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer cb) { record(cb, C_END_PASS, sizeof(cmd)); }
VKFN void VKAPI_CALL vkCmdEndRendering2KHR(VkCommandBuffer cb, const VkRenderingEndInfoKHR *info) { (void)info; record(cb, C_END_PASS, sizeof(cmd)); }
// Dynamic rendering local read: the identity mapping of fragment outputs to attachments and of input attachment
// indices is what Metal does; the commands are taken as that.
VKFN void VKAPI_CALL vkCmdSetRenderingAttachmentLocations(VkCommandBuffer cb, const VkRenderingAttachmentLocationInfo *info) { (void)cb; (void)info; }
VKFN void VKAPI_CALL vkCmdSetRenderingInputAttachmentIndices(VkCommandBuffer cb, const VkRenderingInputAttachmentIndexInfo *info) { (void)cb; (void)info; }
VKFN void VKAPI_CALL vkCmdSetLineStipple(VkCommandBuffer cb, uint32_t factor, uint16_t pattern) { (void)cb; (void)factor; (void)pattern; }
VKFN void VKAPI_CALL vkCmdPushDescriptorSet2(VkCommandBuffer cb, const VkPushDescriptorSetInfo *info) {
    if (info->stageFlags & VK_SHADER_STAGE_ALL_GRAPHICS)
        push_set(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, info->set, info->descriptorWriteCount, info->pDescriptorWrites, NULL, NULL);
    if (info->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
        push_set(cb, VK_PIPELINE_BIND_POINT_COMPUTE, info->layout, info->set, info->descriptorWriteCount, info->pDescriptorWrites, NULL, NULL);
}
VKFN void VKAPI_CALL vkCmdPushDescriptorSetWithTemplate2(VkCommandBuffer cb, const VkPushDescriptorSetWithTemplateInfo *info) {
    push_set(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, info->set, 0, NULL, info->descriptorUpdateTemplate, info->pData);
}

// Copies, blits, clears.
VKFN void VKAPI_CALL vkCmdCopyBuffer2(VkCommandBuffer cb, const VkCopyBufferInfo2 *info) {
    c_copy_buffer *c = record(cb, C_COPY_BUFFER, sizeof *c + info->regionCount * sizeof(VkBufferCopy2));
    c->src = info->srcBuffer; c->dst = info->dstBuffer; c->count = info->regionCount;
    memcpy(c + 1, info->pRegions, info->regionCount * sizeof(VkBufferCopy2));
}
VKFN void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy *regions) {
    c_copy_buffer *c = record(cb, C_COPY_BUFFER, sizeof *c + n * sizeof(VkBufferCopy2));
    c->src = src; c->dst = dst; c->count = n;
    VkBufferCopy2 *r = (VkBufferCopy2 *)(c + 1);
    for (uint32_t i = 0; i < n; i++) r[i] = (VkBufferCopy2){ VK_STRUCTURE_TYPE_BUFFER_COPY_2, NULL, regions[i].srcOffset, regions[i].dstOffset, regions[i].size };
}
static void copy_buffer_image(VkCommandBuffer cb, VkBuffer buf, VkImage img, int to_image, uint32_t n, const VkBufferImageCopy *r1, const VkBufferImageCopy2 *r2) {
    c_copy_buffer_image *c = record(cb, C_COPY_BUFFER_IMAGE, sizeof *c + n * sizeof(VkBufferImageCopy2));
    c->buffer = buf; c->image = img; c->to_image = to_image; c->count = n;
    VkBufferImageCopy2 *r = (VkBufferImageCopy2 *)(c + 1);
    for (uint32_t i = 0; i < n; i++)
        r[i] = r2 ? r2[i] : (VkBufferImageCopy2){ VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2, NULL, r1[i].bufferOffset, r1[i].bufferRowLength,
            r1[i].bufferImageHeight, r1[i].imageSubresource, r1[i].imageOffset, r1[i].imageExtent };
}
VKFN void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) {
    (void)l; copy_buffer_image(cb, b, i, 1, n, r, NULL);
}
VKFN void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *r) {
    (void)l; copy_buffer_image(cb, b, i, 0, n, r, NULL);
}
VKFN void VKAPI_CALL vkCmdCopyBufferToImage2(VkCommandBuffer cb, const VkCopyBufferToImageInfo2 *info) {
    copy_buffer_image(cb, info->srcBuffer, info->dstImage, 1, info->regionCount, NULL, info->pRegions);
}
VKFN void VKAPI_CALL vkCmdCopyImageToBuffer2(VkCommandBuffer cb, const VkCopyImageToBufferInfo2 *info) {
    copy_buffer_image(cb, info->dstBuffer, info->srcImage, 0, info->regionCount, NULL, info->pRegions);
}
VKFN void VKAPI_CALL vkCmdCopyImage2(VkCommandBuffer cb, const VkCopyImageInfo2 *info) {
    c_copy_image *c = record(cb, C_COPY_IMAGE, sizeof *c + info->regionCount * sizeof(VkImageCopy2));
    c->src = info->srcImage; c->dst = info->dstImage; c->count = info->regionCount;
    memcpy(c + 1, info->pRegions, info->regionCount * sizeof(VkImageCopy2));
}
VKFN void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy *r) {
    (void)sl; (void)dl;
    c_copy_image *c = record(cb, C_COPY_IMAGE, sizeof *c + n * sizeof(VkImageCopy2));
    c->src = src; c->dst = dst; c->count = n;
    VkImageCopy2 *o = (VkImageCopy2 *)(c + 1);
    for (uint32_t i = 0; i < n; i++)
        o[i] = (VkImageCopy2){ VK_STRUCTURE_TYPE_IMAGE_COPY_2, NULL, r[i].srcSubresource, r[i].srcOffset, r[i].dstSubresource, r[i].dstOffset, r[i].extent };
}
VKFN void VKAPI_CALL vkCmdBlitImage2(VkCommandBuffer cb, const VkBlitImageInfo2 *info) {
    c_blit_image *c = record(cb, C_BLIT_IMAGE, sizeof *c + info->regionCount * sizeof(VkImageBlit2));
    c->src = info->srcImage; c->dst = info->dstImage; c->filter = info->filter; c->count = info->regionCount;
    memcpy(c + 1, info->pRegions, info->regionCount * sizeof(VkImageBlit2));
}
VKFN void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
                                    const VkImageBlit *r, VkFilter filter) {
    (void)sl; (void)dl;
    c_blit_image *c = record(cb, C_BLIT_IMAGE, sizeof *c + n * sizeof(VkImageBlit2));
    c->src = src; c->dst = dst; c->filter = filter; c->count = n;
    VkImageBlit2 *o = (VkImageBlit2 *)(c + 1);
    for (uint32_t i = 0; i < n; i++) {
        o[i] = (VkImageBlit2){ VK_STRUCTURE_TYPE_IMAGE_BLIT_2, NULL, r[i].srcSubresource, { r[i].srcOffsets[0], r[i].srcOffsets[1] },
                               r[i].dstSubresource, { r[i].dstOffsets[0], r[i].dstOffsets[1] } };
    }
}
VKFN void VKAPI_CALL vkCmdResolveImage2(VkCommandBuffer cb, const VkResolveImageInfo2 *info) {
    c_resolve_image *c = record(cb, C_RESOLVE_IMAGE, sizeof *c + info->regionCount * sizeof(VkImageResolve2));
    c->src = info->srcImage; c->dst = info->dstImage; c->count = info->regionCount;
    memcpy(c + 1, info->pRegions, info->regionCount * sizeof(VkImageResolve2));
}
VKFN void VKAPI_CALL vkCmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageResolve *r) {
    (void)sl; (void)dl;
    c_resolve_image *c = record(cb, C_RESOLVE_IMAGE, sizeof *c + n * sizeof(VkImageResolve2));
    c->src = src; c->dst = dst; c->count = n;
    VkImageResolve2 *o = (VkImageResolve2 *)(c + 1);
    for (uint32_t i = 0; i < n; i++)
        o[i] = (VkImageResolve2){ VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2, NULL, r[i].srcSubresource, r[i].srcOffset, r[i].dstSubresource, r[i].dstOffset, r[i].extent };
}
static void clear_image(VkCommandBuffer cb, VkImage img, int depth, const VkClearValue *v, uint32_t n, const VkImageSubresourceRange *ranges) {
    c_clear_image *c = record(cb, C_CLEAR_IMAGE, sizeof *c + n * sizeof *ranges);
    c->image = img; c->depth = depth; c->value = *v; c->count = n;
    memcpy(c + 1, ranges, n * sizeof *ranges);
}
VKFN void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearColorValue *color, uint32_t n,
                                          const VkImageSubresourceRange *ranges) {
    (void)l;
    VkClearValue v;
    v.color = *color;
    clear_image(cb, img, 0, &v, n, ranges);
}
VKFN void VKAPI_CALL vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearDepthStencilValue *ds,
                                                 uint32_t n, const VkImageSubresourceRange *ranges) {
    (void)l;
    VkClearValue v;
    v.depthStencil = *ds;
    clear_image(cb, img, 1, &v, n, ranges);
}
VKFN void VKAPI_CALL vkCmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *atts, uint32_t nr, const VkClearRect *rects) {
    c_clear_attachments *c = record(cb, C_CLEAR_ATTACHMENTS, sizeof *c + na * sizeof *atts + nr * sizeof *rects);
    c->attachments = na; c->rects = nr;
    memcpy(c + 1, atts, na * sizeof *atts);
    memcpy((uint8_t *)(c + 1) + na * sizeof *atts, rects, nr * sizeof *rects);
}
VKFN void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize offset, VkDeviceSize size, uint32_t data) {
    c_fill *c = record(cb, C_FILL_BUFFER, sizeof *c);
    c->buffer = b; c->offset = offset;
    c->size = size == VK_WHOLE_SIZE ? (b->size - offset) & ~3ull : size;
    c->data = data;
}
VKFN void VKAPI_CALL vkCmdUpdateBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize offset, VkDeviceSize size, const void *data) {
    c_update *c = record(cb, C_UPDATE_BUFFER, sizeof *c + size);
    c->buffer = b; c->offset = offset; c->size = size;
    memcpy(c + 1, data, size);
}

// Synchronization commands: the encoders run in order (see vk_encode), so barriers have nothing left to do; events
// are set when the command buffer completes.
VKFN void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep,
    uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im) {
    (void)src; (void)dst; (void)dep; (void)nm; (void)m; (void)nb; (void)b; (void)ni; (void)im;
    record(cb, C_BARRIER, sizeof(cmd));
}
VKFN void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *info) { (void)info; record(cb, C_BARRIER, sizeof(cmd)); }
VKFN void VKAPI_CALL vkCmdSetEvent(VkCommandBuffer cb, VkEvent e, VkPipelineStageFlags s) {
    (void)s;
    c_event *c = record(cb, C_SET_EVENT, sizeof *c);
    c->event = e; c->set = 1;
}
VKFN void VKAPI_CALL vkCmdSetEvent2(VkCommandBuffer cb, VkEvent e, const VkDependencyInfo *d) { (void)d; vkCmdSetEvent(cb, e, 0); }
VKFN void VKAPI_CALL vkCmdResetEvent(VkCommandBuffer cb, VkEvent e, VkPipelineStageFlags s) {
    (void)s;
    c_event *c = record(cb, C_SET_EVENT, sizeof *c);
    c->event = e; c->set = 0;
}
VKFN void VKAPI_CALL vkCmdResetEvent2(VkCommandBuffer cb, VkEvent e, VkPipelineStageFlags2 s) { (void)s; vkCmdResetEvent(cb, e, 0); }
VKFN void VKAPI_CALL vkCmdWaitEvents(VkCommandBuffer cb, uint32_t n, const VkEvent *e, VkPipelineStageFlags s, VkPipelineStageFlags d,
    uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im) {
    (void)n; (void)e; (void)s; (void)d; (void)nm; (void)m; (void)nb; (void)b; (void)ni; (void)im;
    record(cb, C_BARRIER, sizeof(cmd));
}
VKFN void VKAPI_CALL vkCmdWaitEvents2(VkCommandBuffer cb, uint32_t n, const VkEvent *e, const VkDependencyInfo *d) {
    (void)n; (void)e; (void)d;
    record(cb, C_BARRIER, sizeof(cmd));
}

// Queries.
VKFN void VKAPI_CALL vkCmdBeginQuery(VkCommandBuffer cb, VkQueryPool pool, uint32_t query, VkQueryControlFlags flags) {
    c_query *c = record(cb, C_BEGIN_QUERY, sizeof *c);
    c->pool = pool; c->query = query; c->flags = flags;
}
VKFN void VKAPI_CALL vkCmdEndQuery(VkCommandBuffer cb, VkQueryPool pool, uint32_t query) {
    c_query *c = record(cb, C_END_QUERY, sizeof *c);
    c->pool = pool; c->query = query;
}
VKFN void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t count) {
    c_query *c = record(cb, C_RESET_QUERIES, sizeof *c);
    c->pool = pool; c->query = first; c->count = count;
}
VKFN void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t query) {
    (void)stage;
    c_query *c = record(cb, C_TIMESTAMP, sizeof *c);
    c->pool = pool; c->query = query;
}
VKFN void VKAPI_CALL vkCmdWriteTimestamp2(VkCommandBuffer cb, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t query) {
    vkCmdWriteTimestamp(cb, (VkPipelineStageFlagBits)(uint32_t)stage, pool, query);
}
VKFN void VKAPI_CALL vkCmdCopyQueryPoolResults(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t count, VkBuffer dst,
                                               VkDeviceSize offset, VkDeviceSize stride, VkQueryResultFlags flags) {
    c_copy_queries *c = record(cb, C_COPY_QUERIES, sizeof *c);
    c->pool = pool; c->first = first; c->count = count; c->dst = dst; c->offset = offset; c->stride = stride; c->flags = flags;
}

VKFN void VKAPI_CALL vkCmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *cbs) {
    c_execute *c = record(cb, C_EXECUTE, sizeof *c + n * sizeof *cbs);
    c->count = n;
    memcpy(c + 1, cbs, n * sizeof *cbs);
}

VKFN void VKAPI_CALL vkCmdBeginDebugUtilsLabelEXT(VkCommandBuffer cb, const VkDebugUtilsLabelEXT *l) { (void)cb; (void)l; }
VKFN void VKAPI_CALL vkCmdEndDebugUtilsLabelEXT(VkCommandBuffer cb) { (void)cb; }
VKFN void VKAPI_CALL vkCmdInsertDebugUtilsLabelEXT(VkCommandBuffer cb, const VkDebugUtilsLabelEXT *l) { (void)cb; (void)l; }
VKFN void VKAPI_CALL vkCmdSetDeviceMask(VkCommandBuffer cb, uint32_t mask) { (void)cb; (void)mask; }
VKFN void VKAPI_CALL vkCmdDebugMarkerBeginEXT(VkCommandBuffer cb, const VkDebugMarkerMarkerInfoEXT *m) { (void)cb; (void)m; }
VKFN void VKAPI_CALL vkCmdDebugMarkerEndEXT(VkCommandBuffer cb) { (void)cb; }
VKFN void VKAPI_CALL vkCmdDebugMarkerInsertEXT(VkCommandBuffer cb, const VkDebugMarkerMarkerInfoEXT *m) { (void)cb; (void)m; }
// Buffer markers (crash breadcrumbs): the value is written in order with the other commands.
VKFN void VKAPI_CALL vkCmdWriteBufferMarkerAMD(VkCommandBuffer cb, VkPipelineStageFlagBits stage, VkBuffer b, VkDeviceSize offset, uint32_t marker) {
    (void)stage;
    vkCmdUpdateBuffer(cb, b, offset, 4, &marker);
}
VKFN void VKAPI_CALL vkCmdWriteBufferMarker2AMD(VkCommandBuffer cb, VkPipelineStageFlags2 stage, VkBuffer b, VkDeviceSize offset, uint32_t marker) {
    (void)stage;
    vkCmdUpdateBuffer(cb, b, offset, 4, &marker);
}

// ---- encoding ----------------------------------------------------------------------------------------------------
void vk_end_encoder(vk_enc *e) {
    if (e->render) { [e->render updateFence:e->fence afterStages:MTLRenderStageFragment]; [e->render endEncoding]; e->render = nil; }
    if (e->compute) { [e->compute updateFence:e->fence]; [e->compute endEncoding]; e->compute = nil; }
    if (e->blit) { [e->blit updateFence:e->fence]; [e->blit endEncoding]; e->blit = nil; }
}

static void use_heaps_render(vk_enc *e) {
    vk_heap_list *l = &e->dev->heaps;
    pthread_mutex_lock(&l->lock);
    if (l->count) [e->render useHeaps:l->heaps count:l->count stages:MTLRenderStageVertex | MTLRenderStageFragment];
    pthread_mutex_unlock(&l->lock);
}
static void use_heaps_compute(vk_enc *e) {
    vk_heap_list *l = &e->dev->heaps;
    pthread_mutex_lock(&l->lock);
    if (l->count) [e->compute useHeaps:l->heaps count:l->count];
    pthread_mutex_unlock(&l->lock);
}

static id<MTLComputeCommandEncoder> compute_encoder(vk_enc *e) {
    if (e->compute) return e->compute;
    vk_end_encoder(e);
    e->compute = [e->cb computeCommandEncoder];
    [e->compute waitForFence:e->fence];
    use_heaps_compute(e);
    return e->compute;
}
static id<MTLBlitCommandEncoder> blit_encoder(vk_enc *e) {
    if (e->blit) return e->blit;
    vk_end_encoder(e);
    e->blit = [e->cb blitCommandEncoder];
    [e->blit waitForFence:e->fence];
    return e->blit;
}

static MTLLoadAction mtl_load(uint8_t l) { return l == 1 ? MTLLoadActionClear : l == 0 ? MTLLoadActionLoad : MTLLoadActionDontCare; }

static void begin_render(vk_enc *e, const pass_desc *p) {
    vk_end_encoder(e);
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    uint32_t w = 0, h = 0, samples = 1;
    for (uint32_t i = 0; i < p->color_count; i++) {
        const pass_attachment *a = &p->colors[i];
        if (!a->view || !a->view->mtl) continue;
        MTLRenderPassColorAttachmentDescriptor *c = d.colorAttachments[i];
        c.texture = a->view->mtl;
        c.loadAction = mtl_load(a->load);
        c.clearColor = MTLClearColorMake(a->clear.color.float32[0], a->clear.color.float32[1], a->clear.color.float32[2], a->clear.color.float32[3]);
        const vk_format_info *f = vk_format(a->view->format);
        if (f && f->numeric != VKF_FLOAT) {
            // (integer formats clear to the integer values, which Metal takes as doubles)
            if (f->numeric == VKF_UINT) c.clearColor = MTLClearColorMake(a->clear.color.uint32[0], a->clear.color.uint32[1], a->clear.color.uint32[2], a->clear.color.uint32[3]);
            else c.clearColor = MTLClearColorMake(a->clear.color.int32[0], a->clear.color.int32[1], a->clear.color.int32[2], a->clear.color.int32[3]);
        }
        if (a->resolve && a->resolve->mtl) {
            c.resolveTexture = a->resolve->mtl;
            c.storeAction = a->store == 0 ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionMultisampleResolve;
        } else c.storeAction = a->store == 0 ? MTLStoreActionStore : MTLStoreActionDontCare;
        if (!w) { w = a->view->width; h = a->view->height; samples = (uint32_t)a->view->mtl.sampleCount; }
    }
    if (p->depth.view && p->depth.view->mtl) {
        const pass_attachment *a = &p->depth;
        id<MTLTexture> t = a->view->image->mtl ? a->view->image->mtl : a->view->mtl;
        // (the depth texture's own view at the right level and layer)
        t = a->view->mtl;
        if (p->has_depth) {
            d.depthAttachment.texture = t;
            d.depthAttachment.loadAction = mtl_load(a->load);
            d.depthAttachment.clearDepth = a->clear.depthStencil.depth;
            d.depthAttachment.storeAction = a->store == 0 ? MTLStoreActionStore : MTLStoreActionDontCare;
            if (a->resolve && a->resolve->mtl) {
                d.depthAttachment.resolveTexture = a->resolve->mtl;
                d.depthAttachment.storeAction = a->store == 0 ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionMultisampleResolve;
                d.depthAttachment.depthResolveFilter = a->resolve_mode == VK_RESOLVE_MODE_MIN_BIT ? MTLMultisampleDepthResolveFilterMin :
                    a->resolve_mode == VK_RESOLVE_MODE_MAX_BIT ? MTLMultisampleDepthResolveFilterMax : MTLMultisampleDepthResolveFilterSample0;
            }
        }
        if (p->has_stencil) {
            d.stencilAttachment.texture = t;
            d.stencilAttachment.loadAction = mtl_load(a->sload);
            d.stencilAttachment.clearStencil = a->clear.depthStencil.stencil;
            d.stencilAttachment.storeAction = a->sstore == 0 ? MTLStoreActionStore : MTLStoreActionDontCare;
        }
        if (!w) { w = a->view->width; h = a->view->height; samples = (uint32_t)t.sampleCount; }
    }
    if (!w) { w = p->area.offset.x + p->area.extent.width; h = p->area.offset.y + p->area.extent.height; }
    d.renderTargetWidth = w;
    d.renderTargetHeight = h;
    if (p->layers > 1) d.renderTargetArrayLength = p->layers;
    if (p->occlusion) d.visibilityResultBuffer = p->occlusion->results;
    e->render = [e->cb renderCommandEncoderWithDescriptor:d];
    [e->render waitForFence:e->fence beforeStages:MTLRenderStageVertex];
    use_heaps_render(e);
    e->pass = *p;
    e->pass_width = w;
    e->pass_height = h;
    e->pass_samples = samples;
    e->in_pass = 1;
    e->state_dirty = 1;
}

static void bind_sets_render(vk_enc *e, const struct VkPipeline_T *pl) {
    id<MTLRenderCommandEncoder> r = e->render;
    bindings_state *b = &e->gb;
    uint32_t aux[256];
    uint32_t naux = 0;
    for (uint32_t s = 0; s < VKMSL_MAX_SETS; s++) {
        uint32_t mask = 1u << s;
        if (!((pl->sets_vertex | pl->sets_fragment) & mask)) continue;
        struct VkDescriptorSet_T *ds = b->push[s] ? b->push[s] : b->sets[s];
        if (!ds) continue;
        if (b->push[s]) {
            if (pl->sets_vertex & mask) [r setVertexBytes:ds->words length:ds->size atIndex:s];
            if (pl->sets_fragment & mask) [r setFragmentBytes:ds->words length:ds->size atIndex:s];
        } else {
            if (pl->sets_vertex & mask) [r setVertexBuffer:ds->pool->buffer offset:ds->offset atIndex:s];
            if (pl->sets_fragment & mask) [r setFragmentBuffer:ds->pool->buffer offset:ds->offset atIndex:s];
        }
    }
    if (pl->aux_vertex || pl->aux_fragment) {
        for (uint32_t s = 0; s < pl->layout.set_count; s++)
            for (uint32_t k = 0; k < b->dyn_count[s] && pl->layout.dynamic_base[s] + k < 256; k++) {
                aux[pl->layout.dynamic_base[s] + k] = b->dyn[s][k];
                if (pl->layout.dynamic_base[s] + k + 1 > naux) naux = pl->layout.dynamic_base[s] + k + 1;
            }
        if (!naux) { aux[0] = 0; naux = 1; }
        if (pl->aux_vertex) [r setVertexBytes:aux length:naux * 4 atIndex:VKMSL_AUX_BUFFER];
        if (pl->aux_fragment) [r setFragmentBytes:aux length:naux * 4 atIndex:VKMSL_AUX_BUFFER];
    }
    uint32_t push_size = pl->layout.push_size ? (pl->layout.push_size + 15) & ~15u : 16;
    if (push_size > sizeof e->push) push_size = sizeof e->push;
    if (pl->push_vertex) [r setVertexBytes:e->push length:push_size atIndex:VKMSL_PUSH_BUFFER];
    if (pl->push_fragment) [r setFragmentBytes:e->push length:push_size atIndex:VKMSL_PUSH_BUFFER];
}

static void bind_sets_compute(vk_enc *e, const struct VkPipeline_T *pl) {
    id<MTLComputeCommandEncoder> c = e->compute;
    bindings_state *b = &e->cb_;
    for (uint32_t s = 0; s < VKMSL_MAX_SETS; s++) {
        if (!(pl->sets_compute & (1u << s))) continue;
        struct VkDescriptorSet_T *ds = b->push[s] ? b->push[s] : b->sets[s];
        if (!ds) continue;
        if (b->push[s]) [c setBytes:ds->words length:ds->size atIndex:s];
        else [c setBuffer:ds->pool->buffer offset:ds->offset atIndex:s];
    }
    if (pl->aux_compute) {
        uint32_t aux[256], naux = 1;
        aux[0] = 0;
        for (uint32_t s = 0; s < pl->layout.set_count; s++)
            for (uint32_t k = 0; k < b->dyn_count[s] && pl->layout.dynamic_base[s] + k < 256; k++) {
                aux[pl->layout.dynamic_base[s] + k] = b->dyn[s][k];
                if (pl->layout.dynamic_base[s] + k + 1 > naux) naux = pl->layout.dynamic_base[s] + k + 1;
            }
        [c setBytes:aux length:naux * 4 atIndex:VKMSL_AUX_BUFFER];
    }
    uint32_t push_size = pl->layout.push_size ? (pl->layout.push_size + 15) & ~15u : 16;
    if (push_size > sizeof e->push) push_size = sizeof e->push;
    if (pl->push_compute) [c setBytes:e->push length:push_size atIndex:VKMSL_PUSH_BUFFER];
}

static MTLViewport mtl_viewport(VkViewport v) {
    // (a negative height, maintenance1's flip, is the same viewport from its other edge)
    if (v.height < 0) { v.y += v.height; v.height = -v.height; }
    return (MTLViewport){ v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth };
}

static void apply_render_state(vk_enc *e) {
    struct VkPipeline_T *p = e->gp;
    id<MTLRenderCommandEncoder> r = e->render;
    if (!p || !r) return;
    [r setRenderPipelineState:p->render];
    // Viewports and scissors (Metal needs scissors inside the render target).
    uint32_t nv = e->viewport_count ? e->viewport_count : 1;
    MTLViewport vps[16];
    for (uint32_t i = 0; i < nv && i < 16; i++) vps[i] = mtl_viewport(e->viewports[i]);
    if (nv == 1) [r setViewport:vps[0]]; else [r setViewports:vps count:nv];
    uint32_t ns = e->scissor_count ? e->scissor_count : 1;
    MTLScissorRect sc[16];
    for (uint32_t i = 0; i < ns && i < 16; i++) {
        VkRect2D s = e->scissors[i];
        int64_t x = s.offset.x < 0 ? 0 : s.offset.x, y = s.offset.y < 0 ? 0 : s.offset.y;
        int64_t x2 = (int64_t)s.offset.x + s.extent.width, y2 = (int64_t)s.offset.y + s.extent.height;
        if (x2 > e->pass_width) x2 = e->pass_width;
        if (y2 > e->pass_height) y2 = e->pass_height;
        if (x > x2) x = x2;
        if (y > y2) y = y2;
        sc[i] = (MTLScissorRect){ (NSUInteger)x, (NSUInteger)y, (NSUInteger)(x2 - x), (NSUInteger)(y2 - y) };
    }
    if (ns == 1) [r setScissorRect:sc[0]]; else [r setScissorRects:sc count:ns];
    MTLCullMode cull = e->cull == VK_CULL_MODE_FRONT_BIT ? MTLCullModeFront : e->cull == VK_CULL_MODE_BACK_BIT ? MTLCullModeBack : MTLCullModeNone;
    [r setCullMode:cull];
    [r setFrontFacingWinding:e->front == VK_FRONT_FACE_COUNTER_CLOCKWISE ? MTLWindingCounterClockwise : MTLWindingClockwise];
    [r setTriangleFillMode:p->fill];
    [r setDepthClipMode:p->depth_clip];
    if (e->depth_bias_enable) [r setDepthBias:e->depth_bias[0] slopeScale:e->depth_bias[1] clamp:e->depth_bias[2]];
    else [r setDepthBias:0 slopeScale:0 clamp:0];
    [r setBlendColorRed:e->blend[0] green:e->blend[1] blue:e->blend[2] alpha:e->blend[3]];
    [r setStencilFrontReferenceValue:e->stencil_ref[0] backReferenceValue:e->stencil_ref[1]];
    // Depth/stencil state: the pipeline's, or one made for the dynamic states.
    uint32_t dsd = p->dynamic & (VK_DYN_DEPTH_TEST | VK_DYN_DEPTH_WRITE | VK_DYN_DEPTH_COMPARE | VK_DYN_STENCIL_TEST | VK_DYN_STENCIL_OP |
                                 VK_DYN_STENCIL_COMPARE | VK_DYN_STENCIL_WRITE);
    if (!dsd) [r setDepthStencilState:p->depth_stencil];
    else {
        VkPipelineDepthStencilStateCreateInfo ds = p->ds;
        if (p->dynamic & VK_DYN_DEPTH_TEST) ds.depthTestEnable = e->dyn_depth_test;
        if (p->dynamic & VK_DYN_DEPTH_WRITE) ds.depthWriteEnable = e->dyn_depth_write;
        if (p->dynamic & VK_DYN_DEPTH_COMPARE) ds.depthCompareOp = e->dyn_depth_compare;
        if (p->dynamic & VK_DYN_STENCIL_TEST) ds.stencilTestEnable = e->dyn_stencil_test;
        if (p->dynamic & VK_DYN_STENCIL_COMPARE) { ds.front.compareMask = e->stencil_compare[0]; ds.back.compareMask = e->stencil_compare[1]; }
        if (p->dynamic & VK_DYN_STENCIL_WRITE) { ds.front.writeMask = e->stencil_write[0]; ds.back.writeMask = e->stencil_write[1]; }
        if (p->dynamic & VK_DYN_STENCIL_OP) {
            ds.front.failOp = e->dyn_stencil_ops[0][0]; ds.front.passOp = e->dyn_stencil_ops[0][1]; ds.front.depthFailOp = e->dyn_stencil_ops[0][2];
            ds.back.failOp = e->dyn_stencil_ops[1][0]; ds.back.passOp = e->dyn_stencil_ops[1][1]; ds.back.depthFailOp = e->dyn_stencil_ops[1][2];
            ds.front.compareOp = e->dyn_stencil_compare_op[0]; ds.back.compareOp = e->dyn_stencil_compare_op[1];
        }
        [r setDepthStencilState:vk_depth_stencil_state(e->dev, &ds, e->pass.has_depth, e->pass.has_stencil)];
    }
    e->state_dirty = 0;
}

// Pipeline values for the states the pipeline fixes, as it is bound.
static void take_pipeline_state(vk_enc *e, struct VkPipeline_T *p) {
    if (!(p->dynamic & VK_DYN_VIEWPORT)) { e->viewport_count = p->viewport_count; memcpy(e->viewports, p->viewports, sizeof e->viewports); }
    if (!(p->dynamic & VK_DYN_SCISSOR)) { e->scissor_count = p->scissor_count; memcpy(e->scissors, p->scissors, sizeof e->scissors); }
    if (!(p->dynamic & VK_DYN_DEPTH_BIAS)) memcpy(e->depth_bias, p->depth_bias, sizeof e->depth_bias);
    if (!(p->dynamic & VK_DYN_DEPTH_BIAS_ENABLE)) e->depth_bias_enable = p->depth_bias_enable;
    if (!(p->dynamic & VK_DYN_BLEND_CONSTANTS)) memcpy(e->blend, p->blend_constants, sizeof e->blend);
    if (!(p->dynamic & VK_DYN_STENCIL_REFERENCE)) { e->stencil_ref[0] = p->stencil_ref[0]; e->stencil_ref[1] = p->stencil_ref[1]; }
    if (!(p->dynamic & VK_DYN_CULL_MODE)) e->cull = p->cull == MTLCullModeFront ? VK_CULL_MODE_FRONT_BIT : p->cull == MTLCullModeBack ? VK_CULL_MODE_BACK_BIT : 0;
    if (!(p->dynamic & VK_DYN_FRONT_FACE)) e->front = p->winding == MTLWindingCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
}

static int prepare_draw(vk_enc *e) {
    struct VkPipeline_T *p = e->gp;
    if (!e->render || !p || !p->render) return 0;
    if (e->state_dirty) apply_render_state(e);
    bind_sets_render(e, p);
    for (uint32_t b = 0; b < VKMSL_MAX_VERTEX_BUFFERS; b++) {
        if (!(p->vertex_bindings & (1u << b))) continue;
        VkBuffer vb = e->vb[b];
        id<MTLBuffer> mb = vb && vb->mtl ? vb->mtl : e->dev->zero_buffer;
        uint64_t off = vb && vb->mtl ? vb->offset + e->vb_offset[b] : 0;
        if (p->dynamic & VK_DYN_STRIDE) {
            uint64_t stride = e->vb_stride[b] ? e->vb_stride[b] : p->vertex_strides[b];
            [e->render setVertexBuffer:mb offset:off attributeStride:stride atIndex:VKMSL_VERTEX_BUFFER + b];
        } else [e->render setVertexBuffer:mb offset:off atIndex:VKMSL_VERTEX_BUFFER + b];
    }
    return !p->rasterizer_discard || 1;
}

static MTLPrimitiveType current_primitive(vk_enc *e) {
    struct VkPipeline_T *p = e->gp;
    if (!(p->dynamic & VK_DYN_TOPOLOGY)) return p->primitive;
    switch (e->topology) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return MTLPrimitiveTypePoint;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: return MTLPrimitiveTypeLine;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return MTLPrimitiveTypeLineStrip;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return MTLPrimitiveTypeTriangleStrip;
    default: return MTLPrimitiveTypeTriangle;
    }
}

static void bind_sets(vk_enc *e, const c_bind_sets *c) {
    bindings_state *b = c->point == VK_PIPELINE_BIND_POINT_COMPUTE ? &e->cb_ : &e->gb;
    const VkDescriptorSet *sets = (const VkDescriptorSet *)(c + 1);
    const uint32_t *dyn = (const uint32_t *)(sets + c->count);
    uint32_t d = 0;
    for (uint32_t i = 0; i < c->count && c->first + i < VKMSL_MAX_SETS; i++) {
        uint32_t s = c->first + i;
        b->sets[s] = sets[i];
        b->push[s] = NULL;
        uint32_t n = sets[i] ? sets[i]->layout->dynamic_count : 0;
        b->dyn_count[s] = n < 64 ? n : 64;
        for (uint32_t k = 0; k < n && d < c->dyn_count; k++, d++) if (k < 64) b->dyn[s][k] = dyn[d];
    }
}


static void copy_buffer_image_encode(vk_enc *e, const c_copy_buffer_image *c) {
    id<MTLBlitCommandEncoder> bl = blit_encoder(e);
    const VkBufferImageCopy2 *rs = (const VkBufferImageCopy2 *)(c + 1);
    VkImage img = c->image;
    const vk_format_info *f = img->fmt;
    if (!img->mtl || !c->buffer->mtl) return;
    for (uint32_t i = 0; i < c->count; i++) {
        const VkBufferImageCopy2 *r = &rs[i];
        uint32_t row_texels = r->bufferRowLength ? r->bufferRowLength : r->imageExtent.width;
        uint32_t rows = r->bufferImageHeight ? r->bufferImageHeight : r->imageExtent.height;
        uint32_t bw = f->block_w, bh = f->block_h;
        uint32_t bytes = f->bytes;
        MTLBlitOption opt = MTLBlitOptionNone;
        if ((f->aspects & VKF_DEPTH) && (f->aspects & VKF_STENCIL)) {
            if (r->imageSubresource.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) { opt = MTLBlitOptionStencilFromDepthStencil; bytes = 1; }
            else { opt = MTLBlitOptionDepthFromDepthStencil; bytes = 4; }
        }
        uint64_t bpr = (uint64_t)((row_texels + bw - 1) / bw) * bytes;
        uint64_t bpi = bpr * ((rows + bh - 1) / bh);
        uint32_t layers = r->imageSubresource.layerCount == VK_REMAINING_ARRAY_LAYERS ? img->layers - r->imageSubresource.baseArrayLayer : r->imageSubresource.layerCount;
        MTLSize size = MTLSizeMake(r->imageExtent.width, r->imageExtent.height, r->imageExtent.depth);
        MTLOrigin origin = MTLOriginMake(r->imageOffset.x, r->imageOffset.y, r->imageOffset.z);
        // (Metal wants whole blocks inside the level for compressed formats: the extent clipped to the level)
        uint32_t lw = img->extent.width >> r->imageSubresource.mipLevel, lh = img->extent.height >> r->imageSubresource.mipLevel;
        if (!lw) lw = 1;
        if (!lh) lh = 1;
        if (origin.x + size.width > lw) size.width = lw - origin.x;
        if (origin.y + size.height > lh) size.height = lh - origin.y;
        if (img->type == VK_IMAGE_TYPE_1D) bpi = 0;
        for (uint32_t l = 0; l < layers; l++) {
            uint64_t off = c->buffer->offset + r->bufferOffset + l * bpi * (img->type == VK_IMAGE_TYPE_3D ? r->imageExtent.depth : 1);
            if (c->to_image)
                [bl copyFromBuffer:c->buffer->mtl sourceOffset:off sourceBytesPerRow:bpr sourceBytesPerImage:img->type == VK_IMAGE_TYPE_3D ? bpi : 0
                        sourceSize:size toTexture:img->mtl destinationSlice:r->imageSubresource.baseArrayLayer + l
                  destinationLevel:r->imageSubresource.mipLevel destinationOrigin:origin options:opt];
            else
                [bl copyFromTexture:img->mtl sourceSlice:r->imageSubresource.baseArrayLayer + l sourceLevel:r->imageSubresource.mipLevel
                       sourceOrigin:origin sourceSize:size toBuffer:c->buffer->mtl destinationOffset:off destinationBytesPerRow:bpr
              destinationBytesPerImage:img->type == VK_IMAGE_TYPE_3D ? bpi : 0 options:opt];
        }
    }
}

static void copy_image_encode(vk_enc *e, const c_copy_image *c) {
    id<MTLBlitCommandEncoder> bl = blit_encoder(e);
    if (!c->src->mtl || !c->dst->mtl) return;
    id<MTLTexture> src = c->src->mtl;
    if (src.pixelFormat != c->dst->mtl.pixelFormat) {
        // (size-compatible formats: a view of the source in the destination's format)
        src = [src newTextureViewWithPixelFormat:c->dst->mtl.pixelFormat];
        if (!src) { TRACE("image copy between incompatible formats %d and %d", c->src->format, c->dst->format); return; }
    }
    const VkImageCopy2 *rs = (const VkImageCopy2 *)(c + 1);
    for (uint32_t i = 0; i < c->count; i++) {
        const VkImageCopy2 *r = &rs[i];
        uint32_t layers = r->srcSubresource.layerCount;
        if (c->src->type == VK_IMAGE_TYPE_3D || c->dst->type == VK_IMAGE_TYPE_3D) layers = 1;
        for (uint32_t l = 0; l < layers; l++)
            [bl copyFromTexture:src sourceSlice:r->srcSubresource.baseArrayLayer + l sourceLevel:r->srcSubresource.mipLevel
                   sourceOrigin:MTLOriginMake(r->srcOffset.x, r->srcOffset.y, r->srcOffset.z)
                     sourceSize:MTLSizeMake(r->extent.width, r->extent.height, r->extent.depth)
                      toTexture:c->dst->mtl destinationSlice:r->dstSubresource.baseArrayLayer + l destinationLevel:r->dstSubresource.mipLevel
              destinationOrigin:MTLOriginMake(r->dstOffset.x, r->dstOffset.y, r->dstOffset.z)];
    }
}

static void resolve_encode(vk_enc *e, const c_resolve_image *c) {
    // A render pass that loads the multisampled image and resolves it (whole levels; regions are those).
    const VkImageResolve2 *rs = (const VkImageResolve2 *)(c + 1);
    for (uint32_t i = 0; i < c->count; i++) {
        vk_end_encoder(e);
        MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
        id<MTLTexture> src = [c->src->mtl newTextureViewWithPixelFormat:c->src->mtl.pixelFormat textureType:MTLTextureType2DMultisample
                              levels:NSMakeRange(rs[i].srcSubresource.mipLevel, 1) slices:NSMakeRange(rs[i].srcSubresource.baseArrayLayer, 1)];
        id<MTLTexture> dst = [c->dst->mtl newTextureViewWithPixelFormat:c->dst->mtl.pixelFormat textureType:MTLTextureType2D
                              levels:NSMakeRange(rs[i].dstSubresource.mipLevel, 1) slices:NSMakeRange(rs[i].dstSubresource.baseArrayLayer, 1)];
        if (!src || !dst) continue;
        int depth = (c->src->fmt->aspects & VKF_DEPTH) != 0;
        if (depth) {
            d.depthAttachment.texture = src; d.depthAttachment.loadAction = MTLLoadActionLoad;
            d.depthAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve; d.depthAttachment.resolveTexture = dst;
        } else {
            d.colorAttachments[0].texture = src; d.colorAttachments[0].loadAction = MTLLoadActionLoad;
            d.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve; d.colorAttachments[0].resolveTexture = dst;
        }
        id<MTLRenderCommandEncoder> r = [e->cb renderCommandEncoderWithDescriptor:d];
        [r waitForFence:e->fence beforeStages:MTLRenderStageVertex];
        [r updateFence:e->fence afterStages:MTLRenderStageFragment];
        [r endEncoding];
    }
}

static void timestamp_later(vk_enc *e, VkQueryPool pool, uint32_t query) {
    if (e->timestamp_count == e->timestamp_cap) {
        e->timestamp_cap = e->timestamp_cap ? e->timestamp_cap * 2 : 32;
        e->timestamps = realloc(e->timestamps, e->timestamp_cap * sizeof *e->timestamps);
    }
    e->timestamps[e->timestamp_count].pool = pool;
    e->timestamps[e->timestamp_count++].query = query;
}

static void encode_list(vk_enc *e, VkCommandBuffer cb);

// Looks ahead in a pass for the occlusion query pool its queries use (Metal sets it with the pass).
static VkQueryPool pass_occlusion_pool(const uint8_t *from, const vk_chunk *chunk) {
    for (const vk_chunk *c = chunk; c; c = c->next) {
        const uint8_t *p = c == chunk ? from : c->data;
        while (p < c->data + c->used) {
            const cmd *h = (const cmd *)p;
            if (h->op == C_END_PASS) return NULL;
            if (h->op == C_BEGIN_QUERY && ((const c_query *)h)->pool->type == VK_QUERY_TYPE_OCCLUSION) return ((const c_query *)h)->pool;
            p += h->size;
        }
    }
    return NULL;
}

static void encode_list(vk_enc *e, VkCommandBuffer cb) {
    for (vk_chunk *chunk = cb->first; chunk; chunk = chunk->next) {
        for (size_t pos = 0; pos < chunk->used;) {
            cmd *h = (cmd *)(chunk->data + pos);
            pos += h->size;
            switch (h->op) {
            case C_BIND_PIPELINE: {
                c_bind_pipeline *c = (c_bind_pipeline *)h;
                if (c->point == VK_PIPELINE_BIND_POINT_COMPUTE) e->cp = c->pipeline;
                else { e->gp = c->pipeline; if (e->gp) take_pipeline_state(e, e->gp); e->state_dirty = 1; }
                break;
            }
            case C_BIND_SETS: bind_sets(e, (c_bind_sets *)h); break;
            case C_PUSH_SET: {
                c_push_set *c = (c_push_set *)h;
                bindings_state *b = c->point == VK_PIPELINE_BIND_POINT_COMPUTE ? &e->cb_ : &e->gb;
                if (c->set < VKMSL_MAX_SETS) { b->push[c->set] = c->data; b->sets[c->set] = NULL; }
                break;
            }
            case C_PUSH_CONSTANTS: {
                c_push *c = (c_push *)h;
                if (c->offset + c->size <= sizeof e->push) memcpy(e->push + c->offset, c + 1, c->size);
                break;
            }
            case C_BIND_VERTEX: {
                c_bind_vertex *c = (c_bind_vertex *)h;
                VkBuffer *b = (VkBuffer *)(c + 1);
                uint64_t *o = (uint64_t *)(b + c->count), *s = o + c->count;
                for (uint32_t i = 0; i < c->count && c->first + i < VKMSL_MAX_VERTEX_BUFFERS; i++) {
                    e->vb[c->first + i] = b[i];
                    e->vb_offset[c->first + i] = o[i];
                    if (c->has_strides) e->vb_stride[c->first + i] = s[i];
                }
                break;
            }
            case C_BIND_INDEX: {
                c_bind_index *c = (c_bind_index *)h;
                e->ib = c->buffer; e->ib_offset = c->offset; e->ib_type = c->type;
                break;
            }
            case C_VIEWPORT: case C_SCISSOR: {
                c_viewport *c = (c_viewport *)h;
                if (h->op == C_VIEWPORT) {
                    for (uint32_t i = 0; i < c->count && c->first + i < 16; i++) e->viewports[c->first + i] = ((VkViewport *)(c + 1))[i];
                    if (c->with_count || c->first + c->count > e->viewport_count) e->viewport_count = c->first + c->count;
                } else {
                    for (uint32_t i = 0; i < c->count && c->first + i < 16; i++) e->scissors[c->first + i] = ((VkRect2D *)(c + 1))[i];
                    if (c->with_count || c->first + c->count > e->scissor_count) e->scissor_count = c->first + c->count;
                }
                e->state_dirty = 1;
                break;
            }
            case C_STATE: {
                c_state *c = (c_state *)h;
                switch (c->which) {
                case S_DEPTH_BIAS: memcpy(e->depth_bias, c->f, sizeof e->depth_bias); break;
                case S_BLEND_CONSTANTS: memcpy(e->blend, c->f, sizeof e->blend); break;
                case S_STENCIL_COMPARE: if (c->u[0] & 1) e->stencil_compare[0] = c->u[1]; if (c->u[0] & 2) e->stencil_compare[1] = c->u[1]; break;
                case S_STENCIL_WRITE: if (c->u[0] & 1) e->stencil_write[0] = c->u[1]; if (c->u[0] & 2) e->stencil_write[1] = c->u[1]; break;
                case S_STENCIL_REF: if (c->u[0] & 1) e->stencil_ref[0] = c->u[1]; if (c->u[0] & 2) e->stencil_ref[1] = c->u[1]; break;
                case S_CULL: e->cull = c->u[0]; break;
                case S_FRONT_FACE: e->front = c->u[0]; break;
                case S_TOPOLOGY: e->topology = c->u[0]; break;
                case S_DEPTH_TEST: e->dyn_depth_test = c->u[0]; break;
                case S_DEPTH_WRITE: e->dyn_depth_write = c->u[0]; break;
                case S_DEPTH_COMPARE: e->dyn_depth_compare = c->u[0]; break;
                case S_STENCIL_TEST: e->dyn_stencil_test = c->u[0]; break;
                case S_STENCIL_OP:
                    for (int f = 0; f < 2; f++) if (c->u[0] & (1u << f)) {
                        e->dyn_stencil_ops[f][0] = c->u[1]; e->dyn_stencil_ops[f][1] = c->u[2]; e->dyn_stencil_ops[f][2] = c->u[3];
                        e->dyn_stencil_compare_op[f] = (VkCompareOp)c->f[0];
                    }
                    break;
                case S_DEPTH_BIAS_ENABLE: e->depth_bias_enable = c->u[0]; break;
                default: break;
                }
                e->state_dirty = 1;
                break;
            }
            case C_DRAW: case C_DRAW_INDEXED: {
                c_draw *c = (c_draw *)h;
                if (!c->count || !c->instances || !prepare_draw(e)) break;
                MTLPrimitiveType pt = current_primitive(e);
                if (h->op == C_DRAW)
                    [e->render drawPrimitives:pt vertexStart:c->first vertexCount:c->count instanceCount:c->instances baseInstance:c->first_instance];
                else {
                    if (!e->ib || !e->ib->mtl) break;
                    int i16 = e->ib_type == VK_INDEX_TYPE_UINT16;
                    [e->render drawIndexedPrimitives:pt indexCount:c->count indexType:i16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                                         indexBuffer:e->ib->mtl indexBufferOffset:e->ib->offset + e->ib_offset + (uint64_t)c->first * (i16 ? 2 : 4)
                                       instanceCount:c->instances baseVertex:c->vertex_offset baseInstance:c->first_instance];
                }
                break;
            }
            case C_DRAW_INDIRECT: {
                c_draw_indirect *c = (c_draw_indirect *)h;
                if (!prepare_draw(e) || !c->buffer->mtl) break;
                MTLPrimitiveType pt = current_primitive(e);
                uint32_t draws = c->draws;
                if (c->count_buffer) {
                    // (the draw count is in GPU memory: read it now when the buffer is the CPU's to read)
                    if (c->count_buffer->memory && c->count_buffer->memory->map) {
                        uint32_t n = *(uint32_t *)((uint8_t *)c->count_buffer->memory->map + c->count_buffer->memory_offset + c->count_offset);
                        if (n < draws) draws = n;
                    }
                }
                for (uint32_t i = 0; i < draws; i++) {
                    uint64_t off = c->buffer->offset + c->offset + (uint64_t)i * (c->stride ? c->stride : (c->indexed ? 20 : 16));
                    if (c->indexed) {
                        if (!e->ib || !e->ib->mtl) break;
                        int i16 = e->ib_type == VK_INDEX_TYPE_UINT16;
                        [e->render drawIndexedPrimitives:pt indexType:i16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32 indexBuffer:e->ib->mtl
                                       indexBufferOffset:e->ib->offset + e->ib_offset indirectBuffer:c->buffer->mtl indirectBufferOffset:off];
                    } else [e->render drawPrimitives:pt indirectBuffer:c->buffer->mtl indirectBufferOffset:off];
                }
                break;
            }
            case C_DISPATCH: case C_DISPATCH_INDIRECT: {
                struct VkPipeline_T *p = e->cp;
                if (!p || !p->compute) break;
                id<MTLComputeCommandEncoder> ce = compute_encoder(e);
                [ce setComputePipelineState:p->compute];
                bind_sets_compute(e, p);
                if (h->op == C_DISPATCH) {
                    c_dispatch *c = (c_dispatch *)h;
                    if (!c->x || !c->y || !c->z) break;
                    [ce dispatchThreadgroups:MTLSizeMake(c->x, c->y, c->z) threadsPerThreadgroup:p->threads];
                } else {
                    c_dispatch_indirect *c = (c_dispatch_indirect *)h;
                    if (c->buffer->mtl)
                        [ce dispatchThreadgroupsWithIndirectBuffer:c->buffer->mtl indirectBufferOffset:c->buffer->offset + c->offset
                                             threadsPerThreadgroup:p->threads];
                }
                break;
            }
            case C_BEGIN_PASS: {
                c_begin_pass *c = (c_begin_pass *)h;
                c->pass.occlusion = pass_occlusion_pool((uint8_t *)h + h->size, chunk);
                begin_render(e, &c->pass);
                break;
            }
            case C_END_PASS:
                vk_end_encoder(e);
                e->in_pass = 0;
                break;
            case C_COPY_BUFFER: {
                c_copy_buffer *c = (c_copy_buffer *)h;
                if (!c->src->mtl || !c->dst->mtl) break;
                id<MTLBlitCommandEncoder> bl = blit_encoder(e);
                const VkBufferCopy2 *r = (const VkBufferCopy2 *)(c + 1);
                for (uint32_t i = 0; i < c->count; i++)
                    if (r[i].size)
                        [bl copyFromBuffer:c->src->mtl sourceOffset:c->src->offset + r[i].srcOffset toBuffer:c->dst->mtl
                         destinationOffset:c->dst->offset + r[i].dstOffset size:r[i].size];
                break;
            }
            case C_COPY_BUFFER_IMAGE: copy_buffer_image_encode(e, (c_copy_buffer_image *)h); break;
            case C_COPY_IMAGE: copy_image_encode(e, (c_copy_image *)h); break;
            case C_BLIT_IMAGE: {
                c_blit_image *c = (c_blit_image *)h;
                const VkImageBlit2 *r = (const VkImageBlit2 *)(c + 1);
                for (uint32_t i = 0; i < c->count; i++) vk_internal_blit(e, c->src, c->dst, &r[i], c->filter);
                break;
            }
            case C_RESOLVE_IMAGE: resolve_encode(e, (c_resolve_image *)h); break;
            case C_CLEAR_IMAGE: {
                c_clear_image *c = (c_clear_image *)h;
                const VkImageSubresourceRange *r = (const VkImageSubresourceRange *)(c + 1);
                for (uint32_t i = 0; i < c->count; i++) vk_internal_clear_image(e, c->image, c->depth, &c->value, &r[i]);
                break;
            }
            case C_CLEAR_ATTACHMENTS: {
                c_clear_attachments *c = (c_clear_attachments *)h;
                const VkClearAttachment *a = (const VkClearAttachment *)(c + 1);
                vk_internal_clear_attachments(e, c->attachments, a, c->rects, (const VkClearRect *)(a + c->attachments));
                e->state_dirty = 1;
                break;
            }
            case C_FILL_BUFFER: {
                c_fill *c = (c_fill *)h;
                vk_internal_fill(e, c->buffer, c->offset, c->size, c->data);
                break;
            }
            case C_UPDATE_BUFFER: {
                c_update *c = (c_update *)h;
                if (!c->buffer->mtl) break;
                id<MTLBuffer> tmp = [e->dev->mtl newBufferWithBytes:c + 1 length:c->size options:MTLResourceStorageModeShared];
                [blit_encoder(e) copyFromBuffer:tmp sourceOffset:0 toBuffer:c->buffer->mtl destinationOffset:c->buffer->offset + c->offset size:c->size];
                break;
            }
            case C_BARRIER: break;
            case C_SET_EVENT: {
                c_event *c = (c_event *)h;
                if (e->event_count == e->event_cap) {
                    e->event_cap = e->event_cap ? e->event_cap * 2 : 16;
                    e->events = realloc(e->events, e->event_cap * sizeof *e->events);
                    e->event_values = realloc(e->event_values, e->event_cap * sizeof *e->event_values);
                }
                e->events[e->event_count] = c->event;
                e->event_values[e->event_count++] = c->set;
                break;
            }
            case C_BEGIN_QUERY: {
                c_query *c = (c_query *)h;
                if (c->pool->type == VK_QUERY_TYPE_OCCLUSION && e->render && e->pass.occlusion == c->pool)
                    [e->render setVisibilityResultMode:(c->flags & VK_QUERY_CONTROL_PRECISE_BIT) ? MTLVisibilityResultModeCounting :
                                                       MTLVisibilityResultModeBoolean offset:c->query * 8];
                break;
            }
            case C_END_QUERY: {
                c_query *c = (c_query *)h;
                if (c->pool->type == VK_QUERY_TYPE_OCCLUSION && e->render) [e->render setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
                if (c->query < c->pool->count) c->pool->available[c->query] = 2;   // (available when the command buffer completes)
                break;
            }
            case C_RESET_QUERIES: {
                c_query *c = (c_query *)h;
                for (uint32_t i = 0; i < c->count && c->query + i < c->pool->count; i++) {
                    c->pool->available[c->query + i] = 0;
                    ((uint64_t *)c->pool->results.contents)[c->query + i] = 0;
                }
                break;
            }
            case C_TIMESTAMP: {
                c_query *c = (c_query *)h;
                timestamp_later(e, c->pool, c->query);
                break;
            }
            case C_COPY_QUERIES: {
                c_copy_queries *c = (c_copy_queries *)h;
                vk_internal_copy_queries(e, c->pool, c->first, c->count, c->dst, c->offset, c->stride, c->flags);
                break;
            }
            case C_EXECUTE: {
                c_execute *c = (c_execute *)h;
                VkCommandBuffer *list = (VkCommandBuffer *)(c + 1);
                for (uint32_t i = 0; i < c->count; i++) encode_list(e, list[i]);
                break;
            }
            default: break;
            }
        }
    }
}

// Encodes command buffers into one Metal command buffer on the queue's fence chain; returns the completion work.
typedef struct vk_completion {
    VkEvent *events;
    int *event_values;
    uint32_t event_count;
    struct { VkQueryPool pool; uint32_t query; } *timestamps;
    uint32_t timestamp_count;
    VkQueryPool *occlusion_pools;
} vk_completion;

void vk_encode(VkQueue q, id<MTLCommandBuffer> mcb, const VkCommandBuffer *cbs, uint32_t n);
void vk_encode(VkQueue q, id<MTLCommandBuffer> mcb, const VkCommandBuffer *cbs, uint32_t n) {
    vk_enc *e = calloc(1, sizeof *e);
    e->dev = q->device;
    e->queue = q;
    e->cb = mcb;
    if (!q->last_fence) q->last_fence = [q->device->mtl newFence];
    e->fence = q->last_fence;
    for (uint32_t i = 0; i < n; i++) {
        // (each command buffer starts from no state: Vulkan does not carry state between them)
        VkDevice dev = e->dev;
        id<MTLCommandBuffer> keep = e->cb;
        id<MTLFence> fence = e->fence;
        VkEvent *ev = e->events; int *evv = e->event_values; uint32_t evn = e->event_count, evc = e->event_cap;
        void *ts = e->timestamps; uint32_t tsn = e->timestamp_count, tsc = e->timestamp_cap;
        e->cb = nil; e->fence = nil; e->render = nil; e->compute = nil; e->blit = nil;
        memset((void *)e, 0, sizeof *e);
        e->dev = dev; e->queue = q; e->cb = keep; e->fence = fence;
        e->events = ev; e->event_values = evv; e->event_count = evn; e->event_cap = evc;
        e->timestamps = ts; e->timestamp_count = tsn; e->timestamp_cap = tsc;
        e->front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        encode_list(e, cbs[i]);
        vk_end_encoder(e);
    }
    // Completion: events set, timestamps written (on the CPU: the time the work finished).
    uint32_t evn = e->event_count, tsn = e->timestamp_count;
    VkEvent *events = e->events;
    int *values = e->event_values;
    void *ts = e->timestamps;
    if (evn || tsn) {
        [mcb addCompletedHandler:^(id<MTLCommandBuffer> done) {
            (void)done;
            for (uint32_t i = 0; i < evn; i++) atomic_store(&events[i]->set, values[i]);
            struct { VkQueryPool pool; uint32_t query; } *t = ts;
            uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            for (uint32_t i = 0; i < tsn; i++)
                if (t[i].query < t[i].pool->count) {
                    ((uint64_t *)t[i].pool->results.contents)[t[i].query] = now;
                    t[i].pool->available[t[i].query] = 1;
                }
            free(events); free(values); free(ts);
        }];
    } else { free(events); free(values); free(ts); }
    e->cb = nil; e->fence = nil;
    free(e);
}

// Occlusion queries recorded as pending become available when their command buffer completes.
void vk_mark_queries_available(VkQueryPool pool);
void vk_mark_queries_available(VkQueryPool pool) {
    for (uint32_t i = 0; i < pool->count; i++) if (pool->available[i] == 2) pool->available[i] = 1;
}

#ifdef __aarch64__
#include "thunks/vk_command.m.inc"
#endif
