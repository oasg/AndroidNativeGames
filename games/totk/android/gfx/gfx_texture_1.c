/* Module: texture_1. */
#include "gfx_internal.h"

int texture_read_pixels_locked(const GfxTexture *t, uint32_t level, uint32_t layer,
                                      void *dst, size_t size) {
    if (!t || !dst) return 0;
    if (t->samples != 1) {
        vk_log("ERROR texture read: %u-sample texture needs resolve", t->samples);
        return 0;
    }
    if (!t->readback_ok) {
        vk_log("ERROR texture read: %s was created without transfer-src support",
               vk_format_name(t->format));
        return 0;
    }
    if (level >= t->mip_levels || layer >= t->array_layers) {
        vk_log("ERROR texture read: level=%u/%u layer=%u/%u", level, t->mip_levels,
               layer, t->array_layers);
        return 0;
    }
    GfxTextureDesc desc = {
        .target = t->target,
        .vk_format = t->format,
        .width = t->width,
        .height = t->height,
        .depth = t->target == GFX_TEXTURE_3D ? t->depth : 1,
        .mip_levels = t->mip_levels,
        .array_layers = t->array_layers,
        .samples = t->samples,
    };
    if (t->target == GFX_TEXTURE_3D && layer != 0) return 0;
    size_t slice = vk_texture_slice_size(&desc, level);
    if (!slice || size < slice) {
        vk_log("ERROR texture read: dst size=%zu < %zu", size, slice);
        return 0;
    }
    uint32_t w, h, d;
    vk_mip_extent(&desc, level, &w, &h, &d);

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stage_mem = VK_NULL_HANDLE;
    if (!vk_create_host_buffer(slice, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &staging, &stage_mem))
        return 0;
    VkCommandBuffer cmd = g.cmd;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(texture read)")) {
        vkDestroyBuffer(g.dev, staging, NULL);
        vkFreeMemory(g.dev, stage_mem, NULL);
        return 0;
    }
    VkImageAspectFlags aspect = vk_format_copy_aspect(t->format);
    vk_image_barrier_range(cmd, t->image, t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           0, VK_ACCESS_TRANSFER_READ_BIT,
                           t->layout == VK_IMAGE_LAYOUT_UNDEFINED
                               ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                               : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           aspect, level, 1, t->target == GFX_TEXTURE_3D ? 0 : layer, 1);
    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = aspect,
            .mipLevel = level,
            .baseArrayLayer = t->target == GFX_TEXTURE_3D ? 0 : layer,
            .layerCount = 1,
        },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { w, h, d },
    };
    vkCmdCopyImageToBuffer(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1,
                           &region);
    vk_image_barrier_range(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           aspect, level, 1, t->target == GFX_TEXTURE_3D ? 0 : layer, 1);
    VkBufferMemoryBarrier bb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = staging, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &bb, 0, NULL);
    int submitted = vk_submit_and_wait(cmd, "texture read");
    if (submitted) ((GfxTexture *)t)->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!submitted) {
        vkDestroyBuffer(g.dev, staging, NULL);
        vkFreeMemory(g.dev, stage_mem, NULL);
        return 0;
    }
    void *mapped = NULL;
    if (!vk_ok(vkMapMemory(g.dev, stage_mem, 0, VK_WHOLE_SIZE, 0, &mapped),
               "vkMapMemory(texture read)")) {
        vkDestroyBuffer(g.dev, staging, NULL);
        vkFreeMemory(g.dev, stage_mem, NULL);
        return 0;
    }
    memcpy(dst, mapped, slice);
    vkUnmapMemory(g.dev, stage_mem);
    vkDestroyBuffer(g.dev, staging, NULL);
    vkFreeMemory(g.dev, stage_mem, NULL);
    return 1;
}

int gfx_backend_texture_read_pixels(const GfxTexture *texture, uint32_t level, uint32_t layer,
                                    void *dst, size_t size) {
    pthread_mutex_lock(&g_lock);
    int ok = texture_read_pixels_locked(texture, level, layer, dst, size);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void gfx_backend_texture_destroy(GfxTexture *t) {
    if (!t) return;
    if (g.dev) {
        if (t->sampler) vkDestroySampler(g.dev, t->sampler, NULL);
        if (t->view) vkDestroyImageView(g.dev, t->view, NULL);
        if (t->image) vkDestroyImage(g.dev, t->image, NULL);
        if (t->mem) vkFreeMemory(g.dev, t->mem, NULL);
    }
    free(t);
}

GfxTexturedPipeline *gfx_backend_textured_pipeline_create(int vk_format) {
    return textured_pipeline_create_impl((const uint8_t *)triangle_tex_vert_spv,
                                         triangle_tex_vert_spv_size,
                                         (const uint8_t *)triangle_tex_frag_spv,
                                         triangle_tex_frag_spv_size, vk_format);
}

void gfx_backend_textured_pipeline_destroy(GfxTexturedPipeline *p) {
    if (!p) return;
    if (g.dev) {
        if (p->fb) vkDestroyFramebuffer(g.dev, p->fb, NULL);
        if (p->pipe) vkDestroyPipeline(g.dev, p->pipe, NULL);
        if (p->layout) vkDestroyPipelineLayout(g.dev, p->layout, NULL);
        if (p->pass) vkDestroyRenderPass(g.dev, p->pass, NULL);
        if (p->dpool) vkDestroyDescriptorPool(g.dev, p->dpool, NULL);
        if (p->dsl) vkDestroyDescriptorSetLayout(g.dev, p->dsl, NULL);
        if (p->vs) vkDestroyShaderModule(g.dev, p->vs, NULL);
        if (p->ps) vkDestroyShaderModule(g.dev, p->ps, NULL);
    }
    free(p);
}

int gfx_backend_textured_draw(GfxTexturedPipeline *p, GfxTexture *tex, GfxOffscreenTarget *t,
                              float r, float g_, float b, float a) {
    if (!p || !tex || !t) return 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready || t->rt_layout.color_count < 1 ||
        t->color_format[0] != p->format || t->color_bpp[0] != 4) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (!p->fb || p->fb_view != t->color_view[0] || p->fb_w != t->width || p->fb_h != t->height) {
        if (p->fb) vkDestroyFramebuffer(g.dev, p->fb, NULL);
        VkImageView view = t->color_view[0];
        VkFramebufferCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = p->pass, .attachmentCount = 1, .pAttachments = &view,
            .width = t->width, .height = t->height, .layers = 1,
        };
        if (!vk_ok(vkCreateFramebuffer(g.dev, &fci, NULL, &p->fb), "vkCreateFramebuffer(tex)")) {
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        p->fb_view = t->color_view[0]; p->fb_w = t->width; p->fb_h = t->height;
    }
    VkDescriptorImageInfo di = {
        .sampler = tex->sampler, .imageView = tex->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = p->dset, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &di,
    };
    vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);

    VkCommandBuffer cmd = g.cmd;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(tex)")) {
        pthread_mutex_unlock(&g_lock); return 0;
    }
    VkClearValue clear = { .color = { .float32 = { r, g_, b, a } } };
    VkRenderPassBeginInfo rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = p->pass, .framebuffer = p->fb,
        .renderArea = { { 0, 0 }, { t->width, t->height } },
        .clearValueCount = 1, .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &p->dset, 0, NULL);
    VkViewport viewport = { 0, 0, (float)t->width, (float)t->height, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, { t->width, t->height } };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { t->width, t->height, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, t->color_image[0], VK_IMAGE_LAYOUT_GENERAL,
                           t->color_readback[0], 1, &region);
    VkBufferMemoryBarrier bb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = t->color_readback[0], .offset = 0, .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &bb, 0, NULL);
    if (!vk_submit_and_wait(cmd, "textured triangle")) {
        pthread_mutex_unlock(&g_lock); return 0;
    }
    t->layout_state = VK_IMAGE_LAYOUT_GENERAL;
    void *mapped = NULL;
    if (!vk_ok(vkMapMemory(g.dev, t->color_readback_mem[0], 0, VK_WHOLE_SIZE, 0, &mapped),
               "vkMapMemory(tex)")) {
        pthread_mutex_unlock(&g_lock); return 0;
    }
    memcpy(t->color_pixels[0], mapped, t->color_size[0]);
    vkUnmapMemory(g.dev, t->color_readback_mem[0]);
    pthread_mutex_unlock(&g_lock);
    return 1;
}

void vk_texture_level_extent(const GfxTexture *t, uint32_t level,
                                    uint32_t *w, uint32_t *h, uint32_t *d) {
    *w = t->width >> level; if (!*w) *w = 1;
    *h = t->height >> level; if (!*h) *h = 1;
    *d = t->target == GFX_TEXTURE_3D ? (t->depth >> level) : 1;
    if (!*d) *d = 1;
}

int vk_texture_transfer_check(const GfxTexture *t, uint32_t level, uint32_t layer,
                                     const char *what, int need_dst) {
    if (!t) return 0;
    if (level >= t->mip_levels || layer >= t->array_layers) {
        vk_log("ERROR %s: level=%u/%u layer=%u/%u", what, level, t->mip_levels,
               layer, t->array_layers);
        return 0;
    }
    if (t->target == GFX_TEXTURE_3D && layer != 0) {
        vk_log("ERROR %s: 3D texture layer must be 0 (got %u)", what, layer);
        return 0;
    }
    if (t->samples != 1) {
        vk_log("ERROR %s: %u-sample texture is unsupported", what, t->samples);
        return 0;
    }
    if (vk_format_is_depth(t->format) || t->format == VK_FORMAT_S8_UINT) {
        vk_log("ERROR %s: depth/stencil format %s is unsupported", what,
               vk_format_name(t->format));
        return 0;
    }
    if (need_dst && !t->transfer_dst_ok) {
        vk_log("ERROR %s: %s was created without transfer-dst support", what,
               vk_format_name(t->format));
        return 0;
    }
    return 1;
}

int vk_texture_region_check_blocks(const GfxTexture *t, int32_t x, int32_t y,
                                          int32_t z, uint32_t w, uint32_t h,
                                          const char *what) {
    uint32_t bw = 0, bh = 0, bytes = 0;
    if (!vk_format_block(t->format, &bw, &bh, &bytes)) return 0;
    if (bw == 1 && bh == 1) return 1;
    if ((x % (int32_t)bw) || (y % (int32_t)bh) || (w % bw) || (h % bh) || z) {
        vk_log("ERROR %s: compressed %s region needs %ux%u alignment "
               "(x=%d y=%d z=%d w=%u h=%u)", what, vk_format_name(t->format),
               bw, bh, x, y, z, w, h);
        return 0;
    }
    return 1;
}

/* Packed row geometry for a region.  Compressed formats use one row per block
 * row and `bpp` is bytes per block, so a BC4 block view (one block per texel)
 * can be uploaded/downloaded through the same code as an uncompressed image. */
int vk_texture_packed_geometry(VkFormat fmt, uint32_t w, uint32_t h,
                                      size_t *row_bytes, uint32_t *rows,
                                      uint32_t *bpp, uint32_t *bw, uint32_t *bh) {
    uint32_t block_w = 0, block_h = 0, bytes = 0;
    if (!vk_format_block(fmt, &block_w, &block_h, &bytes)) return 0;
    if (block_w != 1 || block_h != 1) {
        if ((w % block_w) || (h % block_h)) return 0;
        *row_bytes = (size_t)(w / block_w) * bytes;
        *rows = h / block_h;
    } else {
        *row_bytes = (size_t)w * bytes;
        *rows = h;
    }
    if (bpp) *bpp = bytes;
    if (bw) *bw = block_w;
    if (bh) *bh = block_h;
    return 1;
}
