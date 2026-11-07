/* Module: pipeline_reflected_0. */
#include "gfx_internal.h"

VkFormat vk_reflect_texel_format(const GfxBindingSpec *spec, int storage) {
    VkFormat f = (VkFormat)gfx_backend_vk_format_for_texture_format(spec->nvn_format);
    if (!f && storage) f = VK_FORMAT_R32_UINT;
    if (!f) f = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(g.phys, f, &props);
    VkFormatFeatureFlags need = storage ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT
                                        : VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
    if ((props.bufferFeatures & need) == need) return f;
    return storage ? VK_FORMAT_R32_UINT : VK_FORMAT_R8G8B8A8_UNORM;
}

void vk_spirv_move_set(uint8_t *code, size_t size, uint32_t from, uint32_t to) {
    if (!code || size < 20 || (size & 3)) return;
    uint32_t *w = (uint32_t *)code;
    for (size_t i = 5; i < size / 4;) {
        uint32_t op = w[i] & 0xffffu, wc = w[i] >> 16;
        if (!wc || i + wc > size / 4) break;
        if (op == 71 && wc >= 4 && w[i + 2] == 34 && w[i + 3] == from) w[i + 3] = to;
        i += wc;
    }
}

VkDescriptorType reflect_vk_type(GfxBindingKind kind) {
    switch (kind) {
    case GFX_BIND_SSBO: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    case GFX_BIND_SAMPLER: return VK_DESCRIPTOR_TYPE_SAMPLER;
    case GFX_BIND_SAMPLED_IMAGE: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case GFX_BIND_COMBINED_IMAGE_SAMPLER: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case GFX_BIND_STORAGE_IMAGE: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case GFX_BIND_UNIFORM_TEXEL_BUFFER: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    case GFX_BIND_STORAGE_TEXEL_BUFFER: return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    case GFX_BIND_UBO:
    default: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    }
}

uint64_t reflect_state_sig(const GfxPipelineState *st) {
    const uint8_t *p = (const uint8_t *)st;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof *st; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

int reflect_create_pipeline(GfxReflectedPipeline *p, const GfxPipelineState *st,
                                   VkPipeline *out) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = p->vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = p->ps, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = p->nb, .pVertexBindingDescriptions = p->vib,
        .vertexAttributeDescriptionCount = p->na, .pVertexAttributeDescriptions = p->via };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rast = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = st ? (VkPolygonMode)st->polygon_mode : VK_POLYGON_MODE_FILL,
        .cullMode = st ? st->cull_mode : VK_CULL_MODE_NONE,
        .frontFace = st ? (VkFrontFace)st->front_face : VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba[GFX_MAX_COLOR_TARGETS];
    uint32_t ncolor = p->rt_layout.color_count;
    if (!ncolor || ncolor > GFX_MAX_COLOR_TARGETS) ncolor = 1;
    for (uint32_t i = 0; i < ncolor; i++) {
        cba[i] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = st && (st->blend_enable & (1u << i)) ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = st ? vk_nvn_blend_factor(st->blend_src_rgb) : VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = st ? vk_nvn_blend_factor(st->blend_dst_rgb) : VK_BLEND_FACTOR_ZERO,
            .colorBlendOp = st ? vk_nvn_blend_op(st->blend_eq_rgb) : VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = st ? vk_nvn_blend_factor(st->blend_src_a) : VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = st ? vk_nvn_blend_factor(st->blend_dst_a) : VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp = st ? vk_nvn_blend_op(st->blend_eq_a) : VK_BLEND_OP_ADD,
            .colorWriteMask = st ? (st->color_write_mask & 0xf) : 0xf,
        };
    }
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = ncolor, .pAttachments = cba };
    VkPipelineDepthStencilStateCreateInfo dss = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = st && st->depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = st && st->depth_write ? VK_TRUE : VK_FALSE,
        .depthCompareOp = st ? vk_nvn_depth_op(st->depth_func) : VK_COMPARE_OP_LESS,
        .stencilTestEnable = st && st->stencil_test ? VK_TRUE : VK_FALSE,
        .front = { .failOp = st ? vk_nvn_stencil_op(st->stencil_fail_front) : VK_STENCIL_OP_KEEP,
                   .passOp = st ? vk_nvn_stencil_op(st->stencil_pass_front) : VK_STENCIL_OP_KEEP,
                   .depthFailOp = st ? vk_nvn_stencil_op(st->stencil_zfail_front) : VK_STENCIL_OP_KEEP,
                   .compareOp = st ? vk_nvn_depth_op(st->stencil_func_front) : VK_COMPARE_OP_ALWAYS,
                   .compareMask = 0xff, .writeMask = 0xff, .reference = 0 },
        .back = { .failOp = st ? vk_nvn_stencil_op(st->stencil_fail_back) : VK_STENCIL_OP_KEEP,
                  .passOp = st ? vk_nvn_stencil_op(st->stencil_pass_back) : VK_STENCIL_OP_KEEP,
                  .depthFailOp = st ? vk_nvn_stencil_op(st->stencil_zfail_back) : VK_STENCIL_OP_KEEP,
                  .compareOp = st ? vk_nvn_depth_op(st->stencil_func_back) : VK_COMPARE_OP_ALWAYS,
                  .compareMask = 0xff, .writeMask = 0xff, .reference = 0 } };
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                             VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                             VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 4, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rast,
        .pMultisampleState = &ms, .pColorBlendState = &cb,
        .pDepthStencilState = p->rt_layout.depth_format ? &dss : NULL,
        .pDynamicState = &ds, .layout = p->layout, .renderPass = p->pass };
    return vk_ok(vk_create_graphics_pipelines_bigstack(VK_NULL_HANDLE, &gpci, out),
                 "vkCreateGraphicsPipelines(reflected variant)");
}

VkPipeline reflect_pipeline_for_state(GfxReflectedPipeline *p, const GfxPipelineState *st) {
    if (!st) return p->pipe;
    uint64_t sig = reflect_state_sig(st);
    for (unsigned i = 0; i < p->nvariants; i++)
        if (p->variants[i].sig == sig &&
            !memcmp(&p->variants[i].state, st, sizeof *st)) return p->variants[i].pipe;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (!reflect_create_pipeline(p, st, &pipe)) return p->pipe;
    if (p->nvariants >= sizeof p->variants / sizeof p->variants[0]) {
        vk_log("ERROR reflected pipeline variant cache full; using base pipeline");
        vkDestroyPipeline(g.dev, pipe, NULL);
        return p->pipe;
    }
    p->variants[p->nvariants].sig = sig;
    p->variants[p->nvariants].state = *st;
    p->variants[p->nvariants].pipe = pipe;
    p->nvariants++;
    return pipe;
}

GfxReflectedPipeline *gfx_backend_reflected_pipeline_create(const uint8_t *vs, size_t vs_size,
                                                            const uint8_t *ps, size_t ps_size,
                                                            int vk_format,
                                                            const GfxBindingSpec *vs_specs, uint32_t nvs,
                                                            const GfxBindingSpec *ps_specs, uint32_t nps) {
    return gfx_backend_reflected_pipeline_create_ex(vs, vs_size, ps, ps_size, vk_format,
                                                    vs_specs, nvs, ps_specs, nps, NULL);
}
