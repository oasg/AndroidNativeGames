/* Module: texture_2. */
#include "gfx_internal.h"

int gfx_backend_texture_copy(GfxTexture *dst, uint32_t dst_level, uint32_t dst_layer,
                             int32_t dst_x, int32_t dst_y, int32_t dst_z,
                             const GfxTexture *src, uint32_t src_level, uint32_t src_layer,
                             int32_t src_x, int32_t src_y, int32_t src_z,
                             uint32_t width, uint32_t height, uint32_t depth) {
    if (!dst || !src) return 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready || !g.cmd) { pthread_mutex_unlock(&g_lock); return 0; }

    int ok = 0;
    VkCommandBuffer cmd = g.cmd;
    uint32_t sw = 0, sh = 0, sd = 0, dw = 0, dh = 0, dd = 0;
    if (!vk_texture_transfer_check(dst, dst_level, dst_layer, "texture copy dst", 1) ||
        !vk_texture_transfer_check(src, src_level, src_layer, "texture copy src", 0))
        goto out;
    if (!src->readback_ok) {
        vk_log("ERROR texture copy: src %s was created without transfer-src support",
               vk_format_name(src->format));
        goto out;
    }
    if ((dst->target != GFX_TEXTURE_3D && (dst_z != 0 || depth > 1)) ||
        (src->target != GFX_TEXTURE_3D && (src_z != 0 || depth > 1))) {
        vk_log("ERROR texture copy: 2D/array region needs z=0 depth<=1 "
               "(dst z=%d depth=%u src z=%d)", dst_z, depth, src_z);
        goto out;
    }
    vk_texture_level_extent(src, src_level, &sw, &sh, &sd);
    vk_texture_level_extent(dst, dst_level, &dw, &dh, &dd);
    uint32_t cw = width, ch = height, cd = depth;
    uint32_t dw2 = cw, dh2 = ch, dd2 = cd;
    if (!vk_clamp_region(&src_x, &src_y, &src_z, &cw, &ch, &cd,
                         sw, sh, sd, NULL, NULL, NULL)) {
        vk_log("ERROR texture copy: source region is outside %ux%ux%u", sw, sh, sd);
        goto out;
    }
    if (!vk_clamp_region(&dst_x, &dst_y, &dst_z, &dw2, &dh2, &dd2,
                         dw, dh, dd, NULL, NULL, NULL)) {
        vk_log("ERROR texture copy: destination region is outside %ux%ux%u", dw, dh, dd);
        goto out;
    }
    if (dw2 < cw) cw = dw2;
    if (dh2 < ch) ch = dh2;
    if (dd2 < cd) cd = dd2;
    if (!cw || !ch || !cd) {
        vk_log("ERROR texture copy: empty clamped region");
        goto out;
    }
    if (!vk_texture_region_check_blocks(src, src_x, src_y, src_z, cw, ch, "texture copy src") ||
        !vk_texture_region_check_blocks(dst, dst_x, dst_y, dst_z, cw, ch, "texture copy dst"))
        goto out;

    int same_format = src->format == dst->format;
    int same_image = src == dst;
    if (!same_format) {
        if (vk_format_is_compressed(src->format) || vk_format_is_compressed(dst->format)) {
            vk_log("ERROR texture copy: compressed format conversion %s -> %s is unsupported",
                   vk_format_name(src->format), vk_format_name(dst->format));
            goto out;
        }
        if (!vk_format_can_blit(src->format, 0) || !vk_format_can_blit(dst->format, 1)) {
            vk_log("ERROR texture copy: blit %s -> %s lacks BLIT_SRC/BLIT_DST",
                   vk_format_name(src->format), vk_format_name(dst->format));
            goto out;
        }
    }

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(texture copy)"))
        goto out;

    VkImageAspectFlags src_aspect = vk_format_copy_aspect(src->format);
    VkImageAspectFlags dst_aspect = vk_format_copy_aspect(dst->format);
    VkImageLayout src_layout = same_image ? VK_IMAGE_LAYOUT_GENERAL
                                          : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkImageLayout dst_layout = same_image ? VK_IMAGE_LAYOUT_GENERAL
                                          : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkAccessFlags src_prior_access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                     VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkPipelineStageFlags src_prior_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkAccessFlags dst_prior_access = src_prior_access;
    VkPipelineStageFlags dst_prior_stage = src_prior_stage;
    if (src->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        src_prior_access = 0;
        src_prior_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
    if (dst->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        dst_prior_access = 0;
        dst_prior_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }

    if (same_image) {
        vk_image_barrier_range(cmd, src->image, src->layout, VK_IMAGE_LAYOUT_GENERAL,
                               src_prior_access, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                               src_prior_stage, VK_PIPELINE_STAGE_TRANSFER_BIT, src_aspect,
                               0, src->mip_levels, 0,
                               src->target == GFX_TEXTURE_3D ? 1 : src->array_layers);
    } else {
        vk_image_barrier_range(cmd, src->image, src->layout, src_layout,
                               src_prior_access, VK_ACCESS_TRANSFER_READ_BIT, src_prior_stage,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, src_aspect,
                               src_level, 1, src->target == GFX_TEXTURE_3D ? 0 : src_layer, 1);
        vk_image_barrier_range(cmd, dst->image, dst->layout, dst_layout,
                               dst_prior_access, VK_ACCESS_TRANSFER_WRITE_BIT, dst_prior_stage,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, dst_aspect,
                               dst_level, 1, dst->target == GFX_TEXTURE_3D ? 0 : dst_layer, 1);
    }

    if (same_format) {
        VkImageCopy region = {
            .srcSubresource = {
                .aspectMask = src_aspect,
                .mipLevel = src_level,
                .baseArrayLayer = src->target == GFX_TEXTURE_3D ? 0 : src_layer,
                .layerCount = 1,
            },
            .srcOffset = { src_x, src_y, src_z },
            .dstSubresource = {
                .aspectMask = dst_aspect,
                .mipLevel = dst_level,
                .baseArrayLayer = dst->target == GFX_TEXTURE_3D ? 0 : dst_layer,
                .layerCount = 1,
            },
            .dstOffset = { dst_x, dst_y, dst_z },
            .extent = { cw, ch, cd },
        };
        vkCmdCopyImage(cmd, src->image, src_layout, dst->image, dst_layout, 1, &region);
    } else {
        VkImageBlit region = {
            .srcSubresource = {
                .aspectMask = src_aspect,
                .mipLevel = src_level,
                .baseArrayLayer = src->target == GFX_TEXTURE_3D ? 0 : src_layer,
                .layerCount = 1,
            },
            .srcOffsets = {
                { src_x, src_y, src_z },
                { src_x + (int32_t)cw, src_y + (int32_t)ch, src_z + (int32_t)cd },
            },
            .dstSubresource = {
                .aspectMask = dst_aspect,
                .mipLevel = dst_level,
                .baseArrayLayer = dst->target == GFX_TEXTURE_3D ? 0 : dst_layer,
                .layerCount = 1,
            },
            .dstOffsets = {
                { dst_x, dst_y, dst_z },
                { dst_x + (int32_t)cw, dst_y + (int32_t)ch, dst_z + (int32_t)cd },
            },
        };
        vkCmdBlitImage(cmd, src->image, src_layout, dst->image, dst_layout, 1, &region,
                       VK_FILTER_NEAREST);
    }

    if (same_image) {
        vk_image_barrier_range(cmd, dst->image, VK_IMAGE_LAYOUT_GENERAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, dst_aspect,
                               0, dst->mip_levels, 0,
                               dst->target == GFX_TEXTURE_3D ? 1 : dst->array_layers);
    } else {
        vk_image_barrier_range(cmd, src->image, src_layout,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, src_aspect,
                               src_level, 1, src->target == GFX_TEXTURE_3D ? 0 : src_layer, 1);
        vk_image_barrier_range(cmd, dst->image, dst_layout,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, dst_aspect,
                               dst_level, 1, dst->target == GFX_TEXTURE_3D ? 0 : dst_layer, 1);
    }

    if (!vk_submit_and_wait(cmd, same_format ? "texture copy" : "texture blit"))
        goto out;
    dst->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!same_image) ((GfxTexture *)src)->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ok = 1;
    vk_log("texture %s %s -> %s %ux%ux%u src=(%d,%d,%d) dst=(%d,%d,%d)",
           same_format ? "copy" : "blit", vk_format_name(src->format),
           vk_format_name(dst->format), cw, ch, cd, src_x, src_y, src_z,
           dst_x, dst_y, dst_z);
out:
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int gfx_backend_texture_clear(GfxTexture *dst, uint32_t level, uint32_t layer,
                              const float rgba[4],
                              int32_t x, int32_t y, uint32_t width, uint32_t height) {
    if (!dst || !rgba) return 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready || !g.cmd) { pthread_mutex_unlock(&g_lock); return 0; }

    int ok = 0;
    VkCommandBuffer cmd = g.cmd;
    uint32_t ew = 0, eh = 0, ed = 0;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stage_mem = VK_NULL_HANDLE;
    if (!vk_texture_transfer_check(dst, level, layer, "texture clear", 1)) goto out;
    if (vk_format_is_compressed(dst->format)) {
        vk_log("ERROR texture clear: compressed %s region clear is unsupported",
               vk_format_name(dst->format));
        goto out;
    }
    vk_texture_level_extent(dst, level, &ew, &eh, &ed);
    uint32_t w = width, h = height, d = dst->target == GFX_TEXTURE_3D ? ed : 1;
    int32_t z = 0;
    if (!vk_clamp_region(&x, &y, &z, &w, &h, &d, ew, eh, ed, NULL, NULL, NULL)) {
        vk_log("ERROR texture clear: region is outside %ux%ux%u", ew, eh, ed);
        goto out;
    }
    int whole_level = x == 0 && y == 0 && w == ew && h == eh;
    uint8_t texel[16];
    uint32_t bpp = 0;
    size_t row = 0, total = 0;
    if (!whole_level) {
        if (!vk_pack_clear_texel(dst->format, rgba, texel, &bpp)) {
            vk_log("ERROR texture clear: sub-region clear of %s is unsupported "
                   "(only whole-level clears use vkCmdClearColorImage)",
                   vk_format_name(dst->format));
            goto out;
        }
        row = (size_t)w * bpp;
        total = row * h * d;
        if (!vk_create_host_buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   &staging, &stage_mem))
            goto out;
        void *mapped = NULL;
        if (!vk_ok(vkMapMemory(g.dev, stage_mem, 0, VK_WHOLE_SIZE, 0, &mapped),
                   "vkMapMemory(texture clear)"))
            goto out;
        for (uint32_t zz = 0; zz < d; zz++)
            for (uint32_t yy = 0; yy < h; yy++)
                for (uint32_t xx = 0; xx < w; xx++)
                    memcpy((uint8_t *)mapped + ((size_t)zz * h + yy) * row + (size_t)xx * bpp,
                           texel, bpp);
        vkUnmapMemory(g.dev, stage_mem);
    }

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(texture clear)"))
        goto out;

    VkImageAspectFlags aspect = vk_format_copy_aspect(dst->format);
    VkAccessFlags prior_access = dst->layout == VK_IMAGE_LAYOUT_UNDEFINED
        ? 0
        : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
           VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkPipelineStageFlags prior_stage = dst->layout == VK_IMAGE_LAYOUT_UNDEFINED
        ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    vk_image_barrier_range(cmd, dst->image, dst->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           prior_access, VK_ACCESS_TRANSFER_WRITE_BIT, prior_stage,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, aspect, level, 1,
                           dst->target == GFX_TEXTURE_3D ? 0 : layer, 1);

    if (whole_level) {
        VkClearColorValue color;
        memcpy(color.float32, rgba, sizeof color.float32);
        VkImageSubresourceRange range = {
            .aspectMask = aspect,
            .baseMipLevel = level,
            .levelCount = 1,
            .baseArrayLayer = dst->target == GFX_TEXTURE_3D ? 0 : layer,
            .layerCount = 1,
        };
        vkCmdClearColorImage(cmd, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &color, 1, &range);
    } else {
        VkBufferMemoryBarrier bmb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = staging, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &bmb, 0, NULL);
        VkBufferImageCopy region = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {
                .aspectMask = aspect,
                .mipLevel = level,
                .baseArrayLayer = dst->target == GFX_TEXTURE_3D ? 0 : layer,
                .layerCount = 1,
            },
            .imageOffset = { x, y, 0 },
            .imageExtent = { w, h, d },
        };
        vkCmdCopyBufferToImage(cmd, staging, dst->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    vk_image_barrier_range(cmd, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, aspect, level, 1,
                           dst->target == GFX_TEXTURE_3D ? 0 : layer, 1);
    if (!vk_submit_and_wait(cmd, "texture clear")) goto out;
    dst->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ok = 1;
    vk_log("texture clear %s level=%u layer=%u rect=(%d,%d %ux%u) whole=%d",
           vk_format_name(dst->format), level, layer, x, y, w, h, whole_level);
out:
    if (staging) vkDestroyBuffer(g.dev, staging, NULL);
    if (stage_mem) vkFreeMemory(g.dev, stage_mem, NULL);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int gfx_backend_texture_upload(GfxTexture *dst, uint32_t level, uint32_t layer,
                               int32_t x, int32_t y, uint32_t width, uint32_t height,
                               const void *data, size_t size) {
    if (!dst || !data || !size) return 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready || !g.cmd) { pthread_mutex_unlock(&g_lock); return 0; }

    int ok = 0;
    VkCommandBuffer cmd = g.cmd;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stage_mem = VK_NULL_HANDLE;
    uint32_t ew = 0, eh = 0, ed = 0;
    if (!vk_texture_transfer_check(dst, level, layer, "texture upload", 1)) goto out;
    int compressed = vk_format_is_compressed(dst->format);
    if (compressed && (x < 0 || y < 0)) {
        vk_log("ERROR texture upload: compressed %s region cannot start at a "
               "negative offset (x=%d y=%d)", vk_format_name(dst->format), x, y);
        goto out;
    }
    vk_texture_level_extent(dst, level, &ew, &eh, &ed);
    uint32_t w = width, h = height, d = dst->target == GFX_TEXTURE_3D ? ed : 1;
    int32_t z = 0;
    uint32_t cut_x = 0, cut_y = 0;
    if (!vk_clamp_region(&x, &y, &z, &w, &h, &d, ew, eh, ed,
                         &cut_x, &cut_y, NULL)) {
        vk_log("ERROR texture upload: region is outside %ux%ux%u", ew, eh, ed);
        goto out;
    }
    if (!vk_texture_region_check_blocks(dst, x, y, 0, w, h, "texture upload"))
        goto out;
    uint32_t req_w = width ? width : w + cut_x;
    uint32_t req_h = height ? height : h + cut_y;
    size_t row = 0, src_row = 0;
    uint32_t rows = 0, src_rows = 0, bpp = 0, bw = 1, bh = 1;
    if (!vk_texture_packed_geometry(dst->format, w, h, &row, &rows, &bpp, &bw, &bh) ||
        !vk_texture_packed_geometry(dst->format, req_w, req_h, &src_row, &src_rows,
                                    NULL, NULL, NULL)) {
        vk_log("ERROR texture upload: %s region %ux%u is not block-aligned",
               vk_format_name(dst->format), req_w, req_h);
        goto out;
    }
    size_t need = src_row * src_rows * d;
    if (size < need) {
        vk_log("ERROR texture upload: data size=%zu < %zu (req %ux%u depth=%u)",
               size, need, req_w, req_h, d);
        goto out;
    }
    size_t total = row * rows * d;
    if (!vk_create_host_buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               &staging, &stage_mem))
        goto out;
    void *mapped = NULL;
    if (!vk_ok(vkMapMemory(g.dev, stage_mem, 0, VK_WHOLE_SIZE, 0, &mapped),
               "vkMapMemory(texture upload)"))
        goto out;
    for (uint32_t zz = 0; zz < d; zz++)
        for (uint32_t yy = 0; yy < rows; yy++)
            memcpy((uint8_t *)mapped + ((size_t)zz * rows + yy) * row,
                   (const uint8_t *)data + (size_t)zz * src_row * src_rows +
                       (size_t)(yy + cut_y / bh) * src_row +
                       (size_t)(cut_x / bw) * bpp,
                   row);
    vkUnmapMemory(g.dev, stage_mem);

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(texture upload)"))
        goto out;
    VkImageAspectFlags aspect = vk_format_copy_aspect(dst->format);
    VkAccessFlags prior_access = dst->layout == VK_IMAGE_LAYOUT_UNDEFINED
        ? 0
        : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
           VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkPipelineStageFlags prior_stage = dst->layout == VK_IMAGE_LAYOUT_UNDEFINED
        ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkBufferMemoryBarrier bmb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = staging, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &bmb, 0, NULL);
    vk_image_barrier_range(cmd, dst->image, dst->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           prior_access, VK_ACCESS_TRANSFER_WRITE_BIT, prior_stage,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, aspect, level, 1,
                           dst->target == GFX_TEXTURE_3D ? 0 : layer, 1);
    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = aspect,
            .mipLevel = level,
            .baseArrayLayer = dst->target == GFX_TEXTURE_3D ? 0 : layer,
            .layerCount = 1,
        },
        .imageOffset = { x, y, 0 },
        .imageExtent = { w, h, d },
    };
    vkCmdCopyBufferToImage(cmd, staging, dst->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    vk_image_barrier_range(cmd, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, aspect, level, 1,
                           dst->target == GFX_TEXTURE_3D ? 0 : layer, 1);
    if (!vk_submit_and_wait(cmd, "texture upload")) goto out;
    dst->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ok = 1;
    vk_log("texture upload %s level=%u layer=%u rect=(%d,%d %ux%u) depth=%u bytes=%zu",
           vk_format_name(dst->format), level, layer, x, y, w, h, d, total);
out:
    if (staging) vkDestroyBuffer(g.dev, staging, NULL);
    if (stage_mem) vkFreeMemory(g.dev, stage_mem, NULL);
    pthread_mutex_unlock(&g_lock);
    return ok;
}
