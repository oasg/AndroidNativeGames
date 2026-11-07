/* Module: core. */
#include "gfx_internal.h"

int vk_create_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                 VkBuffer *buf, VkDeviceMemory *mem) {
    if (!size) size = 1;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (!vk_ok(vkCreateBuffer(g.dev, &bci, NULL, buf), "vkCreateBuffer(host)")) return 0;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.dev, *buf, &req);
    uint32_t mt = 0;
    if (!vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &mt)) {
        vk_log("ERROR no host-visible memory type");
        vkDestroyBuffer(g.dev, *buf, NULL);
        *buf = VK_NULL_HANDLE;
        return 0;
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = mt,
    };
    if (!vk_ok(vkAllocateMemory(g.dev, &mai, NULL, mem), "vkAllocateMemory(host)")) {
        vkDestroyBuffer(g.dev, *buf, NULL);
        *buf = VK_NULL_HANDLE;
        return 0;
    }
    if (!vk_ok(vkBindBufferMemory(g.dev, *buf, *mem, 0), "vkBindBufferMemory(host)")) {
        vkFreeMemory(g.dev, *mem, NULL);
        vkDestroyBuffer(g.dev, *buf, NULL);
        *buf = VK_NULL_HANDLE;
        *mem = VK_NULL_HANDLE;
        return 0;
    }
    return 1;
}

int vk_create_device_image(VkImageType type, VkFormat fmt, uint32_t w, uint32_t h,
                                  uint32_t d, uint32_t mips, uint32_t layers,
                                  VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                                  VkImageCreateFlags flags, VkImage *image,
                                  VkDeviceMemory *mem) {
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = flags,
        .imageType = type,
        .format = fmt,
        .extent = { w, h, d },
        .mipLevels = mips,
        .arrayLayers = layers,
        .samples = samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (!vk_ok(vkCreateImage(g.dev, &ici, NULL, image), "vkCreateImage")) return 0;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.dev, *image, &req);
    uint32_t mt = 0;
    if (!vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mt) &&
        !vk_mem_type(req.memoryTypeBits, 0, &mt)) {
        vk_log("ERROR no image memory type");
        vkDestroyImage(g.dev, *image, NULL);
        *image = VK_NULL_HANDLE;
        return 0;
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = mt,
    };
    if (!vk_ok(vkAllocateMemory(g.dev, &mai, NULL, mem), "vkAllocateMemory(image)")) {
        vkDestroyImage(g.dev, *image, NULL);
        *image = VK_NULL_HANDLE;
        return 0;
    }
    if (!vk_ok(vkBindImageMemory(g.dev, *image, *mem, 0), "vkBindImageMemory")) {
        vkFreeMemory(g.dev, *mem, NULL);
        vkDestroyImage(g.dev, *image, NULL);
        *image = VK_NULL_HANDLE;
        *mem = VK_NULL_HANDLE;
        return 0;
    }
    return 1;
}

int gfx_backend_attach_android_surface(void *window, uint32_t width, uint32_t height,
                                       uint32_t nvn_format) {
    if (!window) return 0;
    pthread_mutex_lock(&g_lock);
    if (!vk_init_locked()) { pthread_mutex_unlock(&g_lock); return 0; }
    if (!g.has_surface_ext || !g.has_android_ext || !g.has_swapchain_ext) {
        vk_log("ERROR Android surface/swapchain extensions unavailable");
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    vkDeviceWaitIdle(g.dev);
    vk_destroy_swapchain_locked();
    g.android_window = window;
    g.attach_width = width;
    g.attach_height = height;
    g.attach_format = nvn_format;
    int ok = vk_create_android_surface_locked(window) && vk_create_swapchain_locked();
    if (!ok) {
        vk_destroy_swapchain_images_locked();
        if (g.surface) vkDestroySurfaceKHR(g.instance, g.surface, NULL);
        g.surface = VK_NULL_HANDLE;
        g.android_window = NULL;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void gfx_backend_detach_android_surface(void) {
    pthread_mutex_lock(&g_lock);
    if (g.dev) vkDeviceWaitIdle(g.dev);
    vk_destroy_swapchain_locked();
    pthread_mutex_unlock(&g_lock);
}

int vk_make_buffer_usage(uint32_t size, VkBufferUsageFlags usage,
                                VkBuffer *buf, VkDeviceMemory *mem) {
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size, .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (!vk_ok(vkCreateBuffer(g.dev, &bci, NULL, buf), "vkCreateBuffer(reflection)")) return 0;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(g.dev, *buf, &req);
    uint32_t mt = 0;
    if (!vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &mt)) return 0;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = mt };
    if (!vk_ok(vkAllocateMemory(g.dev, &mai, NULL, mem), "vkAllocateMemory(reflection)")) return 0;
    if (!vk_ok(vkBindBufferMemory(g.dev, *buf, *mem, 0), "vkBindBufferMemory(reflection)")) return 0;
    void *mapped = NULL;
    if (!vk_ok(vkMapMemory(g.dev, *mem, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory(reflection)")) return 0;
    memset(mapped, 0, size);
    vkUnmapMemory(g.dev, *mem);
    return 1;
}

int vk_make_buffer(uint32_t size, VkBuffer *buf, VkDeviceMemory *mem) {
    return vk_make_buffer_usage(size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                buf, mem);
}

int vk_make_buffer_view(uint32_t size, int storage, VkFormat format, VkBuffer *buf,
                               VkDeviceMemory *mem, VkBufferView *view) {
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        (storage ? VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT
                 : VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
    if (!vk_make_buffer_usage(size, usage, buf, mem)) return 0;
    VkBufferViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
        .buffer = *buf, .format = format, .offset = 0, .range = size };
    return vk_ok(vkCreateBufferView(g.dev, &vci, NULL, view),
                 "vkCreateBufferView(reflection)");
}

int vk_make_storage_image(uint32_t w, uint32_t h, VkFormat format,
                                 VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    if (format == VK_FORMAT_UNDEFINED) format = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(g.phys, format, &props);
    if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
        format = VK_FORMAT_R32_UINT;
        vkGetPhysicalDeviceFormatProperties(g.phys, format, &props);
        if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
            format = VK_FORMAT_R8G8B8A8_UNORM;
    }
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_GENERAL };
    if (!vk_ok(vkCreateImage(g.dev, &ici, NULL, img), "vkCreateImage(reflection storage)")) return 0;
    VkMemoryRequirements req; vkGetImageMemoryRequirements(g.dev, *img, &req);
    uint32_t mt = 0;
    if (!vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mt) &&
        !vk_mem_type(req.memoryTypeBits, 0, &mt)) return 0;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = mt };
    if (!vk_ok(vkAllocateMemory(g.dev, &mai, NULL, mem), "vkAllocateMemory(reflection storage)")) return 0;
    if (!vk_ok(vkBindImageMemory(g.dev, *img, *mem, 0), "vkBindImageMemory(reflection storage)")) return 0;
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    return vk_ok(vkCreateImageView(g.dev, &vci, NULL, view), "vkCreateImageView(reflection storage)");
}

int gfx_backend_validate_spirv(const uint8_t *code, size_t size, const char *name) {
    if (!code || !size) { vk_log("ERROR SPIR-V %s: empty module", name ? name : "?"); return 0; }
    if (size < 20 || (size & 3)) {
        vk_log("ERROR SPIR-V %s: size=%zu is not a whole SPIR-V header", name ? name : "?", size);
        return 0;
    }
    uint32_t words[5];
    memcpy(words, code, sizeof words);
    if (words[0] != 0x07230203u) {
        vk_log("ERROR SPIR-V %s: bad magic %#x", name ? name : "?", words[0]);
        return 0;
    }
    if (!gfx_backend_vulkan_init()) return 0;
    VkShaderModuleCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = (const uint32_t *)code,
    };
    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(g.dev, &sci, NULL, &mod);
    if (r != VK_SUCCESS) {
        vk_log("ERROR SPIR-V %s: vkCreateShaderModule=%d generator=%u version=%u.%u",
               name ? name : "?", (int)r, words[2], words[1] >> 16, (words[1] >> 8) & 0xff);
        return 0;
    }
    vk_log("SPIR-V module OK name='%s' bytes=%zu version=%u.%u", name ? name : "?", size,
           words[1] >> 16, (words[1] >> 8) & 0xff);
    vkDestroyShaderModule(g.dev, mod, NULL);
    return 1;
}

/* Clamp one axis of a region to [0, extent).  A negative start is cut from
 * the region (the caller sees how many texels were skipped through `cut`).
 * A zero length means "to the edge".  Returns 0 when nothing remains. */
int vk_clamp_axis(int32_t *pos, uint32_t *len, uint32_t extent, uint32_t *cut) {
    int64_t p = *pos;
    uint32_t l = *len;
    uint32_t c = 0;
    if (p < 0) {
        uint64_t skip = (uint64_t)(-p);
        if (skip >= l) return 0;
        c = (uint32_t)skip;
        p = 0;
        l -= c;
    }
    if ((uint64_t)p >= extent) return 0;
    if (l == 0) l = extent - (uint32_t)p;
    else if ((uint64_t)p + l > extent) l = extent - (uint32_t)p;
    if (!l) return 0;
    *pos = (int32_t)p;
    *len = l;
    if (cut) *cut = c;
    return 1;
}

int vk_clamp_region(int32_t *x, int32_t *y, int32_t *z,
                           uint32_t *w, uint32_t *h, uint32_t *d,
                           uint32_t ew, uint32_t eh, uint32_t ed,
                           uint32_t *cx, uint32_t *cy, uint32_t *cz) {
    uint32_t ax = 0, ay = 0, az = 0;
    if (!vk_clamp_axis(x, w, ew, &ax) ||
        !vk_clamp_axis(y, h, eh, &ay) ||
        !vk_clamp_axis(z, d, ed, &az))
        return 0;
    if (cx) *cx = ax;
    if (cy) *cy = ay;
    if (cz) *cz = az;
    return 1;
}

uint8_t vk_unorm8(float v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    return (uint8_t)(v * 255.0f + 0.5f);
}

/* Pack a clear colour for the sub-region path, which has to go through a
 * staging buffer because vkCmdClearColorImage only clears whole
 * subresources.  Unsupported formats are rejected explicitly; the whole-level
 * path still uses vkCmdClearColorImage for any colour format. */
int vk_pack_clear_texel(VkFormat fmt, const float rgba[4],
                               uint8_t *out, uint32_t *bpp) {
    VkFormatInfo info = vk_format_info(fmt);
    if (!info.bpp || vk_format_is_compressed(fmt) || vk_format_is_depth(fmt) ||
        fmt == VK_FORMAT_S8_UINT)
        return 0;
    *bpp = info.bpp;
    memset(out, 0, info.bpp);
    switch (fmt) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        out[0] = vk_unorm8(rgba[0]);
        out[1] = vk_unorm8(rgba[1]);
        out[2] = vk_unorm8(rgba[2]);
        out[3] = vk_unorm8(rgba[3]);
        return 1;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        out[0] = vk_unorm8(rgba[2]);
        out[1] = vk_unorm8(rgba[1]);
        out[2] = vk_unorm8(rgba[0]);
        out[3] = vk_unorm8(rgba[3]);
        return 1;
    case VK_FORMAT_R8_UNORM:
        out[0] = vk_unorm8(rgba[0]);
        return 1;
    case VK_FORMAT_R8G8_UNORM:
        out[0] = vk_unorm8(rgba[0]);
        out[1] = vk_unorm8(rgba[1]);
        return 1;
    default:
        return 0;
    }
}

/* Adreno's pipeline compiler can recurse; give it the same 128 MiB stack the
 * graphics path uses instead of the small default Android thread stack. */
VkResult vk_create_compute_pipeline_bigstack(
        VkPipelineCache cache, const VkComputePipelineCreateInfo *ci, VkPipeline *out) {
    VkComputeCompileJob job = { cache, ci, out, VK_ERROR_UNKNOWN };
    pthread_attr_t attr;
    pthread_t th;
    int attr_ok = pthread_attr_init(&attr) == 0;
    if (attr_ok && pthread_attr_setstacksize(&attr, 128u << 20) == 0 &&
        pthread_create(&th, &attr, vk_compute_compile_thread, &job) == 0) {
        pthread_attr_destroy(&attr);
        pthread_join(th, NULL);
        return job.result;
    }
    if (attr_ok) pthread_attr_destroy(&attr);
    /* Inline creation is still better than refusing a valid shader. */
    return vkCreateComputePipelines(g.dev, cache, 1, ci, NULL, out);
}

float vk_decode_ufloat11(uint32_t v) {
    uint32_t e = (v >> 6) & 0x1f, m = v & 0x3f;
    if (!e) return ldexpf((float)m, -20);
    return ldexpf(1.0f + (float)m / 64.0f, (int)e - 15);
}

float vk_decode_ufloat10(uint32_t v) {
    uint32_t e = (v >> 5) & 0x1f, m = v & 0x1f;
    if (!e) return ldexpf((float)m, -19);
    return ldexpf(1.0f + (float)m / 32.0f, (int)e - 15);
}

/* VK_FORMAT_B10G11R11_UFLOAT_PACK32: R in the low 11 bits, G in bits
 * 11..21 and B in the high 10 bits (Vulkan lists packed components MSB
 * first). */
void vk_decode_b10g11r11(uint32_t v, float out[3]) {
    out[0] = vk_decode_ufloat11(v & 0x7ffu);
    out[1] = vk_decode_ufloat11((v >> 11) & 0x7ffu);
    out[2] = vk_decode_ufloat10((v >> 22) & 0x3ffu);
}
