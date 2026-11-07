/* Module: compute_1. */
#include "gfx_internal.h"

int gfx_backend_compute_dispatch(GfxComputePipeline *pipeline,
                                 uint32_t group_x, uint32_t group_y, uint32_t group_z) {
    if (!pipeline || !group_x || !group_y || !group_z) return 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready || !g.cmd || !pipeline->pipe || !pipeline->dset) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    for (uint32_t i = 0; i < pipeline->nspec; i++) {
        if (!pipeline->bind[i].bound) {
            vk_log("ERROR compute dispatch: binding %u (kind %d) was never bound",
                   pipeline->spec[i].binding, (int)pipeline->spec[i].kind);
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
    }
    double t0 = vk_now_ms();
    VkCommandBuffer cmd = g.cmd;
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(compute dispatch)")) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipe);

    /* Make host writes to the coherent UBO/SSBO arenas visible to the shader. */
    for (uint32_t i = 0; i < pipeline->nspec; i++) {
        GfxComputeBinding *b = &pipeline->bind[i];
        if (b->kind != GFX_BIND_UBO && b->kind != GFX_BIND_SSBO) continue;
        VkBufferMemoryBarrier bmb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = b->buffer,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1, &bmb,
                             0, NULL);
    }

    /* Transition bound images to the layout their descriptor declares. */
    for (uint32_t i = 0; i < pipeline->nspec; i++) {
        GfxComputeBinding *b = &pipeline->bind[i];
        if (!b->texture) continue;
        const GfxTexture *t = b->texture;
        VkAccessFlags src_access = t->layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? 0
            : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
               VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        VkPipelineStageFlags src_stage = t->layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        vk_image_barrier_range(cmd, t->image, t->layout, b->layout,
                               src_access,
                               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                               src_stage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               vk_format_copy_aspect(t->format), 0, t->mip_levels, 0,
                               t->target == GFX_TEXTURE_3D ? 1 : t->array_layers);
    }

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout,
                            0, 1, &pipeline->dset, 0, NULL);
    vkCmdDispatch(cmd, group_x, group_y, group_z);

    /* Make shader writes visible to the host readback and to later passes. */
    for (uint32_t i = 0; i < pipeline->nspec; i++) {
        GfxComputeBinding *b = &pipeline->bind[i];
        if (b->kind != GFX_BIND_UBO && b->kind != GFX_BIND_SSBO) continue;
        VkBufferMemoryBarrier bmb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                             VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = b->buffer,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT |
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, NULL, 1, &bmb, 0, NULL);
    }
    for (uint32_t i = 0; i < pipeline->nspec; i++) {
        GfxComputeBinding *b = &pipeline->bind[i];
        if (!b->texture) continue;
        const GfxTexture *t = b->texture;
        VkImageLayout final_layout = b->kind == GFX_BIND_STORAGE_IMAGE
            ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vk_image_barrier_range(cmd, t->image, b->layout, final_layout,
                               VK_ACCESS_SHADER_WRITE_BIT,
                               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               vk_format_copy_aspect(t->format), 0, t->mip_levels, 0,
                               t->target == GFX_TEXTURE_3D ? 1 : t->array_layers);
    }

    int submitted = vk_submit_and_wait(cmd, "compute dispatch");
    if (submitted) {
        for (uint32_t i = 0; i < pipeline->nspec; i++) {
            GfxComputeBinding *b = &pipeline->bind[i];
            if (!b->texture) continue;
            VkImageLayout final_layout = b->kind == GFX_BIND_STORAGE_IMAGE
                ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ((GfxTexture *)b->texture)->layout = final_layout;
        }
        if (getenv("TOTK_ANDROID_TRACE_COMPUTE"))
            vk_log("compute dispatch groups=%ux%ux%u bindings=%u time=%.2f ms",
                   group_x, group_y, group_z, pipeline->nspec, vk_now_ms() - t0);
    }
    pthread_mutex_unlock(&g_lock);
    return submitted;
}
