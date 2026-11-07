/* Module: texture_3. */
#include "gfx_internal.h"

int gfx_backend_texture_download(const GfxTexture *src, uint32_t level, uint32_t layer,
                                 int32_t x, int32_t y, uint32_t width, uint32_t height,
                                 void *dst, size_t size) {
    if (!src || !dst || !size) return 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready || !g.cmd) { pthread_mutex_unlock(&g_lock); return 0; }

    int ok = 0;
    VkCommandBuffer cmd = g.cmd;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stage_mem = VK_NULL_HANDLE;
    uint32_t ew = 0, eh = 0, ed = 0;
    if (!vk_texture_transfer_check(src, level, layer, "texture download", 0)) goto out;
    if (!src->readback_ok) {
        vk_log("ERROR texture download: %s was created without transfer-src support",
               vk_format_name(src->format));
        goto out;
    }
    int compressed = vk_format_is_compressed(src->format);
    if (compressed && (x < 0 || y < 0)) {
        vk_log("ERROR texture download: compressed %s region cannot start at a "
               "negative offset (x=%d y=%d)", vk_format_name(src->format), x, y);
        goto out;
    }
    vk_texture_level_extent(src, level, &ew, &eh, &ed);
    uint32_t w = width, h = height, d = src->target == GFX_TEXTURE_3D ? ed : 1;
    int32_t z = 0;
    uint32_t cut_x = 0, cut_y = 0;
    if (!vk_clamp_region(&x, &y, &z, &w, &h, &d, ew, eh, ed,
                         &cut_x, &cut_y, NULL)) {
        vk_log("ERROR texture download: region is outside %ux%ux%u", ew, eh, ed);
        goto out;
    }
    if (!vk_texture_region_check_blocks(src, x, y, 0, w, h, "texture download"))
        goto out;
    uint32_t req_w = width ? width : w + cut_x;
    uint32_t req_h = height ? height : h + cut_y;
    size_t row = 0, dst_row = 0;
    uint32_t rows = 0, dst_rows = 0, bpp = 0, bw = 1, bh = 1;
    if (!vk_texture_packed_geometry(src->format, w, h, &row, &rows, &bpp, &bw, &bh) ||
        !vk_texture_packed_geometry(src->format, req_w, req_h, &dst_row, &dst_rows,
                                    NULL, NULL, NULL)) {
        vk_log("ERROR texture download: %s region %ux%u is not block-aligned",
               vk_format_name(src->format), req_w, req_h);
        goto out;
    }
    size_t need = dst_row * dst_rows * d;
    if (size < need) {
        vk_log("ERROR texture download: dst size=%zu < %zu (req %ux%u depth=%u)",
               size, need, req_w, req_h, d);
        goto out;
    }
    size_t total = row * rows * d;
    if (!vk_create_host_buffer(total, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               &staging, &stage_mem))
        goto out;

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(texture download)"))
        goto out;
    VkImageAspectFlags aspect = vk_format_copy_aspect(src->format);
    VkAccessFlags prior_access = src->layout == VK_IMAGE_LAYOUT_UNDEFINED
        ? 0
        : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
           VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkPipelineStageFlags prior_stage = src->layout == VK_IMAGE_LAYOUT_UNDEFINED
        ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    vk_image_barrier_range(cmd, src->image, src->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           prior_access, VK_ACCESS_TRANSFER_READ_BIT, prior_stage,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, aspect, level, 1,
                           src->target == GFX_TEXTURE_3D ? 0 : layer, 1);
    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = aspect,
            .mipLevel = level,
            .baseArrayLayer = src->target == GFX_TEXTURE_3D ? 0 : layer,
            .layerCount = 1,
        },
        .imageOffset = { x, y, 0 },
        .imageExtent = { w, h, d },
    };
    vkCmdCopyImageToBuffer(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging, 1, &region);
    VkBufferMemoryBarrier bmb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = staging, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bmb, 0, NULL);
    vk_image_barrier_range(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, aspect, level, 1,
                           src->target == GFX_TEXTURE_3D ? 0 : layer, 1);
    if (!vk_submit_and_wait(cmd, "texture download")) goto out;
    ((GfxTexture *)src)->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    void *mapped = NULL;
    if (!vk_ok(vkMapMemory(g.dev, stage_mem, 0, VK_WHOLE_SIZE, 0, &mapped),
               "vkMapMemory(texture download)"))
        goto out;
    for (uint32_t zz = 0; zz < d; zz++)
        for (uint32_t yy = 0; yy < rows; yy++)
            memcpy((uint8_t *)dst + (size_t)zz * dst_row * dst_rows +
                       (size_t)(yy + cut_y / bh) * dst_row +
                       (size_t)(cut_x / bw) * bpp,
                   (const uint8_t *)mapped + ((size_t)zz * rows + yy) * row,
                   row);
    vkUnmapMemory(g.dev, stage_mem);
    ok = 1;
    vk_log("texture download %s level=%u layer=%u rect=(%d,%d %ux%u) depth=%u bytes=%zu",
           vk_format_name(src->format), level, layer, x, y, w, h, d, total);
out:
    if (staging) vkDestroyBuffer(g.dev, staging, NULL);
    if (stage_mem) vkFreeMemory(g.dev, stage_mem, NULL);
    pthread_mutex_unlock(&g_lock);
    return ok;
}
