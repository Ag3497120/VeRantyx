// Descriptor sets as Metal 3 argument buffers: each set is a run of 8-byte words in its pool's buffer, written here
// with GPU addresses (buffers: address and size) and resource IDs (textures, samplers); the shaders vk_msl.c writes
// declare the same words. Copies between sets copy words.
#include "vk_impl.h"
#include <stdlib.h>
#include <string.h>

static int binding_order(const void *a, const void *b) {
    const vk_set_binding *x = a, *y = b;
    return x->binding < y->binding ? -1 : x->binding > y->binding;
}

static uint32_t binding_words(const vk_set_binding *b, uint32_t count) {
    return b->type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK ? (count + 7) / 8 : count * vkmsl_descriptor_words(b->type);
}

VKFN VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice dev, const VkDescriptorSetLayoutCreateInfo *info,
                                                     const VkAllocationCallbacks *alloc, VkDescriptorSetLayout *out) {
    (void)dev; (void)alloc;
    struct VkDescriptorSetLayout_T *l = calloc(1, sizeof *l);
    if (!l) return VK_ERROR_OUT_OF_HOST_MEMORY;
    l->refs = 1;
    l->flags = info->flags;
    l->binding_count = info->bindingCount;
    l->bindings = calloc(info->bindingCount ? info->bindingCount : 1, sizeof *l->bindings);
    l->variable_binding = -1;
    const VkDescriptorSetLayoutBindingFlagsCreateInfo *bf =
        vk_find_struct(info->pNext, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
    for (uint32_t i = 0; i < info->bindingCount; i++) {
        const VkDescriptorSetLayoutBinding *src = &info->pBindings[i];
        vk_set_binding *b = &l->bindings[i];
        b->binding = src->binding;
        b->type = src->descriptorType;
        b->count = src->descriptorCount;
        b->stages = src->stageFlags;
        b->flags = bf && i < bf->bindingCount ? bf->pBindingFlags[i] : 0;
        if (src->pImmutableSamplers && src->descriptorCount &&
            (b->type == VK_DESCRIPTOR_TYPE_SAMPLER || b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)) {
            b->immutable = malloc(src->descriptorCount * sizeof *b->immutable);
            memcpy(b->immutable, src->pImmutableSamplers, src->descriptorCount * sizeof *b->immutable);
        }
    }
    qsort(l->bindings, l->binding_count, sizeof *l->bindings, binding_order);
    uint32_t word = 0, dyn = 0;
    for (uint32_t i = 0; i < l->binding_count; i++) {
        vk_set_binding *b = &l->bindings[i];
        b->word = word;
        word += binding_words(b, b->count);
        b->dynamic = VKMSL_NO_DYNAMIC;
        if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
            b->dynamic = dyn;
            dyn += b->count;
        }
        if (b->flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) l->variable_binding = (int)i;
    }
    l->words = word;
    l->dynamic_count = dyn;
    *out = l;
    return VK_SUCCESS;
}

static void layout_release(VkDescriptorSetLayout l) {
    if (!l || --l->refs) return;
    for (uint32_t i = 0; i < l->binding_count; i++) free(l->bindings[i].immutable);
    free(l->bindings);
    free(l);
}
VKFN void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice dev, VkDescriptorSetLayout l, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    layout_release(l);
}

VKFN void VKAPI_CALL vkGetDescriptorSetLayoutSupport(VkDevice dev, const VkDescriptorSetLayoutCreateInfo *info,
                                                    VkDescriptorSetLayoutSupport *support) {
    (void)dev; (void)info;
    support->supported = VK_TRUE;
    VkDescriptorSetVariableDescriptorCountLayoutSupport *v =
        vk_find_struct_mut(support->pNext, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT);
    if (v) v->maxVariableDescriptorCount = 500000;
}

const vk_set_binding *vk_layout_binding(VkDescriptorSetLayout l, uint32_t binding);
const vk_set_binding *vk_layout_binding(VkDescriptorSetLayout l, uint32_t binding) {
    uint32_t lo = 0, hi = l->binding_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (l->bindings[mid].binding < binding) lo = mid + 1;
        else hi = mid;
    }
    return lo < l->binding_count && l->bindings[lo].binding == binding ? &l->bindings[lo] : NULL;
}

// ---- pipeline layouts --------------------------------------------------------------------------------------------
VKFN VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice dev, const VkPipelineLayoutCreateInfo *info,
                                                const VkAllocationCallbacks *alloc, VkPipelineLayout *out) {
    (void)dev; (void)alloc;
    if (info->setLayoutCount > VKMSL_MAX_SETS) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkPipelineLayout_T *l = calloc(1, sizeof *l);
    if (!l) return VK_ERROR_OUT_OF_HOST_MEMORY;
    l->set_count = info->setLayoutCount;
    for (uint32_t i = 0; i < info->setLayoutCount; i++) {
        l->sets[i] = info->pSetLayouts[i];
        if (l->sets[i]) l->sets[i]->refs++;
        l->dynamic_base[i] = l->dynamic_count;
        l->dynamic_count += l->sets[i] ? l->sets[i]->dynamic_count : 0;
    }
    for (uint32_t i = 0; i < info->pushConstantRangeCount; i++) {
        uint32_t end = info->pPushConstantRanges[i].offset + info->pPushConstantRanges[i].size;
        if (end > l->push_size) l->push_size = end;
        l->push_stages |= info->pPushConstantRanges[i].stageFlags;
    }
    *out = l;
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkDestroyPipelineLayout(VkDevice dev, VkPipelineLayout l, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    if (!l) return;
    for (uint32_t i = 0; i < l->set_count; i++) layout_release(l->sets[i]);
    free(l);
}
void vk_pipeline_layout_copy(struct VkPipelineLayout_T *dst, VkPipelineLayout src);
void vk_pipeline_layout_copy(struct VkPipelineLayout_T *dst, VkPipelineLayout src) {
    *dst = *src;
    for (uint32_t i = 0; i < dst->set_count; i++) if (dst->sets[i]) dst->sets[i]->refs++;
}
void vk_pipeline_layout_release_copy(struct VkPipelineLayout_T *l);
void vk_pipeline_layout_release_copy(struct VkPipelineLayout_T *l) {
    for (uint32_t i = 0; i < l->set_count; i++) layout_release(l->sets[i]);
    l->set_count = 0;
}

// The translator's view of a pipeline layout: every binding of every set, with its words.
uint32_t vk_layout_bindings(const struct VkPipelineLayout_T *l, vkmsl_binding *out, uint32_t cap);
uint32_t vk_layout_bindings(const struct VkPipelineLayout_T *l, vkmsl_binding *out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t s = 0; s < l->set_count; s++) {
        VkDescriptorSetLayout sl = l->sets[s];
        if (!sl) continue;
        for (uint32_t i = 0; i < sl->binding_count && n < cap; i++) {
            const vk_set_binding *b = &sl->bindings[i];
            out[n++] = (vkmsl_binding){ s, b->binding, (uint32_t)b->type, b->count, b->word,
                                        b->dynamic == VKMSL_NO_DYNAMIC ? VKMSL_NO_DYNAMIC : l->dynamic_base[s] + b->dynamic };
        }
    }
    return n;
}

// ---- pools and sets ----------------------------------------------------------------------------------------------
VKFN VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice dev, const VkDescriptorPoolCreateInfo *info,
                                                const VkAllocationCallbacks *alloc, VkDescriptorPool *out) {
    (void)alloc;
    uint64_t words = 0;
    for (uint32_t i = 0; i < info->poolSizeCount; i++) {
        const VkDescriptorPoolSize *ps = &info->pPoolSizes[i];
        words += ps->type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK ? (ps->descriptorCount + 7) / 8 :
                 (uint64_t)ps->descriptorCount * vkmsl_descriptor_words(ps->type);
    }
    // (each set is aligned to 16 words; generous room for that)
    uint64_t size = (words + (uint64_t)info->maxSets * 16 + 16) * 8;
    struct VkDescriptorPool_T *p = calloc(1, sizeof *p);
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->buffer = [dev->mtl newBufferWithLength:size options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];
    if (!p->buffer) { free(p); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    p->size = size;
    p->flags = info->flags;
    pthread_mutex_init(&p->lock, NULL);
    *out = p;
    return VK_SUCCESS;
}

static void set_free(struct VkDescriptorSet_T *s) {
    if (!s) return;
    layout_release(s->layout);
    free(s);
}

VKFN VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice dev, VkDescriptorPool p, VkDescriptorPoolResetFlags flags) {
    (void)dev; (void)flags;
    pthread_mutex_lock(&p->lock);
    for (uint32_t i = 0; i < p->set_count; i++) set_free(p->sets[i]);
    p->set_count = 0;
    p->used = 0;
    p->free_count = 0;
    pthread_mutex_unlock(&p->lock);
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkDestroyDescriptorPool(VkDevice dev, VkDescriptorPool p, const VkAllocationCallbacks *alloc) {
    (void)alloc;
    if (!p) return;
    vkResetDescriptorPool(dev, p, 0);
    free(p->sets);
    free(p->free);
    p->buffer = nil;
    pthread_mutex_destroy(&p->lock);
    free(p);
}

static int pool_take(VkDescriptorPool p, uint64_t size, uint64_t *offset) {
    for (uint32_t i = 0; i < p->free_count; i++) {
        if (p->free[i].size < size) continue;
        *offset = p->free[i].offset;
        p->free[i].offset += size;
        p->free[i].size -= size;
        if (!p->free[i].size) p->free[i] = p->free[--p->free_count];
        return 1;
    }
    if (p->used + size > p->size) return 0;
    *offset = p->used;
    p->used += size;
    return 1;
}
static void pool_give(VkDescriptorPool p, uint64_t offset, uint64_t size) {
    if (!size) return;
    if (offset + size == p->used) { p->used = offset; return; }
    for (uint32_t i = 0; i < p->free_count; i++) {
        if (p->free[i].offset + p->free[i].size == offset) { p->free[i].size += size; return; }
        if (offset + size == p->free[i].offset) { p->free[i].offset = offset; p->free[i].size += size; return; }
    }
    if (p->free_count == p->free_cap) {
        p->free_cap = p->free_cap ? p->free_cap * 2 : 64;
        p->free = realloc(p->free, p->free_cap * sizeof *p->free);
    }
    p->free[p->free_count++] = (struct vk_free_range){ offset, size };
}

static void write_word(struct VkDescriptorSet_T *s, uint32_t w, uint64_t v) {
    if ((uint64_t)w * 8 < s->size) s->words[w] = v;
}

VKFN VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice dev, const VkDescriptorSetAllocateInfo *info, VkDescriptorSet *out) {
    (void)dev;
    VkDescriptorPool p = info->descriptorPool;
    const VkDescriptorSetVariableDescriptorCountAllocateInfo *var =
        vk_find_struct(info->pNext, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO);
    for (uint32_t i = 0; i < info->descriptorSetCount; i++) out[i] = VK_NULL_HANDLE;
    pthread_mutex_lock(&p->lock);
    for (uint32_t i = 0; i < info->descriptorSetCount; i++) {
        VkDescriptorSetLayout l = info->pSetLayouts[i];
        uint32_t words = l->words;
        uint32_t variable = 0;
        if (l->variable_binding >= 0) {
            const vk_set_binding *vb = &l->bindings[l->variable_binding];
            variable = var && i < var->descriptorSetCount ? var->pDescriptorCounts[i] : 0;
            // (a combined image sampler binding keeps its samplers after all its textures: at the most)
            words = vb->word + binding_words(vb, vb->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? vb->count : variable);
        }
        uint64_t size = ((uint64_t)words * 8 + 127) & ~127ull;
        if (!size) size = 128;
        uint64_t offset;
        if (!pool_take(p, size, &offset)) {
            pthread_mutex_unlock(&p->lock);
            for (uint32_t k = 0; k < i; k++) { /* (freed with the pool) */ }
            return VK_ERROR_OUT_OF_POOL_MEMORY;
        }
        struct VkDescriptorSet_T *s = calloc(1, sizeof *s);
        s->pool = p;
        s->layout = l;
        l->refs++;
        s->offset = offset;
        s->size = size;
        s->words = (uint64_t *)((uint8_t *)p->buffer.contents + offset);
        s->variable_count = variable;
        memset(s->words, 0, size);
        // Immutable samplers are part of the layout: written once here.
        for (uint32_t b = 0; b < l->binding_count; b++) {
            const vk_set_binding *lb = &l->bindings[b];
            if (!lb->immutable) continue;
            uint32_t base = lb->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? lb->word + lb->count : lb->word;
            for (uint32_t k = 0; k < lb->count; k++)
                if (lb->immutable[k]) write_word(s, base + k, lb->immutable[k]->mtl.gpuResourceID._impl);
        }
        if (p->set_count == p->set_cap) {
            p->set_cap = p->set_cap ? p->set_cap * 2 : 64;
            p->sets = realloc(p->sets, p->set_cap * sizeof *p->sets);
        }
        p->sets[p->set_count++] = s;
        out[i] = s;
    }
    pthread_mutex_unlock(&p->lock);
    return VK_SUCCESS;
}

VKFN VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice dev, VkDescriptorPool p, uint32_t n, const VkDescriptorSet *sets) {
    (void)dev;
    pthread_mutex_lock(&p->lock);
    for (uint32_t i = 0; i < n; i++) {
        struct VkDescriptorSet_T *s = sets[i];
        if (!s) continue;
        for (uint32_t k = 0; k < p->set_count; k++)
            if (p->sets[k] == s) { p->sets[k] = p->sets[--p->set_count]; break; }
        pool_give(p, s->offset, s->size);
        set_free(s);
    }
    pthread_mutex_unlock(&p->lock);
    return VK_SUCCESS;
}

// One descriptor into a set: binding b's element `element` (which may run past the binding into the next ones, as
// Vulkan's consecutive-binding updates do; the caller steps).
static void write_one(struct VkDescriptorSet_T *s, const vk_set_binding *b, uint32_t element, VkDescriptorType type,
                      const VkDescriptorImageInfo *ii, const VkDescriptorBufferInfo *bi, VkBufferView tv) {
    uint32_t w = b->word;
    switch (type) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
        VkBuffer buf = bi->buffer;
        uint64_t addr = buf ? buf->gpu + bi->offset : 0;
        uint64_t range = !buf ? 0 : bi->range == VK_WHOLE_SIZE ? buf->size - bi->offset : bi->range;
        write_word(s, w + 2 * element, addr);
        write_word(s, w + 2 * element + 1, range);
        break;
    }
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        write_word(s, w + element, ii->imageView && ii->imageView->mtl ? ii->imageView->mtl.gpuResourceID._impl : 0);
        break;
    case VK_DESCRIPTOR_TYPE_SAMPLER:
        if (!b->immutable) write_word(s, w + element, ii->sampler ? ii->sampler->mtl.gpuResourceID._impl : 0);
        break;
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        write_word(s, w + element, ii->imageView && ii->imageView->mtl ? ii->imageView->mtl.gpuResourceID._impl : 0);
        if (!b->immutable) write_word(s, w + b->count + element, ii->sampler ? ii->sampler->mtl.gpuResourceID._impl : 0);
        break;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        write_word(s, w + element, tv && tv->mtl ? tv->mtl.gpuResourceID._impl : 0);
        break;
    default:
        break;
    }
}

// Walks `count` descriptors from (binding, element), across bindings as Vulkan does.
typedef struct { struct VkDescriptorSet_T *set; uint32_t index; uint32_t element; } walk;
static const vk_set_binding *walk_start(walk *wk, struct VkDescriptorSet_T *s, uint32_t binding, uint32_t element) {
    wk->set = s;
    wk->element = element;
    for (uint32_t i = 0; i < s->layout->binding_count; i++)
        if (s->layout->bindings[i].binding == binding) { wk->index = i; return &s->layout->bindings[i]; }
    wk->index = s->layout->binding_count;
    return NULL;
}
static const vk_set_binding *walk_next(walk *wk) {
    VkDescriptorSetLayout l = wk->set->layout;
    if (wk->index >= l->binding_count) return NULL;
    wk->element++;
    while (wk->index < l->binding_count) {
        const vk_set_binding *b = &l->bindings[wk->index];
        uint32_t count = (int)wk->index == l->variable_binding ? wk->set->variable_count : b->count;
        if (wk->element < count) return b;
        wk->index++;
        wk->element = 0;
    }
    return NULL;
}

VKFN void VKAPI_CALL vkUpdateDescriptorSets(VkDevice dev, uint32_t write_count, const VkWriteDescriptorSet *writes,
                                            uint32_t copy_count, const VkCopyDescriptorSet *copies) {
    (void)dev;
    for (uint32_t i = 0; i < write_count; i++) {
        const VkWriteDescriptorSet *wr = &writes[i];
        struct VkDescriptorSet_T *s = wr->dstSet;
        if (!s) continue;
        if (wr->descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
            const VkWriteDescriptorSetInlineUniformBlock *ib = vk_find_struct(wr->pNext, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK);
            const vk_set_binding *b = vk_layout_binding(s->layout, wr->dstBinding);
            if (ib && b) memcpy((uint8_t *)s->words + b->word * 8 + wr->dstArrayElement, ib->pData, ib->dataSize);
            continue;
        }
        walk wk;
        const vk_set_binding *b = walk_start(&wk, s, wr->dstBinding, wr->dstArrayElement);
        for (uint32_t k = 0; k < wr->descriptorCount && b; k++, b = walk_next(&wk))
            write_one(s, b, wk.element, wr->descriptorType, wr->pImageInfo ? &wr->pImageInfo[k] : NULL,
                      wr->pBufferInfo ? &wr->pBufferInfo[k] : NULL, wr->pTexelBufferView ? wr->pTexelBufferView[k] : NULL);
    }
    for (uint32_t i = 0; i < copy_count; i++) {
        const VkCopyDescriptorSet *c = &copies[i];
        struct VkDescriptorSet_T *src = c->srcSet, *dst = c->dstSet;
        walk ws, wd;
        const vk_set_binding *bs = walk_start(&ws, src, c->srcBinding, c->srcArrayElement);
        const vk_set_binding *bd = walk_start(&wd, dst, c->dstBinding, c->dstArrayElement);
        for (uint32_t k = 0; k < c->descriptorCount && bs && bd; k++, bs = walk_next(&ws), bd = walk_next(&wd)) {
            uint32_t per = vkmsl_descriptor_words(bs->type);
            if (bs->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                write_word(dst, bd->word + wd.element, src->words[bs->word + ws.element]);
                if (!bd->immutable) write_word(dst, bd->word + bd->count + wd.element, src->words[bs->word + bs->count + ws.element]);
            } else for (uint32_t w = 0; w < per; w++)
                write_word(dst, bd->word + wd.element * per + w, src->words[bs->word + ws.element * per + w]);
        }
    }
}

// ---- update templates --------------------------------------------------------------------------------------------
VKFN VkResult VKAPI_CALL vkCreateDescriptorUpdateTemplate(VkDevice dev, const VkDescriptorUpdateTemplateCreateInfo *info,
                                                          const VkAllocationCallbacks *alloc, VkDescriptorUpdateTemplate *out) {
    (void)dev; (void)alloc;
    struct VkDescriptorUpdateTemplate_T *t = calloc(1, sizeof *t);
    if (!t) return VK_ERROR_OUT_OF_HOST_MEMORY;
    t->count = info->descriptorUpdateEntryCount;
    t->entries = malloc((t->count ? t->count : 1) * sizeof *t->entries);
    memcpy(t->entries, info->pDescriptorUpdateEntries, t->count * sizeof *t->entries);
    t->layout = info->descriptorSetLayout;
    *out = t;
    return VK_SUCCESS;
}
VKFN void VKAPI_CALL vkDestroyDescriptorUpdateTemplate(VkDevice dev, VkDescriptorUpdateTemplate t, const VkAllocationCallbacks *alloc) {
    (void)dev; (void)alloc;
    if (!t) return;
    free(t->entries);
    free(t);
}
void vk_apply_template(struct VkDescriptorSet_T *s, VkDescriptorUpdateTemplate t, const void *data);
void vk_apply_template(struct VkDescriptorSet_T *s, VkDescriptorUpdateTemplate t, const void *data) {
    for (uint32_t i = 0; i < t->count; i++) {
        const VkDescriptorUpdateTemplateEntry *e = &t->entries[i];
        const uint8_t *p = (const uint8_t *)data + e->offset;
        if (e->descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
            const vk_set_binding *b = vk_layout_binding(s->layout, e->dstBinding);
            if (b) memcpy((uint8_t *)s->words + b->word * 8 + e->dstArrayElement, p, e->descriptorCount);
            continue;
        }
        walk wk;
        const vk_set_binding *b = walk_start(&wk, s, e->dstBinding, e->dstArrayElement);
        for (uint32_t k = 0; k < e->descriptorCount && b; k++, b = walk_next(&wk), p += e->stride)
            write_one(s, b, wk.element, e->descriptorType, (const VkDescriptorImageInfo *)p, (const VkDescriptorBufferInfo *)p,
                      *(const VkBufferView *)p);
    }
}
VKFN void VKAPI_CALL vkUpdateDescriptorSetWithTemplate(VkDevice dev, VkDescriptorSet s, VkDescriptorUpdateTemplate t, const void *data) {
    (void)dev;
    vk_apply_template(s, t, data);
}

// Push descriptors write into a set that lives as long as the command buffer's recording (vk_command.m keeps it).
struct VkDescriptorSet_T *vk_push_set_new(VkDescriptorSetLayout l, uint64_t *words, uint64_t size);
struct VkDescriptorSet_T *vk_push_set_new(VkDescriptorSetLayout l, uint64_t *words, uint64_t size) {
    struct VkDescriptorSet_T *s = calloc(1, sizeof *s);
    s->layout = l;
    l->refs++;
    s->words = words;
    s->size = size;
    return s;
}
void vk_push_set_free(struct VkDescriptorSet_T *s);
void vk_push_set_free(struct VkDescriptorSet_T *s) { set_free(s); }
void vk_push_set_write(struct VkDescriptorSet_T *s, uint32_t n, const VkWriteDescriptorSet *writes);
void vk_push_set_write(struct VkDescriptorSet_T *s, uint32_t n, const VkWriteDescriptorSet *writes) {
    for (uint32_t i = 0; i < n; i++) {
        const VkWriteDescriptorSet *wr = &writes[i];
        walk wk;
        const vk_set_binding *b = walk_start(&wk, s, wr->dstBinding, wr->dstArrayElement);
        for (uint32_t k = 0; k < wr->descriptorCount && b; k++, b = walk_next(&wk))
            write_one(s, b, wk.element, wr->descriptorType, wr->pImageInfo ? &wr->pImageInfo[k] : NULL,
                      wr->pBufferInfo ? &wr->pBufferInfo[k] : NULL, wr->pTexelBufferView ? wr->pTexelBufferView[k] : NULL);
    }
}

#ifdef __aarch64__
#include "thunks/vk_descriptor.m.inc"
#endif
