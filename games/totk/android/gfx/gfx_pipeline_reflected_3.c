/* Module: pipeline_reflected_3. */
#include "gfx_internal.h"

int gfx_backend_reflected_execute_ex(GfxReflectedPipeline *p, GfxOffscreenTarget *t,
                                     const GfxCmd *cmds, uint32_t count,
                                     const float default_clear[4],
                                     const GfxVertexStream *streams, uint32_t nstreams) {
    if (!p || !t) return 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready || !reflect_layout_matches_target(&p->rt_layout, t)) {
        vk_log("ERROR reflected execute: target layout does not match pipeline "
               "(colors=%u/%u depth=%d/%d)",
               t->rt_layout.color_count, p->rt_layout.color_count,
               (int)t->depth_format, p->rt_layout.depth_format);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    uint32_t ncolor = p->rt_layout.color_count;
    int has_depth = p->rt_layout.depth_format != 0;
    uint32_t attach_count = ncolor + (has_depth ? 1u : 0u);
    VkImageView views[GFX_MAX_COLOR_TARGETS + 1];
    for (uint32_t i = 0; i < ncolor; i++) views[i] = t->color_view[i];
    if (has_depth) views[ncolor] = t->depth_view;
    int fb_dirty = !p->fb || p->fb_w != t->width || p->fb_h != t->height ||
                   p->fb_attach_count != attach_count;
    for (uint32_t i = 0; !fb_dirty && i < attach_count; i++)
        if (p->fb_views[i] != views[i]) fb_dirty = 1;
    if (fb_dirty) {
        if (p->fb) vkDestroyFramebuffer(g.dev, p->fb, NULL);
        p->fb = VK_NULL_HANDLE;
        VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = p->pass, .attachmentCount = attach_count, .pAttachments = views,
            .width = t->width, .height = t->height, .layers = 1 };
        if (!vk_ok(vkCreateFramebuffer(g.dev, &fci, NULL, &p->fb), "vkCreateFramebuffer(reflected)")) {
            pthread_mutex_unlock(&g_lock); return 0;
        }
        memcpy(p->fb_views, views, attach_count * sizeof views[0]);
        p->fb_attach_count = attach_count;
        p->fb_view = views[0]; p->fb_w = t->width; p->fb_h = t->height;
    }

    /* Pre-scan descriptor binds: one arena slice per change, and one set pair
     * per epoch.  UBO/SSBO slices are selected by dynamic offsets, so two
     * draws in the same command buffer can see different constant data. */
    uint32_t changes[2][GFX_REFLECT_MAX_BINDINGS];
    memset(changes, 0, sizeof changes);
    uint32_t nbinds = 0;
    for (uint32_t i = 0; i < count; i++) {
        const GfxCmd *c = &cmds[i];
        if (c->kind != GFX_CMD_BIND_DESC) continue;
        nbinds++;
        if (c->u.bind.kind != GFX_DESC_UBO && c->u.bind.kind != GFX_DESC_SSBO) continue;
        int idx = reflect_find_bind_spec(p, c->u.bind.stage, c->u.bind.slot, c->u.bind.kind);
        if (idx < 0) continue;
        int set = c->u.bind.stage == 0 ? 0 : 1;
        size_t stride = reflect_align(c->u.bind.size ? c->u.bind.size : 1);
        if (stride > p->dyn_stride[set][idx]) p->dyn_stride[set][idx] = stride;
        changes[set][idx]++;
    }
    for (uint32_t s = 0; s < 2; s++)
        for (uint32_t i = 0; i < p->nspec[s]; i++) {
            if (!changes[s][i]) continue;
            size_t stride = p->dyn_stride[s][i] ? p->dyn_stride[s][i] : REFLECT_ALIGN;
            size_t need = stride * (changes[s][i] + 1);
            if (need > (64u << 20) || !reflect_arena_ensure(p, (int)s, (int)i, need)) {
                vk_log("ERROR reflected arena too large/alloc failed set=%u binding=%u need=%zu",
                       s, p->spec[s][i].binding, need);
                changes[s][i] = 0;
            }
        }
    if (nbinds) vkResetDescriptorPool(g.dev, p->dpool_draw, 0);
    ReflectEpochState *st = calloc(1, sizeof *st);
    if (!st) { pthread_mutex_unlock(&g_lock); return 0; }
    for (uint32_t s = 0; s < 2; s++)
        for (uint32_t i = 0; i < p->nspec[s]; i++)
            st->dyn[s][i] = changes[s][i] ? 1 : 0;
    VkDescriptorSet cur_sets[2] = { p->dset[0], p->dset[1] };
    uint32_t cur_epochs = 0;
    uint32_t used[2][GFX_REFLECT_MAX_BINDINGS];
    memset(used, 0, sizeof used);

    /* Upload the guest stream ranges before recording.  These buffers are
     * host-visible and are rebuilt only when the bound range grows. */
    for (uint32_t s = 0; s < nstreams; s++) {
        uint32_t binding = streams[s].binding;
        if (binding >= 16 || !streams[s].data || !streams[s].size) continue;
        if (!vk_reflect_ensure_buffer(&p->vbuf[binding], &p->vbuf_mem[binding],
                                      &p->vbuf_cap[binding], streams[s].size,
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
            !vk_reflect_upload(p->vbuf_mem[binding], 0, streams[s].data, streams[s].size)) {
            vk_log("ERROR reflected vertex stream upload binding=%u size=%zu", binding, streams[s].size);
            free(st);
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
    }

    size_t *index_offsets = calloc(count ? count : 1, sizeof *index_offsets);
    if (!index_offsets) { free(st); pthread_mutex_unlock(&g_lock); return 0; }
    size_t index_total = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (cmds[i].kind != GFX_CMD_DRAW_INDEXED) continue;
        const GfxCmd *c = &cmds[i];
        uint32_t it = c->u.draw_indexed.index_type;
        size_t src = c->u.draw_indexed.index_size;
        if (!src) src = (size_t)c->u.draw_indexed.count * (it == 2 ? 4 : it == 1 ? 2 : 1);
        size_t dst = it == 0 ? (size_t)c->u.draw_indexed.count * 2 : src;
        index_offsets[i] = (index_total + 3) & ~(size_t)3;
        index_total = index_offsets[i] + dst;
    }
    if (index_total) {
        if (!vk_reflect_ensure_buffer(&p->index_buf, &p->index_mem, &p->index_cap,
                                      index_total, VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) {
            free(index_offsets);
            free(st);
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        for (uint32_t i = 0; i < count; i++) {
            if (cmds[i].kind != GFX_CMD_DRAW_INDEXED) continue;
            const GfxCmd *c = &cmds[i];
            uint32_t it = c->u.draw_indexed.index_type;
            size_t src = c->u.draw_indexed.index_size;
            if (!src) src = (size_t)c->u.draw_indexed.count * (it == 2 ? 4 : it == 1 ? 2 : 1);
            if (it == 0) {
                size_t n = c->u.draw_indexed.count;
                uint16_t *wide = malloc(n * 2);
                if (!wide) { free(index_offsets); free(st); pthread_mutex_unlock(&g_lock); return 0; }
                const uint8_t *in = c->u.draw_indexed.indices;
                for (size_t k = 0; k < n; k++) wide[k] = in[k];
                int ok = vk_reflect_upload(p->index_mem, index_offsets[i], wide, n * 2);
                free(wide);
                if (!ok) { free(index_offsets); free(st); pthread_mutex_unlock(&g_lock); return 0; }
            } else if (!vk_reflect_upload(p->index_mem, index_offsets[i],
                                          c->u.draw_indexed.indices, src)) {
                free(index_offsets);
                free(st);
                pthread_mutex_unlock(&g_lock);
                return 0;
            }
        }
    }

    /* Create state variants before recording; some drivers dislike pipeline
     * creation inside an active render pass. */
    for (uint32_t i = 0; i < count; i++) {
        const GfxPipelineState *ps = NULL;
        if (cmds[i].kind == GFX_CMD_DRAW && cmds[i].u.draw.has_state)
            ps = &cmds[i].u.draw.state;
        else if (cmds[i].kind == GFX_CMD_DRAW_INDEXED && cmds[i].u.draw_indexed.has_state)
            ps = &cmds[i].u.draw_indexed.state;
        if (ps) (void)reflect_pipeline_for_state(p, ps);
    }

    float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
    if (default_clear) memcpy(clear, default_clear, sizeof clear);
    int first_is_clear = count && cmds[0].kind == GFX_CMD_CLEAR_COLOR;
    if (first_is_clear) memcpy(clear, cmds[0].u.clear.rgba, sizeof clear);

    VkCommandBuffer cmd = g.cmd;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(reflected-execute)")) {
        free(index_offsets);
        free(st);
        pthread_mutex_unlock(&g_lock); return 0;
    }
    VkClearValue cv[GFX_MAX_COLOR_TARGETS + 1];
    for (uint32_t i = 0; i < ncolor; i++)
        cv[i] = (VkClearValue){ .color = { .float32 = { clear[0], clear[1], clear[2], clear[3] } } };
    if (has_depth)
        cv[ncolor] = (VkClearValue){ .depthStencil = { 1.0f, 0 } };
    VkRenderPassBeginInfo rp = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = p->pass, .framebuffer = p->fb,
        .renderArea = { { 0, 0 }, { t->width, t->height } },
        .clearValueCount = attach_count, .pClearValues = cv };
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipe);
    {
        const float zero[4] = { 0, 0, 0, 0 };
        vkCmdSetBlendConstants(cmd, zero);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    }
    for (uint32_t s = 0; s < nstreams; s++) {
        uint32_t binding = streams[s].binding;
        if (binding >= 16 || !streams[s].data || !streams[s].size) continue;
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, binding, 1, &p->vbuf[binding], &offset);
    }

    VkViewport viewport = { 0, 0, (float)t->width, (float)t->height, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, { t->width, t->height } };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    for (uint32_t i = 0; i < count; i++) {
        const GfxCmd *c = &cmds[i];
        switch (c->kind) {
        case GFX_CMD_CLEAR_COLOR: {
            if (i == 0 && first_is_clear) break;   /* already the pass load value */
            VkClearAttachment ca = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                     .colorAttachment = 0,
                                     .clearValue = { .color = { .float32 = {
                                         c->u.clear.rgba[0], c->u.clear.rgba[1],
                                         c->u.clear.rgba[2], c->u.clear.rgba[3] } } } };
            VkClearRect cr = { { { 0, 0 }, { t->width, t->height } }, 0, 1 };
            vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
            break;
        }
        case GFX_CMD_CLEAR_DEPTH_STENCIL: {
            VkClearAttachment ca = { .clearValue = { .depthStencil = {
                c->u.clear_depth.depth, (uint32_t)c->u.clear_depth.stencil } } };
            if (c->u.clear_depth.dmask) ca.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
            if (c->u.clear_depth.smask) ca.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            if (ca.aspectMask) {
                VkClearRect cr = { { { 0, 0 }, { t->width, t->height } }, 0, 1 };
                vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
            }
            break;
        }
        case GFX_CMD_SET_VIEWPORT:
            viewport = (VkViewport){ (float)c->u.rect.x, (float)c->u.rect.y,
                                     (float)c->u.rect.width, (float)c->u.rect.height, 0, 1 };
            /* A negative height is the NVN upper-left-origin transform and is
             * legal with Vulkan 1.1+ (VK_KHR_maintenance1). */
            if (c->u.rect.width > 0 && c->u.rect.height != 0)
                vkCmdSetViewport(cmd, 0, 1, &viewport);
            break;
        case GFX_CMD_SET_SCISSOR:
            if (c->u.rect.width > 0 && c->u.rect.height > 0) {
                scissor = (VkRect2D){ { c->u.rect.x, c->u.rect.y },
                                      { (uint32_t)c->u.rect.width, (uint32_t)c->u.rect.height } };
                vkCmdSetScissor(cmd, 0, 1, &scissor);
            }
            break;
        case GFX_CMD_BIND_DESC: {
            const GfxCmd *b = c;
            int idx = reflect_find_bind_spec(p, b->u.bind.stage, b->u.bind.slot, b->u.bind.kind);
            if (idx < 0) break;
            int set = b->u.bind.stage == 0 ? 0 : 1;
            if (b->u.bind.kind == GFX_DESC_UBO || b->u.bind.kind == GFX_DESC_SSBO) {
                if (!st->dyn[set][idx] || !p->dyn_buf[set][idx]) break;
                size_t stride = p->dyn_stride[set][idx];
                used[set][idx]++;
                size_t off = stride * used[set][idx];
                if (off + b->u.bind.size > p->dyn_cap[set][idx] ||
                    !vk_reflect_upload(p->dyn_mem[set][idx], off, b->u.bind.data, b->u.bind.size)) {
                    vk_log("ERROR reflected binding upload set=%d slot=%u size=%zu",
                           set, b->u.bind.slot, b->u.bind.size);
                    break;
                }
                if (b->u.bind.kind == GFX_DESC_UBO) st->ubo_off[set][idx] = off;
                else st->ssbo_off[set][idx] = off;
            } else {
                st->tex[set][idx] = b->u.bind.texture;
                st->samp[set][idx] = b->u.bind.sampler;
            }
            if (cur_epochs < GFX_DESC_MAX_EPOCHS) {
                VkDescriptorSetLayout layouts[2] = { p->dsl[0], p->dsl[1] };
                VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                    .descriptorPool = p->dpool_draw, .descriptorSetCount = 2, .pSetLayouts = layouts };
                VkDescriptorSet ns[2];
                if (vkAllocateDescriptorSets(g.dev, &ai, ns) == VK_SUCCESS) {
                    cur_sets[0] = ns[0]; cur_sets[1] = ns[1];
                    reflect_write_epoch_set(p, 0, ns[0], st);
                    reflect_write_epoch_set(p, 1, ns[1], st);
                    cur_epochs++;
                } else {
                    vk_log("ERROR reflected epoch set allocation failed at %u", cur_epochs);
                }
            } else {
                unsigned epoch_full_logged;
                if (epoch_full_logged++ < 4)
                    vk_log("ERROR reflected descriptor epoch limit (%u) reached; reusing last set",
                           GFX_DESC_MAX_EPOCHS);
            }
            break;
        }
        case GFX_CMD_DRAW: {
            uint32_t prim = c->u.draw.primitive;
            if (vk_reflect_topology(prim) != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                vk_log("reflected execute: primitive %u uses the TRIANGLE_LIST pipeline", prim);
            reflect_bind_epoch_sets(cmd, p, cur_sets, st);
            reflect_apply_draw_state(cmd, p, c->u.draw.has_state ? &c->u.draw.state : NULL);
            vkCmdDraw(cmd, c->u.draw.count, 1, c->u.draw.first, 0);
            break;
        }
        case GFX_CMD_DRAW_INDEXED: {
            uint32_t prim = c->u.draw_indexed.primitive;
            if (vk_reflect_topology(prim) != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                vk_log("reflected execute: indexed primitive %u uses the TRIANGLE_LIST pipeline", prim);
            reflect_bind_epoch_sets(cmd, p, cur_sets, st);
            reflect_apply_draw_state(cmd, p, c->u.draw_indexed.has_state
                                              ? &c->u.draw_indexed.state : NULL);
            vkCmdBindIndexBuffer(cmd, p->index_buf, index_offsets[i],
                                 vk_reflect_index_type(c->u.draw_indexed.index_type));
            vkCmdDrawIndexed(cmd, c->u.draw_indexed.count, 1, 0,
                             (int32_t)c->u.draw_indexed.base_vertex, 0);
            break;
        }
        default:
            break;
        }
    }
    vkCmdEndRenderPass(cmd);
    for (uint32_t i = 0; i < ncolor; i++) {
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { t->width, t->height, 1 },
        };
        vkCmdCopyImageToBuffer(cmd, t->color_image[i], VK_IMAGE_LAYOUT_GENERAL,
                               t->color_readback[i], 1, &region);
        VkBufferMemoryBarrier bb = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = t->color_readback[i], .offset = 0, .size = VK_WHOLE_SIZE };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, NULL, 1, &bb, 0, NULL);
    }
    if (!vk_submit_and_wait(cmd, "reflected-execute")) {
        free(index_offsets);
        free(st);
        pthread_mutex_unlock(&g_lock); return 0;
    }
    t->layout_state = VK_IMAGE_LAYOUT_GENERAL;
    for (uint32_t i = 0; i < ncolor; i++) {
        void *mapped = NULL;
        if (!vk_ok(vkMapMemory(g.dev, t->color_readback_mem[i], 0, VK_WHOLE_SIZE, 0, &mapped),
                   "vkMapMemory(reflected-execute)")) {
            free(index_offsets);
            free(st);
            pthread_mutex_unlock(&g_lock); return 0;
        }
        memcpy(t->color_pixels[i], mapped, t->color_size[i]);
        vkUnmapMemory(g.dev, t->color_readback_mem[i]);
    }
    free(index_offsets);
    free(st);
    pthread_mutex_unlock(&g_lock);
    return 1;
}

int gfx_backend_reflected_draw(GfxReflectedPipeline *p, GfxOffscreenTarget *t,
                               float r, float g_, float b, float a) {
    const float clear[4] = { r, g_, b, a };
    GfxCmd cmd = { .kind = GFX_CMD_DRAW };
    cmd.u.draw.primitive = 4;
    cmd.u.draw.first = 0;
    cmd.u.draw.count = 3;
    return gfx_backend_reflected_execute(p, t, &cmd, 1, clear);
}
