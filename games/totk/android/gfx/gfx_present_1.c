/* Module: present_1. */
#include "gfx_internal.h"

/* Record the target -> dst copy.  Leaves `dst` in `dst_final_layout` and the
 * target back in GENERAL (the layout every NVN producer leaves active).  The
 * caller owns command-buffer submission. */
int vk_present_target_locked(VkCommandBuffer cmd, GfxOffscreenTarget *t,
                                    VkImage dst, VkImageView dst_view,
                                    VkImageLayout dst_old,
                                    VkFormat dst_format, VkExtent2D extent,
                                    uint32_t scale_mode, VkImageLayout dst_final,
                                    VkFramebuffer *out_fb) {
    if (out_fb) *out_fb = VK_NULL_HANDLE;
    if (!t || t->rt_layout.color_count < 1 || !extent.width || !extent.height) return 0;
    VkFormat src_format = t->color_format[0];
    if (!src_format || src_format == VK_FORMAT_UNDEFINED) return 0;
    VkRect2D rect = vk_present_rect(t, extent, scale_mode);
    VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkFilter filter = VK_FILTER_NEAREST;
    if (vk_present_blit_ok(src_format, dst_format, &filter)) {
        vk_image_barrier(cmd, dst, dst_old, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         dst_old == VK_IMAGE_LAYOUT_UNDEFINED
                             ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                             : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);
        vk_image_barrier_range(cmd, t->color_image[0], t->layout_state,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                               VK_ACCESS_TRANSFER_READ_BIT,
                               t->layout_state == VK_IMAGE_LAYOUT_UNDEFINED
                                   ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                   : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
        vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        VkImageBlit blit = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        };
        blit.srcOffsets[1] = (VkOffset3D){ (int32_t)t->width, (int32_t)t->height, 1 };
        blit.dstOffsets[0] = (VkOffset3D){ rect.offset.x, rect.offset.y, 0 };
        blit.dstOffsets[1] = (VkOffset3D){ rect.offset.x + (int32_t)rect.extent.width,
                                           rect.offset.y + (int32_t)rect.extent.height, 1 };
        vkCmdBlitImage(cmd, t->color_image[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
        vk_image_barrier_range(cmd, t->color_image[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, 0,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
        vk_image_barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dst_final,
                         VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         dst_final == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                             ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                             : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        t->layout_state = VK_IMAGE_LAYOUT_GENERAL;
        return 1;
    }

    /* Format conversion path.  The target must be samplable and its format must
     * be representable by a float sampler; integer/compressed targets are not
     * silently converted. */
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g.phys, src_format, &fp);
    if (!vk_present_format_is_shader_src(src_format) ||
        !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        vk_log("ERROR present target: %s -> %s needs a sampled conversion the driver "
               "or format does not support", vk_format_name(src_format),
               vk_format_name(dst_format));
        return 0;
    }
    GfxPresentPipe *p = vk_present_pipe_get_locked(src_format, dst_format, dst_final);
    if (!p) return 0;

    VkDescriptorImageInfo di = {
        .sampler = p->sampler, .imageView = t->color_view[0],
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = p->set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &di,
    };
    vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);

    vk_image_barrier(cmd, dst, dst_old, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     dst_old == VK_IMAGE_LAYOUT_UNDEFINED
                         ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                         : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    vk_image_barrier_range(cmd, t->color_image[0], t->layout_state,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                           VK_ACCESS_SHADER_READ_BIT,
                           t->layout_state == VK_IMAGE_LAYOUT_UNDEFINED
                               ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                               : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);

    VkRenderPassBeginInfo rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = p->pass,
        .renderArea = { { 0, 0 }, { extent.width, extent.height } },
        .clearValueCount = 1,
        .pClearValues = (VkClearValue[]){ { .color = { .float32 = { 0, 0, 0, 1 } } } },
    };
    /* The framebuffer is transient: one per present.  It must outlive command
     * buffer submission, so ownership is handed back to the caller. */
    if (!dst_view) {
        vk_log("ERROR present target: destination image has no view");
        return 0;
    }
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkFramebufferCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = p->pass, .attachmentCount = 1, .pAttachments = &dst_view,
        .width = extent.width, .height = extent.height, .layers = 1,
    };
    if (!vk_ok(vkCreateFramebuffer(g.dev, &fci, NULL, &fb), "vkCreateFramebuffer(present)"))
        return 0;
    rp.framebuffer = fb;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1,
                            &p->set, 0, NULL);
    VkViewport vport = { (float)rect.offset.x, (float)rect.offset.y,
                         (float)rect.extent.width, (float)rect.extent.height, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &vport);
    vkCmdSetScissor(cmd, 0, 1, &rect);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    if (out_fb) *out_fb = fb;

    vk_image_barrier_range(cmd, t->color_image[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, 0,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
    t->layout_state = VK_IMAGE_LAYOUT_GENERAL;
    return 1;
}

/* Acquire one swapchain image, recreating once when the surface says the
 * swapchain is stale.  Returns the image index or -1; on -1 the caller reports
 * failure.  `suboptimal` is informational: the frame can still be presented. */
int vk_surface_acquire_locked(uint32_t *out_idx, int *out_suboptimal) {
    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t idx = 0;
        VkResult ar = vkAcquireNextImageKHR(g.dev, g.swapchain, UINT64_MAX, VK_NULL_HANDLE,
                                            g.fence, &idx);
        if (ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR) {
            VkResult wr = vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX);
            if (wr != VK_SUCCESS) {
                vk_log("ERROR vkWaitForFences(acquire): %d", (int)wr);
                return -1;
            }
            if (out_suboptimal) *out_suboptimal = ar == VK_SUBOPTIMAL_KHR;
            *out_idx = idx;
            return 0;
        }
        if (ar != VK_ERROR_OUT_OF_DATE_KHR && ar != VK_SUBOPTIMAL_KHR) {
            vk_log("ERROR vkAcquireNextImageKHR: %d", (int)ar);
            return -1;
        }
        vk_log("swapchain out of date/suboptimal during acquire; recreating");
        if (!vk_recreate_swapchain_locked()) return -1;
    }
    vk_log("ERROR swapchain still out of date after recreate");
    return -1;
}

int gfx_backend_surface_present_target(const GfxOffscreenTarget *target, uint32_t scale_mode) {
    if (!target) return 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready || !g.surface_ready || !g.swapchain) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (g.sc_suboptimal && !vk_recreate_swapchain_locked()) {
        vk_log("ERROR swapchain recreate after suboptimal present failed");
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    uint32_t idx = 0;
    int suboptimal = 0;
    if (vk_surface_acquire_locked(&idx, &suboptimal)) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (suboptimal) g.sc_suboptimal = 1;

    VkCommandBuffer cmd = g.cmd;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(surface-target)")) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    GfxOffscreenTarget *t = (GfxOffscreenTarget *)target;   /* layout_state is a cache */
    VkFramebuffer fb = VK_NULL_HANDLE;
    int ok = vk_present_target_locked(cmd, t, g.sc_images[idx], g.sc_views[idx],
                                      g.sc_layouts[idx], g.sc_format, g.sc_extent, scale_mode,
                                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, &fb);
    if (!ok) {
        vkEndCommandBuffer(cmd);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    t->layout_state = VK_IMAGE_LAYOUT_GENERAL;
    int submitted = vk_submit_and_wait(cmd, "surface-present-target");
    if (fb) vkDestroyFramebuffer(g.dev, fb, NULL);
    if (!submitted) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    g.sc_layouts[idx] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 1,
        .pSwapchains = &g.swapchain,
        .pImageIndices = &idx,
    };
    VkResult pr = vkQueuePresentKHR(g.queue, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR) {
        vk_log("swapchain out of date on present; recreating");
        g.sc_layouts[idx] = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vk_recreate_swapchain_locked())
            vk_log("swapchain recreated after out-of-date present");
        else
            vk_log("ERROR swapchain recreate after out-of-date present failed");
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (pr == VK_SUBOPTIMAL_KHR) {
        g.sc_suboptimal = 1;
        vk_log("swapchain present suboptimal; recreate scheduled");
    } else if (pr != VK_SUCCESS) {
        vk_log("ERROR vkQueuePresentKHR(target): %d", (int)pr);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    g.surface_presents++;
    if (getenv("TOTK_ANDROID_TRACE_VK"))
        vk_log("surface target present #%u image=%u target=%ux%u %s mode=%s%s",
               g.surface_presents, idx, t->width, t->height,
               vk_format_name(t->color_format[0]),
               scale_mode == GFX_SURFACE_SCALE_STRETCH ? "stretch" : "fit",
               suboptimal ? " (suboptimal)" : "");
    pthread_mutex_unlock(&g_lock);
    return 1;
}

int gfx_backend_surface_present(float r, float g_, float b, float a) {
    pthread_mutex_lock(&g_lock);
    if (!g.ready || !g.surface_ready || !g.swapchain) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (g.sc_suboptimal && !vk_recreate_swapchain_locked()) {
        vk_log("ERROR swapchain recreate after suboptimal present failed");
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    uint32_t idx = 0;
    int suboptimal = 0;
    if (vk_surface_acquire_locked(&idx, &suboptimal)) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (suboptimal) g.sc_suboptimal = 1;
    VkCommandBuffer cmd = g.cmd;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(surface)")) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    VkImageLayout old = g.sc_layouts[idx];
    vk_image_barrier(cmd, g.sc_images[idx], old, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     old == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ? 0 : 0,
                     VK_ACCESS_TRANSFER_WRITE_BIT,
                     old == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                      : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue color = { .float32 = { r, g_, b, a } };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, g.sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &color, 1, &range);
    vk_image_barrier(cmd, g.sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (!vk_submit_and_wait(cmd, "surface-present")) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 1,
        .pSwapchains = &g.swapchain,
        .pImageIndices = &idx,
    };
    VkResult pr = vkQueuePresentKHR(g.queue, &pi);
    g.sc_layouts[idx] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        vk_log("swapchain present result %d", (int)pr);
        pthread_mutex_unlock(&g_lock);
        return pr == VK_SUBOPTIMAL_KHR;
    }
    if (pr != VK_SUCCESS) {
        vk_log("ERROR vkQueuePresentKHR: %d", (int)pr);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    g.surface_presents++;
    if (getenv("TOTK_ANDROID_TRACE_VK"))
        vk_log("surface present #%u image=%u", g.surface_presents, idx);
    pthread_mutex_unlock(&g_lock);
    return 1;
}
