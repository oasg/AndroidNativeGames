/* Module: pipeline_reflected_1. */
#include "gfx_internal.h"

GfxReflectedPipeline *reflect_pipeline_create_impl(
                                                            const uint8_t *vs, size_t vs_size,
                                                            const uint8_t *ps, size_t ps_size,
                                                            const GfxRenderTargetLayout *layout,
                                                            const GfxBindingSpec *vs_specs, uint32_t nvs,
                                                            const GfxBindingSpec *ps_specs, uint32_t nps,
                                                            const GfxVertexLayout *input) {
    pthread_mutex_lock(&g_lock);
    if (!vk_init_locked()) { pthread_mutex_unlock(&g_lock); return NULL; }
    if (!layout || layout->color_count < 1 || layout->color_count > GFX_MAX_COLOR_TARGETS) {
        vk_log("ERROR reflected pipeline: color_count=%u", layout ? layout->color_count : 0);
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    for (uint32_t i = 0; i < layout->color_count; i++) {
        VkFormat cf = (VkFormat)layout->color_formats[i];
        if (cf == VK_FORMAT_UNDEFINED || !vk_format_is_color_target(cf) ||
            !vk_format_info(cf).bpp) {
            uint32_t hint_w = 0, hint_h = 0;
            vk_rt_dim_hint_lookup(layout->color_formats[i], &hint_w, &hint_h);
            vk_log_bad_color_attachment("reflected pipeline", i, cf, layout,
                                        hint_w, hint_h, &g_bad_color_pipeline_logs);
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g.phys, cf, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
            vk_log("ERROR reflected pipeline: attachment %u format %s lacks color-attachment",
                   i, vk_format_name(cf));
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }
    }
    if (layout->depth_format &&
        (layout->depth_format == (int)VK_FORMAT_UNDEFINED ||
         !vk_format_is_depth((VkFormat)layout->depth_format))) {
        vk_log("ERROR reflected pipeline: depth format %d is not a depth format",
               layout->depth_format);
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    if (layout->depth_format) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g.phys, (VkFormat)layout->depth_format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            vk_log("ERROR reflected pipeline: depth format %s lacks depth-attachment",
                   vk_format_name((VkFormat)layout->depth_format));
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }
    }
    VkFormat fmt = (VkFormat)layout->color_formats[0];
    GfxReflectedPipeline *p = calloc(1, sizeof *p);
    if (!p) { pthread_mutex_unlock(&g_lock); return NULL; }
    p->format = fmt;
    p->rt_layout = *layout;
    if (input) p->input = *input;
    /* The pixel module is always in set 1.  Copy before patching so the
     * caller's SPIR-V (including the built-in test shader arrays) is not
     * modified; callers that already moved the set are unaffected. */
    uint8_t *ps_copy = NULL;
    if (ps && ps_size >= 20 && !(ps_size & 3)) {
        ps_copy = malloc(ps_size);
        if (!ps_copy) goto fail;
        memcpy(ps_copy, ps, ps_size);
        vk_spirv_move_set(ps_copy, ps_size, 0, 1);
    }
    int modules_ok = vk_shader_module(vs, vs_size, "vkCreateShaderModule(reflected vs)", &p->vs) &&
                     ps_copy && vk_shader_module(ps_copy, ps_size, "vkCreateShaderModule(reflected ps)", &p->ps);
    free(ps_copy);
    if (!modules_ok) goto fail;
    vk_log("reflected step: shader modules");

    const GfxBindingSpec *sets[2] = { vs_specs, ps_specs };
    uint32_t counts[2] = { nvs, nps };
    for (unsigned s = 0; s < 2; s++) {
        p->nspec[s] = counts[s] < GFX_REFLECT_MAX_BINDINGS ? counts[s] : GFX_REFLECT_MAX_BINDINGS;
        for (uint32_t i = 0; i < p->nspec[s]; i++) {
            p->spec[s][i] = sets[s][i];
            p->spec[s][i].set = s;
            p->bind_buf[s][i] = -1;
            p->bind_tex[s][i] = -1;
            p->bind_sampler[s][i] = -1;
            p->bind_img[s][i] = -1;
            p->bind_view[s][i] = -1;
        }
    }
    VkDescriptorPoolSize pool[8] = {0};
    uint32_t npool = 0;
    VkDescriptorSetLayoutBinding b[GFX_REFLECT_MAX_BINDINGS];
    for (unsigned s = 0; s < 2; s++) {
        if (counts[s] > GFX_REFLECT_MAX_BINDINGS) goto fail;
        for (uint32_t i = 0; i < counts[s]; i++) {
            VkDescriptorType dt = reflect_vk_type(sets[s][i].kind);
            uint32_t count = sets[s][i].count;
            if (sets[s][i].kind == GFX_BIND_UBO || sets[s][i].kind == GFX_BIND_SSBO)
                count = 1;
            else if (!count)
                count = GFX_REFLECT_RUNTIME_ARRAY;   /* runtime array / bindless pool */
            if (count > GFX_REFLECT_MAX_BINDINGS) goto fail;
            b[i] = (VkDescriptorSetLayoutBinding){ .binding = sets[s][i].binding,
                .descriptorType = dt, .descriptorCount = count,
                .stageFlags = s == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT };
            int found = -1;
            for (uint32_t j = 0; j < npool; j++) if (pool[j].type == dt) found = (int)j;
            if (found < 0) {
                if (npool >= sizeof pool / sizeof pool[0]) goto fail;
                pool[npool].type = dt; found = (int)npool++;
            }
            pool[found].descriptorCount += count;
        }
        VkDescriptorSetLayoutCreateInfo dlci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .bindingCount = counts[s], .pBindings = b };
        if (!vk_ok(vkCreateDescriptorSetLayout(g.dev, &dlci, NULL, &p->dsl[s]), "vkCreateDescriptorSetLayout(reflected)")) goto fail;
        vk_log("reflected step: set %u layout (%u bindings)", s, counts[s]);
    }
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                        .maxSets = 2, .poolSizeCount = npool, .pPoolSizes = pool };
    if (!vk_ok(vkCreateDescriptorPool(g.dev, &dpci, NULL, &p->dpool), "vkCreateDescriptorPool(reflected)")) goto fail;
    vk_log("reflected step: pool (%u sizes)", npool);
    VkDescriptorPoolSize draw_pool[8];
    for (uint32_t i = 0; i < npool; i++) {
        draw_pool[i] = pool[i];
        draw_pool[i].descriptorCount *= GFX_DESC_MAX_EPOCHS;
    }
    VkDescriptorPoolCreateInfo ddpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2 * GFX_DESC_MAX_EPOCHS, .poolSizeCount = npool, .pPoolSizes = draw_pool };
    if (!vk_ok(vkCreateDescriptorPool(g.dev, &ddpci, NULL, &p->dpool_draw),
               "vkCreateDescriptorPool(reflected draw)")) goto fail;
    vk_log("reflected step: draw pool (%u sizes x %u epochs)", npool, GFX_DESC_MAX_EPOCHS);
    for (unsigned s = 0; s < 2; s++) {
        VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                             .descriptorPool = p->dpool, .descriptorSetCount = 1, .pSetLayouts = &p->dsl[s] };
        if (!vk_ok(vkAllocateDescriptorSets(g.dev, &dsai, &p->dset[s]), "vkAllocateDescriptorSets(reflected)")) goto fail;
    }
    vk_log("reflected step: sets allocated");

    VkWriteDescriptorSet writes[GFX_REFLECT_MAX_BINDINGS * 2];
    VkDescriptorBufferInfo binfo[GFX_REFLECT_MAX_BINDINGS * 2];
    VkDescriptorImageInfo *iinfo = calloc((size_t)2 * GFX_REFLECT_MAX_BINDINGS *
                                          GFX_REFLECT_MAX_BINDINGS, sizeof *iinfo);
    VkBufferView *texel_views = calloc((size_t)2 * GFX_REFLECT_MAX_BINDINGS *
                                       GFX_REFLECT_MAX_BINDINGS, sizeof *texel_views);
    if (!iinfo || !texel_views) { free(iinfo); free(texel_views); goto fail; }
    uint32_t nw = 0, niinfo = 0, ntexel = 0;
    static const uint8_t checker[16] = { 255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255 };

    for (unsigned s = 0; s < 2; s++) {
        for (uint32_t i = 0; i < counts[s]; i++) {
            const GfxBindingSpec *spec = &sets[s][i];
            uint32_t count = spec->count;
            if (spec->kind == GFX_BIND_UBO || spec->kind == GFX_BIND_SSBO) count = 1;
            else if (!count) count = GFX_REFLECT_RUNTIME_ARRAY;
            if (!count || count > GFX_REFLECT_MAX_BINDINGS ||
                nw >= sizeof writes / sizeof writes[0]) goto cleanup_fail;
            VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = p->dset[s], .dstBinding = spec->binding,
                                       .descriptorCount = count,
                                       .descriptorType = reflect_vk_type(spec->kind) };
            vk_log("reflected descriptor: set %u binding %u kind=%d count=%u", s, spec->binding,
                   (int)spec->kind, count);

            if (spec->kind == GFX_BIND_UBO || spec->kind == GFX_BIND_SSBO) {
                if (p->nbuf >= GFX_REFLECT_MAX_BINDINGS) goto cleanup_fail;
                uint32_t sz = spec->kind == GFX_BIND_UBO ? 0x10000 : 0x1000;
                if (!vk_make_buffer(sz, &p->buf[p->nbuf], &p->buf_mem[p->nbuf])) goto cleanup_fail;
                binfo[nw] = (VkDescriptorBufferInfo){ .buffer = p->buf[p->nbuf], .offset = 0,
                                                      .range = VK_WHOLE_SIZE };
                w.pBufferInfo = &binfo[nw];
                p->bind_buf[s][i] = (int32_t)p->nbuf;
                p->buf_size[p->nbuf] = sz;
                p->nbuf++;
            } else if (spec->kind == GFX_BIND_SAMPLER) {
                if (p->nsampler >= GFX_REFLECT_MAX_BINDINGS) goto cleanup_fail;
                if (!vk_make_sampler(&p->sampler[p->nsampler])) goto cleanup_fail;
                for (uint32_t k = 0; k < count; k++)
                    iinfo[niinfo + k] = (VkDescriptorImageInfo){
                        .sampler = p->sampler[p->nsampler], .imageView = VK_NULL_HANDLE,
                        .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED };
                w.pImageInfo = &iinfo[niinfo];
                niinfo += count;
                p->bind_sampler[s][i] = (int32_t)p->nsampler;
                p->nsampler++;
            } else if (spec->kind == GFX_BIND_SAMPLED_IMAGE ||
                       spec->kind == GFX_BIND_COMBINED_IMAGE_SAMPLER) {
                if (p->ntex >= GFX_REFLECT_MAX_BINDINGS) goto cleanup_fail;
                GfxTexture *tex = texture_create_rgba8_locked(2, 2, checker);
                if (!tex) goto cleanup_fail;
                p->bind_tex[s][i] = (int32_t)p->ntex;
                p->tex[p->ntex++] = tex;
                for (uint32_t k = 0; k < count; k++)
                    iinfo[niinfo + k] = (VkDescriptorImageInfo){
                        .sampler = spec->kind == GFX_BIND_COMBINED_IMAGE_SAMPLER ? tex->sampler : VK_NULL_HANDLE,
                        .imageView = tex->view,
                        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                w.pImageInfo = &iinfo[niinfo];
                niinfo += count;
            } else if (spec->kind == GFX_BIND_STORAGE_IMAGE) {
                if (p->nimg >= GFX_REFLECT_MAX_BINDINGS) goto cleanup_fail;
                VkFormat f = (VkFormat)gfx_backend_vk_format_for_texture_format(spec->nvn_format);
                if (!vk_make_storage_image(2, 2, f, &p->img[p->nimg], &p->img_mem[p->nimg],
                                           &p->img_view[p->nimg])) goto cleanup_fail;
                for (uint32_t k = 0; k < count; k++)
                    iinfo[niinfo + k] = (VkDescriptorImageInfo){
                        .sampler = VK_NULL_HANDLE, .imageView = p->img_view[p->nimg],
                        .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
                w.pImageInfo = &iinfo[niinfo];
                niinfo += count;
                p->bind_img[s][i] = (int32_t)p->nimg;
                p->nimg++;
            } else { /* UNIFORM_TEXEL_BUFFER / STORAGE_TEXEL_BUFFER */
                if (p->nbuf >= GFX_REFLECT_MAX_BINDINGS ||
                    p->nbuf_view >= GFX_REFLECT_MAX_BINDINGS) goto cleanup_fail;
                int storage = spec->kind == GFX_BIND_STORAGE_TEXEL_BUFFER;
                VkFormat f = vk_reflect_texel_format(spec, storage);
                if (!vk_make_buffer_view(4096, storage, f, &p->buf[p->nbuf],
                                         &p->buf_mem[p->nbuf], &p->buf_view[p->nbuf_view]))
                    goto cleanup_fail;
                for (uint32_t k = 0; k < count; k++) texel_views[ntexel + k] = p->buf_view[p->nbuf_view];
                w.pTexelBufferView = &texel_views[ntexel];
                ntexel += count;
                p->bind_view[s][i] = (int32_t)p->nbuf_view;
                p->nbuf++;
                p->nbuf_view++;
            }
            writes[nw++] = w;
        }
    }
    vk_log("reflected step: descriptors written (%u)", nw);
    if (nw) vkUpdateDescriptorSets(g.dev, nw, writes, 0, NULL);
    free(iinfo);
    free(texel_views);
    goto descriptors_done;

cleanup_fail:
    free(iinfo);
    free(texel_views);
    goto fail;

descriptors_done:;

    uint32_t ncolor = p->rt_layout.color_count;
    int has_depth = p->rt_layout.depth_format != 0;
    VkAttachmentDescription att[GFX_MAX_COLOR_TARGETS + 1];
    VkAttachmentReference ref[GFX_MAX_COLOR_TARGETS];
    memset(att, 0, sizeof att);
    for (uint32_t i = 0; i < ncolor; i++) {
        att[i] = (VkAttachmentDescription){
            .format = (VkFormat)p->rt_layout.color_formats[i],
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        ref[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    }
    VkAttachmentReference dref = { ncolor, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    if (has_depth) {
        att[ncolor] = (VkAttachmentDescription){
            .format = (VkFormat)p->rt_layout.depth_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
    }
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = ncolor, .pColorAttachments = ref,
                                 .pDepthStencilAttachment = has_depth ? &dref : NULL };
    VkPipelineStageFlags dep_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkAccessFlags dep_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (has_depth) {
        dep_stage |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dep_access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    VkSubpassDependency dep = { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = dep_stage, .dstStageMask = dep_stage,
        .dstAccessMask = dep_access };
    VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = ncolor + (has_depth ? 1u : 0u), .pAttachments = att,
        .subpassCount = 1, .pSubpasses = &sub,
        .dependencyCount = 1, .pDependencies = &dep };
    if (!vk_ok(vkCreateRenderPass(g.dev, &rpci, NULL, &p->pass), "vkCreateRenderPass(reflected)")) goto fail;
    VkDescriptorSetLayout layouts[2] = { p->dsl[0], p->dsl[1] };
    VkPipelineLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 2, .pSetLayouts = layouts };
    if (!vk_ok(vkCreatePipelineLayout(g.dev, &lci, NULL, &p->layout), "vkCreatePipelineLayout(reflected)")) goto fail;
    vk_log("reflected step: pipeline layout");
    p->nb = input ? (input->nbindings < 16 ? input->nbindings : 16) : 0;
    p->na = input ? (input->nattributes < 16 ? input->nattributes : 16) : 0;
    for (uint32_t i = 0; i < p->nb; i++)
        p->vib[i] = (VkVertexInputBindingDescription){
            .binding = input->bindings[i].binding,
            .stride = input->bindings[i].stride,
            .inputRate = input->bindings[i].input_rate ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                       : VK_VERTEX_INPUT_RATE_VERTEX };
    for (uint32_t i = 0; i < p->na; i++)
        p->via[i] = (VkVertexInputAttributeDescription){
            .location = input->attributes[i].location,
            .binding = input->attributes[i].binding,
            .format = (VkFormat)input->attributes[i].format,
            .offset = input->attributes[i].offset };
    vk_log("reflected step: vkCreateGraphicsPipelines begin");
    if (!reflect_create_pipeline(p, NULL, &p->pipe)) goto fail;
    vk_log("reflected step: vkCreateGraphicsPipelines end");
    pthread_mutex_unlock(&g_lock);
    vk_log("reflected pipeline ready vs=%u ps=%u bindings colors=%u depth=%s format=%s",
           nvs, nps, p->rt_layout.color_count,
           p->rt_layout.depth_format ? vk_format_name((VkFormat)p->rt_layout.depth_format) : "none",
           vk_format_name(fmt));
    return p;
fail:
    gfx_backend_reflected_pipeline_destroy(p);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

GfxReflectedPipeline *gfx_backend_reflected_pipeline_create_ex(
                                                            const uint8_t *vs, size_t vs_size,
                                                            const uint8_t *ps, size_t ps_size,
                                                            int vk_format,
                                                            const GfxBindingSpec *vs_specs, uint32_t nvs,
                                                            const GfxBindingSpec *ps_specs, uint32_t nps,
                                                            const GfxVertexLayout *input) {
    GfxRenderTargetLayout layout = { 0 };
    layout.color_count = 1;
    layout.color_formats[0] = vk_format;
    /* The legacy entry point always had a depth attachment. */
    if (!gfx_backend_vulkan_init()) return NULL;
    layout.depth_format = (int)g.depth_format;
    return reflect_pipeline_create_impl(vs, vs_size, ps, ps_size, &layout,
                                        vs_specs, nvs, ps_specs, nps, input);
}

GfxReflectedPipeline *gfx_backend_reflected_pipeline_create_layout(
                                                            const uint8_t *vs, size_t vs_size,
                                                            const uint8_t *ps, size_t ps_size,
                                                            const GfxRenderTargetLayout *layout,
                                                            const GfxBindingSpec *vs_specs, uint32_t nvs,
                                                            const GfxBindingSpec *ps_specs, uint32_t nps,
                                                            const GfxVertexLayout *input) {
    return reflect_pipeline_create_impl(vs, vs_size, ps, ps_size, layout,
                                        vs_specs, nvs, ps_specs, nps, input);
}

void gfx_backend_reflected_pipeline_destroy(GfxReflectedPipeline *p) {
    if (!p) return;
    if (g.dev) {
        for (unsigned i = 0; i < p->ntex; i++) if (p->tex[i]) gfx_backend_texture_destroy(p->tex[i]);
        for (unsigned i = 0; i < p->nimg; i++) {
            if (p->img_view[i]) vkDestroyImageView(g.dev, p->img_view[i], NULL);
            if (p->img[i]) vkDestroyImage(g.dev, p->img[i], NULL);
            if (p->img_mem[i]) vkFreeMemory(g.dev, p->img_mem[i], NULL);
        }
        for (unsigned i = 0; i < p->nbuf_view; i++)
            if (p->buf_view[i]) vkDestroyBufferView(g.dev, p->buf_view[i], NULL);
        for (unsigned i = 0; i < p->nsampler; i++)
            if (p->sampler[i]) vkDestroySampler(g.dev, p->sampler[i], NULL);
        for (unsigned i = 0; i < p->nbuf; i++) {
            if (p->buf[i]) vkDestroyBuffer(g.dev, p->buf[i], NULL);
            if (p->buf_mem[i]) vkFreeMemory(g.dev, p->buf_mem[i], NULL);
        }
        for (unsigned i = 0; i < 16; i++) {
            if (p->vbuf[i]) vkDestroyBuffer(g.dev, p->vbuf[i], NULL);
            if (p->vbuf_mem[i]) vkFreeMemory(g.dev, p->vbuf_mem[i], NULL);
        }
        for (unsigned s = 0; s < 2; s++)
            for (unsigned i = 0; i < GFX_REFLECT_MAX_BINDINGS; i++) {
                if (p->dyn_buf[s][i]) vkDestroyBuffer(g.dev, p->dyn_buf[s][i], NULL);
                if (p->dyn_mem[s][i]) vkFreeMemory(g.dev, p->dyn_mem[s][i], NULL);
            }
        if (p->dpool_draw) vkDestroyDescriptorPool(g.dev, p->dpool_draw, NULL);
        if (p->index_buf) vkDestroyBuffer(g.dev, p->index_buf, NULL);
        if (p->index_mem) vkFreeMemory(g.dev, p->index_mem, NULL);
        if (p->fb) vkDestroyFramebuffer(g.dev, p->fb, NULL);
        if (p->pipe) vkDestroyPipeline(g.dev, p->pipe, NULL);
        if (p->layout) vkDestroyPipelineLayout(g.dev, p->layout, NULL);
        if (p->pass) vkDestroyRenderPass(g.dev, p->pass, NULL);
        if (p->dpool) vkDestroyDescriptorPool(g.dev, p->dpool, NULL);
        if (p->dsl[0]) vkDestroyDescriptorSetLayout(g.dev, p->dsl[0], NULL);
        if (p->dsl[1]) vkDestroyDescriptorSetLayout(g.dev, p->dsl[1], NULL);
        if (p->vs) vkDestroyShaderModule(g.dev, p->vs, NULL);
        if (p->ps) vkDestroyShaderModule(g.dev, p->ps, NULL);
    }
    free(p);
}

int reflect_find_spec(GfxReflectedPipeline *p, uint32_t set, uint32_t binding) {
    if (!p || set > 1) return -1;
    for (uint32_t i = 0; i < p->nspec[set]; i++)
        if (p->spec[set][i].binding == binding) return (int)i;
    return -1;
}

int gfx_backend_reflected_bind_texture(GfxReflectedPipeline *p, uint32_t set,
                                       uint32_t binding, const GfxTexture *texture) {
    if (!p || !texture) return 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready) { pthread_mutex_unlock(&g_lock); return 0; }
    int i = reflect_find_spec(p, set, binding);
    if (i < 0) { pthread_mutex_unlock(&g_lock); return 0; }
    const GfxBindingSpec *spec = &p->spec[set][i];
    if (spec->kind != GFX_BIND_COMBINED_IMAGE_SAMPLER &&
        spec->kind != GFX_BIND_SAMPLED_IMAGE) {
        pthread_mutex_unlock(&g_lock); return 0;
    }
    uint32_t count = spec->count ? spec->count : GFX_REFLECT_RUNTIME_ARRAY;
    VkDescriptorImageInfo *ii = calloc(count, sizeof *ii);
    if (!ii) { pthread_mutex_unlock(&g_lock); return 0; }
    for (uint32_t k = 0; k < count; k++) {
        ii[k] = (VkDescriptorImageInfo){
            .sampler = spec->kind == GFX_BIND_COMBINED_IMAGE_SAMPLER ? texture->sampler : VK_NULL_HANDLE,
            .imageView = texture->view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
    }
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = p->dset[set], .dstBinding = binding, .descriptorCount = count,
        .descriptorType = spec->kind == GFX_BIND_COMBINED_IMAGE_SAMPLER
                              ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                              : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .pImageInfo = ii };
    vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
    free(ii);
    pthread_mutex_unlock(&g_lock);
    vk_log("reflected bind texture set=%u binding=%u texture=%p", set, binding, (const void *)texture);
    return 1;
}

int reflect_write_buffer_locked(GfxReflectedPipeline *p, uint32_t set, uint32_t binding,
                                       uint32_t offset, const void *data, size_t size, int storage) {
    if (!p || !data || !size) return 0;
    if (!g.ready) return 0;
    int i = reflect_find_spec(p, set, binding);
    if (i < 0) return 0;
    const GfxBindingSpec *spec = &p->spec[set][i];
    if ((storage && spec->kind != GFX_BIND_SSBO) ||
        (!storage && spec->kind != GFX_BIND_UBO))
        return 0;
    int32_t idx = p->bind_buf[set][i];
    if (idx < 0 || (uint64_t)offset + size > p->buf_size[idx]) return 0;
    void *mapped = NULL;
    if (!vk_ok(vkMapMemory(g.dev, p->buf_mem[idx], offset, size, 0, &mapped),
               "vkMapMemory(reflected write)"))
        return 0;
    memcpy(mapped, data, size);
    vkUnmapMemory(g.dev, p->buf_mem[idx]);
    return 1;
}
