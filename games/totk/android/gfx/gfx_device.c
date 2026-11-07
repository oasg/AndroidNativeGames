/* Module: device. */
#include "gfx_internal.h"

void vk_log(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "totk-vk", "%s", buf);
#endif
    fprintf(stderr, "[totk-vk] %s\n", buf);
}

int vk_ok(VkResult r, const char *what) {
    if (r == VK_SUCCESS) return 1;
    vk_log("ERROR %s: VkResult %d", what, (int)r);
    return 0;
}

int vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, uint32_t *out) {
    for (uint32_t i = 0; i < g.mem.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (g.mem.memoryTypes[i].propertyFlags & want) == want) {
            *out = i;
            return 1;
        }
    }
    return 0;
}

void vk_destroy_swapchain_images_locked(void) {
    if (!g.dev) return;
    for (uint32_t i = 0; i < g.sc_count; i++) {
        if (g.sc_views[i]) vkDestroyImageView(g.dev, g.sc_views[i], NULL);
        g.sc_views[i] = VK_NULL_HANDLE;
        g.sc_images[i] = VK_NULL_HANDLE;
        g.sc_layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    g.sc_count = 0;
    if (g.swapchain) vkDestroySwapchainKHR(g.dev, g.swapchain, NULL);
    g.swapchain = VK_NULL_HANDLE;
    g.sc_extent.width = g.sc_extent.height = 0;
    g.sc_format = VK_FORMAT_UNDEFINED;
    g.sc_suboptimal = 0;
    g.surface_ready = 0;
}

void vk_destroy_swapchain_locked(void) {
    if (!g.dev) return;
    vk_destroy_swapchain_images_locked();
    if (g.surface) vkDestroySurfaceKHR(g.instance, g.surface, NULL);
    g.surface = VK_NULL_HANDLE;
    g.android_window = NULL;
    g.attach_width = g.attach_height = 0;
    g.attach_format = 0;
}

void vk_reset_locked(void) {
    if (g.dev) vkDeviceWaitIdle(g.dev);
    vk_destroy_swapchain_locked();
    if (g.dev) {
        if (g.fence) vkDestroyFence(g.dev, g.fence, NULL);
        if (g.pool) vkDestroyCommandPool(g.dev, g.pool, NULL);
        vk_present_cache_destroy_locked();
        vkDestroyDevice(g.dev, NULL);
    }
    if (g.instance) vkDestroyInstance(g.instance, NULL);
    memset(&g, 0, sizeof g);
}

int vk_init_locked(void) {
    if (g.tried) return g.ready;
    g.tried = 1;

    uint32_t ext_count = 0;
    VkExtensionProperties *exts = NULL;
    if (vkEnumerateInstanceExtensionProperties(NULL, &ext_count, NULL) == VK_SUCCESS && ext_count) {
        exts = calloc(ext_count, sizeof *exts);
        if (exts && vkEnumerateInstanceExtensionProperties(NULL, &ext_count, exts) != VK_SUCCESS) {
            free(exts);
            exts = NULL;
            ext_count = 0;
        }
    }
    for (uint32_t i = 0; i < ext_count; i++) {
        if (!strcmp(exts[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME)) g.has_surface_ext = 1;
        if (!strcmp(exts[i].extensionName, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) g.has_android_ext = 1;
    }
    free(exts);

    const char *enabled[2];
    uint32_t n_enabled = 0;
    if (g.has_surface_ext && g.has_android_ext) {
        enabled[n_enabled++] = VK_KHR_SURFACE_EXTENSION_NAME;
        enabled[n_enabled++] = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
    }

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "totk-android",
        .applicationVersion = 1,
        .pEngineName = "nvn-cpu",
        .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = n_enabled,
        .ppEnabledExtensionNames = n_enabled ? enabled : NULL,
    };
    VkResult r = vkCreateInstance(&ici, NULL, &g.instance);
    if (r != VK_SUCCESS) {
        /* Older Android Vulkan 1.0 loaders reject the 1.1 application
         * version; keep the 1.0 path and disable the negative viewport. */
        app.apiVersion = VK_API_VERSION_1_0;
        r = vkCreateInstance(&ici, NULL, &g.instance);
    }
    if (r != VK_SUCCESS && n_enabled) {
        vk_log("surface extensions unavailable at instance creation (%d); retrying offscreen-only", (int)r);
        g.has_surface_ext = g.has_android_ext = 0;
        n_enabled = 0;
        ici.enabledExtensionCount = 0;
        ici.ppEnabledExtensionNames = NULL;
        r = vkCreateInstance(&ici, NULL, &g.instance);
    }
    if (!vk_ok(r, "vkCreateInstance")) {
        vk_reset_locked();
        return 0;
    }

    uint32_t phys_count = 0;
    r = vkEnumeratePhysicalDevices(g.instance, &phys_count, NULL);
    if (r != VK_SUCCESS || !phys_count) {
        vk_log("ERROR no Vulkan physical device (result=%d count=%u)", (int)r, phys_count);
        vk_reset_locked();
        return 0;
    }
    VkPhysicalDevice *phys = calloc(phys_count, sizeof *phys);
    if (!phys) { vk_reset_locked(); return 0; }
    r = vkEnumeratePhysicalDevices(g.instance, &phys_count, phys);
    if (r != VK_SUCCESS) {
        free(phys);
        vk_reset_locked();
        return 0;
    }
    int chosen = -1;
    for (uint32_t i = 0; i < phys_count && chosen < 0; i++) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &qn, NULL);
        VkQueueFamilyProperties *q = calloc(qn, sizeof *q);
        if (!q) continue;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &qn, q);
        for (uint32_t j = 0; j < qn; j++) {
            if (q[j].queueCount && (q[j].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                chosen = (int)i;
                g.family = j;
                break;
            }
        }
        free(q);
    }
    free(phys);
    if (chosen < 0) {
        vk_log("ERROR no graphics queue family");
        vk_reset_locked();
        return 0;
    }
    g.phys = VK_NULL_HANDLE;
    /* Re-enumerate once to keep the chosen handle (the array above was freed). */
    {
        uint32_t n = 0;
        VkPhysicalDevice *p = NULL;
        vkEnumeratePhysicalDevices(g.instance, &n, NULL);
        p = calloc(n, sizeof *p);
        if (!p) { vk_reset_locked(); return 0; }
        vkEnumeratePhysicalDevices(g.instance, &n, p);
        if ((uint32_t)chosen < n) g.phys = p[chosen];
        free(p);
    }
    if (!g.phys) { vk_reset_locked(); return 0; }
    vkGetPhysicalDeviceProperties(g.phys, &g.props);
    VkFormatProperties dfp;
    vkGetPhysicalDeviceFormatProperties(g.phys, VK_FORMAT_D24_UNORM_S8_UINT, &dfp);
    g.depth_format = (dfp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                         ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT;
    vkGetPhysicalDeviceMemoryProperties(g.phys, &g.mem);
    g.has_swapchain_ext = 0;
    if (g.has_surface_ext && g.has_android_ext) {
        uint32_t dn = 0;
        vkEnumerateDeviceExtensionProperties(g.phys, NULL, &dn, NULL);
        VkExtensionProperties *de = calloc(dn, sizeof *de);
        if (de) {
            vkEnumerateDeviceExtensionProperties(g.phys, NULL, &dn, de);
            for (uint32_t i = 0; i < dn; i++)
                if (!strcmp(de[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) g.has_swapchain_ext = 1;
            free(de);
        }
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g.family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char *device_ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = g.has_swapchain_ext ? 1u : 0u,
        .ppEnabledExtensionNames = g.has_swapchain_ext ? device_ext : NULL,
    };
    r = vkCreateDevice(g.phys, &dci, NULL, &g.dev);
    if (!vk_ok(r, "vkCreateDevice")) { vk_reset_locked(); return 0; }
    vkGetDeviceQueue(g.dev, g.family, 0, &g.queue);

    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g.family,
    };
    if (!vk_ok(vkCreateCommandPool(g.dev, &pci, NULL, &g.pool), "vkCreateCommandPool")) {
        vk_reset_locked();
        return 0;
    }
    VkCommandBufferAllocateInfo cbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (!vk_ok(vkAllocateCommandBuffers(g.dev, &cbi, &g.cmd), "vkAllocateCommandBuffers")) {
        vk_reset_locked();
        return 0;
    }
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (!vk_ok(vkCreateFence(g.dev, &fci, NULL, &g.fence), "vkCreateFence")) {
        vk_reset_locked();
        return 0;
    }

    g.ready = 1;
    vk_log("instance=%p device=%p queue-family=%u api=%u.%u.%u device='%s'",
           (void *)g.instance, (void *)g.dev, g.family,
           VK_VERSION_MAJOR(g.props.apiVersion), VK_VERSION_MINOR(g.props.apiVersion),
           VK_VERSION_PATCH(g.props.apiVersion), g.props.deviceName);
    if (!g.has_surface_ext || !g.has_android_ext)
        vk_log("Android surface extensions unavailable; offscreen target only");
    else if (!g.has_swapchain_ext)
        vk_log("VK_KHR_swapchain unavailable; offscreen target only");
    return 1;
}

int gfx_backend_vulkan_init(void) {
    pthread_mutex_lock(&g_lock);
    int ok = vk_init_locked();
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int gfx_backend_vulkan_available(void) {
    return gfx_backend_vulkan_init();
}

void gfx_backend_vulkan_shutdown(void) {
    pthread_mutex_lock(&g_lock);
    vk_reset_locked();
    g.tried = 0;
    pthread_mutex_unlock(&g_lock);
}

const char *gfx_backend_name(void) {
    return g.ready ? "vulkan" : "software";
}

int gfx_backend_negative_viewport_supported(void) {
    if (!g.ready && !gfx_backend_vulkan_init()) return 0;
    return g.ready && g.props.apiVersion >= VK_API_VERSION_1_1;
}

void vk_log_bad_color_attachment(const char *what, uint32_t attachment,
                                        VkFormat fmt, const GfxRenderTargetLayout *layout,
                                        uint32_t width, uint32_t height,
                                        unsigned *counter) {
    unsigned n = (*counter)++;
    if (n >= 8) return;
    uint32_t bw = 0, bh = 0, bytes = 0;
    vk_format_block(fmt, &bw, &bh, &bytes);
    const char *tail = n == 7 ? " (8/8, further errors suppressed)" : "";
    if (width && height)
        vk_log("ERROR %s: attachment %u format %s(%d) block=%ux%u bpp=%u "
               "texture=%ux%u colors=%u depth=%d is not color%s",
               what, attachment, vk_format_name(fmt), (int)fmt, bw, bh, bytes,
               width, height, layout ? layout->color_count : 0,
               layout ? layout->depth_format : 0, tail);
    else
        vk_log("ERROR %s: attachment %u format %s(%d) block=%ux%u bpp=%u "
               "texture=?x? colors=%u depth=%d is not color%s",
               what, attachment, vk_format_name(fmt), (int)fmt, bw, bh, bytes,
               layout ? layout->color_count : 0,
               layout ? layout->depth_format : 0, tail);
}

int vk_submit_and_wait(VkCommandBuffer cmd, const char *what) {
    if (getenv("TOTK_ANDROID_TRACE_SHADER"))
        vk_log("submit begin %s cmd=%p", what, (void *)cmd);
    VkResult r = vkEndCommandBuffer(cmd);
    if (!vk_ok(r, "vkEndCommandBuffer")) return 0;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    r = vkResetFences(g.dev, 1, &g.fence);
    if (!vk_ok(r, "vkResetFences")) return 0;
    r = vkQueueSubmit(g.queue, 1, &si, g.fence);
    if (!vk_ok(r, "vkQueueSubmit")) return 0;
    r = vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX);
    if (!vk_ok(r, "vkWaitForFences")) return 0;
    if (getenv("TOTK_ANDROID_TRACE_SHADER"))
        vk_log("submit end %s", what);
    (void)what;
    return 1;
}

GfxFence *gfx_backend_fence_create(void) {
    GfxFence *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    return f;
}

void gfx_backend_fence_signal(GfxFence *fence) {
    if (!fence) return;
    pthread_mutex_lock(&g_lock);
    int done = 1;
    if (g.ready && g.queue) {
        VkResult r = vkQueueWaitIdle(g.queue);
        if (r != VK_SUCCESS) {
            vk_log("ERROR fence signal: vkQueueWaitIdle=%d", (int)r);
            done = 0;
        }
    }
    if (done) fence->signaled = 1;
    pthread_mutex_unlock(&g_lock);
}

int gfx_backend_fence_wait(GfxFence *fence, uint64_t timeout_ns) {
    if (!fence) return 0;
    pthread_mutex_lock(&g_lock);
    if (fence->signaled) {
        pthread_mutex_unlock(&g_lock);
        return 1;
    }
    int done = 0;
    if (!g.ready || !g.queue) {
        /* No GPU work was ever submitted; the fence is trivially idle. */
        done = 1;
    } else if (timeout_ns > 0) {
        VkResult r = vkQueueWaitIdle(g.queue);
        if (r == VK_SUCCESS) done = 1;
        else vk_log("ERROR fence wait: vkQueueWaitIdle=%d", (int)r);
    }
    if (done) fence->signaled = 1;
    pthread_mutex_unlock(&g_lock);
    return done;
}

void gfx_backend_fence_destroy(GfxFence *fence) {
    free(fence);
}

int gfx_backend_queue_idle(void) {
    pthread_mutex_lock(&g_lock);
    int idle = 1;
    if (g.ready && g.dev && g.queue) {
        VkResult r = vkQueueWaitIdle(g.queue);
        idle = r == VK_SUCCESS;
        if (!idle) vk_log("ERROR queue_idle: vkQueueWaitIdle=%d", (int)r);
    }
    pthread_mutex_unlock(&g_lock);
    return idle;
}

/* ------------------------------------------------------------------ */
/* Compute and texture transfer (Wave 0 contract).
 *
 * All entry points below are synchronous: they submit and wait before
 * returning.  Failures are explicit (0/NULL) and never reported as success.
 */


double vk_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
