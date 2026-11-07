/* Module: pipeline_triangle. */
#include "gfx_internal.h"

static GfxTrianglePipeline *g_builtin_triangle;

static GfxTexturedPipeline *g_builtin_textured;

void *vk_pipe_compile_thread(void *arg) {
    VkPipeCompileJob *job = arg;
    job->result = vkCreateGraphicsPipelines(g.dev, job->cache, 1, job->ci, NULL, job->out);
    return NULL;
}

VkResult vk_create_graphics_pipelines_bigstack(
        VkPipelineCache cache, const VkGraphicsPipelineCreateInfo *ci, VkPipeline *out) {
    VkPipeCompileJob job = { cache, ci, out, VK_ERROR_UNKNOWN };
    pthread_attr_t attr;
    pthread_t th;
    int attr_ok = pthread_attr_init(&attr) == 0;
    if (attr_ok && pthread_attr_setstacksize(&attr, 128u << 20) == 0 &&
        pthread_create(&th, &attr, vk_pipe_compile_thread, &job) == 0) {
        pthread_attr_destroy(&attr);
        pthread_join(th, NULL);
        return job.result;
    }
    if (attr_ok) pthread_attr_destroy(&attr);
    /* If a worker cannot be created, creating the pipeline inline is still
     * preferable to skipping the draw; the callers report the result. */
    return vkCreateGraphicsPipelines(g.dev, cache, 1, ci, NULL, out);
}

int vk_shader_module(const uint8_t *code, size_t size, const char *what,
                            VkShaderModule *out) {
    if (!code || size < 20 || (size & 3)) {
        vk_log("ERROR %s: invalid SPIR-V size %zu", what, size);
        return 0;
    }
    uint32_t magic;
    memcpy(&magic, code, 4);
    if (magic != 0x07230203u) {
        vk_log("ERROR %s: bad SPIR-V magic %#x", what, magic);
        return 0;
    }
    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = (const uint32_t *)code,
    };
    if (getenv("TOTK_ANDROID_TRACE_SHADER"))
        vk_log("shader module begin %s bytes=%zu", what, size);
    VkResult r = vkCreateShaderModule(g.dev, &ci, NULL, out);
    if (getenv("TOTK_ANDROID_TRACE_SHADER"))
        vk_log("shader module end %s result=%d", what, (int)r);
    return vk_ok(r, what);
}

GfxTrianglePipeline *triangle_create_impl(const uint8_t *vs, size_t vs_size,
                                                  const uint8_t *ps, size_t ps_size,
                                                  int vk_format, const GfxUboBinding *ubos,
                                                  uint32_t nubo) {
    pthread_mutex_lock(&g_lock);
    if (!vk_init_locked()) { pthread_mutex_unlock(&g_lock); return NULL; }
    VkFormat fmt = (VkFormat)vk_format;
    if (fmt == VK_FORMAT_UNDEFINED) {
        vk_log("ERROR triangle pipeline: undefined format");
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    GfxTrianglePipeline *p = calloc(1, sizeof *p);
    if (!p) { pthread_mutex_unlock(&g_lock); return NULL; }
    p->format = fmt;
    if (!vk_shader_module(vs, vs_size, "vkCreateShaderModule(triangle vs)", &p->vs) ||
        !vk_shader_module(ps, ps_size, "vkCreateShaderModule(triangle ps)", &p->ps))
        goto fail;

    VkAttachmentDescription att = {
        .format = fmt,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ref,
    };
    VkSubpassDependency dep = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &att,
        .subpassCount = 1,
        .pSubpasses = &sub,
        .dependencyCount = 1,
        .pDependencies = &dep,
    };
    if (!vk_ok(vkCreateRenderPass(g.dev, &rpci, NULL, &p->pass), "vkCreateRenderPass(triangle)"))
        goto fail;
    if (nubo) {
        if (nubo > 8) goto fail;
        VkDescriptorSetLayoutBinding b[8];
        for (uint32_t i = 0; i < nubo; i++) {
            b[i] = (VkDescriptorSetLayoutBinding){ .binding = ubos[i].binding,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT };
        }
        VkDescriptorSetLayoutCreateInfo dlci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .bindingCount = nubo, .pBindings = b };
        if (!vk_ok(vkCreateDescriptorSetLayout(g.dev, &dlci, NULL, &p->dsl), "vkCreateDescriptorSetLayout(triangle-ubo)")) goto fail;
        VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = nubo };
        VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                            .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
        if (!vk_ok(vkCreateDescriptorPool(g.dev, &dpci, NULL, &p->dpool), "vkCreateDescriptorPool(triangle-ubo)")) goto fail;
        VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                             .descriptorPool = p->dpool, .descriptorSetCount = 1, .pSetLayouts = &p->dsl };
        if (!vk_ok(vkAllocateDescriptorSets(g.dev, &dsai, &p->dset), "vkAllocateDescriptorSets(triangle-ubo)")) goto fail;
        VkDescriptorBufferInfo bi[8];
        VkWriteDescriptorSet wr[8];
        for (uint32_t i = 0; i < nubo; i++) {
            p->ubo_binding[i] = ubos[i].binding;
            VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = ubos[i].size ? ubos[i].size : 0x1000,
                                       .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                       .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
            p->ubo_size[i] = (uint32_t)bci.size;
            if (!vk_ok(vkCreateBuffer(g.dev, &bci, NULL, &p->ubo[i]), "vkCreateBuffer(triangle-ubo)")) goto fail;
            VkMemoryRequirements mreq; vkGetBufferMemoryRequirements(g.dev, p->ubo[i], &mreq);
            uint32_t mt = 0;
            if (!vk_mem_type(mreq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &mt)) goto fail;
            VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                         .allocationSize = mreq.size, .memoryTypeIndex = mt };
            if (!vk_ok(vkAllocateMemory(g.dev, &mai, NULL, &p->ubo_mem[i]), "vkAllocateMemory(triangle-ubo)")) goto fail;
            if (!vk_ok(vkBindBufferMemory(g.dev, p->ubo[i], p->ubo_mem[i], 0), "vkBindBufferMemory(triangle-ubo)")) goto fail;
            void *mapped = NULL;
            if (!vk_ok(vkMapMemory(g.dev, p->ubo_mem[i], 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory(triangle-ubo)")) goto fail;
            memset(mapped, 0, bci.size);
            vkUnmapMemory(g.dev, p->ubo_mem[i]);
            bi[i] = (VkDescriptorBufferInfo){ .buffer = p->ubo[i], .offset = 0, .range = VK_WHOLE_SIZE };
            wr[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = p->dset, .dstBinding = ubos[i].binding,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                            .pBufferInfo = &bi[i] };
        }
        vkUpdateDescriptorSets(g.dev, nubo, wr, 0, NULL);
        p->nubo = nubo;
    }
    VkPipelineLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = nubo ? 1u : 0u,
                                       .pSetLayouts = nubo ? &p->dsl : NULL };
    if (!vk_ok(vkCreatePipelineLayout(g.dev, &lci, NULL, &p->layout), "vkCreatePipelineLayout(triangle)"))
        goto fail;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = p->vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = p->ps, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rast = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState cba = {
        .blendEnable = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &cba,
    };
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dyn,
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rast,
        .pMultisampleState = &ms,
        .pColorBlendState = &cb,
        .pDynamicState = &ds,
        .layout = p->layout,
        .renderPass = p->pass,
        .subpass = 0,
    };
    if (!vk_ok(vk_create_graphics_pipelines_bigstack(VK_NULL_HANDLE, &gpci, &p->pipe),
               "vkCreateGraphicsPipelines(triangle)"))
        goto fail;
    pthread_mutex_unlock(&g_lock);
    vk_log("triangle pipeline ready format=%s", vk_format_name(fmt));
    return p;

fail:
    gfx_backend_triangle_pipeline_destroy(p);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

GfxTrianglePipeline *gfx_backend_triangle_pipeline_create(const uint8_t *vs, size_t vs_size,
                                                          const uint8_t *ps, size_t ps_size,
                                                          int vk_format) {
    return triangle_create_impl(vs, vs_size, ps, ps_size, vk_format, NULL, 0);
}

GfxTrianglePipeline *gfx_backend_triangle_pipeline_create_ubo(const uint8_t *vs, size_t vs_size,
                                                              const uint8_t *ps, size_t ps_size,
                                                              int vk_format,
                                                              const GfxUboBinding *ubos, uint32_t nubo) {
    return triangle_create_impl(vs, vs_size, ps, ps_size, vk_format, ubos, nubo);
}

int gfx_backend_triangle_write_ubo(GfxTrianglePipeline *p, uint32_t binding,
                                   uint32_t offset, const void *data, size_t size) {
    if (!p || !data || !size) return 0;
    pthread_mutex_lock(&g_lock);
    uint32_t idx = p->nubo;
    for (uint32_t i = 0; i < p->nubo; i++)
        if (p->ubo_binding[i] == binding) { idx = i; break; }
    if (idx >= p->nubo || !p->ubo[idx]) { pthread_mutex_unlock(&g_lock); return 0; }
    if (offset > p->ubo_size[idx] || size > p->ubo_size[idx] - offset) {
        vk_log("ERROR ubo write out of range: binding=%u offset=%u size=%zu buffer=%u",
               binding, offset, size, p->ubo_size[idx]);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    void *mapped = NULL;
    if (!vk_ok(vkMapMemory(g.dev, p->ubo_mem[idx], 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory(ubo-write)")) {
        pthread_mutex_unlock(&g_lock); return 0;
    }
    memcpy((uint8_t *)mapped + offset, data, size);
    vkUnmapMemory(g.dev, p->ubo_mem[idx]);
    pthread_mutex_unlock(&g_lock);
    return 1;
}

void gfx_backend_triangle_pipeline_destroy(GfxTrianglePipeline *p) {
    if (!p) return;
    if (g.dev) {
        if (p->fb) vkDestroyFramebuffer(g.dev, p->fb, NULL);
        if (p->pipe) vkDestroyPipeline(g.dev, p->pipe, NULL);
        if (p->layout) vkDestroyPipelineLayout(g.dev, p->layout, NULL);
        if (p->pass) vkDestroyRenderPass(g.dev, p->pass, NULL);
        for (uint32_t i = 0; i < p->nubo; i++) {
            if (p->ubo[i]) vkDestroyBuffer(g.dev, p->ubo[i], NULL);
            if (p->ubo_mem[i]) vkFreeMemory(g.dev, p->ubo_mem[i], NULL);
        }
        if (p->dpool) vkDestroyDescriptorPool(g.dev, p->dpool, NULL);
        if (p->dsl) vkDestroyDescriptorSetLayout(g.dev, p->dsl, NULL);
        if (p->vs) vkDestroyShaderModule(g.dev, p->vs, NULL);
        if (p->ps) vkDestroyShaderModule(g.dev, p->ps, NULL);
    }
    free(p);
}

int gfx_backend_triangle_draw(GfxTrianglePipeline *p, GfxOffscreenTarget *t,
                              float r, float g_, float b, float a) {
    if (!p || !t) return 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready || t->rt_layout.color_count < 1 ||
        t->color_format[0] != p->format || t->color_bpp[0] != 4) {
        vk_log("ERROR triangle draw: format/bpp mismatch (pipeline=%d target=%d bpp=%u)",
               (int)p->format, (int)t->color_format[0], t->color_bpp[0]);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (!p->fb || p->fb_view != t->color_view[0] || p->fb_w != t->width || p->fb_h != t->height) {
        if (p->fb) vkDestroyFramebuffer(g.dev, p->fb, NULL);
        VkImageView view = t->color_view[0];
        VkFramebufferCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = p->pass,
            .attachmentCount = 1,
            .pAttachments = &view,
            .width = t->width,
            .height = t->height,
            .layers = 1,
        };
        if (!vk_ok(vkCreateFramebuffer(g.dev, &fci, NULL, &p->fb), "vkCreateFramebuffer(triangle)")) {
            p->fb_view = VK_NULL_HANDLE;
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        p->fb_view = t->color_view[0];
        p->fb_w = t->width;
        p->fb_h = t->height;
    }
    VkCommandBuffer cmd = g.cmd;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(triangle)")) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    VkClearValue clear = { .color = { .float32 = { r, g_, b, a } } };
    VkRenderPassBeginInfo rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = p->pass,
        .framebuffer = p->fb,
        .renderArea = { { 0, 0 }, { t->width, t->height } },
        .clearValueCount = 1,
        .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipe);
    if (p->nubo) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &p->dset, 0, NULL);
    VkViewport viewport = { 0, 0, (float)t->width, (float)t->height, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, { t->width, t->height } };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                              .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
        .imageOffset = { 0, 0, 0 },
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
        .buffer = t->color_readback[0],
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &bb, 0, NULL);
    if (!vk_submit_and_wait(cmd, "triangle")) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    t->layout_state = VK_IMAGE_LAYOUT_GENERAL;
    void *mapped = NULL;
    if (!vk_ok(vkMapMemory(g.dev, t->color_readback_mem[0], 0, VK_WHOLE_SIZE, 0, &mapped),
               "vkMapMemory(triangle)")) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    memcpy(t->color_pixels[0], mapped, t->color_size[0]);
    vkUnmapMemory(g.dev, t->color_readback_mem[0]);
    pthread_mutex_unlock(&g_lock);
    return 1;
}

GfxTrianglePipeline *gfx_backend_totk_vs_pipeline_create(const uint8_t *vs, size_t vs_size,
                                                         int vk_format) {
    GfxUboBinding ubos[2] = { {1, 0x10000}, {2, 0x10000} };
    return triangle_create_impl(vs, vs_size, (const uint8_t *)triangle_frag_spv,
                                triangle_frag_spv_size, vk_format, ubos, 2);
}

GfxTrianglePipeline *gfx_backend_builtin_triangle_pipeline(void) {
    if (!g_builtin_triangle)
        g_builtin_triangle = gfx_backend_triangle_pipeline_create(
            (const uint8_t *)triangle_vert_spv, triangle_vert_spv_size,
            (const uint8_t *)triangle_frag_spv, triangle_frag_spv_size, VK_FORMAT_R8G8B8A8_UNORM);
    return g_builtin_triangle;
}

void gfx_backend_builtin_triangle_modules(const uint8_t **vs, size_t *vs_size,
                                          const uint8_t **ps, size_t *ps_size) {
    if (vs) *vs = (const uint8_t *)triangle_vert_spv;
    if (vs_size) *vs_size = triangle_vert_spv_size;
    if (ps) *ps = (const uint8_t *)triangle_frag_spv;
    if (ps_size) *ps_size = triangle_frag_spv_size;
}

void gfx_backend_builtin_texture_modules(const uint8_t **vs, size_t *vs_size,
                                         const uint8_t **ps, size_t *ps_size) {
    if (vs) *vs = (const uint8_t *)triangle_tex_vert_spv;
    if (vs_size) *vs_size = triangle_tex_vert_spv_size;
    if (ps) *ps = (const uint8_t *)triangle_tex_frag_spv;
    if (ps_size) *ps_size = triangle_tex_frag_spv_size;
}

void gfx_backend_builtin_ubo_modules(const uint8_t **vs, size_t *vs_size,
                                     const uint8_t **ps, size_t *ps_size) {
    if (vs) *vs = (const uint8_t *)triangle_vert_spv;
    if (vs_size) *vs_size = triangle_vert_spv_size;
    if (ps) *ps = (const uint8_t *)reflect_ubo_frag_spv;
    if (ps_size) *ps_size = reflect_ubo_frag_spv_size;
}

void gfx_backend_builtin_attr_modules(const uint8_t **vs, size_t *vs_size,
                                      const uint8_t **ps, size_t *ps_size) {
    if (vs) *vs = (const uint8_t *)triangle_attr_vert_spv;
    if (vs_size) *vs_size = triangle_attr_vert_spv_size;
    if (ps) *ps = (const uint8_t *)triangle_attr_frag_spv;
    if (ps_size) *ps_size = triangle_attr_frag_spv_size;
}

void gfx_backend_builtin_depth_modules(const uint8_t **vs, size_t *vs_size,
                                       const uint8_t **ps, size_t *ps_size) {
    if (vs) *vs = (const uint8_t *)triangle_z_vert_spv;
    if (vs_size) *vs_size = triangle_z_vert_spv_size;
    if (ps) *ps = (const uint8_t *)reflect_ubo_frag_spv;
    if (ps_size) *ps_size = reflect_ubo_frag_spv_size;
}

GfxTexturedPipeline *gfx_backend_builtin_textured_pipeline(void) {
    if (!g_builtin_textured)
        g_builtin_textured = gfx_backend_textured_pipeline_create(VK_FORMAT_R8G8B8A8_UNORM);
    return g_builtin_textured;
}
