/* Module: texture_0. */
#include "gfx_internal.h"

int vk_image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                            VkImageLayout new_layout, VkAccessFlags src_access,
                            VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                            VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
    return 1;
}

int vk_image_barrier_range(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                                  VkImageLayout new_layout, VkAccessFlags src_access,
                                  VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                                  VkPipelineStageFlags dst_stage, VkImageAspectFlags aspect,
                                  uint32_t base_level, uint32_t level_count,
                                  uint32_t base_layer, uint32_t layer_count) {
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = aspect,
            .baseMipLevel = base_level,
            .levelCount = level_count,
            .baseArrayLayer = base_layer,
            .layerCount = layer_count,
        },
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
    return 1;
}

void vk_mip_extent(const GfxTextureDesc *d, uint32_t level,
                          uint32_t *w, uint32_t *h, uint32_t *depth) {
    *w = d->width >> level; if (!*w) *w = 1;
    *h = d->height >> level; if (!*h) *h = 1;
    *depth = d->target == GFX_TEXTURE_3D ? (d->depth >> level) : 1;
    if (!*depth) *depth = 1;
}

/* Tightly packed size of one mip slice (all depth slices for a 3D image). */
size_t vk_texture_slice_size(const GfxTextureDesc *d, uint32_t level) {
    uint32_t w, h, depth, bw, bh, bytes;
    if (!vk_format_block((VkFormat)d->vk_format, &bw, &bh, &bytes)) return 0;
    vk_mip_extent(d, level, &w, &h, &depth);
    size_t bx = (w + bw - 1) / bw;
    size_t by = (h + bh - 1) / bh;
    return bx * by * bytes * depth;
}

GfxTexture *texture_create_desc_locked(const GfxTextureDesc *desc,
                                              const GfxTextureSubresource *subs,
                                              uint32_t count) {
    if (!desc || !desc->width || !desc->height || !desc->mip_levels || !desc->array_layers) {
        vk_log("ERROR texture desc: width=%u height=%u mips=%u layers=%u",
               desc ? desc->width : 0, desc ? desc->height : 0,
               desc ? desc->mip_levels : 0, desc ? desc->array_layers : 0);
        return NULL;
    }
    VkFormat fmt = (VkFormat)desc->vk_format;
    VkFormatInfo info = vk_format_info(fmt);
    int tex_trace = getenv("TOTK_ANDROID_TRACE_TEX") != NULL;
    if (tex_trace)
        vk_log("texture desc begin target=%u fmt=%s(%d) %ux%u depth=%u mips=%u layers=%u "
               "samples=%u subresources=%u",
               desc->target, info.name, (int)fmt, desc->width, desc->height, desc->depth,
               desc->mip_levels, desc->array_layers, desc->samples, count);
    uint32_t bw = 0, bh = 0, block_bytes = 0;
    if (!vk_format_block(fmt, &bw, &bh, &block_bytes)) {
        vk_log("ERROR texture desc: unsupported VkFormat %d", (int)fmt);
        return NULL;
    }
    if (desc->mip_levels > 32 || desc->array_layers > 2048 || desc->depth > 2048) {
        vk_log("ERROR texture desc: mips=%u layers=%u depth=%u", desc->mip_levels,
               desc->array_layers, desc->depth);
        return NULL;
    }
    VkSampleCountFlagBits samples = vk_sample_count(desc->samples ? desc->samples : 1);
    if (!samples) {
        vk_log("ERROR texture desc: unsupported sample count %u", desc->samples);
        return NULL;
    }

    VkImageType image_type = VK_IMAGE_TYPE_2D;
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    VkImageCreateFlags flags = 0;
    uint32_t depth = 1;
    switch (desc->target) {
    case GFX_TEXTURE_2D:
        if (desc->array_layers != 1 || desc->depth > 1) {
            vk_log("ERROR texture desc: 2D texture with layers=%u depth=%u",
                   desc->array_layers, desc->depth);
            return NULL;
        }
        break;
    case GFX_TEXTURE_3D:
        if (desc->array_layers != 1 || desc->depth < 1) {
            vk_log("ERROR texture desc: 3D texture with layers=%u depth=%u",
                   desc->array_layers, desc->depth);
            return NULL;
        }
        image_type = VK_IMAGE_TYPE_3D;
        view_type = VK_IMAGE_VIEW_TYPE_3D;
        depth = desc->depth;
        break;
    case GFX_TEXTURE_CUBE:
        if (desc->array_layers != 6 || desc->width != desc->height || desc->depth > 1) {
            vk_log("ERROR texture desc: cube layers=%u size=%ux%u depth=%u",
                   desc->array_layers, desc->width, desc->height, desc->depth);
            return NULL;
        }
        flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        view_type = VK_IMAGE_VIEW_TYPE_CUBE;
        break;
    case GFX_TEXTURE_2D_ARRAY:
        if (desc->depth > 1) {
            vk_log("ERROR texture desc: 2D array depth=%u", desc->depth);
            return NULL;
        }
        view_type = desc->array_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                           : VK_IMAGE_VIEW_TYPE_2D;
        break;
    case GFX_TEXTURE_CUBE_ARRAY:
        if (desc->array_layers < 6 || (desc->array_layers % 6) ||
            desc->width != desc->height || desc->depth > 1) {
            vk_log("ERROR texture desc: cube array layers=%u size=%ux%u depth=%u",
                   desc->array_layers, desc->width, desc->height, desc->depth);
            return NULL;
        }
        flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        view_type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        break;
    default:
        vk_log("ERROR texture desc: unknown target %u", desc->target);
        return NULL;
    }

    if (samples != VK_SAMPLE_COUNT_1_BIT && count) {
        vk_log("ERROR texture desc: MSAA texture (%u samples) cannot be uploaded directly",
               desc->samples);
        return NULL;
    }
    VkFormatFeatureFlags features = 0;
    if (samples != VK_SAMPLE_COUNT_1_BIT) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g.phys, fmt, &fp);
        features = fp.optimalTilingFeatures;
        if (!(features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            vk_log("ERROR texture desc: %s cannot be sampled with %u samples (optimal=%#x)",
                   info.name, desc->samples, features);
            return NULL;
        }
    } else if (!vk_format_can_texture(fmt, &features)) {
        vk_log("ERROR texture desc: %s lacks sampled/transfer features (optimal=%#x)",
               info.name, features);
        return NULL;
    }

    GfxTexture *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->width = desc->width;
    t->height = desc->height;
    t->depth = depth;
    t->mip_levels = desc->mip_levels;
    t->array_layers = desc->array_layers;
    t->samples = desc->samples ? desc->samples : 1;
    t->target = (GfxTextureTarget)desc->target;
    t->format = fmt;
    t->view_type = view_type;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    t->readback_ok = samples == VK_SAMPLE_COUNT_1_BIT &&
                     (features & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0;
    t->transfer_dst_ok = samples == VK_SAMPLE_COUNT_1_BIT &&
                         (features & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0;
    t->storage_ok = samples == VK_SAMPLE_COUNT_1_BIT &&
                    !vk_format_is_compressed(fmt) && !vk_format_is_depth(fmt) &&
                    (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;

    VkImageUsageFlags usage;
    if (samples == VK_SAMPLE_COUNT_1_BIT) {
        usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (t->readback_ok) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (t->storage_ok) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    } else {
        usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        if (vk_format_is_color_target(fmt)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (!vk_create_device_image(image_type, fmt, desc->width, desc->height, depth,
                                desc->mip_levels, desc->array_layers, samples,
                                usage, flags, &t->image, &t->mem))
        goto fail;
    if (tex_trace) vk_log("texture desc image ok image=%p", (void *)t->image);
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = t->image,
        .viewType = view_type,
        .format = fmt,
        .subresourceRange = {
            .aspectMask = vk_format_view_aspect(fmt),
            .baseMipLevel = 0,
            .levelCount = desc->mip_levels,
            .baseArrayLayer = 0,
            .layerCount = image_type == VK_IMAGE_TYPE_3D ? 1 : desc->array_layers,
        },
    };
    if (!vk_ok(vkCreateImageView(g.dev, &vci, NULL, &t->view), "vkCreateImageView(texture)"))
        goto fail;
    if (tex_trace) vk_log("texture desc view ok view=%p", (void *)t->view);
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = desc->mip_levels > 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                           : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .minLod = 0,
        .maxLod = desc->mip_levels > 1 ? (float)(desc->mip_levels - 1) : 0,
    };
    if (!vk_ok(vkCreateSampler(g.dev, &sci, NULL, &t->sampler), "vkCreateSampler(texture)"))
        goto fail;
    if (tex_trace) vk_log("texture desc sampler ok sampler=%p", (void *)t->sampler);

    if (count) {
        if (tex_trace) vk_log("texture desc upload begin count=%u", count);
        size_t *offsets = calloc(count, sizeof *offsets);
        if (!offsets) goto fail;
        size_t total = 0;
        for (uint32_t i = 0; i < count; i++) {
            const GfxTextureSubresource *s = &subs[i];
            if (!s->data || s->level >= desc->mip_levels || s->layer >= desc->array_layers ||
                (image_type == VK_IMAGE_TYPE_3D && s->layer != 0)) {
                vk_log("ERROR texture subresource %u: level=%u/%u layer=%u/%u data=%p",
                       i, s->level, desc->mip_levels, s->layer, desc->array_layers, s->data);
                free(offsets);
                goto fail;
            }
            size_t slice = vk_texture_slice_size(desc, s->level);
            if (!slice || s->size < slice) {
                vk_log("ERROR texture subresource %u: size=%zu < %zu", i, s->size, slice);
                free(offsets);
                goto fail;
            }
            size_t align = block_bytes ? block_bytes : 1;
            while (align % 4) align += block_bytes ? block_bytes : 1;
            offsets[i] = (total + align - 1) / align * align;
            total = offsets[i] + slice;
        }
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory stage_mem = VK_NULL_HANDLE;
        if (!vk_create_host_buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   &staging, &stage_mem)) {
            free(offsets);
            goto fail;
        }
        void *mapped = NULL;
        if (!vk_ok(vkMapMemory(g.dev, stage_mem, 0, VK_WHOLE_SIZE, 0, &mapped),
                   "vkMapMemory(texture upload)")) {
            free(offsets);
            vkDestroyBuffer(g.dev, staging, NULL);
            vkFreeMemory(g.dev, stage_mem, NULL);
            goto fail;
        }
        for (uint32_t i = 0; i < count; i++)
            memcpy((uint8_t *)mapped + offsets[i], subs[i].data,
                   vk_texture_slice_size(desc, subs[i].level));
        vkUnmapMemory(g.dev, stage_mem);

        VkCommandBuffer cmd = g.cmd;
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(texture upload)")) {
            free(offsets);
            vkDestroyBuffer(g.dev, staging, NULL);
            vkFreeMemory(g.dev, stage_mem, NULL);
            goto fail;
        }
        VkBufferMemoryBarrier bmb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = staging, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 1, &bmb, 0, NULL);
        vk_image_barrier_range(cmd, t->image, VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                               VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               vk_format_copy_aspect(fmt), 0, desc->mip_levels,
                               0, image_type == VK_IMAGE_TYPE_3D ? 1 : desc->array_layers);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t w, h, d;
            vk_mip_extent(desc, subs[i].level, &w, &h, &d);
            VkBufferImageCopy region = {
                .bufferOffset = offsets[i],
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource = {
                    .aspectMask = vk_format_copy_aspect(fmt),
                    .mipLevel = subs[i].level,
                    .baseArrayLayer = image_type == VK_IMAGE_TYPE_3D ? 0 : subs[i].layer,
                    .layerCount = 1,
                },
                .imageOffset = { 0, 0, 0 },
                .imageExtent = { w, h, d },
            };
            vkCmdCopyBufferToImage(cmd, staging, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &region);
        }
        vk_image_barrier_range(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                               vk_format_copy_aspect(fmt), 0, desc->mip_levels,
                               0, image_type == VK_IMAGE_TYPE_3D ? 1 : desc->array_layers);
        int submitted = vk_submit_and_wait(cmd, "texture upload");
        free(offsets);
        vkDestroyBuffer(g.dev, staging, NULL);
        vkFreeMemory(g.dev, stage_mem, NULL);
        if (!submitted) goto fail;
        if (tex_trace) vk_log("texture desc upload ok total=%zu", total);
        t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    vk_log("texture %ux%ux%u target=%u %s mips=%u layers=%u samples=%u image=%p",
           desc->width, desc->height, depth, desc->target, info.name,
           desc->mip_levels, desc->array_layers, t->samples, (void *)t->image);
    return t;

fail:
    gfx_backend_texture_destroy(t);
    return NULL;
}

GfxTexture *texture_create_rgba8_locked(uint32_t width, uint32_t height,
                                               const uint8_t *pixels) {
    if (!width || !height || width > 8192 || height > 8192 || !pixels) return NULL;
    if (!vk_init_locked()) return NULL;
    GfxTextureDesc desc = {
        .target = GFX_TEXTURE_2D,
        .vk_format = VK_FORMAT_R8G8B8A8_UNORM,
        .width = width,
        .height = height,
        .depth = 1,
        .mip_levels = 1,
        .array_layers = 1,
        .samples = 1,
    };
    GfxTextureSubresource sub = {
        .level = 0,
        .layer = 0,
        .data = pixels,
        .size = (size_t)width * height * 4,
    };
    return texture_create_desc_locked(&desc, &sub, 1);
}

GfxTexture *gfx_backend_texture_create_rgba8(uint32_t width, uint32_t height,
                                             const uint8_t *pixels) {
    pthread_mutex_lock(&g_lock);
    GfxTexture *t = texture_create_rgba8_locked(width, height, pixels);
    pthread_mutex_unlock(&g_lock);
    return t;
}

GfxTexture *gfx_backend_texture_create_desc(const GfxTextureDesc *desc,
                                            const GfxTextureSubresource *subresources,
                                            uint32_t count) {
    if (!desc) return NULL;
    pthread_mutex_lock(&g_lock);
    if (!vk_init_locked()) { pthread_mutex_unlock(&g_lock); return NULL; }
    GfxTexture *t = texture_create_desc_locked(desc, subresources, count);
    pthread_mutex_unlock(&g_lock);
    return t;
}
