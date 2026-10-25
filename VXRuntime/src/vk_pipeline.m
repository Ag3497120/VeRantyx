// Shader modules, pipelines (SPIR-V translated by vk_msl.c into Metal functions), render passes and framebuffers.
#include "vk_impl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t vk_layout_bindings(const struct VkPipelineLayout_T *l, vkmsl_binding *out, uint32_t cap);
void vk_pipeline_layout_copy(struct VkPipelineLayout_T *dst, VkPipelineLayout src);
void vk_pipeline_layout_release_copy(struct VkPipelineLayout_T *l);
MTLCompareFunction vk_compare(VkCompareOp op);
#define TRACE(...) do { if (vk_trace()) vx_log("vulkan: " __VA_ARGS__); } while (0)

// ---- shader modules ----------------------------------------------------------------------------------------------
VKFN VkResult VKAPI_CALL vkCreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo *info, const VkAllocationCallbacks *alloc,
                                              VkShaderModule *out) {
    (void)dev; (void)alloc;
    struct VkShaderModule_T *m = calloc(1, sizeof *m);
    if (!m) return VK_ERROR_OUT_OF_HOST_MEMORY;
    m->words = info->codeSize / 4;
    m->code = malloc(info->codeSize ? info->codeSize : 4);
    memcpy(m->code, info->pCode, info->codeSize);
    *out = m;
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkDestroyShaderModule(VkDevice dev, VkShaderModule m, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    if (!m) return;
    free(m->code);
    free(m);
}

// ---- pipeline caches (Metal keeps its own) -----------------------------------------------------------------------
VKFN VkResult VKAPI_CALL vkCreatePipelineCache(VkDevice dev, const VkPipelineCacheCreateInfo *info, const VkAllocationCallbacks *alloc,
                                               VkPipelineCache *out) {
    (void)dev; (void)info; (void)alloc;
    *out = calloc(1, sizeof(struct VkPipelineCache_T));
    return *out ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
VKFN void VKAPI_CALL vkDestroyPipelineCache(VkDevice dev, VkPipelineCache c, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    free(c);
}
VKFN VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice dev, VkPipelineCache c, size_t *size, void *data) {
    (void)c;
    // (only the header the specification requires)
    if (!data) { *size = 32; return VK_SUCCESS; }
    if (*size < 32) { *size = 0; return VK_INCOMPLETE; }
    VkPhysicalDeviceProperties p;
    extern VKFN void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties *);
    vkGetPhysicalDeviceProperties(dev->phys, &p);
    uint32_t header[4] = { 32, VK_PIPELINE_CACHE_HEADER_VERSION_ONE, p.vendorID, p.deviceID };
    memcpy(data, header, 16);
    memcpy((uint8_t *)data + 16, p.pipelineCacheUUID, 16);
    *size = 32;
    return VK_SUCCESS;
}
VKFN VkResult VKAPI_CALL vkMergePipelineCaches(VkDevice dev, VkPipelineCache dst, uint32_t n, const VkPipelineCache *src) {
    (void)dev; (void)dst; (void)n; (void)src;
    return VK_SUCCESS;
}

// ---- shader translation and compilation --------------------------------------------------------------------------
static id<MTLFunction> compile_stage(VkDevice dev, const VkPipelineShaderStageCreateInfo *stage, uint32_t vkmsl_stage,
                                     const struct VkPipelineLayout_T *layout, uint32_t flags, uint32_t vary_slots,
                                     vkmsl_result *res, id<MTLLibrary> *lib_out, VkResult *err, const vkmsl_tess *tess) {
    VkShaderModule m = stage->module;
    const VkShaderModuleCreateInfo *inline_code = vk_find_struct(stage->pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    const uint32_t *code = m ? m->code : inline_code ? inline_code->pCode : NULL;
    size_t words = m ? m->words : inline_code ? inline_code->codeSize / 4 : 0;
    vkmsl_binding bindings[512];
    uint32_t nb = vk_layout_bindings(layout, bindings, 512);
    vkmsl_spec specs[64];
    uint32_t ns = 0;
    const VkSpecializationInfo *si = stage->pSpecializationInfo;
    for (uint32_t i = 0; si && i < si->mapEntryCount && ns < 64; i++) {
        const VkSpecializationMapEntry *e = &si->pMapEntries[i];
        uint64_t v = 0;
        memcpy(&v, (const uint8_t *)si->pData + e->offset, e->size < 8 ? e->size : 8);
        specs[ns++] = (vkmsl_spec){ e->constantID, (uint32_t)e->size, v };
    }
    vkmsl_options o = { .stage = vkmsl_stage, .entry = stage->pName, .bindings = bindings, .binding_count = nb,
                        .specs = specs, .spec_count = ns, .flags = flags, .vary_slots = vary_slots };
    if (tess) o.tess = *tess;
    int rc = vkmsl_translate(code, words, &o, res);
    if (rc) {
        vk_log_unique("vulkan: %s shader \"%s\" not translated: %s", vkmsl_stage == VKMSL_STAGE_VERTEX ? "vertex" :
               vkmsl_stage == VKMSL_STAGE_FRAGMENT ? "fragment" : vkmsl_stage == VKMSL_STAGE_COMPUTE ? "compute" :
               vkmsl_stage == VKMSL_STAGE_GEOMETRY ? "geometry" : vkmsl_stage == VKMSL_STAGE_TESS_CONTROL ? "tessellation control" :
               vkmsl_stage == VKMSL_STAGE_TESS_EVALUATION ? "tessellation evaluation" : "other",
               stage->pName, res->error);
        *err = VK_ERROR_INITIALIZATION_FAILED;
        return nil;
    }
    NSString *src = [[NSString alloc] initWithBytes:res->msl length:res->msl_size encoding:NSUTF8StringEncoding];
    id<MTLLibrary> lib = nil;
    pthread_mutex_lock(&dev->internal_lock);
    if (!dev->internal_pipelines) dev->internal_pipelines = [NSMutableDictionary new];
    lib = dev->internal_pipelines[src];
    pthread_mutex_unlock(&dev->internal_lock);
    if (!lib) {
        MTLCompileOptions *opts = [MTLCompileOptions new];
        opts.languageVersion = MTLLanguageVersion3_1;
        NSError *e = nil;
        lib = [dev->mtl newLibraryWithSource:src options:opts error:&e];
        if (!lib) {
            vk_log_unique("vulkan: shader \"%s\" did not compile: %s", stage->pName, e.localizedDescription.UTF8String);
            if (vk_trace()) vx_log("%s", res->msl);
            *err = VK_ERROR_INITIALIZATION_FAILED;
            return nil;
        }
        pthread_mutex_lock(&dev->internal_lock);
        dev->internal_pipelines[src] = lib;
        pthread_mutex_unlock(&dev->internal_lock);
    }
    if (lib_out) *lib_out = lib;
    return [lib newFunctionWithName:@"vkmain"];
}

// ---- graphics pipelines -----------------------------------------------------------------------------------------
static MTLBlendFactor blend_factor(VkBlendFactor f) {
    static const MTLBlendFactor map[] = {
        MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorSourceColor, MTLBlendFactorOneMinusSourceColor,
        MTLBlendFactorDestinationColor, MTLBlendFactorOneMinusDestinationColor, MTLBlendFactorSourceAlpha,
        MTLBlendFactorOneMinusSourceAlpha, MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha,
        MTLBlendFactorBlendColor, MTLBlendFactorOneMinusBlendColor, MTLBlendFactorBlendAlpha,
        MTLBlendFactorOneMinusBlendAlpha, MTLBlendFactorSourceAlphaSaturated, MTLBlendFactorSource1Color,
        MTLBlendFactorOneMinusSource1Color, MTLBlendFactorSource1Alpha, MTLBlendFactorOneMinusSource1Alpha };
    return (uint32_t)f < sizeof map / sizeof map[0] ? map[f] : MTLBlendFactorOne;
}
static MTLBlendOperation blend_op(VkBlendOp op) {
    static const MTLBlendOperation map[] = { MTLBlendOperationAdd, MTLBlendOperationSubtract,
        MTLBlendOperationReverseSubtract, MTLBlendOperationMin, MTLBlendOperationMax };
    return (uint32_t)op < 5 ? map[op] : MTLBlendOperationAdd;
}
static MTLStencilDescriptor *stencil_desc(const VkStencilOpState *s) {
    MTLStencilDescriptor *d = [MTLStencilDescriptor new];
    d.stencilCompareFunction = vk_compare(s->compareOp);
    d.stencilFailureOperation = (MTLStencilOperation)s->failOp;
    d.depthFailureOperation = (MTLStencilOperation)s->depthFailOp;
    d.depthStencilPassOperation = (MTLStencilOperation)s->passOp;
    d.readMask = s->compareMask;
    d.writeMask = s->writeMask;
    return d;
}
id<MTLDepthStencilState> vk_depth_stencil_state(VkDevice dev, const VkPipelineDepthStencilStateCreateInfo *ds, int has_depth, int has_stencil);
id<MTLDepthStencilState> vk_depth_stencil_state(VkDevice dev, const VkPipelineDepthStencilStateCreateInfo *ds, int has_depth, int has_stencil) {
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    if (ds && has_depth && ds->depthTestEnable) {
        d.depthCompareFunction = vk_compare(ds->depthCompareOp);
        d.depthWriteEnabled = ds->depthWriteEnable != 0;
    }
    if (ds && has_stencil && ds->stencilTestEnable) {
        d.frontFaceStencil = stencil_desc(&ds->front);
        d.backFaceStencil = stencil_desc(&ds->back);
    }
    return [dev->mtl newDepthStencilStateWithDescriptor:d];
}

static MTLPrimitiveType primitive(VkPrimitiveTopology t, int *ok) {
    *ok = 1;
    switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return MTLPrimitiveTypePoint;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: return MTLPrimitiveTypeLine;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return MTLPrimitiveTypeLineStrip;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: return MTLPrimitiveTypeTriangle;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return MTLPrimitiveTypeTriangleStrip;
    // Metal has no fans: their draws are made triangle lists, with indices built when they are encoded (vk_command.m).
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: return MTLPrimitiveTypeTriangle;
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST: return MTLPrimitiveTypeTriangle;   // (with tessellation: Metal's triangles)
    default: *ok = 0; return MTLPrimitiveTypeTriangle;
    }
}
static MTLPrimitiveTopologyClass topology_class(MTLPrimitiveType p) {
    return p == MTLPrimitiveTypePoint ? MTLPrimitiveTopologyClassPoint :
           p == MTLPrimitiveTypeLine || p == MTLPrimitiveTypeLineStrip ? MTLPrimitiveTopologyClassLine :
           MTLPrimitiveTopologyClassTriangle;
}

// The attachment formats a pipeline renders to: from its render pass's subpass, or from dynamic rendering's info.
typedef struct { uint32_t count; VkFormat colors[8]; VkFormat depth, stencil; uint32_t samples; } attachment_formats;
static void formats_of(const VkGraphicsPipelineCreateInfo *info, attachment_formats *af) {
    memset(af, 0, sizeof *af);
    af->samples = info->pMultisampleState ? info->pMultisampleState->rasterizationSamples : 1;
    if (info->renderPass) {
        VkRenderPass rp = info->renderPass;
        const vk_subpass *sp = &rp->subpasses[info->subpass < rp->subpass_count ? info->subpass : 0];
        af->count = sp->color_count;
        for (uint32_t i = 0; i < sp->color_count; i++)
            af->colors[i] = sp->colors[i] == VK_ATTACHMENT_UNUSED ? VK_FORMAT_UNDEFINED : rp->attachments[sp->colors[i]].format;
        if (sp->depth != VK_ATTACHMENT_UNUSED) {
            VkFormat f = rp->attachments[sp->depth].format;
            const vk_format_info *fi = vk_format(f);
            if (fi && (fi->aspects & VKF_DEPTH)) af->depth = f;
            if (fi && (fi->aspects & VKF_STENCIL)) af->stencil = f;
        }
        return;
    }
    const VkPipelineRenderingCreateInfo *r = vk_find_struct(info->pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
    if (!r) return;
    af->count = r->colorAttachmentCount < 8 ? r->colorAttachmentCount : 8;
    for (uint32_t i = 0; i < af->count; i++) af->colors[i] = r->pColorAttachmentFormats ? r->pColorAttachmentFormats[i] : VK_FORMAT_UNDEFINED;
    af->depth = r->depthAttachmentFormat;
    af->stencil = r->stencilAttachmentFormat;
}

static void pipeline_free(struct VkPipeline_T *p) {
    if (!p) return;
    p->render = nil; p->compute = nil; p->depth_stencil = nil;
    for (int i = 0; i < 3; i++) p->gs_vs[i] = nil;
    p->gs_kernel = nil;
    p->tcs_kernel = nil;
    vk_pipeline_layout_release_copy(&p->layout);
    free(p);
}

static VkResult create_graphics(VkDevice dev, const VkGraphicsPipelineCreateInfo *info, VkPipeline *out) {
    struct VkPipeline_T *p = calloc(1, sizeof *p);
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (info->layout) vk_pipeline_layout_copy(&p->layout, info->layout);
    const VkPipelineDynamicStateCreateInfo *dyn = info->pDynamicState;
    for (uint32_t i = 0; dyn && i < dyn->dynamicStateCount; i++) {
        switch (dyn->pDynamicStates[i]) {
        case VK_DYNAMIC_STATE_VIEWPORT: p->dynamic |= VK_DYN_VIEWPORT; break;
        case VK_DYNAMIC_STATE_SCISSOR: p->dynamic |= VK_DYN_SCISSOR; break;
        case VK_DYNAMIC_STATE_LINE_WIDTH: p->dynamic |= VK_DYN_LINE_WIDTH; break;
        case VK_DYNAMIC_STATE_DEPTH_BIAS: p->dynamic |= VK_DYN_DEPTH_BIAS; break;
        case VK_DYNAMIC_STATE_BLEND_CONSTANTS: p->dynamic |= VK_DYN_BLEND_CONSTANTS; break;
        case VK_DYNAMIC_STATE_DEPTH_BOUNDS: p->dynamic |= VK_DYN_DEPTH_BOUNDS; break;
        case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK: p->dynamic |= VK_DYN_STENCIL_COMPARE; break;
        case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK: p->dynamic |= VK_DYN_STENCIL_WRITE; break;
        case VK_DYNAMIC_STATE_STENCIL_REFERENCE: p->dynamic |= VK_DYN_STENCIL_REFERENCE; break;
        case VK_DYNAMIC_STATE_CULL_MODE: p->dynamic |= VK_DYN_CULL_MODE; break;
        case VK_DYNAMIC_STATE_FRONT_FACE: p->dynamic |= VK_DYN_FRONT_FACE; break;
        case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY: p->dynamic |= VK_DYN_TOPOLOGY; break;
        case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT: p->dynamic |= VK_DYN_VIEWPORT | VK_DYN_VIEWPORT_COUNT; break;
        case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT: p->dynamic |= VK_DYN_SCISSOR | VK_DYN_SCISSOR_COUNT; break;
        case VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE: p->dynamic |= VK_DYN_STRIDE; break;
        case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE: p->dynamic |= VK_DYN_DEPTH_TEST; break;
        case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE: p->dynamic |= VK_DYN_DEPTH_WRITE; break;
        case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP: p->dynamic |= VK_DYN_DEPTH_COMPARE; break;
        case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE: p->dynamic |= VK_DYN_DEPTH_BOUNDS_TEST; break;
        case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE: p->dynamic |= VK_DYN_STENCIL_TEST; break;
        case VK_DYNAMIC_STATE_STENCIL_OP: p->dynamic |= VK_DYN_STENCIL_OP; break;
        case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE: p->dynamic |= VK_DYN_DEPTH_BIAS_ENABLE; break;
        case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE: p->dynamic |= VK_DYN_PRIMITIVE_RESTART; break;
        case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE: p->dynamic |= VK_DYN_RASTERIZER_DISCARD; break;
        default: TRACE("dynamic state %d ignored", dyn->pDynamicStates[i]); break;
        }
    }
    // Stages.
    const VkPipelineShaderStageCreateInfo *vs = NULL, *fs = NULL, *gs = NULL, *tcs = NULL, *tes = NULL;
    for (uint32_t i = 0; i < info->stageCount; i++) {
        const VkPipelineShaderStageCreateInfo *s = &info->pStages[i];
        if (s->stage == VK_SHADER_STAGE_VERTEX_BIT) vs = s;
        else if (s->stage == VK_SHADER_STAGE_FRAGMENT_BIT) fs = s;
        else if (s->stage == VK_SHADER_STAGE_GEOMETRY_BIT) gs = s;
        else if (s->stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) tcs = s;
        else if (s->stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) tes = s;
        else {
            if (vk_log_once("shader stage", s->stage)) vx_log("vulkan: shader stage 0x%x is not supported (logged once)", s->stage);
            pipeline_free(p);
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    if (!vs || (!tcs != !tes) || (tcs && gs)) {
        if (vs && tcs && gs && vk_log_once("tessellation with a geometry shader", 0))
            vx_log("vulkan: tessellation with a geometry shader is not supported (logged once)");
        pipeline_free(p);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    int tess = tcs != NULL;
    const VkPipelineRasterizationStateCreateInfo *rs = info->pRasterizationState;
    p->rasterizer_discard = rs && rs->rasterizerDiscardEnable;
    VkResult err = VK_SUCCESS;
    vkmsl_result vr, fr, gr, cr, er;
    memset(&fr, 0, sizeof fr);
    memset(&gr, 0, sizeof gr);
    memset(&cr, 0, sizeof cr);
    memset(&er, 0, sizeof er);
    id<MTLFunction> vf = compile_stage(dev, vs, VKMSL_STAGE_VERTEX, &p->layout, gs || tess ? VKMSL_FLAG_VERTEX_KERNEL : 0, 0, &vr, NULL, &err, NULL);
    id<MTLFunction> ff = nil, gf = nil, pass = nil, cf = nil, ef = nil;
    if (vf && gs) {
        id<MTLLibrary> lib = nil;
        gf = compile_stage(dev, gs, VKMSL_STAGE_GEOMETRY, &p->layout, 0, vr.vary_locations, &gr, &lib, &err, NULL);
        if (gf) pass = [lib newFunctionWithName:@"vkpass"];
    }
    vkmsl_tess tmodes = { 0 };
    if (vf && tess) {
        cf = compile_stage(dev, tcs, VKMSL_STAGE_TESS_CONTROL, &p->layout, 0, vr.vary_locations, &cr, NULL, &err, NULL);
        if (cf) {
            // (the evaluation shader is given what the control shader declares and writes; modes from either)
            ef = compile_stage(dev, tes, VKMSL_STAGE_TESS_EVALUATION, &p->layout, 0, 0, &er, NULL, &err, &cr.tess);
            tmodes = er.tess;
            if (!tmodes.domain) tmodes.domain = cr.tess.domain;
            if (!tmodes.spacing) tmodes.spacing = cr.tess.spacing;
            if (!tmodes.order) tmodes.order = cr.tess.order;
            tmodes.point_mode |= cr.tess.point_mode;
        }
        if (!ef) pass = nil;
    }
    if (tess) pass = ef;   // (the render pipeline's vertex function)
    attachment_formats pass_formats;
    formats_of(info, &pass_formats);
    uint32_t fs_flags = pass_formats.depth == VK_FORMAT_UNDEFINED ? VKMSL_FLAG_NO_DEPTH_ATTACHMENT : 0;
    if (vf && (!(gs || tess) || pass) && fs && !p->rasterizer_discard) ff = compile_stage(dev, fs, VKMSL_STAGE_FRAGMENT, &p->layout, fs_flags, 0, &fr, NULL, &err, NULL);
    if (!vf || ((gs || tess) && !pass) || (fs && !p->rasterizer_discard && !ff)) {
        vkmsl_result_free(&vr); vkmsl_result_free(&fr); vkmsl_result_free(&gr); vkmsl_result_free(&cr); vkmsl_result_free(&er);
        pipeline_free(p);
        return err ? err : VK_ERROR_INITIALIZATION_FAILED;
    }
    p->sets_vertex = vr.sets_used; p->sets_fragment = fr.sets_used; p->sets_geometry = gr.sets_used;
    p->push_vertex = vr.uses_push_constants; p->push_fragment = fr.uses_push_constants; p->push_geometry = gr.uses_push_constants;
    p->aux_vertex = vr.uses_aux; p->aux_fragment = fr.uses_aux; p->aux_geometry = gr.uses_aux;
    p->sets_tcs = cr.sets_used; p->push_tcs = cr.uses_push_constants; p->aux_tcs = cr.uses_aux;
    p->sets_tes = er.sets_used; p->push_tes = er.uses_push_constants; p->aux_tes = er.uses_aux;

    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = gs || tess ? pass : vf;
    if (tess) {
        // Metal's tessellator, with the factors the control kernel writes (half precision, per patch and instance).
        p->tess = 1;
        p->tess_quad = tmodes.domain == 2;
        p->patch_cp = info->pTessellationState ? info->pTessellationState->patchControlPoints : 0;
        p->tess_out_vertices = cr.tess.out_vertices;
        p->tess_cp_slots = cr.tess.cp_locations + 4;
        p->tess_patch_slots = cr.tess.patch_locations + 2;
        d.tessellationFactorFormat = MTLTessellationFactorFormatHalf;
        d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatchAndPerInstance;
        d.tessellationControlPointIndexType = MTLTessellationControlPointIndexTypeNone;
        d.tessellationFactorScaleEnabled = NO;
        d.tessellationPartitionMode = tmodes.spacing == 2 ? MTLTessellationPartitionModeFractionalEven :
                                      tmodes.spacing == 3 ? MTLTessellationPartitionModeFractionalOdd : MTLTessellationPartitionModeInteger;
        d.maxTessellationFactor = 64;
        // Vulkan's domain has its origin at the upper left: its counter-clockwise is Metal's clockwise (tests/vulkan_shaders.c).
        d.tessellationOutputWindingOrder = tmodes.order == 1 ? MTLWindingCounterClockwise : MTLWindingClockwise;
        if (tmodes.point_mode && vk_log_once("tessellation point mode", 0))
            vx_log("vulkan: tessellation in point mode is drawn as triangles (logged once)");
    }
    d.fragmentFunction = ff;
    // Vertex input.
    const VkPipelineVertexInputStateCreateInfo *vi = info->pVertexInputState;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    int vkernel = gs || tess;   // (the vertex shader as kernels)
    MTLStageInputOutputDescriptor *sd = vkernel ? [MTLStageInputOutputDescriptor stageInputOutputDescriptor] : nil;
    const VkPipelineVertexInputDivisorStateCreateInfoEXT *div = vi ? vk_find_struct(vi->pNext, VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT) : NULL;
    for (uint32_t i = 0; vi && i < vi->vertexAttributeDescriptionCount; i++) {
        const VkVertexInputAttributeDescription *a = &vi->pVertexAttributeDescriptions[i];
        if (a->location >= 31 || !(vr.vertex_inputs & (1u << a->location))) continue;
        const vk_format_info *f = vk_format(a->format);
        if (!f || f->vertex == MTLVertexFormatInvalid) {
            if (vk_log_once("vertex format", a->format)) vx_log("vulkan: vertex format %d is not supported (logged once)", a->format);
            continue;
        }
        vd.attributes[a->location].format = f->vertex;
        vd.attributes[a->location].offset = a->offset;
        vd.attributes[a->location].bufferIndex = VKMSL_VERTEX_BUFFER + a->binding;
        if (vkernel) {
            sd.attributes[a->location].format = (MTLAttributeFormat)f->vertex;   // (the same values)
            sd.attributes[a->location].offset = a->offset;
            sd.attributes[a->location].bufferIndex = VKMSL_VERTEX_BUFFER + a->binding;
        }
        p->vertex_bindings |= 1u << a->binding;
    }
    for (uint32_t i = 0; vi && i < vi->vertexBindingDescriptionCount; i++) {
        const VkVertexInputBindingDescription *b = &vi->pVertexBindingDescriptions[i];
        if (b->binding >= VKMSL_MAX_VERTEX_BUFFERS || !(p->vertex_bindings & (1u << b->binding))) continue;
        MTLVertexBufferLayoutDescriptor *l = vd.layouts[VKMSL_VERTEX_BUFFER + b->binding];
        p->vertex_strides[b->binding] = b->stride;
        if (p->dynamic & VK_DYN_STRIDE) l.stride = MTLBufferLayoutStrideDynamic;
        else l.stride = b->stride ? b->stride : 4;
        if (!b->stride && !(p->dynamic & VK_DYN_STRIDE)) { l.stepFunction = MTLVertexStepFunctionConstant; l.stepRate = 0; }
        else if (b->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE) {
            l.stepFunction = MTLVertexStepFunctionPerInstance;
            l.stepRate = 1;
            for (uint32_t k = 0; div && k < div->vertexBindingDivisorCount; k++)
                if (div->pVertexBindingDivisors[k].binding == b->binding) {
                    uint32_t dv = div->pVertexBindingDivisors[k].divisor;
                    if (dv) l.stepRate = dv;
                    else { l.stepFunction = MTLVertexStepFunctionConstant; l.stepRate = 0; }
                }
        } else l.stepFunction = MTLVertexStepFunctionPerVertex;
        if (vkernel) {
            // (the vertex kernel's threads: x the vertex, y the instance; the first vertex and instance are buffer offsets)
            MTLBufferLayoutDescriptor *k = sd.layouts[VKMSL_VERTEX_BUFFER + b->binding];
            k.stride = l.stride;
            p->vertex_rate[b->binding] = b->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE;
            p->vertex_divisor[b->binding] = l.stepFunction == MTLVertexStepFunctionConstant ? 0 : (uint32_t)l.stepRate;
            if (l.stepFunction == MTLVertexStepFunctionConstant) { k.stepFunction = MTLStepFunctionConstant; k.stepRate = 0; }
            else if (l.stepFunction == MTLVertexStepFunctionPerInstance) { k.stepFunction = MTLStepFunctionThreadPositionInGridY; k.stepRate = l.stepRate; }
            else k.stepFunction = MTLStepFunctionThreadPositionInGridX;
        }
    }
    if (!vkernel) d.vertexDescriptor = vd;
    else {
        // The vertex shader as kernels: not indexed, 16-bit and 32-bit indices (the stage input fetches through them).
        for (int k = 0; k < 3; k++) {
            MTLComputePipelineDescriptor *cd = [MTLComputePipelineDescriptor new];
            cd.computeFunction = vf;
            if (p->vertex_bindings) {
                MTLStageInputOutputDescriptor *s = [sd copy];
                s.indexType = k == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
                s.indexBufferIndex = VKMSL_GS_INDEX;
                for (uint32_t b = 0; k && b < VKMSL_MAX_VERTEX_BUFFERS; b++)
                    if (s.layouts[VKMSL_VERTEX_BUFFER + b].stepFunction == MTLStepFunctionThreadPositionInGridX)
                        s.layouts[VKMSL_VERTEX_BUFFER + b].stepFunction = MTLStepFunctionThreadPositionInGridXIndexed;
                cd.stageInputDescriptor = s;
            } else if (k) { p->gs_vs[k] = p->gs_vs[0]; continue; }
            NSError *e = nil;
            p->gs_vs[k] = [dev->mtl newComputePipelineStateWithDescriptor:cd options:0 reflection:nil error:&e];
            if (!p->gs_vs[k]) {
                vk_log_unique("vulkan: vertex kernel failed: %s", e.localizedDescription.UTF8String);
                vkmsl_result_free(&vr); vkmsl_result_free(&fr); vkmsl_result_free(&gr); vkmsl_result_free(&cr); vkmsl_result_free(&er);
                pipeline_free(p);
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
        NSError *e = nil;
        if (tess) {
            p->tcs_kernel = [dev->mtl newComputePipelineStateWithFunction:cf error:&e];
            if (!p->tcs_kernel || p->tcs_kernel.maxTotalThreadsPerThreadgroup < p->tess_out_vertices) {
                vk_log_unique("vulkan: tessellation control kernel failed: %s", e ? e.localizedDescription.UTF8String : "too many control points");
                vkmsl_result_free(&vr); vkmsl_result_free(&fr); vkmsl_result_free(&gr); vkmsl_result_free(&cr); vkmsl_result_free(&er);
                pipeline_free(p);
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            p->gs_in_slots = vr.vary_locations + 4;
        } else
        p->gs_kernel = [dev->mtl newComputePipelineStateWithFunction:gf error:&e];
        if (!tess && !p->gs_kernel) {
            vk_log_unique("vulkan: geometry kernel failed: %s", e.localizedDescription.UTF8String);
            vkmsl_result_free(&vr); vkmsl_result_free(&fr); vkmsl_result_free(&gr); vkmsl_result_free(&cr); vkmsl_result_free(&er);
            pipeline_free(p);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (!tess) {
        p->gs_in_vertices = gr.gs_in_vertices;
        p->gs_max_tris = gr.gs_max_triangles;
        p->gs_out_slots = gr.gs_out_slots;
        p->gs_invocations = gr.gs_invocations ? gr.gs_invocations : 1;
        p->gs_in_slots = vr.vary_locations + 4;
        }
    }
    // Attachments.
    attachment_formats af;
    formats_of(info, &af);
    const VkPipelineColorBlendStateCreateInfo *cb = info->pColorBlendState;
    for (uint32_t i = 0; i < af.count; i++) {
        const vk_format_info *f = vk_format(af.colors[i]);
        if (!f || af.colors[i] == VK_FORMAT_UNDEFINED) continue;
        MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
        c.pixelFormat = f->mtl;
        if (cb && i < cb->attachmentCount) {
            const VkPipelineColorBlendAttachmentState *a = &cb->pAttachments[i];
            c.writeMask = ((a->colorWriteMask & 1) ? MTLColorWriteMaskRed : 0) | ((a->colorWriteMask & 2) ? MTLColorWriteMaskGreen : 0) |
                          ((a->colorWriteMask & 4) ? MTLColorWriteMaskBlue : 0) | ((a->colorWriteMask & 8) ? MTLColorWriteMaskAlpha : 0);
            if (a->blendEnable && f->numeric == VKF_FLOAT) {
                c.blendingEnabled = YES;
                c.sourceRGBBlendFactor = blend_factor(a->srcColorBlendFactor);
                c.destinationRGBBlendFactor = blend_factor(a->dstColorBlendFactor);
                c.rgbBlendOperation = blend_op(a->colorBlendOp);
                c.sourceAlphaBlendFactor = blend_factor(a->srcAlphaBlendFactor);
                c.destinationAlphaBlendFactor = blend_factor(a->dstAlphaBlendFactor);
                c.alphaBlendOperation = blend_op(a->alphaBlendOp);
            }
        }
    }
    if (cb) memcpy(p->blend_constants, cb->blendConstants, sizeof p->blend_constants);
    const vk_format_info *df = vk_format(af.depth), *sf = vk_format(af.stencil);
    if (df) d.depthAttachmentPixelFormat = df->mtl;
    if (sf) d.stencilAttachmentPixelFormat = sf->mtl;
    const VkPipelineMultisampleStateCreateInfo *ms = info->pMultisampleState;
    d.rasterSampleCount = af.samples ? af.samples : 1;
    if (ms) {
        d.alphaToCoverageEnabled = ms->alphaToCoverageEnable != 0;
        d.alphaToOneEnabled = ms->alphaToOneEnable != 0;
        p->sample_mask = ms->pSampleMask ? ms->pSampleMask[0] : 0xffffffffu;
    }
    d.rasterizationEnabled = !p->rasterizer_discard;
    const VkPipelineInputAssemblyStateCreateInfo *ia = info->pInputAssemblyState;
    int ok = 1;
    p->topology = ia ? ia->topology : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    p->primitive_restart = ia && ia->primitiveRestartEnable;
    if (gs || tess) p->primitive = MTLPrimitiveTypeTriangle;   // (the geometry kernel's strips, the tessellator's triangles)
    else {
        p->primitive = primitive(p->topology, &ok);
        if (!ok && vk_log_once("topology", ia->topology)) vx_log("vulkan: topology %d is not supported (logged once)", ia->topology);
    }
    if (vr.uses_layer || gr.uses_layer || (p->dynamic & VK_DYN_TOPOLOGY)) d.inputPrimitiveTopology = topology_class(p->primitive);
    NSError *e = nil;
    p->render = [dev->mtl newRenderPipelineStateWithDescriptor:d error:&e];
    if (!p->render) {
        vk_log_unique("vulkan: render pipeline failed: %s", e.localizedDescription.UTF8String);
        if (vk_trace()) { vx_log("%s", vr.msl); if (gr.msl) vx_log("%s", gr.msl); if (er.msl) vx_log("%s", er.msl); if (fr.msl) vx_log("%s", fr.msl); }
        vkmsl_result_free(&vr); vkmsl_result_free(&fr); vkmsl_result_free(&gr); vkmsl_result_free(&cr); vkmsl_result_free(&er);
        pipeline_free(p);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    vkmsl_result_free(&vr); vkmsl_result_free(&fr); vkmsl_result_free(&gr); vkmsl_result_free(&cr); vkmsl_result_free(&er);
    // Fixed state.
    if (rs) {
        p->cull = rs->cullMode == VK_CULL_MODE_FRONT_BIT ? MTLCullModeFront : rs->cullMode == VK_CULL_MODE_BACK_BIT ? MTLCullModeBack :
                  MTLCullModeNone;
        if (rs->cullMode == VK_CULL_MODE_FRONT_AND_BACK) p->rasterizer_discard = 1;
        p->winding = rs->frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE ? MTLWindingCounterClockwise : MTLWindingClockwise;
        p->fill = rs->polygonMode == VK_POLYGON_MODE_LINE ? MTLTriangleFillModeLines : MTLTriangleFillModeFill;
        p->depth_clip = rs->depthClampEnable ? MTLDepthClipModeClamp : MTLDepthClipModeClip;
        p->depth_bias_enable = rs->depthBiasEnable;
        p->depth_bias[0] = rs->depthBiasConstantFactor; p->depth_bias[1] = rs->depthBiasSlopeFactor; p->depth_bias[2] = rs->depthBiasClamp;
    }
    const VkPipelineViewportStateCreateInfo *vp = info->pViewportState;
    if (vp) {
        p->viewport_count = vp->viewportCount < 16 ? vp->viewportCount : 16;
        p->scissor_count = vp->scissorCount < 16 ? vp->scissorCount : 16;
        for (uint32_t i = 0; vp->pViewports && i < p->viewport_count; i++) p->viewports[i] = vp->pViewports[i];
        for (uint32_t i = 0; vp->pScissors && i < p->scissor_count; i++) p->scissors[i] = vp->pScissors[i];
    }
    if (info->pDepthStencilState) p->ds = *info->pDepthStencilState;
    p->ds.pNext = NULL;
    p->stencil_ref[0] = p->ds.front.reference;
    p->stencil_ref[1] = p->ds.back.reference;
    p->depth_stencil = vk_depth_stencil_state(dev, info->pDepthStencilState, df != NULL, sf != NULL);
    *out = p;
    return VK_SUCCESS;
}

VKFN VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *infos,
                                                   const VkAllocationCallbacks *alloc, VkPipeline *out) {
    (void)cache; (void)alloc;
    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = VK_NULL_HANDLE;
        @autoreleasepool {
            VkResult r = create_graphics(dev, &infos[i], &out[i]);
            if (r != VK_SUCCESS) result = r;
        }
    }
    return result;
}

VKFN VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice dev, VkPipelineCache cache, uint32_t n, const VkComputePipelineCreateInfo *infos,
                                                  const VkAllocationCallbacks *alloc, VkPipeline *out) {
    (void)cache; (void)alloc;
    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < n; i++) @autoreleasepool {
        out[i] = VK_NULL_HANDLE;
        const VkComputePipelineCreateInfo *info = &infos[i];
        struct VkPipeline_T *p = calloc(1, sizeof *p);
        if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
        p->bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
        if (info->layout) vk_pipeline_layout_copy(&p->layout, info->layout);
        VkResult err = VK_SUCCESS;
        vkmsl_result cr;
        id<MTLFunction> f = compile_stage(dev, &info->stage, VKMSL_STAGE_COMPUTE, &p->layout, 0, 0, &cr, NULL, &err, NULL);
        if (!f) { pipeline_free(p); result = err; continue; }
        p->threads = MTLSizeMake(cr.workgroup_size[0], cr.workgroup_size[1], cr.workgroup_size[2]);
        p->sets_compute = cr.sets_used;
        p->push_compute = cr.uses_push_constants;
        p->aux_compute = cr.uses_aux;
        vkmsl_result_free(&cr);
        NSError *e = nil;
        p->compute = [dev->mtl newComputePipelineStateWithFunction:f error:&e];
        if (!p->compute) {
            vk_log_unique("vulkan: compute pipeline failed: %s", e.localizedDescription.UTF8String);
            pipeline_free(p);
            result = VK_ERROR_INITIALIZATION_FAILED;
            continue;
        }
        out[i] = p;
    }
    return result;
}

VKFN void VKAPI_CALL vkDestroyPipeline(VkDevice dev, VkPipeline p, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    pipeline_free(p);
}

// ---- render passes -----------------------------------------------------------------------------------------------
static void subpass_from(vk_subpass *sp, const VkSubpassDescription2 *s, const VkAttachmentReference2 *depth_resolve,
                         VkResolveModeFlagBits dmode, VkResolveModeFlagBits smode) {
    memset(sp, 0, sizeof *sp);
    sp->color_count = s->colorAttachmentCount < 8 ? s->colorAttachmentCount : 8;
    for (uint32_t i = 0; i < sp->color_count; i++) {
        sp->colors[i] = s->pColorAttachments[i].attachment;
        sp->resolves[i] = s->pResolveAttachments ? s->pResolveAttachments[i].attachment : VK_ATTACHMENT_UNUSED;
    }
    sp->depth = s->pDepthStencilAttachment ? s->pDepthStencilAttachment->attachment : VK_ATTACHMENT_UNUSED;
    sp->depth_resolve = depth_resolve ? depth_resolve->attachment : VK_ATTACHMENT_UNUSED;
    sp->depth_resolve_mode = dmode;
    sp->stencil_resolve_mode = smode;
    sp->input_count = s->inputAttachmentCount < 8 ? s->inputAttachmentCount : 8;
    for (uint32_t i = 0; i < sp->input_count; i++) sp->inputs[i] = s->pInputAttachments[i].attachment;
    sp->view_mask = s->viewMask;
}

static void render_pass_uses(struct VkRenderPass_T *rp) {
    rp->first_use = malloc((rp->attachment_count ? rp->attachment_count : 1) * sizeof *rp->first_use);
    rp->last_use = malloc((rp->attachment_count ? rp->attachment_count : 1) * sizeof *rp->last_use);
    for (uint32_t a = 0; a < rp->attachment_count; a++) { rp->first_use[a] = UINT32_MAX; rp->last_use[a] = 0; }
    for (uint32_t s = 0; s < rp->subpass_count; s++) {
        const vk_subpass *sp = &rp->subpasses[s];
        uint32_t used[32], n = 0;
        for (uint32_t i = 0; i < sp->color_count; i++) { used[n++] = sp->colors[i]; used[n++] = sp->resolves[i]; }
        used[n++] = sp->depth;
        used[n++] = sp->depth_resolve;
        // (an input attachment read in a later subpass keeps what the earlier ones wrote: stored, whatever its storeOp)
        for (uint32_t i = 0; i < sp->input_count && i < 8; i++) used[n++] = sp->inputs[i];
        for (uint32_t i = 0; i < n; i++) {
            uint32_t a = used[i];
            if (a == VK_ATTACHMENT_UNUSED || a >= rp->attachment_count) continue;
            if (rp->first_use[a] == UINT32_MAX) rp->first_use[a] = s;
            rp->last_use[a] = s;
        }
    }
}

VKFN VkResult VKAPI_CALL vkCreateRenderPass2(VkDevice dev, const VkRenderPassCreateInfo2 *info, const VkAllocationCallbacks *alloc,
                                             VkRenderPass *out) {
    (void)dev; (void)alloc;
    struct VkRenderPass_T *rp = calloc(1, sizeof *rp);
    if (!rp) return VK_ERROR_OUT_OF_HOST_MEMORY;
    rp->attachment_count = info->attachmentCount;
    rp->attachments = calloc(info->attachmentCount ? info->attachmentCount : 1, sizeof *rp->attachments);
    memcpy(rp->attachments, info->pAttachments, info->attachmentCount * sizeof *rp->attachments);
    rp->subpass_count = info->subpassCount;
    rp->subpasses = calloc(info->subpassCount ? info->subpassCount : 1, sizeof *rp->subpasses);
    for (uint32_t i = 0; i < info->subpassCount; i++) {
        const VkSubpassDescriptionDepthStencilResolve *dr =
            vk_find_struct(info->pSubpasses[i].pNext, VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE);
        subpass_from(&rp->subpasses[i], &info->pSubpasses[i], dr ? dr->pDepthStencilResolveAttachment : NULL,
                     dr ? dr->depthResolveMode : 0, dr ? dr->stencilResolveMode : 0);
    }
    render_pass_uses(rp);
    *out = rp;
    return VK_SUCCESS;
}

VKFN VkResult VKAPI_CALL vkCreateRenderPass(VkDevice dev, const VkRenderPassCreateInfo *info, const VkAllocationCallbacks *alloc,
                                            VkRenderPass *out) {
    // (as the version 2 structures, which hold everything version 1 says)
    VkAttachmentDescription2 *atts = calloc(info->attachmentCount ? info->attachmentCount : 1, sizeof *atts);
    VkSubpassDescription2 *subs = calloc(info->subpassCount ? info->subpassCount : 1, sizeof *subs);
    VkAttachmentReference2 *refs = calloc(info->subpassCount * 26 + 1, sizeof *refs);
    for (uint32_t i = 0; i < info->attachmentCount; i++) {
        const VkAttachmentDescription *a = &info->pAttachments[i];
        atts[i] = (VkAttachmentDescription2){ VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, NULL, a->flags, a->format, a->samples,
            a->loadOp, a->storeOp, a->stencilLoadOp, a->stencilStoreOp, a->initialLayout, a->finalLayout };
    }
    uint32_t r = 0;
    for (uint32_t i = 0; i < info->subpassCount; i++) {
        const VkSubpassDescription *s = &info->pSubpasses[i];
        VkSubpassDescription2 *d = &subs[i];
        d->sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
        d->pipelineBindPoint = s->pipelineBindPoint;
        d->colorAttachmentCount = s->colorAttachmentCount < 8 ? s->colorAttachmentCount : 8;
        d->inputAttachmentCount = s->inputAttachmentCount < 8 ? s->inputAttachmentCount : 8;
        VkAttachmentReference2 *c = &refs[r]; r += 8;
        VkAttachmentReference2 *res = &refs[r]; r += 8;
        VkAttachmentReference2 *in = &refs[r]; r += 8;
        VkAttachmentReference2 *ds = &refs[r]; r += 2;
        for (uint32_t k = 0; k < d->colorAttachmentCount; k++) {
            c[k] = (VkAttachmentReference2){ VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL, s->pColorAttachments[k].attachment, s->pColorAttachments[k].layout, 0 };
            res[k] = (VkAttachmentReference2){ VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL,
                s->pResolveAttachments ? s->pResolveAttachments[k].attachment : VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED, 0 };
        }
        for (uint32_t k = 0; k < d->inputAttachmentCount; k++)
            in[k] = (VkAttachmentReference2){ VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL, s->pInputAttachments[k].attachment, s->pInputAttachments[k].layout, 0 };
        d->pColorAttachments = c;
        d->pResolveAttachments = s->pResolveAttachments ? res : NULL;
        d->pInputAttachments = in;
        if (s->pDepthStencilAttachment) {
            ds[0] = (VkAttachmentReference2){ VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL, s->pDepthStencilAttachment->attachment, s->pDepthStencilAttachment->layout, 0 };
            d->pDepthStencilAttachment = ds;
        }
    }
    const VkRenderPassMultiviewCreateInfo *mv = vk_find_struct(info->pNext, VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO);
    for (uint32_t i = 0; mv && i < mv->subpassCount && i < info->subpassCount; i++) subs[i].viewMask = mv->pViewMasks[i];
    VkRenderPassCreateInfo2 i2 = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2, NULL, info->flags, info->attachmentCount, atts,
                                   info->subpassCount, subs, 0, NULL, 0, NULL };
    VkResult res = vkCreateRenderPass2(dev, &i2, alloc, out);
    free(atts); free(subs); free(refs);
    return res;
}

VKFN void VKAPI_CALL vkDestroyRenderPass(VkDevice dev, VkRenderPass rp, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    if (!rp) return;
    free(rp->attachments); free(rp->subpasses); free(rp->first_use); free(rp->last_use);
    free(rp);
}
VKFN void VKAPI_CALL vkGetRenderAreaGranularity(VkDevice dev, VkRenderPass rp, VkExtent2D *g) {
    (void)dev; (void)rp;
    *g = (VkExtent2D){ 1, 1 };
}
VKFN void VKAPI_CALL vkGetRenderingAreaGranularityKHR(VkDevice dev, const VkRenderingAreaInfoKHR *info, VkExtent2D *g) {
    (void)dev; (void)info;
    *g = (VkExtent2D){ 1, 1 };
}

VKFN VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice dev, const VkFramebufferCreateInfo *info, const VkAllocationCallbacks *alloc,
                                             VkFramebuffer *out) {
    (void)dev; (void)alloc;
    struct VkFramebuffer_T *fb = calloc(1, sizeof *fb);
    if (!fb) return VK_ERROR_OUT_OF_HOST_MEMORY;
    fb->width = info->width;
    fb->height = info->height;
    fb->layers = info->layers;
    fb->attachment_count = info->attachmentCount;
    fb->views = calloc(info->attachmentCount ? info->attachmentCount : 1, sizeof *fb->views);
    if (!(info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && info->pAttachments)
        memcpy(fb->views, info->pAttachments, info->attachmentCount * sizeof *fb->views);
    *out = fb;
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkDestroyFramebuffer(VkDevice dev, VkFramebuffer fb, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    if (!fb) return;
    free(fb->views);
    free(fb);
}

#ifdef __aarch64__
#include "vx.h"   // (vx_low, vx_unlow_at for the thunks)
#include "thunks/vk_pipeline.m.inc"
#endif
