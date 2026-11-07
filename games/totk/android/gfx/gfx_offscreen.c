/* Module: offscreen. */
#include "gfx_internal.h"

static unsigned g_bad_color_offscreen_logs;

GfxOffscreenTarget *vk_offscreen_create_layout_locked(uint32_t width, uint32_t height,
                                                             const GfxRenderTargetLayout *layout) {
    if (!layout || layout->color_count < 1 || layout->color_count > GFX_MAX_COLOR_TARGETS) {
        vk_log("ERROR offscreen layout: color_count=%u", layout ? layout->color_count : 0);
        return NULL;
    }
    if (!width || !height || width > 16384 || height > 16384) {
        vk_log("ERROR offscreen size %ux%u", width, height);
        return NULL;
    }
    if (layout->color_count)
        for (uint32_t i = 0; i < layout->color_count; i++)
            vk_rt_dim_hint_record(layout->color_formats[i], width, height);
    for (uint32_t i = 0; i < layout->color_count; i++) {
        VkFormat fmt = (VkFormat)layout->color_formats[i];
        VkFormatInfo info = vk_format_info(fmt);
        if (fmt == VK_FORMAT_UNDEFINED || !info.bpp) {
            vk_log("ERROR offscreen attachment %u: unsupported format %d", i, (int)fmt);
            return NULL;
        }
        if (!vk_format_is_color_target(fmt) || info.bpp < 1 || info.bpp > 16) {
            vk_log_bad_color_attachment("offscreen", i, fmt, layout, width, height,
                                        &g_bad_color_offscreen_logs);
            return NULL;
        }
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g.phys, fmt, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((fp.optimalTilingFeatures & need) != need) {
            vk_log("ERROR offscreen attachment %u: %s lacks color/transfer feature (optimal=%#x)",
                   i, info.name, fp.optimalTilingFeatures);
            return NULL;
        }
    }
    VkFormat depth_fmt = (VkFormat)layout->depth_format;
    if (layout->depth_format) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g.phys, depth_fmt, &fp);
        if (!vk_format_is_depth(depth_fmt) ||
            !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            vk_log("ERROR offscreen depth format %d (%s) is not a depth attachment",
                   (int)depth_fmt, vk_format_name(depth_fmt));
            return NULL;
        }
    }

    GfxOffscreenTarget *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->rt_layout = *layout;
    t->width = width;
    t->height = height;
    t->layout_state = VK_IMAGE_LAYOUT_UNDEFINED;

    for (uint32_t i = 0; i < layout->color_count; i++) {
        VkFormat fmt = (VkFormat)layout->color_formats[i];
        VkFormatInfo info = vk_format_info(fmt);
        VkFormatProperties fmt_props;
        vkGetPhysicalDeviceFormatProperties(g.phys, fmt, &fmt_props);
        size_t bytes = (size_t)width * height * info.bpp;
        t->color_format[i] = fmt;
        t->color_bpp[i] = info.bpp;
        t->color_size[i] = bytes;
        t->color_pixels[i] = calloc(1, bytes ? bytes : 1);
        if (!t->color_pixels[i]) goto fail;
        if (!vk_create_device_image(VK_IMAGE_TYPE_2D, fmt, width, height, 1, 1, 1,
                                    VK_SAMPLE_COUNT_1_BIT,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    ((fmt_props.optimalTilingFeatures &
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
                                         ? VK_IMAGE_USAGE_SAMPLED_BIT : 0),
                                    0, &t->color_image[i], &t->color_mem[i]))
            goto fail;
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = t->color_image[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = fmt,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 1,
            },
        };
        if (!vk_ok(vkCreateImageView(g.dev, &vci, NULL, &t->color_view[i]),
                   "vkCreateImageView(offscreen color)"))
            goto fail;
        if (!vk_create_host_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   &t->color_readback[i], &t->color_readback_mem[i]))
            goto fail;
    }

    if (layout->depth_format) {
        t->depth_format = depth_fmt;
        if (!vk_create_device_image(VK_IMAGE_TYPE_2D, depth_fmt, width, height, 1, 1, 1,
                                    VK_SAMPLE_COUNT_1_BIT,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                    0, &t->depth_image, &t->depth_mem))
            goto fail;
        VkImageViewCreateInfo dvci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = t->depth_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = depth_fmt,
            .subresourceRange = {
                .aspectMask = vk_format_is_depth(depth_fmt)
                                  ? (VkImageAspectFlags)(vk_format_copy_aspect(depth_fmt) &
                                                         (VK_IMAGE_ASPECT_DEPTH_BIT |
                                                          VK_IMAGE_ASPECT_STENCIL_BIT))
                                  : VK_IMAGE_ASPECT_DEPTH_BIT,
                .baseMipLevel = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 1,
            },
        };
        if (!vk_ok(vkCreateImageView(g.dev, &dvci, NULL, &t->depth_view),
                   "vkCreateImageView(offscreen depth)"))
            goto fail;
    }
    return t;

fail:
    gfx_backend_offscreen_destroy(t);
    return NULL;
}

GfxOffscreenTarget *gfx_backend_offscreen_create_layout(uint32_t width, uint32_t height,
                                                        const GfxRenderTargetLayout *layout) {
    pthread_mutex_lock(&g_lock);
    if (!vk_init_locked()) { pthread_mutex_unlock(&g_lock); return NULL; }
    GfxOffscreenTarget *t = vk_offscreen_create_layout_locked(width, height, layout);
    if (t) {
        vk_log("offscreen %ux%u color_count=%u layouts=%s,%s depth=%s image=%p",
               width, height, t->rt_layout.color_count,
               vk_format_name(t->color_format[0]),
               t->rt_layout.color_count > 1 ? vk_format_name(t->color_format[1]) : "-",
               t->depth_view ? vk_format_name(t->depth_format) : "none",
               (void *)t->color_image[0]);
    }
    pthread_mutex_unlock(&g_lock);
    return t;
}

GfxOffscreenTarget *gfx_backend_offscreen_create(uint32_t width, uint32_t height,
                                                  uint32_t nvn_format) {
    pthread_mutex_lock(&g_lock);
    if (!vk_init_locked()) { pthread_mutex_unlock(&g_lock); return NULL; }
    VkFormat fmt = (VkFormat)gfx_backend_vk_format_for_nvn(nvn_format);
    VkFormatInfo info = vk_format_info(fmt);
    if (fmt == VK_FORMAT_UNDEFINED || !info.bpp) {
        vk_log("ERROR no Vulkan format for NVN format %#x", nvn_format);
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    if (!vk_format_is_color_target(fmt) || info.bpp < 1 || info.bpp > 16) {
        vk_log("ERROR unsupported offscreen color format %s bpp=%u", info.name, info.bpp);
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    GfxRenderTargetLayout layout = { 0 };
    layout.color_count = 1;
    layout.color_formats[0] = (int)fmt;
    layout.depth_format = (int)g.depth_format;
    GfxOffscreenTarget *t = vk_offscreen_create_layout_locked(width, height, &layout);
    if (t)
        vk_log("offscreen %ux%u nvn=%#x vk=%s(%d) bpp=%u image=%p",
               width, height, nvn_format, info.name, (int)fmt, info.bpp,
               (void *)t->color_image[0]);
    pthread_mutex_unlock(&g_lock);
    return t;
}

void gfx_backend_offscreen_destroy(GfxOffscreenTarget *t) {
    if (!t) return;
    /* destroy is also called from the create failure path while g_lock is held;
     * keep it lock-free and only touch per-target objects. */
    if (g.dev) {
        for (uint32_t i = 0; i < t->rt_layout.color_count; i++) {
            if (t->color_readback[i]) vkDestroyBuffer(g.dev, t->color_readback[i], NULL);
            if (t->color_readback_mem[i]) vkFreeMemory(g.dev, t->color_readback_mem[i], NULL);
            if (t->color_view[i]) vkDestroyImageView(g.dev, t->color_view[i], NULL);
            if (t->color_image[i]) vkDestroyImage(g.dev, t->color_image[i], NULL);
            if (t->color_mem[i]) vkFreeMemory(g.dev, t->color_mem[i], NULL);
            free(t->color_pixels[i]);
        }
        if (t->depth_view) vkDestroyImageView(g.dev, t->depth_view, NULL);
        if (t->depth_image) vkDestroyImage(g.dev, t->depth_image, NULL);
        if (t->depth_mem) vkFreeMemory(g.dev, t->depth_mem, NULL);
    } else {
        for (uint32_t i = 0; i < t->rt_layout.color_count; i++) free(t->color_pixels[i]);
    }
    free(t);
}

int gfx_backend_offscreen_clear(GfxOffscreenTarget *t, float r, float g_, float b, float a) {
    if (!t) return 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready || !g.cmd) { pthread_mutex_unlock(&g_lock); return 0; }

    VkCommandBuffer cmd = g.cmd;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(clear)")) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    VkClearColorValue color = { .float32 = { r, g_, b, a } };
    VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1,
    };
    for (uint32_t i = 0; i < t->rt_layout.color_count; i++) {
        if (t->layout_state != VK_IMAGE_LAYOUT_GENERAL) {
            vk_image_barrier_range(cmd, t->color_image[i], t->layout_state, VK_IMAGE_LAYOUT_GENERAL,
                                   0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                   t->layout_state == VK_IMAGE_LAYOUT_UNDEFINED
                                       ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                       : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
        }
        vkCmdClearColorImage(cmd, t->color_image[i], VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
        VkBufferImageCopy region = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
            .imageOffset = { 0, 0, 0 },
            .imageExtent = { t->width, t->height, 1 },
        };
        vkCmdCopyImageToBuffer(cmd, t->color_image[i], VK_IMAGE_LAYOUT_GENERAL,
                               t->color_readback[i], 1, &region);
        VkBufferMemoryBarrier bb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = t->color_readback[i],
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, NULL, 1, &bb, 0, NULL);
    }
    if (!vk_submit_and_wait(cmd, "clear")) { pthread_mutex_unlock(&g_lock); return 0; }
    t->layout_state = VK_IMAGE_LAYOUT_GENERAL;

    for (uint32_t i = 0; i < t->rt_layout.color_count; i++) {
        void *mapped = NULL;
        if (!vk_ok(vkMapMemory(g.dev, t->color_readback_mem[i], 0, VK_WHOLE_SIZE, 0, &mapped),
                   "vkMapMemory(clear readback)")) {
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        memcpy(t->color_pixels[i], mapped, t->color_size[i]);
        vkUnmapMemory(g.dev, t->color_readback_mem[i]);
    }
    pthread_mutex_unlock(&g_lock);
    if (getenv("TOTK_ANDROID_TRACE_VK"))
        vk_log("clear %ux%u colors=%u color=(%.3f,%.3f,%.3f,%.3f) first=%02x%02x%02x%02x",
               t->width, t->height, t->rt_layout.color_count, r, g_, b, a,
               t->color_pixels[0][0], t->color_pixels[0][1],
               t->color_pixels[0][2], t->color_pixels[0][3]);
    return 1;
}

int gfx_backend_offscreen_read_pixel_index(const GfxOffscreenTarget *t, uint32_t index,
                                           uint32_t x, uint32_t y, uint8_t out[4]) {
    if (!t || !out || index >= t->rt_layout.color_count ||
        x >= t->width || y >= t->height) return 0;
    uint32_t bpp = t->color_bpp[index];
    size_t off = ((size_t)y * t->width + x) * bpp;
    if (off + bpp > t->color_size[index]) return 0;
    memset(out, 0, 4);
    memcpy(out, t->color_pixels[index] + off, bpp < 4 ? bpp : 4);
    return 1;
}

int gfx_backend_offscreen_read_pixel(GfxOffscreenTarget *t, uint32_t x, uint32_t y,
                                     uint8_t out[4]) {
    return gfx_backend_offscreen_read_pixel_index(t, 0, x, y, out);
}

uint32_t gfx_backend_offscreen_color_count(const GfxOffscreenTarget *t) {
    return t ? t->rt_layout.color_count : 0;
}

const GfxRenderTargetLayout *gfx_backend_offscreen_layout(const GfxOffscreenTarget *t) {
    return t ? &t->rt_layout : NULL;
}

int gfx_backend_offscreen_present(GfxOffscreenTarget *t) {
    if (!t) return 0;
    t->presents++;
    vk_log("present #%u offscreen %ux%u %s colors=%u", t->presents, t->width, t->height,
           vk_format_name(t->color_format[0]), t->rt_layout.color_count);
    return 1;
}

uint32_t gfx_backend_offscreen_width(const GfxOffscreenTarget *t) { return t ? t->width : 0; }

uint32_t gfx_backend_offscreen_height(const GfxOffscreenTarget *t) { return t ? t->height : 0; }

const uint8_t *gfx_backend_offscreen_pixels(const GfxOffscreenTarget *t, size_t *size) {
    if (size) *size = t ? t->color_size[0] : 0;
    return t ? t->color_pixels[0] : NULL;
}

const char *gfx_backend_offscreen_format_name(const GfxOffscreenTarget *t) {
    return t ? vk_format_name(t->color_format[0]) : "none";
}

int gfx_backend_offscreen_write_ppm_index(const GfxOffscreenTarget *t, uint32_t index,
                                          const char *path) {
    if (!t || !path || index >= t->rt_layout.color_count ||
        t->color_bpp[index] != 4 || !t->color_pixels[index]) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) { vk_log("ERROR screenshot open %s", path); return 0; }
    fprintf(f, "P6\n%u %u\n255\n", t->width, t->height);
    size_t n = (size_t)t->width * t->height;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = t->color_pixels[index] + i * 4;
        fwrite(p, 1, 3, f);
    }
    int ok = ferror(f) == 0;
    fclose(f);
    if (ok) vk_log("screenshot %s (%ux%u attachment=%u, %zu bytes)", path, t->width,
                   t->height, index, n * 3);
    else vk_log("ERROR screenshot write %s", path);
    return ok;
}

int gfx_backend_offscreen_write_ppm(const GfxOffscreenTarget *t, const char *path) {
    return gfx_backend_offscreen_write_ppm_index(t, 0, path);
}

uint64_t gfx_backend_offscreen_hash(const GfxOffscreenTarget *t) {
    if (!t || !t->color_pixels[0]) return 0;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < t->color_size[0]; i++) {
        h ^= t->color_pixels[0][i];
        h *= 1099511628211ull;
    }
    return h;
}
