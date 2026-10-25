// The encoder state shared by vk_command.m (which records and encodes command buffers) and vk_internal.m (blits,
// clears and fills done with shaders of our own).
#ifndef VX_VK_ENCODE_H
#define VX_VK_ENCODE_H
#include "vk_impl.h"

// One Metal render pass's attachments.
typedef struct {
    VkImageView view, resolve;
    uint8_t load, store, sload, sstore;
    uint8_t resolve_mode;
    VkClearValue clear;
} pass_attachment;
typedef struct {
    uint32_t color_count;
    pass_attachment colors[8];
    pass_attachment depth;              // view NULL: none
    uint8_t has_depth, has_stencil;
    VkRect2D area;
    uint32_t layers, view_mask;
    VkQueryPool occlusion;              // the occlusion query pool used in the pass, if any
    uint8_t has_gs;                     // it draws with a geometry shader (which ends the Metal pass and resumes it)
    uint64_t dep_dst;                   // the stages its render pass's dependencies into it make wait
} pass_desc;

typedef struct {
    VkDescriptorSet sets[VKMSL_MAX_SETS];
    struct VkDescriptorSet_T *push[VKMSL_MAX_SETS];
    uint32_t dyn[VKMSL_MAX_SETS][64];
    uint32_t dyn_count[VKMSL_MAX_SETS];
} bindings_state;

typedef struct vk_enc {
    VkDevice dev;
    VkQueue queue;
    id<MTLCommandBuffer> cb;
    id<MTLRenderCommandEncoder> render;
    id<MTLComputeCommandEncoder> compute;
    id<MTLBlitCommandEncoder> blit;
    id<MTLFence> fence;
    VkPipeline gp, cp;
    bindings_state gb, cb_;             // graphics and compute bind points
    uint8_t push[256];
    VkBuffer vb[VKMSL_MAX_VERTEX_BUFFERS];
    uint64_t vb_offset[VKMSL_MAX_VERTEX_BUFFERS], vb_stride[VKMSL_MAX_VERTEX_BUFFERS];
    int vb_has_stride;
    VkBuffer ib;
    uint64_t ib_offset;
    VkIndexType ib_type;
    // Dynamic state (the pipeline's when it is not dynamic).
    VkViewport viewports[16];
    VkRect2D scissors[16];
    uint32_t viewport_count, scissor_count;
    float depth_bias[3], blend[4];
    int depth_bias_enable;
    uint32_t stencil_ref[2], stencil_compare[2], stencil_write[2];
    VkCullModeFlags cull;
    VkFrontFace front;
    VkPrimitiveTopology topology;
    int primitive_restart, primitive_restart_set;
    int dyn_depth_test, dyn_depth_write, dyn_stencil_test;
    VkCompareOp dyn_depth_compare;
    VkStencilOp dyn_stencil_ops[2][3];
    VkCompareOp dyn_stencil_compare_op[2];
    uint32_t dyn_set;                   // VK_DYN_* states set by commands since the pipeline was bound
    pass_desc pass;
    uint32_t pass_width, pass_height, pass_samples;
    int in_pass;
    int state_dirty;
    MTLVisibilityResultMode vis_mode;   // (the occlusion query running, set again when a pass resumes)
    uint64_t vis_offset;
    // Things to do when the command buffer completes.
    VkEvent *events;
    int *event_values;
    uint32_t event_count, event_cap;
    struct { VkQueryPool pool; uint32_t query; } *timestamps;
    uint32_t timestamp_count, timestamp_cap;
    // VXRT_GPU_PROFILE (vk_command.m): the open encoder's timestamp samples (-1: none), its draws, what a compute
    // encoder is opened for.
    int prof_sample;
    uint32_t prof_draws;
    const char *prof_tag;
    uint32_t prof_first;                // (this command buffer's first sample)
    // The pass's tessellation draws whose kernels already ran before the pass began (vk_command.m, hoist_tessellation):
    // where each draw's results are in the queue's scratch buffers, in the order of the draws.
    struct { uint64_t vout, cp, pb, tf; uint8_t valid; } tess_pre[256];
    uint32_t tess_pre_count, tess_pre_next;
    // The stages the barriers since the last encoder began make wait (begin_render: a render pass waits for the work
    // before it at its vertex stage only when they include stages before rasterization), and a render pass that must
    // wait there anyway (after the runtime's own kernels whose results its vertex stage reads).
    uint64_t wait_dst;
    int vertex_wait;
    int no_use_heaps;
    int compute_concurrent;             // (the next compute encoder: dispatches that may run together, hoist_tessellation)
    int hoist_phase;
    __unsafe_unretained id<MTLBuffer> fan_buf;   // (triangle fans' indices: one buffer a command buffer, the Metal command buffer keeps it)
    uint64_t fan_used;                    // (hoist_walk: 1 the vertex kernels, 2 the control kernels, 3 both)                   // (the device's residency set holds the heaps: vk_residency_on)
    // What the open render encoder has been given (vk_command.m, VXRT_VK_ENCODE_CACHE): the same again is not sent.
    // valid 0: nothing known (a new encoder, or one that code of its own drew with: clears).
    struct {
        int valid;
        __unsafe_unretained id<MTLRenderPipelineState> pipeline;
        __unsafe_unretained id<MTLDepthStencilState> depth_stencil;
        __unsafe_unretained id<MTLBuffer> vbuf[32], fbuf[32];   // (nil: bytes or unknown)
        uint64_t voff[32], foff[32];
        uint32_t vbytes_len[2], fbytes_len[2];                    // push constants [0], dynamic offsets [1]
        uint8_t vbytes[2][1024], fbytes[2][1024];
        MTLViewport viewports[16]; uint32_t nviewports;
        MTLScissorRect scissors[16]; uint32_t nscissors;
        int cull, winding, fill, clip;
        float bias[3], blend[4];
        uint32_t sref[2];
        int have_bias, have_blend, have_sref;
    } rc;
} vk_enc;

void vk_end_encoder(vk_enc *e);
void vk_internal_clear_attachments(vk_enc *e, uint32_t na, const VkClearAttachment *atts, uint32_t nr, const VkClearRect *rects);
void vk_internal_blit(vk_enc *e, VkImage src, VkImage dst, const VkImageBlit2 *r, VkFilter filter);
void vk_internal_clear_image(vk_enc *e, VkImage img, int depth, const VkClearValue *v, const VkImageSubresourceRange *range);
void vk_internal_fill(vk_enc *e, VkBuffer b, uint64_t offset, uint64_t size, uint32_t data);
void vk_internal_copy_queries(vk_enc *e, VkQueryPool pool, uint32_t first, uint32_t count, VkBuffer dst, uint64_t offset,
                              uint64_t stride, VkQueryResultFlags flags);

#endif
