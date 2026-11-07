/* Module: pipeline_textured. */
#include "gfx_internal.h"

/* Repository-owned BC4 sampled-draw probe shaders.  GLSL source:
 *   games/totk/android/shaders/bc4_probe.vert
 *   games/totk/android/shaders/bc4_probe.frag
 *
 * Regenerate with the NDK glslc:
 *   /opt/homebrew/share/android-ndk/shader-tools/darwin-x86_64/glslc \
 *       --target-env=vulkan1.0 -O -fshader-stage=vert \
 *       games/totk/android/shaders/bc4_probe.vert -o /tmp/bc4_probe.vert.spv
 *   /opt/homebrew/share/android-ndk/shader-tools/darwin-x86_64/glslc \
 *       --target-env=vulkan1.0 -O -fshader-stage=frag \
 *       games/totk/android/shaders/bc4_probe.frag -o /tmp/bc4_probe.frag.spv
 * then emit little-endian uint32_t words in the same style as triangle_spv.h.
 */
static const uint32_t bc4_probe_vert_spv[] __attribute__((aligned(4))) = {
    0x07230203u, 0x00010000u, 0x000d000au, 0x0000002fu, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0008000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000cu, 0x00000018u, 0x00000020u,
    0x00040047u, 0x0000000cu, 0x0000000bu, 0x0000002au, 0x00040047u, 0x00000018u, 0x0000001eu, 0x00000000u,
    0x00050048u, 0x0000001eu, 0x00000000u, 0x0000000bu, 0x00000000u, 0x00050048u, 0x0000001eu, 0x00000001u,
    0x0000000bu, 0x00000001u, 0x00050048u, 0x0000001eu, 0x00000002u, 0x0000000bu, 0x00000003u, 0x00050048u,
    0x0000001eu, 0x00000003u, 0x0000000bu, 0x00000004u, 0x00030047u, 0x0000001eu, 0x00000002u, 0x00020013u,
    0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u,
    0x00000007u, 0x00000006u, 0x00000002u, 0x00040015u, 0x0000000au, 0x00000020u, 0x00000001u, 0x00040020u,
    0x0000000bu, 0x00000001u, 0x0000000au, 0x0004003bu, 0x0000000bu, 0x0000000cu, 0x00000001u, 0x0004002bu,
    0x0000000au, 0x0000000eu, 0x00000001u, 0x0004002bu, 0x0000000au, 0x00000010u, 0x00000002u, 0x00040020u,
    0x00000017u, 0x00000003u, 0x00000007u, 0x0004003bu, 0x00000017u, 0x00000018u, 0x00000003u, 0x00040017u,
    0x0000001au, 0x00000006u, 0x00000004u, 0x00040015u, 0x0000001bu, 0x00000020u, 0x00000000u, 0x0004002bu,
    0x0000001bu, 0x0000001cu, 0x00000001u, 0x0004001cu, 0x0000001du, 0x00000006u, 0x0000001cu, 0x0006001eu,
    0x0000001eu, 0x0000001au, 0x00000006u, 0x0000001du, 0x0000001du, 0x00040020u, 0x0000001fu, 0x00000003u,
    0x0000001eu, 0x0004003bu, 0x0000001fu, 0x00000020u, 0x00000003u, 0x0004002bu, 0x0000000au, 0x00000021u,
    0x00000000u, 0x0004002bu, 0x00000006u, 0x00000023u, 0x40000000u, 0x0004002bu, 0x00000006u, 0x00000025u,
    0x3f800000u, 0x0004002bu, 0x00000006u, 0x00000028u, 0x00000000u, 0x00040020u, 0x0000002cu, 0x00000003u,
    0x0000001au, 0x0005002cu, 0x00000007u, 0x0000002eu, 0x00000025u, 0x00000025u, 0x00050036u, 0x00000002u,
    0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x0004003du, 0x0000000au, 0x0000000du,
    0x0000000cu, 0x000500c4u, 0x0000000au, 0x0000000fu, 0x0000000du, 0x0000000eu, 0x000500c7u, 0x0000000au,
    0x00000011u, 0x0000000fu, 0x00000010u, 0x0004006fu, 0x00000006u, 0x00000012u, 0x00000011u, 0x000500c7u,
    0x0000000au, 0x00000014u, 0x0000000du, 0x00000010u, 0x0004006fu, 0x00000006u, 0x00000015u, 0x00000014u,
    0x00050050u, 0x00000007u, 0x00000016u, 0x00000012u, 0x00000015u, 0x0003003eu, 0x00000018u, 0x00000016u,
    0x0005008eu, 0x00000007u, 0x00000024u, 0x00000016u, 0x00000023u, 0x00050083u, 0x00000007u, 0x00000027u,
    0x00000024u, 0x0000002eu, 0x00050051u, 0x00000006u, 0x00000029u, 0x00000027u, 0x00000000u, 0x00050051u,
    0x00000006u, 0x0000002au, 0x00000027u, 0x00000001u, 0x00070050u, 0x0000001au, 0x0000002bu, 0x00000029u,
    0x0000002au, 0x00000028u, 0x00000025u, 0x00050041u, 0x0000002cu, 0x0000002du, 0x00000020u, 0x00000021u,
    0x0003003eu, 0x0000002du, 0x0000002bu, 0x000100fdu, 0x00010038u,
};

static const size_t bc4_probe_vert_spv_size = sizeof bc4_probe_vert_spv;

static const uint32_t bc4_probe_frag_spv[] __attribute__((aligned(4))) = {
    0x07230203u, 0x00010000u, 0x000d000au, 0x00000014u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0007000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000009u, 0x00000011u, 0x00030010u,
    0x00000004u, 0x00000007u, 0x00040047u, 0x00000009u, 0x0000001eu, 0x00000000u, 0x00040047u, 0x0000000du,
    0x00000022u, 0x00000000u, 0x00040047u, 0x0000000du, 0x00000021u, 0x00000000u, 0x00040047u, 0x00000011u,
    0x0000001eu, 0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u,
    0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040020u, 0x00000008u,
    0x00000003u, 0x00000007u, 0x0004003bu, 0x00000008u, 0x00000009u, 0x00000003u, 0x00090019u, 0x0000000au,
    0x00000006u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000000u, 0x0003001bu,
    0x0000000bu, 0x0000000au, 0x00040020u, 0x0000000cu, 0x00000000u, 0x0000000bu, 0x0004003bu, 0x0000000cu,
    0x0000000du, 0x00000000u, 0x00040017u, 0x0000000fu, 0x00000006u, 0x00000002u, 0x00040020u, 0x00000010u,
    0x00000001u, 0x0000000fu, 0x0004003bu, 0x00000010u, 0x00000011u, 0x00000001u, 0x00050036u, 0x00000002u,
    0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x0004003du, 0x0000000bu, 0x0000000eu,
    0x0000000du, 0x0004003du, 0x0000000fu, 0x00000012u, 0x00000011u, 0x00050057u, 0x00000007u, 0x00000013u,
    0x0000000eu, 0x00000012u, 0x0003003eu, 0x00000009u, 0x00000013u, 0x000100fdu, 0x00010038u,
};

static const size_t bc4_probe_frag_spv_size = sizeof bc4_probe_frag_spv;

GfxTexturedPipeline *textured_pipeline_create_impl(const uint8_t *vs, size_t vs_size,
                                                          const uint8_t *ps, size_t ps_size,
                                                          int vk_format) {
    pthread_mutex_lock(&g_lock);
    if (!vk_init_locked()) { pthread_mutex_unlock(&g_lock); return NULL; }
    VkFormat fmt = (VkFormat)vk_format;
    if (fmt == VK_FORMAT_UNDEFINED) { pthread_mutex_unlock(&g_lock); return NULL; }
    GfxTexturedPipeline *p = calloc(1, sizeof *p);
    if (!p) { pthread_mutex_unlock(&g_lock); return NULL; }
    p->format = fmt;
    if (!vk_shader_module(vs, vs_size, "vkCreateShaderModule(tex vs)", &p->vs) ||
        !vk_shader_module(ps, ps_size, "vkCreateShaderModule(tex ps)", &p->ps))
        goto fail;
    VkDescriptorSetLayoutBinding b = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dlci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &b,
    };
    if (!vk_ok(vkCreateDescriptorSetLayout(g.dev, &dlci, NULL, &p->dsl), "vkCreateDescriptorSetLayout(tex)")) goto fail;
    VkDescriptorPoolSize pool_size = { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1 };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size,
    };
    if (!vk_ok(vkCreateDescriptorPool(g.dev, &dpci, NULL, &p->dpool), "vkCreateDescriptorPool(tex)")) goto fail;
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = p->dpool, .descriptorSetCount = 1, .pSetLayouts = &p->dsl,
    };
    if (!vk_ok(vkAllocateDescriptorSets(g.dev, &dsai, &p->dset), "vkAllocateDescriptorSets(tex)")) goto fail;

    VkAttachmentDescription att = {
        .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = 1, .pColorAttachments = &ref };
    VkSubpassDependency dep = {
        .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                    .attachmentCount = 1, .pAttachments = &att,
                                    .subpassCount = 1, .pSubpasses = &sub,
                                    .dependencyCount = 1, .pDependencies = &dep };
    if (!vk_ok(vkCreateRenderPass(g.dev, &rpci, NULL, &p->pass), "vkCreateRenderPass(tex)")) goto fail;
    VkPipelineLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1, .pSetLayouts = &p->dsl };
    if (!vk_ok(vkCreatePipelineLayout(g.dev, &lci, NULL, &p->layout), "vkCreatePipelineLayout(tex)")) goto fail;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = p->vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = p->ps, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rast = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState cba = {
        .blendEnable = VK_FALSE,
        .colorWriteMask = 0xf,
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba,
    };
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn,
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
        .pRasterizationState = &rast, .pMultisampleState = &ms, .pColorBlendState = &cb,
        .pDynamicState = &ds, .layout = p->layout, .renderPass = p->pass, .subpass = 0,
    };
    if (!vk_ok(vk_create_graphics_pipelines_bigstack(VK_NULL_HANDLE, &gpci, &p->pipe),
               "vkCreateGraphicsPipelines(tex)")) goto fail;
    pthread_mutex_unlock(&g_lock);
    vk_log("textured triangle pipeline ready format=%s", vk_format_name(fmt));
    return p;
fail:
    gfx_backend_textured_pipeline_destroy(p);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

GfxTexturedPipeline *textured_pipeline_create_bc4(int vk_format) {
    return textured_pipeline_create_impl((const uint8_t *)bc4_probe_vert_spv,
                                         bc4_probe_vert_spv_size,
                                         (const uint8_t *)bc4_probe_frag_spv,
                                         bc4_probe_frag_spv_size, vk_format);
}
