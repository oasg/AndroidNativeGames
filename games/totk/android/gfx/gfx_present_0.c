/* Module: present_0. */
#include "gfx_internal.h"

void vk_present_cache_destroy_locked(void) {
    if (!g.dev) {
        memset(g_present_pipes, 0, sizeof g_present_pipes);
        return;
    }
    for (unsigned i = 0; i < GFX_PRESENT_PIPE_MAX; i++) {
        GfxPresentPipe *p = &g_present_pipes[i];
        if (!p->used) continue;
        if (p->pipe) vkDestroyPipeline(g.dev, p->pipe, NULL);
        if (p->layout) vkDestroyPipelineLayout(g.dev, p->layout, NULL);
        if (p->pass) vkDestroyRenderPass(g.dev, p->pass, NULL);
        if (p->pool) vkDestroyDescriptorPool(g.dev, p->pool, NULL);
        if (p->dsl) vkDestroyDescriptorSetLayout(g.dev, p->dsl, NULL);
        if (p->sampler) vkDestroySampler(g.dev, p->sampler, NULL);
        memset(p, 0, sizeof *p);
    }
}

int vk_choose_surface_format(VkSurfaceKHR surface, VkFormat *out_format,
                                    VkColorSpaceKHR *out_space) {
    uint32_t n = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, surface, &n, NULL) != VK_SUCCESS || !n) return 0;
    VkSurfaceFormatKHR *f = calloc(n, sizeof *f);
    if (!f) return 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, surface, &n, f) != VK_SUCCESS) {
        free(f);
        return 0;
    }
    VkSurfaceFormatKHR chosen = f[0];
    for (uint32_t i = 0; i < n; i++) {
        if (f[i].format == VK_FORMAT_R8G8B8A8_UNORM &&
            f[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f[i];
            break;
        }
    }
    if (chosen.format == VK_FORMAT_UNDEFINED) {
        chosen.format = VK_FORMAT_R8G8B8A8_UNORM;
        chosen.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }
    *out_format = chosen.format;
    *out_space = chosen.colorSpace;
    free(f);
    return 1;
}

int vk_create_android_surface_locked(void *window) {
    if (!window) return 0;
    if (g.surface) return 1;
    VkAndroidSurfaceCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = (ANativeWindow *)window,
    };
    if (!vk_ok(vkCreateAndroidSurfaceKHR(g.instance, &sci, NULL, &g.surface),
               "vkCreateAndroidSurfaceKHR"))
        return 0;
    return 1;
}

/* Create (or recreate) the swapchain for the current surface.  The caller must
 * have destroyed the previous swapchain images and the device must be idle. */
int vk_create_swapchain_locked(void) {
    if (!g.dev || !g.surface) return 0;
    VkBool32 supported = VK_FALSE;
    if (!vk_ok(vkGetPhysicalDeviceSurfaceSupportKHR(g.phys, g.family, g.surface, &supported),
               "vkGetPhysicalDeviceSurfaceSupportKHR") || !supported) {
        vk_log("ERROR graphics queue family does not support the Android surface");
        return 0;
    }
    VkSurfaceCapabilitiesKHR caps;
    if (!vk_ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps),
               "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
        return 0;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    if (!vk_choose_surface_format(g.surface, &fmt, &color_space)) {
        vk_log("ERROR no surface format");
        return 0;
    }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = g.attach_width ? g.attach_width : 1280;
        extent.height = g.attach_height ? g.attach_height : 720;
        if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
        if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
        if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
        if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        vk_log("ERROR surface does not support color/transfer-dst swapchain usage (%#x)",
               caps.supportedUsageFlags);
        return 0;
    }
    uint32_t images = caps.minImageCount + 1;
    if (caps.maxImageCount && images > caps.maxImageCount) images = caps.maxImageCount;
    VkSwapchainCreateInfoKHR cci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = g.surface,
        .minImageCount = images,
        .imageFormat = fmt,
        .imageColorSpace = color_space,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE,
    };
    if (!vk_ok(vkCreateSwapchainKHR(g.dev, &cci, NULL, &g.swapchain),
               "vkCreateSwapchainKHR"))
        return 0;
    uint32_t count = VK_MAX_SWAPCHAIN_IMAGES;
    if (!vk_ok(vkGetSwapchainImagesKHR(g.dev, g.swapchain, &count, g.sc_images),
               "vkGetSwapchainImagesKHR"))
        return 0;
    g.sc_count = count;
    for (uint32_t i = 0; i < count; i++) {
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = g.sc_images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = fmt,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        if (!vk_ok(vkCreateImageView(g.dev, &vci, NULL, &g.sc_views[i]), "vkCreateImageView(swapchain)"))
            return 0;
        g.sc_layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    g.sc_extent = extent;
    g.sc_format = fmt;
    g.surface_ready = 1;
    vk_log("Android surface %ux%u format=%s images=%u", extent.width, extent.height,
           vk_format_name(fmt), count);
    return 1;
}

/* Acquire-time out-of-date/suboptimal recovery.  The ANativeWindow from attach
 * is retained so a rotation/resize does not need another shell callback; if the
 * window itself is gone (surface lost), recreation fails explicitly and the
 * caller reports 0 rather than presenting into a dead surface. */
int vk_recreate_swapchain_locked(void) {
    if (!g.dev || !g.android_window) {
        vk_log("ERROR swapchain recreate: no ANativeWindow");
        return 0;
    }
    vkDeviceWaitIdle(g.dev);
    vk_destroy_swapchain_images_locked();
    if (!g.surface && !vk_create_android_surface_locked(g.android_window)) {
        vk_log("ERROR swapchain recreate: vkCreateAndroidSurfaceKHR failed");
        return 0;
    }
    if (!vk_create_swapchain_locked()) {
        /* The VkSurfaceKHR itself may be stale after the window was replaced;
         * rebuild it once from the retained ANativeWindow. */
        vk_destroy_swapchain_images_locked();
        if (g.surface) vkDestroySurfaceKHR(g.instance, g.surface, NULL);
        g.surface = VK_NULL_HANDLE;
        if (!vk_create_android_surface_locked(g.android_window) ||
            !vk_create_swapchain_locked()) {
            vk_destroy_swapchain_images_locked();
            if (g.surface) vkDestroySurfaceKHR(g.instance, g.surface, NULL);
            g.surface = VK_NULL_HANDLE;
            vk_log("ERROR swapchain recreate failed; on-screen present disabled");
            return 0;
        }
    }
    vk_log("swapchain recreated %ux%u format=%s images=%u",
           g.sc_extent.width, g.sc_extent.height, vk_format_name(g.sc_format), g.sc_count);
    return 1;
}

int gfx_backend_surface_ready(void) { return g.surface_ready; }

/* ------------------------------------------------------------------ */
/* Presenting a real NVN target to the Android surface.
 *
 * Scale policy: GFX_SURFACE_SCALE_FIT (0) is the default and preserves the
 * target aspect ratio, centring the image and filling the uncovered bars with
 * opaque black.  GFX_SURFACE_SCALE_STRETCH (1) fills the surface exactly and
 * is intended for debugging only.
 *
 * Format policy: when the target format equals the swapchain format (the
 * common RGBA8 case) the copy is a scaled vkCmdBlitImage.  Any other pair
 * (notably R11G11B10F -> RGBA8) is converted by sampling the target in a
 * fullscreen pass; vkCmdBlitImage is not valid between incompatible formats. */

int vk_present_format_is_shader_src(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
        return 1;
    default:
        return 0;
    }
}

VkFilter vk_present_filter(VkFormat src) {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g.phys, src, &fp);
    return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
               ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

int vk_present_blit_ok(VkFormat src, VkFormat dst, VkFilter *out_filter) {
    if (src != dst || src == VK_FORMAT_UNDEFINED) return 0;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g.phys, src, &fp);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((fp.optimalTilingFeatures & need) != need) return 0;
    if (out_filter) *out_filter = vk_present_filter(src);
    return 1;
}

VkRect2D vk_present_rect(const GfxOffscreenTarget *t, VkExtent2D extent,
                                uint32_t scale_mode) {
    uint32_t dw = extent.width, dh = extent.height;
    if (scale_mode != GFX_SURFACE_SCALE_STRETCH && t->width && t->height) {
        uint64_t fit_by_width = (uint64_t)extent.width * t->height;
        uint64_t fit_by_height = (uint64_t)extent.height * t->width;
        if (fit_by_width <= fit_by_height) {
            dw = extent.width;
            dh = (uint32_t)((uint64_t)t->height * extent.width / t->width);
        } else {
            dh = extent.height;
            dw = (uint32_t)((uint64_t)t->width * extent.height / t->height);
        }
        if (!dw) dw = 1;
        if (!dh) dh = 1;
    }
    return (VkRect2D){ { (int32_t)((extent.width - dw) / 2),
                         (int32_t)((extent.height - dh) / 2) },
                       { dw, dh } };
}

GfxPresentPipe *vk_present_pipe_get_locked(VkFormat src, VkFormat dst,
                                                  VkImageLayout final_layout) {
    GfxPresentPipe *p = NULL;
    for (unsigned i = 0; i < GFX_PRESENT_PIPE_MAX; i++) {
        if (g_present_pipes[i].used && g_present_pipes[i].src_format == src &&
            g_present_pipes[i].dst_format == dst &&
            g_present_pipes[i].final_layout == final_layout) {
            return &g_present_pipes[i];
        }
        if (!g_present_pipes[i].used && !p) p = &g_present_pipes[i];
    }
    if (!p) {
        /* Small cache; a present pipeline is cheap to rebuild and the surface
         * format rarely changes.  Drop everything and start again. */
        vk_present_cache_destroy_locked();
        p = &g_present_pipes[0];
    }
    memset(p, 0, sizeof *p);
    p->src_format = src;
    p->dst_format = dst;
    p->final_layout = final_layout;

    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    if (getenv("TOTK_ANDROID_TRACE_SHADER"))
        vk_log("present pipeline begin src=%d dst=%d", (int)src, (int)dst);
    if (!vk_ok(vkCreateShaderModule(g.dev, &(VkShaderModuleCreateInfo){
                   .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                   .codeSize = present_vert_spv_size,
                   .pCode = present_vert_spv }, NULL, &vs),
               "vkCreateShaderModule(present vs)"))
        goto fail;
    if (!vk_ok(vkCreateShaderModule(g.dev, &(VkShaderModuleCreateInfo){
                   .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                   .codeSize = present_frag_spv_size,
                   .pCode = present_frag_spv }, NULL, &fs),
               "vkCreateShaderModule(present fs)"))
        goto fail;

    VkDescriptorSetLayoutBinding b = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dlci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &b,
    };
    if (!vk_ok(vkCreateDescriptorSetLayout(g.dev, &dlci, NULL, &p->dsl),
               "vkCreateDescriptorSetLayout(present)"))
        goto fail;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps,
    };
    if (!vk_ok(vkCreateDescriptorPool(g.dev, &dpci, NULL, &p->pool),
               "vkCreateDescriptorPool(present)"))
        goto fail;
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = p->pool, .descriptorSetCount = 1, .pSetLayouts = &p->dsl,
    };
    if (!vk_ok(vkAllocateDescriptorSets(g.dev, &dsai, &p->set),
               "vkAllocateDescriptorSets(present)"))
        goto fail;

    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = vk_present_filter(src), .minFilter = vk_present_filter(src),
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0,
    };
    if (!vk_ok(vkCreateSampler(g.dev, &sci, NULL, &p->sampler), "vkCreateSampler(present)"))
        goto fail;

    VkAttachmentDescription att = {
        .format = dst, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = final_layout,
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
    if (!vk_ok(vkCreateRenderPass(g.dev, &rpci, NULL, &p->pass), "vkCreateRenderPass(present)"))
        goto fail;
    VkPipelineLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1, .pSetLayouts = &p->dsl };
    if (!vk_ok(vkCreatePipelineLayout(g.dev, &lci, NULL, &p->layout),
               "vkCreatePipelineLayout(present)"))
        goto fail;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
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
        .blendEnable = VK_FALSE, .colorWriteMask = 0xf,
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
    int ok = vk_ok(vk_create_graphics_pipelines_bigstack(VK_NULL_HANDLE, &gpci, &p->pipe),
                   "vkCreateGraphicsPipelines(present)");
    if (vs) vkDestroyShaderModule(g.dev, vs, NULL);
    if (fs) vkDestroyShaderModule(g.dev, fs, NULL);
    if (!ok) goto fail;
    p->used = 1;
    return p;

fail:
    if (vs) vkDestroyShaderModule(g.dev, vs, NULL);
    if (fs) vkDestroyShaderModule(g.dev, fs, NULL);
    if (p->pipe) vkDestroyPipeline(g.dev, p->pipe, NULL);
    if (p->layout) vkDestroyPipelineLayout(g.dev, p->layout, NULL);
    if (p->pass) vkDestroyRenderPass(g.dev, p->pass, NULL);
    if (p->pool) vkDestroyDescriptorPool(g.dev, p->pool, NULL);
    if (p->dsl) vkDestroyDescriptorSetLayout(g.dev, p->dsl, NULL);
    if (p->sampler) vkDestroySampler(g.dev, p->sampler, NULL);
    memset(p, 0, sizeof *p);
    return NULL;
}
