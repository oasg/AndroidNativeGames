/* Module: pipeline_reflected_2. */
#include "gfx_internal.h"

int reflect_write_buffer(GfxReflectedPipeline *p, uint32_t set, uint32_t binding,
                                uint32_t offset, const void *data, size_t size, int storage) {
    if (!p || !data || !size) return 0;
    pthread_mutex_lock(&g_lock);
    int ok = reflect_write_buffer_locked(p, set, binding, offset, data, size, storage);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int gfx_backend_reflected_write_uniform(GfxReflectedPipeline *p, uint32_t set,
                                        uint32_t binding, uint32_t offset,
                                        const void *data, size_t size) {
    return reflect_write_buffer(p, set, binding, offset, data, size, 0);
}

int gfx_backend_reflected_write_storage(GfxReflectedPipeline *p, uint32_t set,
                                        uint32_t binding, uint32_t offset,
                                        const void *data, size_t size) {
    return reflect_write_buffer(p, set, binding, offset, data, size, 1);
}

int reflect_set_for_stage(uint32_t stage) {
    return stage == 0 ? 0 : stage == 1 ? 1 : -1;
}

/* Find a reflected binding by the NVN stage/slot the guest bound.  The
 * .res.json slot is authoritative; a missing slot (-1) falls back to the
 * binding number used by the built-in test shaders. */
int reflect_find_slot_locked(GfxReflectedPipeline *p, int set, uint32_t slot,
                                    int want_texture) {
    if (set < 0 || set > 1) return -1;
    int fallback = -1;
    for (uint32_t i = 0; i < p->nspec[set]; i++) {
        const GfxBindingSpec *s = &p->spec[set][i];
        int texture_kind = s->kind == GFX_BIND_COMBINED_IMAGE_SAMPLER ||
                           s->kind == GFX_BIND_SAMPLED_IMAGE;
        if (want_texture ? !texture_kind : s->kind != GFX_BIND_UBO) continue;
        if (s->nvn_slot == (int32_t)slot) return (int)i;
        if (fallback < 0 && (s->nvn_slot < 0 || s->nvn_slot == 0) && s->binding == slot)
            fallback = (int)i;
    }
    return fallback;
}

/* The guest binds the whole NVN range, which is often larger than the
 * UBO/SSBO allocation the reflection derived from the shader.  Copy what fits
 * and report the truncation instead of dropping the binding entirely. */
int reflect_write_slot_locked(GfxReflectedPipeline *p, int set, int i,
                                     const void *data, size_t size, int storage) {
    if (i < 0) return 0;
    int32_t idx = p->bind_buf[set][i];
    if (idx < 0) return 0;
    size_t cap = (size_t)p->buf_size[idx];
    if (size > cap) {
        static unsigned truncated;
        if (truncated++ < 8)
            vk_log("reflected slot copy truncated binding=%u size=%zu cap=%zu",
                   p->spec[set][i].binding, size, cap);
        size = cap;
    }
    if (!size) return 0;
    return reflect_write_buffer_locked(p, set, p->spec[set][i].binding, 0, data, size, storage);
}

int gfx_backend_reflected_bind_ubo_slot(GfxReflectedPipeline *p, uint32_t stage,
                                        uint32_t slot, const void *data, size_t size) {
    if (!p || !data || !size) return 0;
    int set = reflect_set_for_stage(stage);
    if (set < 0) return 0;
    pthread_mutex_lock(&g_lock);
    int i = reflect_find_slot_locked(p, set, slot, 0);
    int ok = reflect_write_slot_locked(p, set, i, data, size, 0);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int gfx_backend_reflected_bind_storage_slot(GfxReflectedPipeline *p, uint32_t stage,
                                            uint32_t slot, const void *data, size_t size) {
    if (!p || !data || !size) return 0;
    int set = reflect_set_for_stage(stage);
    if (set < 0) return 0;
    pthread_mutex_lock(&g_lock);
    int found = -1;
    for (uint32_t i = 0; i < p->nspec[set]; i++) {
        if (p->spec[set][i].kind != GFX_BIND_SSBO) continue;
        if (p->spec[set][i].nvn_slot == (int32_t)slot) { found = (int)i; break; }
    }
    int ok = reflect_write_slot_locked(p, set, found, data, size, 1);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int gfx_backend_reflected_bind_texture_slot(GfxReflectedPipeline *p, uint32_t stage,
                                            uint32_t slot, const GfxTexture *texture) {
    if (!p || !texture) return 0;
    int set = reflect_set_for_stage(stage);
    if (set < 0) return 0;
    pthread_mutex_lock(&g_lock);
    int i = reflect_find_slot_locked(p, set, slot, 1);
    uint32_t binding = i >= 0 ? p->spec[set][i].binding : 0;
    pthread_mutex_unlock(&g_lock);
    if (i < 0) return 0;
    return gfx_backend_reflected_bind_texture(p, (uint32_t)set, binding, texture);
}

GfxReflectedPipeline *gfx_backend_reflected_builtin_texture_pipeline_create(void) {
    GfxBindingSpec ps = {
        .set = 1, .binding = 0, .kind = GFX_BIND_COMBINED_IMAGE_SAMPLER, .count = 1,
    };
    return gfx_backend_reflected_pipeline_create(
        (const uint8_t *)triangle_tex_vert_spv, triangle_tex_vert_spv_size,
        (const uint8_t *)triangle_tex_frag_spv, triangle_tex_frag_spv_size,
        gfx_backend_vk_format_for_nvn(0x25), NULL, 0, &ps, 1);
}

VkPrimitiveTopology vk_reflect_topology(uint32_t primitive) {
    /* NVNdrawPrimitive (matches the macOS NVN mapping): 0 points, 1 lines,
     * 3 line strip, 4 triangles, 5 triangle strip, 7 quads (triangle list).
     * The reflected pipeline is created with TRIANGLE_LIST, which covers the
     * first real frames; non-triangle types still draw with that topology and
     * are logged by the caller. */
    switch (primitive) {
    case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case 4:
    case 7:
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

int vk_reflect_ensure_buffer(VkBuffer *buf, VkDeviceMemory *mem, size_t *cap,
                                    size_t need, VkBufferUsageFlags usage) {
    if (need < 1) need = 1;
    if (*buf && *cap >= need) return 1;
    if (*buf) vkDestroyBuffer(g.dev, *buf, NULL);
    if (*mem) vkFreeMemory(g.dev, *mem, NULL);
    *buf = VK_NULL_HANDLE;
    *mem = VK_NULL_HANDLE;
    size_t size = (need + 255) & ~(size_t)255;
    if (!vk_make_buffer_usage((uint32_t)size, usage, buf, mem)) return 0;
    *cap = size;
    return 1;
}

int vk_reflect_upload(VkDeviceMemory mem, size_t offset, const void *data, size_t size) {
    if (!data || !size) return 0;
    void *mapped = NULL;
    if (!vk_ok(vkMapMemory(g.dev, mem, offset, size, 0, &mapped), "vkMapMemory(reflected stream)"))
        return 0;
    memcpy(mapped, data, size);
    vkUnmapMemory(g.dev, mem);
    return 1;
}

VkIndexType vk_reflect_index_type(uint32_t type) {
    /* NVN: 0 u8, 1 u16, 2 u32.  u8 is widened to u16 because the base Vulkan
     * 1.0 path does not require VK_EXT_index_type_uint8. */
    return type == 2 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
}

/* ---- per-draw descriptor epochs ---------------------------------------- */

size_t reflect_align(size_t v) { return (v + REFLECT_ALIGN - 1) & ~(size_t)(REFLECT_ALIGN - 1); }

/* Find the reflected binding for an NVN stage/slot.  The .res.json nvn_slot is
 * authoritative; the built-in test shaders fall back to binding == slot. */
int reflect_find_bind_spec(GfxReflectedPipeline *p, uint32_t stage, uint32_t slot,
                                  uint32_t kind) {
    int set = stage == 0 ? 0 : stage == 1 ? 1 : -1;
    if (set < 0) return -1;
    int fallback = -1;
    for (uint32_t i = 0; i < p->nspec[set]; i++) {
        const GfxBindingSpec *sp = &p->spec[set][i];
        int match = kind == GFX_DESC_UBO ? sp->kind == GFX_BIND_UBO
                  : kind == GFX_DESC_SSBO ? sp->kind == GFX_BIND_SSBO
                  : (sp->kind == GFX_BIND_COMBINED_IMAGE_SAMPLER ||
                     sp->kind == GFX_BIND_SAMPLED_IMAGE);
        if (!match) continue;
        if (sp->nvn_slot == (int32_t)slot) return (int)i;
        if (fallback < 0 && (sp->nvn_slot <= 0) && sp->binding == slot) fallback = (int)i;
    }
    return fallback;
}

int reflect_arena_ensure(GfxReflectedPipeline *p, int set, int idx, size_t need) {
    if (need < REFLECT_ALIGN) need = REFLECT_ALIGN;
    if (!vk_reflect_ensure_buffer(&p->dyn_buf[set][idx], &p->dyn_mem[set][idx],
                                  &p->dyn_cap[set][idx], need,
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        vk_log("ERROR reflected arena set=%d idx=%d size=%zu", set, idx, need);
        return 0;
    }
    return 1;
}

void reflect_write_epoch_set(GfxReflectedPipeline *p, uint32_t s, VkDescriptorSet dset,
                                    const ReflectEpochState *st) {
    VkDescriptorImageInfo ii[GFX_REFLECT_MAX_BINDINGS];
    for (uint32_t i = 0; i < p->nspec[s]; i++) {
        const GfxBindingSpec *sp = &p->spec[s][i];
        uint32_t count = sp->count ? sp->count : GFX_REFLECT_RUNTIME_ARRAY;
        if (count > GFX_REFLECT_MAX_BINDINGS) count = GFX_REFLECT_MAX_BINDINGS;
        VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = dset, .dstBinding = sp->binding, .descriptorCount = count,
            .descriptorType = reflect_vk_type(sp->kind) };
        if (sp->kind == GFX_BIND_UBO || sp->kind == GFX_BIND_SSBO) {
            int use_dyn = st->dyn[s][i] && p->dyn_buf[s][i];
            int32_t b = use_dyn ? (int32_t)i : p->bind_buf[s][i];
            if (b < 0) continue;
            VkBuffer buffer = use_dyn ? p->dyn_buf[s][i] : p->buf[b];
            VkDeviceSize range = use_dyn ? (VkDeviceSize)p->dyn_stride[s][i] : VK_WHOLE_SIZE;
            VkDescriptorBufferInfo bi = { .buffer = buffer, .offset = 0, .range = range };
            w.descriptorCount = 1;
            w.pBufferInfo = &bi;
            vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
        } else if (sp->kind == GFX_BIND_SAMPLER) {
            int32_t si = p->bind_sampler[s][i];
            VkSampler sampler = si >= 0 ? p->sampler[si] : VK_NULL_HANDLE;
            for (uint32_t k = 0; k < count; k++)
                ii[k] = (VkDescriptorImageInfo){ .sampler = sampler, .imageView = VK_NULL_HANDLE,
                                                 .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED };
            w.pImageInfo = ii;
            vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
        } else if (sp->kind == GFX_BIND_SAMPLED_IMAGE ||
                   sp->kind == GFX_BIND_COMBINED_IMAGE_SAMPLER) {
            const GfxTexture *t = st->tex[s][i];
            if (!t && p->bind_tex[s][i] >= 0) t = p->tex[p->bind_tex[s][i]];
            if (!t) continue;
            VkSampler sampler = VK_NULL_HANDLE;
            if (sp->kind == GFX_BIND_COMBINED_IMAGE_SAMPLER) {
                sampler = st->samp[s][i] ? st->samp[s][i]->sampler : t->sampler;
            }
            for (uint32_t k = 0; k < count; k++)
                ii[k] = (VkDescriptorImageInfo){ .sampler = sampler, .imageView = t->view,
                                                 .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            w.pImageInfo = ii;
            vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
        } else if (sp->kind == GFX_BIND_STORAGE_IMAGE) {
            int32_t ii_idx = p->bind_img[s][i];
            if (ii_idx < 0) continue;
            for (uint32_t k = 0; k < count; k++)
                ii[k] = (VkDescriptorImageInfo){ .sampler = VK_NULL_HANDLE,
                                                 .imageView = p->img_view[ii_idx],
                                                 .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
            w.pImageInfo = ii;
            vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
        } else {
            int32_t vi = p->bind_view[s][i];
            if (vi < 0) continue;
            VkBufferView views[GFX_REFLECT_MAX_BINDINGS];
            for (uint32_t k = 0; k < count; k++) views[k] = p->buf_view[vi];
            w.pTexelBufferView = views;
            vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
        }
    }
}

void reflect_apply_draw_state(VkCommandBuffer cmd, GfxReflectedPipeline *p,
                                     const GfxPipelineState *st) {
    VkPipeline pipe = reflect_pipeline_for_state(p, st);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe ? pipe : p->pipe);
    float zero[4] = { 0, 0, 0, 0 };
    vkCmdSetBlendConstants(cmd, st ? st->blend_color : zero);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, st ? st->stencil_ref : 0);
}

void reflect_bind_epoch_sets(VkCommandBuffer cmd, GfxReflectedPipeline *p,
                                    const VkDescriptorSet sets[2],
                                    const ReflectEpochState *st) {
    uint32_t offsets[2 * GFX_REFLECT_MAX_BINDINGS];
    uint32_t n = 0;
    for (uint32_t s = 0; s < 2; s++)
        for (uint32_t i = 0; i < p->nspec[s]; i++) {
            if (p->spec[s][i].kind != GFX_BIND_UBO && p->spec[s][i].kind != GFX_BIND_SSBO)
                continue;
            size_t off = p->spec[s][i].kind == GFX_BIND_UBO ? st->ubo_off[s][i]
                                                           : st->ssbo_off[s][i];
            offsets[n++] = (uint32_t)off;
        }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 2, sets,
                            n, n ? offsets : NULL);
}

int gfx_backend_reflected_execute(GfxReflectedPipeline *p, GfxOffscreenTarget *t,
                                  const GfxCmd *cmds, uint32_t count,
                                  const float default_clear[4]) {
    return gfx_backend_reflected_execute_ex(p, t, cmds, count, default_clear, NULL, 0);
}

int reflect_layout_matches_target(const GfxRenderTargetLayout *l,
                                         const GfxOffscreenTarget *t) {
    if (!l || !t || l->color_count != t->rt_layout.color_count) return 0;
    for (uint32_t i = 0; i < l->color_count; i++)
        if (l->color_formats[i] != (int)t->color_format[i]) return 0;
    if (l->depth_format != (int)t->depth_format) return 0;
    return 1;
}
