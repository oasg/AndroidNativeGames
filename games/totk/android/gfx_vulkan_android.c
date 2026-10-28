/* Vulkan backend for the Android TOTK port.
 *
 * This is a 2B.1 bring-up backend, not a renderer replacement yet.  It owns the
 * Android Vulkan instance/device/queue, an offscreen VkImage render target and
 * an optional VkSwapchainKHR for a real ANativeWindow.  The NVN object layer in
 * nvn_android.c keeps the object graph, format/layout semantics and submission
 * order; this file only turns the selected resources into Vulkan work.
 *
 * Errors are explicit.  If Vulkan is unavailable, `gfx_backend_vulkan_init`
 * returns 0 and the caller can select the software path; when the caller asks
 * for Vulkan specifically, no fake success object is produced.
 */
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <vulkan/vulkan.h>
#include <android/native_window.h>

#include "gfx_backend_android.h"
#include "triangle_spv.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include <stdlib.h>
#include <string.h>

#define VK_MAX_SWAPCHAIN_IMAGES 8

typedef struct {
    VkFormat format;
    const char *name;
    uint32_t bpp;
} VkFormatInfo;

typedef struct {
    int tried;
    int ready;

    VkInstance instance;
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties props;
    VkFormat depth_format;
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkPhysicalDeviceMemoryProperties mem;

    int has_surface_ext;
    int has_android_ext;
    int has_swapchain_ext;

    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    uint32_t sc_count;
    VkImage sc_images[VK_MAX_SWAPCHAIN_IMAGES];
    VkImageView sc_views[VK_MAX_SWAPCHAIN_IMAGES];
    VkImageLayout sc_layouts[VK_MAX_SWAPCHAIN_IMAGES];
    VkExtent2D sc_extent;
    VkFormat sc_format;
    int surface_ready;
    unsigned surface_presents;
} VulkanState;

struct GfxOffscreenTarget {
    GfxRenderTargetLayout rt_layout;
    VkImage color_image[GFX_MAX_COLOR_TARGETS];
    VkDeviceMemory color_mem[GFX_MAX_COLOR_TARGETS];
    VkImageView color_view[GFX_MAX_COLOR_TARGETS];
    VkBuffer color_readback[GFX_MAX_COLOR_TARGETS];
    VkDeviceMemory color_readback_mem[GFX_MAX_COLOR_TARGETS];
    uint8_t *color_pixels[GFX_MAX_COLOR_TARGETS];
    size_t color_size[GFX_MAX_COLOR_TARGETS];
    VkFormat color_format[GFX_MAX_COLOR_TARGETS];
    uint32_t color_bpp[GFX_MAX_COLOR_TARGETS];
    VkImage depth_image;
    VkDeviceMemory depth_mem;
    VkImageView depth_view;
    VkFormat depth_format;
    uint32_t width;
    uint32_t height;
    VkImageLayout layout_state;
    unsigned presents;
};

static VulkanState g;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void vk_log(const char *fmt, ...) {
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

static int vk_ok(VkResult r, const char *what) {
    if (r == VK_SUCCESS) return 1;
    vk_log("ERROR %s: VkResult %d", what, (int)r);
    return 0;
}

static int vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, uint32_t *out) {
    for (uint32_t i = 0; i < g.mem.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (g.mem.memoryTypes[i].propertyFlags & want) == want) {
            *out = i;
            return 1;
        }
    }
    return 0;
}

static void vk_destroy_swapchain_locked(void) {
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
    if (g.surface) vkDestroySurfaceKHR(g.instance, g.surface, NULL);
    g.surface = VK_NULL_HANDLE;
    g.surface_ready = 0;
}

static void vk_reset_locked(void) {
    if (g.dev) vkDeviceWaitIdle(g.dev);
    vk_destroy_swapchain_locked();
    if (g.dev) {
        if (g.fence) vkDestroyFence(g.dev, g.fence, NULL);
        if (g.pool) vkDestroyCommandPool(g.dev, g.pool, NULL);
        vkDestroyDevice(g.dev, NULL);
    }
    if (g.instance) vkDestroyInstance(g.instance, NULL);
    memset(&g, 0, sizeof g);
}

static int vk_init_locked(void) {
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
        .apiVersion = VK_API_VERSION_1_0,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = n_enabled,
        .ppEnabledExtensionNames = n_enabled ? enabled : NULL,
    };
    VkResult r = vkCreateInstance(&ici, NULL, &g.instance);
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

static VkFormatInfo vk_format_info(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: return (VkFormatInfo){f, "R8_UNORM", 1};
    case VK_FORMAT_R8_SNORM: return (VkFormatInfo){f, "R8_SNORM", 1};
    case VK_FORMAT_R8_UINT: return (VkFormatInfo){f, "R8_UINT", 1};
    case VK_FORMAT_R8_SINT: return (VkFormatInfo){f, "R8_SINT", 1};
    case VK_FORMAT_R8G8_UNORM: return (VkFormatInfo){f, "R8G8_UNORM", 2};
    case VK_FORMAT_R8G8_SNORM: return (VkFormatInfo){f, "R8G8_SNORM", 2};
    case VK_FORMAT_R8G8_UINT: return (VkFormatInfo){f, "R8G8_UINT", 2};
    case VK_FORMAT_R8G8_SINT: return (VkFormatInfo){f, "R8G8_SINT", 2};
    case VK_FORMAT_R8G8B8_UNORM: return (VkFormatInfo){f, "R8G8B8_UNORM", 3};
    case VK_FORMAT_R8G8B8_SNORM: return (VkFormatInfo){f, "R8G8B8_SNORM", 3};
    case VK_FORMAT_R8G8B8_UINT: return (VkFormatInfo){f, "R8G8B8_UINT", 3};
    case VK_FORMAT_R8G8B8_SINT: return (VkFormatInfo){f, "R8G8B8_SINT", 3};
    case VK_FORMAT_R8G8B8A8_UNORM: return (VkFormatInfo){f, "R8G8B8A8_UNORM", 4};
    case VK_FORMAT_R8G8B8A8_SRGB: return (VkFormatInfo){f, "R8G8B8A8_SRGB", 4};
    case VK_FORMAT_R5G6B5_UNORM_PACK16: return (VkFormatInfo){f, "R5G6B5_UNORM", 2};
    case VK_FORMAT_R4G4B4A4_UNORM_PACK16: return (VkFormatInfo){f, "R4G4B4A4_UNORM", 2};
    case VK_FORMAT_R5G5B5A1_UNORM_PACK16: return (VkFormatInfo){f, "R5G5B5A1_UNORM", 2};
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return (VkFormatInfo){f, "A2B10G10R10_UNORM", 4};
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: return (VkFormatInfo){f, "A2B10G10R10_UINT", 4};
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return (VkFormatInfo){f, "B10G11R11_UFLOAT", 4};
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return (VkFormatInfo){f, "E5B9G9R9_UFLOAT", 4};
    case VK_FORMAT_R16_UNORM: return (VkFormatInfo){f, "R16_UNORM", 2};
    case VK_FORMAT_R16_SNORM: return (VkFormatInfo){f, "R16_SNORM", 2};
    case VK_FORMAT_R16_UINT: return (VkFormatInfo){f, "R16_UINT", 2};
    case VK_FORMAT_R16_SINT: return (VkFormatInfo){f, "R16_SINT", 2};
    case VK_FORMAT_R16_SFLOAT: return (VkFormatInfo){f, "R16_SFLOAT", 2};
    case VK_FORMAT_R16G16_UNORM: return (VkFormatInfo){f, "R16G16_UNORM", 4};
    case VK_FORMAT_R16G16_SNORM: return (VkFormatInfo){f, "R16G16_SNORM", 4};
    case VK_FORMAT_R16G16_UINT: return (VkFormatInfo){f, "R16G16_UINT", 4};
    case VK_FORMAT_R16G16_SINT: return (VkFormatInfo){f, "R16G16_SINT", 4};
    case VK_FORMAT_R16G16_SFLOAT: return (VkFormatInfo){f, "R16G16_SFLOAT", 4};
    case VK_FORMAT_R16G16B16_UNORM: return (VkFormatInfo){f, "R16G16B16_UNORM", 6};
    case VK_FORMAT_R16G16B16_SNORM: return (VkFormatInfo){f, "R16G16B16_SNORM", 6};
    case VK_FORMAT_R16G16B16_UINT: return (VkFormatInfo){f, "R16G16B16_UINT", 6};
    case VK_FORMAT_R16G16B16_SINT: return (VkFormatInfo){f, "R16G16B16_SINT", 6};
    case VK_FORMAT_R16G16B16_SFLOAT: return (VkFormatInfo){f, "R16G16B16_SFLOAT", 6};
    case VK_FORMAT_R16G16B16A16_UNORM: return (VkFormatInfo){f, "R16G16B16A16_UNORM", 8};
    case VK_FORMAT_R16G16B16A16_SNORM: return (VkFormatInfo){f, "R16G16B16A16_SNORM", 8};
    case VK_FORMAT_R16G16B16A16_UINT: return (VkFormatInfo){f, "R16G16B16A16_UINT", 8};
    case VK_FORMAT_R16G16B16A16_SINT: return (VkFormatInfo){f, "R16G16B16A16_SINT", 8};
    case VK_FORMAT_R16G16B16A16_SFLOAT: return (VkFormatInfo){f, "R16G16B16A16_SFLOAT", 8};
    case VK_FORMAT_R32_SFLOAT: return (VkFormatInfo){f, "R32_SFLOAT", 4};
    case VK_FORMAT_R32_UINT: return (VkFormatInfo){f, "R32_UINT", 4};
    case VK_FORMAT_R32_SINT: return (VkFormatInfo){f, "R32_SINT", 4};
    case VK_FORMAT_R32G32_SFLOAT: return (VkFormatInfo){f, "R32G32_SFLOAT", 8};
    case VK_FORMAT_R32G32_UINT: return (VkFormatInfo){f, "R32G32_UINT", 8};
    case VK_FORMAT_R32G32_SINT: return (VkFormatInfo){f, "R32G32_SINT", 8};
    case VK_FORMAT_R32G32B32_SFLOAT: return (VkFormatInfo){f, "R32G32B32_SFLOAT", 12};
    case VK_FORMAT_R32G32B32_UINT: return (VkFormatInfo){f, "R32G32B32_UINT", 12};
    case VK_FORMAT_R32G32B32_SINT: return (VkFormatInfo){f, "R32G32B32_SINT", 12};
    case VK_FORMAT_R32G32B32A32_SFLOAT: return (VkFormatInfo){f, "R32G32B32A32_SFLOAT", 16};
    case VK_FORMAT_R32G32B32A32_UINT: return (VkFormatInfo){f, "R32G32B32A32_UINT", 16};
    case VK_FORMAT_R32G32B32A32_SINT: return (VkFormatInfo){f, "R32G32B32A32_SINT", 16};
    case VK_FORMAT_B8G8R8A8_UNORM: return (VkFormatInfo){f, "B8G8R8A8_UNORM", 4};
    case VK_FORMAT_A2B10G10R10_SNORM_PACK32: return (VkFormatInfo){f, "A2B10G10R10_SNORM", 4};
    case VK_FORMAT_D16_UNORM: return (VkFormatInfo){f, "D16_UNORM", 2};
    case VK_FORMAT_X8_D24_UNORM_PACK32: return (VkFormatInfo){f, "X8_D24_UNORM", 4};
    case VK_FORMAT_D24_UNORM_S8_UINT: return (VkFormatInfo){f, "D24_UNORM_S8", 4};
    case VK_FORMAT_D32_SFLOAT: return (VkFormatInfo){f, "D32_SFLOAT", 4};
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return (VkFormatInfo){f, "D32_SFLOAT_S8", 8};
    case VK_FORMAT_S8_UINT: return (VkFormatInfo){f, "S8_UINT", 1};
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return (VkFormatInfo){f, "BC1_RGB_UNORM", 8};
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return (VkFormatInfo){f, "BC1_RGBA_UNORM", 8};
    case VK_FORMAT_BC2_UNORM_BLOCK: return (VkFormatInfo){f, "BC2_UNORM", 16};
    case VK_FORMAT_BC3_UNORM_BLOCK: return (VkFormatInfo){f, "BC3_UNORM", 16};
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return (VkFormatInfo){f, "BC1_RGB_SRGB", 8};
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return (VkFormatInfo){f, "BC1_RGBA_SRGB", 8};
    case VK_FORMAT_BC2_SRGB_BLOCK: return (VkFormatInfo){f, "BC2_SRGB", 16};
    case VK_FORMAT_BC3_SRGB_BLOCK: return (VkFormatInfo){f, "BC3_SRGB", 16};
    case VK_FORMAT_BC4_UNORM_BLOCK: return (VkFormatInfo){f, "BC4_UNORM", 8};
    case VK_FORMAT_BC4_SNORM_BLOCK: return (VkFormatInfo){f, "BC4_SNORM", 8};
    case VK_FORMAT_BC5_UNORM_BLOCK: return (VkFormatInfo){f, "BC5_UNORM", 16};
    case VK_FORMAT_BC5_SNORM_BLOCK: return (VkFormatInfo){f, "BC5_SNORM", 16};
    case VK_FORMAT_BC7_UNORM_BLOCK: return (VkFormatInfo){f, "BC7_UNORM", 16};
    case VK_FORMAT_BC7_SRGB_BLOCK: return (VkFormatInfo){f, "BC7_SRGB", 16};
    default: return (VkFormatInfo){VK_FORMAT_UNDEFINED, "UNSUPPORTED", 0};
    }
}

int gfx_backend_vk_format_for_nvn(uint32_t f) {
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;
    case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;
    case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;
    case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;
    case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;
    case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;
    case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;
    case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;
    case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;
    case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;
    case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;
    case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;
    case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;
    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;
    case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;
    case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;
    case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x31: return VK_FORMAT_S8_UINT;
    case 0x32: return VK_FORMAT_D16_UNORM;
    case 0x33: return VK_FORMAT_X8_D24_UNORM_PACK32;
    case 0x34: return VK_FORMAT_D32_SFLOAT;
    case 0x35: return VK_FORMAT_D24_UNORM_S8_UINT;
    case 0x36: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 0x37: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x38: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x39: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case 0x3A: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case 0x3B: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case 0x3C: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x41: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case 0x42: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case 0x43: return VK_FORMAT_BC2_UNORM_BLOCK;
    case 0x44: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 0x45: return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
    case 0x46: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case 0x47: return VK_FORMAT_BC2_SRGB_BLOCK;
    case 0x48: return VK_FORMAT_BC3_SRGB_BLOCK;
    case 0x49: return VK_FORMAT_BC4_UNORM_BLOCK;
    case 0x4A: return VK_FORMAT_BC4_SNORM_BLOCK;
    case 0x4B: return VK_FORMAT_BC5_UNORM_BLOCK;
    case 0x4C: return VK_FORMAT_BC5_SNORM_BLOCK;
    case 0x4D: return VK_FORMAT_BC7_UNORM_BLOCK;
    case 0x4E: return VK_FORMAT_BC7_SRGB_BLOCK;
    /* NVN "unconverted" vertex formats. */
    case 0x51: return VK_FORMAT_R8_UINT;
    case 0x52: return VK_FORMAT_R8_SINT;
    case 0x53: return VK_FORMAT_R16_UINT;
    case 0x54: return VK_FORMAT_R16_SINT;
    case 0x55: return VK_FORMAT_R32_UINT;
    case 0x56: return VK_FORMAT_R32_SINT;
    case 0x57: return VK_FORMAT_R8G8_UINT;
    case 0x58: return VK_FORMAT_R8G8_SINT;
    case 0x59: return VK_FORMAT_R16G16_UINT;
    case 0x5A: return VK_FORMAT_R16G16_SINT;
    case 0x5B: return VK_FORMAT_R32G32_UINT;
    case 0x5C: return VK_FORMAT_R32G32_SINT;
    case 0x5D: return VK_FORMAT_R8G8B8_UINT;
    case 0x5E: return VK_FORMAT_R8G8B8_SINT;
    case 0x5F: return VK_FORMAT_R16G16B16_UINT;
    case 0x60: return VK_FORMAT_R16G16B16_SINT;
    case 0x61: return VK_FORMAT_R32G32B32_UINT;
    case 0x62: return VK_FORMAT_R32G32B32_SINT;
    case 0x63: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x64: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x65: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x66: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x67: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x68: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x6D: return VK_FORMAT_R8G8B8A8_UNORM;
    /* NVN_FORMAT_BGRX8/BGRA8, also usable as vertex formats. */
    case 0x99: return VK_FORMAT_B8G8R8A8_UNORM;
    case 0x9A: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

/* Ryujinx.Graphics.Shader.TextureFormat, in declaration order.  The .res.json
 * writes the enum name, the Android loader converts it back to this number. */
int gfx_backend_vk_format_for_texture_format(uint32_t f) {
    switch (f) {
    case 1:  return VK_FORMAT_R8_UNORM;
    case 2:  return VK_FORMAT_R8_SNORM;
    case 3:  return VK_FORMAT_R8_UINT;
    case 4:  return VK_FORMAT_R8_SINT;
    case 5:  return VK_FORMAT_R16_SFLOAT;
    case 6:  return VK_FORMAT_R16_UNORM;
    case 7:  return VK_FORMAT_R16_SNORM;
    case 8:  return VK_FORMAT_R16_UINT;
    case 9:  return VK_FORMAT_R16_SINT;
    case 10: return VK_FORMAT_R32_SFLOAT;
    case 11: return VK_FORMAT_R32_UINT;
    case 12: return VK_FORMAT_R32_SINT;
    case 13: return VK_FORMAT_R8G8_UNORM;
    case 14: return VK_FORMAT_R8G8_SNORM;
    case 15: return VK_FORMAT_R8G8_UINT;
    case 16: return VK_FORMAT_R8G8_SINT;
    case 17: return VK_FORMAT_R16G16_SFLOAT;
    case 18: return VK_FORMAT_R16G16_UNORM;
    case 19: return VK_FORMAT_R16G16_SNORM;
    case 20: return VK_FORMAT_R16G16_UINT;
    case 21: return VK_FORMAT_R16G16_SINT;
    case 22: return VK_FORMAT_R32G32_SFLOAT;
    case 23: return VK_FORMAT_R32G32_UINT;
    case 24: return VK_FORMAT_R32G32_SINT;
    case 25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 27: return VK_FORMAT_R8G8B8A8_UINT;
    case 28: return VK_FORMAT_R8G8B8A8_SINT;
    case 29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 30: return VK_FORMAT_R16G16B16A16_UNORM;
    case 31: return VK_FORMAT_R16G16B16A16_SNORM;
    case 32: return VK_FORMAT_R16G16B16A16_UINT;
    case 33: return VK_FORMAT_R16G16B16A16_SINT;
    case 34: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 35: return VK_FORMAT_R32G32B32A32_UINT;
    case 36: return VK_FORMAT_R32G32B32A32_SINT;
    case 37: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 38: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 39: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}

int gfx_backend_vk_format_bpp(int vk_format) {
    VkFormatInfo i = vk_format_info((VkFormat)vk_format);
    return (int)i.bpp;
}

static const char *vk_format_name(VkFormat f) {
    return vk_format_info(f).name;
}

static int vk_format_is_color_target(VkFormat f) {
    switch (f) {
    case VK_FORMAT_UNDEFINED:
    case VK_FORMAT_S8_UINT:
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
        return 0;
    default:
        return vk_format_info(f).bpp != 0;
    }
}

static int vk_format_is_depth(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return 1;
    default:
        return 0;
    }
}

static int vk_format_is_compressed(VkFormat f) {
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
        return 1;
    default:
        return 0;
    }
}

/* Tightly packed block geometry for one formatted image slice.  Compressed
 * formats are 4x4 blocks sized 8 or 16 bytes; everything else is 1x1 texels. */
static int vk_format_block(VkFormat f, uint32_t *bw, uint32_t *bh, uint32_t *bytes) {
    VkFormatInfo info = vk_format_info(f);
    if (info.bpp == 0) return 0;
    if (vk_format_is_compressed(f)) {
        *bw = 4; *bh = 4; *bytes = info.bpp;
    } else {
        *bw = 1; *bh = 1; *bytes = info.bpp;
    }
    return 1;
}

static VkImageAspectFlags vk_format_copy_aspect(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

/* A sampled texture view can only expose the depth aspect of a combined
 * depth/stencil format. */
static VkImageAspectFlags vk_format_view_aspect(VkFormat f) {
    if (vk_format_is_depth(f)) return VK_IMAGE_ASPECT_DEPTH_BIT;
    if (f == VK_FORMAT_S8_UINT) return VK_IMAGE_ASPECT_STENCIL_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

static VkSampleCountFlagBits vk_sample_count(uint32_t samples) {
    switch (samples) {
    case 1: return VK_SAMPLE_COUNT_1_BIT;
    case 2: return VK_SAMPLE_COUNT_2_BIT;
    case 4: return VK_SAMPLE_COUNT_4_BIT;
    case 8: return VK_SAMPLE_COUNT_8_BIT;
    case 16: return VK_SAMPLE_COUNT_16_BIT;
    case 32: return VK_SAMPLE_COUNT_32_BIT;
    case 64: return VK_SAMPLE_COUNT_64_BIT;
    default: return (VkSampleCountFlagBits)0;
    }
}

static int vk_format_can_texture(VkFormat f, VkFormatFeatureFlags *out_features) {
    if (f == VK_FORMAT_UNDEFINED || vk_format_info(f).bpp == 0) return 0;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g.phys, f, &fp);
    if (out_features) *out_features = fp.optimalTilingFeatures;
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (fp.optimalTilingFeatures & need) == need;
}

static int vk_image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
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

static int vk_image_barrier_range(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
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

static int vk_submit_and_wait(VkCommandBuffer cmd, const char *what) {
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
    (void)what;
    return 1;
}

/* Submission fences.  Every backend submission above is synchronous, so a
 * fence can only become signaled once the queue has actually drained.  We
 * still wait on the queue in signal()/wait() so a future async submit path
 * cannot report completion early. */
struct GfxFence {
    int signaled;
};

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

static int vk_create_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
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

static int vk_create_device_image(VkImageType type, VkFormat fmt, uint32_t w, uint32_t h,
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

static GfxOffscreenTarget *vk_offscreen_create_layout_locked(uint32_t width, uint32_t height,
                                                             const GfxRenderTargetLayout *layout) {
    if (!layout || layout->color_count < 1 || layout->color_count > GFX_MAX_COLOR_TARGETS) {
        vk_log("ERROR offscreen layout: color_count=%u", layout ? layout->color_count : 0);
        return NULL;
    }
    if (!width || !height || width > 16384 || height > 16384) {
        vk_log("ERROR offscreen size %ux%u", width, height);
        return NULL;
    }
    for (uint32_t i = 0; i < layout->color_count; i++) {
        VkFormat fmt = (VkFormat)layout->color_formats[i];
        VkFormatInfo info = vk_format_info(fmt);
        if (fmt == VK_FORMAT_UNDEFINED || !info.bpp) {
            vk_log("ERROR offscreen attachment %u: unsupported format %d", i, (int)fmt);
            return NULL;
        }
        if (!vk_format_is_color_target(fmt) || info.bpp < 1 || info.bpp > 16) {
            vk_log("ERROR offscreen attachment %u: %s is not a color format", i, info.name);
            return NULL;
        }
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g.phys, fmt, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((fp.optimalTilingFeatures & need) != need) {
            vk_log("ERROR offscreen attachment %u: %s lacks color/transfer feature (optimal=%#x)",
                   i, info.name, fp.optimalTilingFeatures);
            return NULL;
        }
    }
    VkFormat depth_fmt = (VkFormat)layout->depth_format;
    if (layout->depth_format) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g.phys, depth_fmt, &fp);
        if (!vk_format_is_depth(depth_fmt) ||
            !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            vk_log("ERROR offscreen depth format %d (%s) is not a depth attachment",
                   (int)depth_fmt, vk_format_name(depth_fmt));
            return NULL;
        }
    }

    GfxOffscreenTarget *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->rt_layout = *layout;
    t->width = width;
    t->height = height;
    t->layout_state = VK_IMAGE_LAYOUT_UNDEFINED;

    for (uint32_t i = 0; i < layout->color_count; i++) {
        VkFormat fmt = (VkFormat)layout->color_formats[i];
        VkFormatInfo info = vk_format_info(fmt);
        size_t bytes = (size_t)width * height * info.bpp;
        t->color_format[i] = fmt;
        t->color_bpp[i] = info.bpp;
        t->color_size[i] = bytes;
        t->color_pixels[i] = calloc(1, bytes ? bytes : 1);
        if (!t->color_pixels[i]) goto fail;
        if (!vk_create_device_image(VK_IMAGE_TYPE_2D, fmt, width, height, 1, 1, 1,
                                    VK_SAMPLE_COUNT_1_BIT,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    0, &t->color_image[i], &t->color_mem[i]))
            goto fail;
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = t->color_image[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = fmt,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 1,
            },
        };
        if (!vk_ok(vkCreateImageView(g.dev, &vci, NULL, &t->color_view[i]),
                   "vkCreateImageView(offscreen color)"))
            goto fail;
        if (!vk_create_host_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   &t->color_readback[i], &t->color_readback_mem[i]))
            goto fail;
    }

    if (layout->depth_format) {
        t->depth_format = depth_fmt;
        if (!vk_create_device_image(VK_IMAGE_TYPE_2D, depth_fmt, width, height, 1, 1, 1,
                                    VK_SAMPLE_COUNT_1_BIT,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                    0, &t->depth_image, &t->depth_mem))
            goto fail;
        VkImageViewCreateInfo dvci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = t->depth_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = depth_fmt,
            .subresourceRange = {
                .aspectMask = vk_format_is_depth(depth_fmt)
                                  ? (VkImageAspectFlags)(vk_format_copy_aspect(depth_fmt) &
                                                         (VK_IMAGE_ASPECT_DEPTH_BIT |
                                                          VK_IMAGE_ASPECT_STENCIL_BIT))
                                  : VK_IMAGE_ASPECT_DEPTH_BIT,
                .baseMipLevel = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 1,
            },
        };
        if (!vk_ok(vkCreateImageView(g.dev, &dvci, NULL, &t->depth_view),
                   "vkCreateImageView(offscreen depth)"))
            goto fail;
    }
    return t;

fail:
    gfx_backend_offscreen_destroy(t);
    return NULL;
}

GfxOffscreenTarget *gfx_backend_offscreen_create_layout(uint32_t width, uint32_t height,
                                                        const GfxRenderTargetLayout *layout) {
    pthread_mutex_lock(&g_lock);
    if (!vk_init_locked()) { pthread_mutex_unlock(&g_lock); return NULL; }
    GfxOffscreenTarget *t = vk_offscreen_create_layout_locked(width, height, layout);
    if (t) {
        vk_log("offscreen %ux%u color_count=%u layouts=%s,%s depth=%s image=%p",
               width, height, t->rt_layout.color_count,
               vk_format_name(t->color_format[0]),
               t->rt_layout.color_count > 1 ? vk_format_name(t->color_format[1]) : "-",
               t->depth_view ? vk_format_name(t->depth_format) : "none",
               (void *)t->color_image[0]);
    }
    pthread_mutex_unlock(&g_lock);
    return t;
}

GfxOffscreenTarget *gfx_backend_offscreen_create(uint32_t width, uint32_t height,
                                                  uint32_t nvn_format) {
    pthread_mutex_lock(&g_lock);
    if (!vk_init_locked()) { pthread_mutex_unlock(&g_lock); return NULL; }
    VkFormat fmt = (VkFormat)gfx_backend_vk_format_for_nvn(nvn_format);
    VkFormatInfo info = vk_format_info(fmt);
    if (fmt == VK_FORMAT_UNDEFINED || !info.bpp) {
        vk_log("ERROR no Vulkan format for NVN format %#x", nvn_format);
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    if (!vk_format_is_color_target(fmt) || info.bpp < 1 || info.bpp > 16) {
        vk_log("ERROR unsupported offscreen color format %s bpp=%u", info.name, info.bpp);
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    GfxRenderTargetLayout layout = { 0 };
    layout.color_count = 1;
    layout.color_formats[0] = (int)fmt;
    layout.depth_format = (int)g.depth_format;
    GfxOffscreenTarget *t = vk_offscreen_create_layout_locked(width, height, &layout);
    if (t)
        vk_log("offscreen %ux%u nvn=%#x vk=%s(%d) bpp=%u image=%p",
               width, height, nvn_format, info.name, (int)fmt, info.bpp,
               (void *)t->color_image[0]);
    pthread_mutex_unlock(&g_lock);
    return t;
}

void gfx_backend_offscreen_destroy(GfxOffscreenTarget *t) {
    if (!t) return;
    /* destroy is also called from the create failure path while g_lock is held;
     * keep it lock-free and only touch per-target objects. */
    if (g.dev) {
        for (uint32_t i = 0; i < t->rt_layout.color_count; i++) {
            if (t->color_readback[i]) vkDestroyBuffer(g.dev, t->color_readback[i], NULL);
            if (t->color_readback_mem[i]) vkFreeMemory(g.dev, t->color_readback_mem[i], NULL);
            if (t->color_view[i]) vkDestroyImageView(g.dev, t->color_view[i], NULL);
            if (t->color_image[i]) vkDestroyImage(g.dev, t->color_image[i], NULL);
            if (t->color_mem[i]) vkFreeMemory(g.dev, t->color_mem[i], NULL);
            free(t->color_pixels[i]);
        }
        if (t->depth_view) vkDestroyImageView(g.dev, t->depth_view, NULL);
        if (t->depth_image) vkDestroyImage(g.dev, t->depth_image, NULL);
        if (t->depth_mem) vkFreeMemory(g.dev, t->depth_mem, NULL);
    } else {
        for (uint32_t i = 0; i < t->rt_layout.color_count; i++) free(t->color_pixels[i]);
    }
    free(t);
}

int gfx_backend_offscreen_clear(GfxOffscreenTarget *t, float r, float g_, float b, float a) {
    if (!t) return 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready || !g.cmd) { pthread_mutex_unlock(&g_lock); return 0; }

    VkCommandBuffer cmd = g.cmd;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(clear)")) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    VkClearColorValue color = { .float32 = { r, g_, b, a } };
    VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1,
    };
    for (uint32_t i = 0; i < t->rt_layout.color_count; i++) {
        if (t->layout_state != VK_IMAGE_LAYOUT_GENERAL) {
            vk_image_barrier_range(cmd, t->color_image[i], t->layout_state, VK_IMAGE_LAYOUT_GENERAL,
                                   0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                   t->layout_state == VK_IMAGE_LAYOUT_UNDEFINED
                                       ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                       : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
        }
        vkCmdClearColorImage(cmd, t->color_image[i], VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
        VkBufferImageCopy region = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
            .imageOffset = { 0, 0, 0 },
            .imageExtent = { t->width, t->height, 1 },
        };
        vkCmdCopyImageToBuffer(cmd, t->color_image[i], VK_IMAGE_LAYOUT_GENERAL,
                               t->color_readback[i], 1, &region);
        VkBufferMemoryBarrier bb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = t->color_readback[i],
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, NULL, 1, &bb, 0, NULL);
    }
    if (!vk_submit_and_wait(cmd, "clear")) { pthread_mutex_unlock(&g_lock); return 0; }
    t->layout_state = VK_IMAGE_LAYOUT_GENERAL;

    for (uint32_t i = 0; i < t->rt_layout.color_count; i++) {
        void *mapped = NULL;
        if (!vk_ok(vkMapMemory(g.dev, t->color_readback_mem[i], 0, VK_WHOLE_SIZE, 0, &mapped),
                   "vkMapMemory(clear readback)")) {
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        memcpy(t->color_pixels[i], mapped, t->color_size[i]);
        vkUnmapMemory(g.dev, t->color_readback_mem[i]);
    }
    pthread_mutex_unlock(&g_lock);
    if (getenv("TOTK_ANDROID_TRACE_VK"))
        vk_log("clear %ux%u colors=%u color=(%.3f,%.3f,%.3f,%.3f) first=%02x%02x%02x%02x",
               t->width, t->height, t->rt_layout.color_count, r, g_, b, a,
               t->color_pixels[0][0], t->color_pixels[0][1],
               t->color_pixels[0][2], t->color_pixels[0][3]);
    return 1;
}

int gfx_backend_offscreen_read_pixel_index(const GfxOffscreenTarget *t, uint32_t index,
                                           uint32_t x, uint32_t y, uint8_t out[4]) {
    if (!t || !out || index >= t->rt_layout.color_count ||
        x >= t->width || y >= t->height) return 0;
    uint32_t bpp = t->color_bpp[index];
    size_t off = ((size_t)y * t->width + x) * bpp;
    if (off + bpp > t->color_size[index]) return 0;
    memset(out, 0, 4);
    memcpy(out, t->color_pixels[index] + off, bpp < 4 ? bpp : 4);
    return 1;
}

int gfx_backend_offscreen_read_pixel(GfxOffscreenTarget *t, uint32_t x, uint32_t y,
                                     uint8_t out[4]) {
    return gfx_backend_offscreen_read_pixel_index(t, 0, x, y, out);
}

uint32_t gfx_backend_offscreen_color_count(const GfxOffscreenTarget *t) {
    return t ? t->rt_layout.color_count : 0;
}

const GfxRenderTargetLayout *gfx_backend_offscreen_layout(const GfxOffscreenTarget *t) {
    return t ? &t->rt_layout : NULL;
}

int gfx_backend_offscreen_present(GfxOffscreenTarget *t) {
    if (!t) return 0;
    t->presents++;
    vk_log("present #%u offscreen %ux%u %s colors=%u", t->presents, t->width, t->height,
           vk_format_name(t->color_format[0]), t->rt_layout.color_count);
    return 1;
}

uint32_t gfx_backend_offscreen_width(const GfxOffscreenTarget *t) { return t ? t->width : 0; }
uint32_t gfx_backend_offscreen_height(const GfxOffscreenTarget *t) { return t ? t->height : 0; }
const uint8_t *gfx_backend_offscreen_pixels(const GfxOffscreenTarget *t, size_t *size) {
    if (size) *size = t ? t->color_size[0] : 0;
    return t ? t->color_pixels[0] : NULL;
}
const char *gfx_backend_offscreen_format_name(const GfxOffscreenTarget *t) {
    return t ? vk_format_name(t->color_format[0]) : "none";
}

static int vk_choose_surface_format(VkSurfaceKHR surface, VkFormat *out_format,
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

int gfx_backend_attach_android_surface(void *window, uint32_t width, uint32_t height,
                                       uint32_t nvn_format) {
    (void)nvn_format;
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

    VkAndroidSurfaceCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = (ANativeWindow *)window,
    };
    if (!vk_ok(vkCreateAndroidSurfaceKHR(g.instance, &sci, NULL, &g.surface),
               "vkCreateAndroidSurfaceKHR")) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    VkBool32 supported = VK_FALSE;
    if (!vk_ok(vkGetPhysicalDeviceSurfaceSupportKHR(g.phys, g.family, g.surface, &supported),
               "vkGetPhysicalDeviceSurfaceSupportKHR") || !supported) {
        vk_log("ERROR graphics queue family does not support the Android surface");
        vk_destroy_swapchain_locked();
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    VkSurfaceCapabilitiesKHR caps;
    if (!vk_ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps),
               "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
        vk_destroy_swapchain_locked();
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    if (!vk_choose_surface_format(g.surface, &fmt, &color_space)) {
        vk_log("ERROR no surface format");
        vk_destroy_swapchain_locked();
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = width ? width : 1280;
        extent.height = height ? height : 720;
        if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
        if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
        if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
        if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        vk_log("ERROR surface does not support color/transfer-dst swapchain usage (%#x)",
               caps.supportedUsageFlags);
        vk_destroy_swapchain_locked();
        pthread_mutex_unlock(&g_lock);
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
               "vkCreateSwapchainKHR")) {
        vk_destroy_swapchain_locked();
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    uint32_t count = VK_MAX_SWAPCHAIN_IMAGES;
    if (!vk_ok(vkGetSwapchainImagesKHR(g.dev, g.swapchain, &count, g.sc_images),
               "vkGetSwapchainImagesKHR")) {
        vk_destroy_swapchain_locked();
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    g.sc_count = count;
    for (uint32_t i = 0; i < count; i++) {
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = g.sc_images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = fmt,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        if (!vk_ok(vkCreateImageView(g.dev, &vci, NULL, &g.sc_views[i]), "vkCreateImageView(swapchain)")) {
            vk_destroy_swapchain_locked();
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        g.sc_layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    g.sc_extent = extent;
    g.sc_format = fmt;
    g.surface_ready = 1;
    vk_log("Android surface %ux%u format=%s images=%u", extent.width, extent.height,
           vk_format_name(fmt), count);
    pthread_mutex_unlock(&g_lock);
    return 1;
}

void gfx_backend_detach_android_surface(void) {
    pthread_mutex_lock(&g_lock);
    if (g.dev) vkDeviceWaitIdle(g.dev);
    vk_destroy_swapchain_locked();
    pthread_mutex_unlock(&g_lock);
}

int gfx_backend_surface_ready(void) { return g.surface_ready; }

int gfx_backend_surface_present(float r, float g_, float b, float a) {
    pthread_mutex_lock(&g_lock);
    if (!g.ready || !g.surface_ready || !g.swapchain) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    uint32_t idx = 0;
    VkResult ar = vkAcquireNextImageKHR(g.dev, g.swapchain, UINT64_MAX, VK_NULL_HANDLE,
                                        g.fence, &idx);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
        vk_log("swapchain out of date during acquire");
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
        vk_log("ERROR vkAcquireNextImageKHR: %d", (int)ar);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    VkResult wr = vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX);
    if (wr != VK_SUCCESS) {
        vk_log("ERROR vkWaitForFences(acquire): %d", (int)wr);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
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


struct GfxTrianglePipeline {
    VkShaderModule vs;
    VkShaderModule ps;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool dpool;
    VkDescriptorSet dset;
    VkBuffer ubo[8];
    VkDeviceMemory ubo_mem[8];
    uint32_t ubo_binding[8];
    uint32_t ubo_size[8];
    uint32_t nubo;
    VkPipelineLayout layout;
    VkRenderPass pass;
    VkPipeline pipe;
    VkFormat format;
    VkFramebuffer fb;
    uint32_t fb_w, fb_h;
    VkImageView fb_view;
};

static int vk_shader_module(const uint8_t *code, size_t size, const char *what,
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
    return vk_ok(vkCreateShaderModule(g.dev, &ci, NULL, out), what);
}

static GfxTrianglePipeline *triangle_create_impl(const uint8_t *vs, size_t vs_size,
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
    if (!vk_ok(vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &gpci, NULL, &p->pipe),
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

static GfxTrianglePipeline *g_builtin_triangle;
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


struct GfxTexture {
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    VkSampler sampler;
    uint32_t width, height, depth;
    uint32_t mip_levels, array_layers, samples;
    GfxTextureTarget target;
    VkFormat format;
    VkImageViewType view_type;
    VkImageLayout layout;
    int readback_ok;
};

static void vk_mip_extent(const GfxTextureDesc *d, uint32_t level,
                          uint32_t *w, uint32_t *h, uint32_t *depth) {
    *w = d->width >> level; if (!*w) *w = 1;
    *h = d->height >> level; if (!*h) *h = 1;
    *depth = d->target == GFX_TEXTURE_3D ? (d->depth >> level) : 1;
    if (!*depth) *depth = 1;
}

/* Tightly packed size of one mip slice (all depth slices for a 3D image). */
static size_t vk_texture_slice_size(const GfxTextureDesc *d, uint32_t level) {
    uint32_t w, h, depth, bw, bh, bytes;
    if (!vk_format_block((VkFormat)d->vk_format, &bw, &bh, &bytes)) return 0;
    vk_mip_extent(d, level, &w, &h, &depth);
    size_t bx = (w + bw - 1) / bw;
    size_t by = (h + bh - 1) / bh;
    return bx * by * bytes * depth;
}

static GfxTexture *texture_create_desc_locked(const GfxTextureDesc *desc,
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

    VkImageUsageFlags usage;
    if (samples == VK_SAMPLE_COUNT_1_BIT) {
        usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (t->readback_ok) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    } else {
        usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        if (vk_format_is_color_target(fmt)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (!vk_create_device_image(image_type, fmt, desc->width, desc->height, depth,
                                desc->mip_levels, desc->array_layers, samples,
                                usage, flags, &t->image, &t->mem))
        goto fail;
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

    if (count) {
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

static GfxTexture *texture_create_rgba8_locked(uint32_t width, uint32_t height,
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

static int texture_read_pixels_locked(const GfxTexture *t, uint32_t level, uint32_t layer,
                                      void *dst, size_t size) {
    if (!t || !dst) return 0;
    if (t->samples != 1) {
        vk_log("ERROR texture read: %u-sample texture needs resolve", t->samples);
        return 0;
    }
    if (!t->readback_ok) {
        vk_log("ERROR texture read: %s was created without transfer-src support",
               vk_format_name(t->format));
        return 0;
    }
    if (level >= t->mip_levels || layer >= t->array_layers) {
        vk_log("ERROR texture read: level=%u/%u layer=%u/%u", level, t->mip_levels,
               layer, t->array_layers);
        return 0;
    }
    GfxTextureDesc desc = {
        .target = t->target,
        .vk_format = t->format,
        .width = t->width,
        .height = t->height,
        .depth = t->target == GFX_TEXTURE_3D ? t->depth : 1,
        .mip_levels = t->mip_levels,
        .array_layers = t->array_layers,
        .samples = t->samples,
    };
    if (t->target == GFX_TEXTURE_3D && layer != 0) return 0;
    size_t slice = vk_texture_slice_size(&desc, level);
    if (!slice || size < slice) {
        vk_log("ERROR texture read: dst size=%zu < %zu", size, slice);
        return 0;
    }
    uint32_t w, h, d;
    vk_mip_extent(&desc, level, &w, &h, &d);

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stage_mem = VK_NULL_HANDLE;
    if (!vk_create_host_buffer(slice, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &staging, &stage_mem))
        return 0;
    VkCommandBuffer cmd = g.cmd;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(texture read)")) {
        vkDestroyBuffer(g.dev, staging, NULL);
        vkFreeMemory(g.dev, stage_mem, NULL);
        return 0;
    }
    VkImageAspectFlags aspect = vk_format_copy_aspect(t->format);
    vk_image_barrier_range(cmd, t->image, t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           0, VK_ACCESS_TRANSFER_READ_BIT,
                           t->layout == VK_IMAGE_LAYOUT_UNDEFINED
                               ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                               : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           aspect, level, 1, t->target == GFX_TEXTURE_3D ? 0 : layer, 1);
    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = aspect,
            .mipLevel = level,
            .baseArrayLayer = t->target == GFX_TEXTURE_3D ? 0 : layer,
            .layerCount = 1,
        },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { w, h, d },
    };
    vkCmdCopyImageToBuffer(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1,
                           &region);
    vk_image_barrier_range(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           aspect, level, 1, t->target == GFX_TEXTURE_3D ? 0 : layer, 1);
    VkBufferMemoryBarrier bb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = staging, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &bb, 0, NULL);
    int submitted = vk_submit_and_wait(cmd, "texture read");
    if (submitted) ((GfxTexture *)t)->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!submitted) {
        vkDestroyBuffer(g.dev, staging, NULL);
        vkFreeMemory(g.dev, stage_mem, NULL);
        return 0;
    }
    void *mapped = NULL;
    if (!vk_ok(vkMapMemory(g.dev, stage_mem, 0, VK_WHOLE_SIZE, 0, &mapped),
               "vkMapMemory(texture read)")) {
        vkDestroyBuffer(g.dev, staging, NULL);
        vkFreeMemory(g.dev, stage_mem, NULL);
        return 0;
    }
    memcpy(dst, mapped, slice);
    vkUnmapMemory(g.dev, stage_mem);
    vkDestroyBuffer(g.dev, staging, NULL);
    vkFreeMemory(g.dev, stage_mem, NULL);
    return 1;
}

int gfx_backend_texture_read_pixels(const GfxTexture *texture, uint32_t level, uint32_t layer,
                                    void *dst, size_t size) {
    pthread_mutex_lock(&g_lock);
    int ok = texture_read_pixels_locked(texture, level, layer, dst, size);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void gfx_backend_texture_destroy(GfxTexture *t) {
    if (!t) return;
    if (g.dev) {
        if (t->sampler) vkDestroySampler(g.dev, t->sampler, NULL);
        if (t->view) vkDestroyImageView(g.dev, t->view, NULL);
        if (t->image) vkDestroyImage(g.dev, t->image, NULL);
        if (t->mem) vkFreeMemory(g.dev, t->mem, NULL);
    }
    free(t);
}

struct GfxTexturedPipeline {
    VkShaderModule vs, ps;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool dpool;
    VkDescriptorSet dset;
    VkPipelineLayout layout;
    VkRenderPass pass;
    VkPipeline pipe;
    VkFormat format;
    VkFramebuffer fb;
    uint32_t fb_w, fb_h;
    VkImageView fb_view;
};

GfxTexturedPipeline *gfx_backend_textured_pipeline_create(int vk_format) {
    pthread_mutex_lock(&g_lock);
    if (!vk_init_locked()) { pthread_mutex_unlock(&g_lock); return NULL; }
    VkFormat fmt = (VkFormat)vk_format;
    if (fmt == VK_FORMAT_UNDEFINED) { pthread_mutex_unlock(&g_lock); return NULL; }
    GfxTexturedPipeline *p = calloc(1, sizeof *p);
    if (!p) { pthread_mutex_unlock(&g_lock); return NULL; }
    p->format = fmt;
    if (!vk_shader_module((const uint8_t *)triangle_tex_vert_spv, triangle_tex_vert_spv_size,
                          "vkCreateShaderModule(tex vs)", &p->vs) ||
        !vk_shader_module((const uint8_t *)triangle_tex_frag_spv, triangle_tex_frag_spv_size,
                          "vkCreateShaderModule(tex ps)", &p->ps))
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
    VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1 };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps,
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
    if (!vk_ok(vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &gpci, NULL, &p->pipe),
               "vkCreateGraphicsPipelines(tex)")) goto fail;
    pthread_mutex_unlock(&g_lock);
    vk_log("textured triangle pipeline ready format=%s", vk_format_name(fmt));
    return p;
fail:
    gfx_backend_textured_pipeline_destroy(p);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

void gfx_backend_textured_pipeline_destroy(GfxTexturedPipeline *p) {
    if (!p) return;
    if (g.dev) {
        if (p->fb) vkDestroyFramebuffer(g.dev, p->fb, NULL);
        if (p->pipe) vkDestroyPipeline(g.dev, p->pipe, NULL);
        if (p->layout) vkDestroyPipelineLayout(g.dev, p->layout, NULL);
        if (p->pass) vkDestroyRenderPass(g.dev, p->pass, NULL);
        if (p->dpool) vkDestroyDescriptorPool(g.dev, p->dpool, NULL);
        if (p->dsl) vkDestroyDescriptorSetLayout(g.dev, p->dsl, NULL);
        if (p->vs) vkDestroyShaderModule(g.dev, p->vs, NULL);
        if (p->ps) vkDestroyShaderModule(g.dev, p->ps, NULL);
    }
    free(p);
}

int gfx_backend_textured_draw(GfxTexturedPipeline *p, GfxTexture *tex, GfxOffscreenTarget *t,
                              float r, float g_, float b, float a) {
    if (!p || !tex || !t) return 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready || t->rt_layout.color_count < 1 ||
        t->color_format[0] != p->format || t->color_bpp[0] != 4) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (!p->fb || p->fb_view != t->color_view[0] || p->fb_w != t->width || p->fb_h != t->height) {
        if (p->fb) vkDestroyFramebuffer(g.dev, p->fb, NULL);
        VkImageView view = t->color_view[0];
        VkFramebufferCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = p->pass, .attachmentCount = 1, .pAttachments = &view,
            .width = t->width, .height = t->height, .layers = 1,
        };
        if (!vk_ok(vkCreateFramebuffer(g.dev, &fci, NULL, &p->fb), "vkCreateFramebuffer(tex)")) {
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        p->fb_view = t->color_view[0]; p->fb_w = t->width; p->fb_h = t->height;
    }
    VkDescriptorImageInfo di = {
        .sampler = tex->sampler, .imageView = tex->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = p->dset, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &di,
    };
    vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);

    VkCommandBuffer cmd = g.cmd;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(tex)")) {
        pthread_mutex_unlock(&g_lock); return 0;
    }
    VkClearValue clear = { .color = { .float32 = { r, g_, b, a } } };
    VkRenderPassBeginInfo rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = p->pass, .framebuffer = p->fb,
        .renderArea = { { 0, 0 }, { t->width, t->height } },
        .clearValueCount = 1, .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &p->dset, 0, NULL);
    VkViewport viewport = { 0, 0, (float)t->width, (float)t->height, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, { t->width, t->height } };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
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
        .buffer = t->color_readback[0], .offset = 0, .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &bb, 0, NULL);
    if (!vk_submit_and_wait(cmd, "textured triangle")) {
        pthread_mutex_unlock(&g_lock); return 0;
    }
    t->layout_state = VK_IMAGE_LAYOUT_GENERAL;
    void *mapped = NULL;
    if (!vk_ok(vkMapMemory(g.dev, t->color_readback_mem[0], 0, VK_WHOLE_SIZE, 0, &mapped),
               "vkMapMemory(tex)")) {
        pthread_mutex_unlock(&g_lock); return 0;
    }
    memcpy(t->color_pixels[0], mapped, t->color_size[0]);
    vkUnmapMemory(g.dev, t->color_readback_mem[0]);
    pthread_mutex_unlock(&g_lock);
    return 1;
}


#define GFX_REFLECT_MAX_BINDINGS 64
#define GFX_REFLECT_RUNTIME_ARRAY 32
#define GFX_DESC_MAX_EPOCHS 512

struct GfxReflectedPipeline {
    VkShaderModule vs, ps;
    VkDescriptorSetLayout dsl[2];
    VkDescriptorPool dpool;
    VkDescriptorSet dset[2];
    VkPipelineLayout layout;
    VkRenderPass pass;
    VkPipeline pipe;
    VkFormat format;
    GfxRenderTargetLayout rt_layout;
    VkFramebuffer fb;
    uint32_t fb_w, fb_h;
    VkImageView fb_view;
    VkImageView fb_views[GFX_MAX_COLOR_TARGETS + 1];
    uint32_t fb_attach_count;
    GfxTexture *tex[GFX_REFLECT_MAX_BINDINGS];
    unsigned ntex;
    VkImage img[GFX_REFLECT_MAX_BINDINGS];
    VkDeviceMemory img_mem[GFX_REFLECT_MAX_BINDINGS];
    VkImageView img_view[GFX_REFLECT_MAX_BINDINGS];
    unsigned nimg;
    VkBuffer buf[GFX_REFLECT_MAX_BINDINGS];
    VkDeviceMemory buf_mem[GFX_REFLECT_MAX_BINDINGS];
    unsigned nbuf;
    VkBufferView buf_view[GFX_REFLECT_MAX_BINDINGS];
    unsigned nbuf_view;
    VkSampler sampler[GFX_REFLECT_MAX_BINDINGS];
    unsigned nsampler;
    /* Transient guest-backed streams for the translated draw.  Vertex streams
     * are indexed by NVN stream binding (0..15); the index buffer is one
     * upload per indexed draw. */
    VkBuffer vbuf[16];
    VkDeviceMemory vbuf_mem[16];
    size_t vbuf_cap[16];
    VkBuffer index_buf;
    VkDeviceMemory index_mem;
    size_t index_cap;
    GfxBindingSpec spec[2][GFX_REFLECT_MAX_BINDINGS];
    uint32_t nspec[2];
    int32_t bind_buf[2][GFX_REFLECT_MAX_BINDINGS];
    int32_t bind_tex[2][GFX_REFLECT_MAX_BINDINGS];
    int32_t bind_sampler[2][GFX_REFLECT_MAX_BINDINGS];
    int32_t bind_img[2][GFX_REFLECT_MAX_BINDINGS];
    int32_t bind_view[2][GFX_REFLECT_MAX_BINDINGS];
    VkDeviceSize buf_size[GFX_REFLECT_MAX_BINDINGS];
    /* Per-draw descriptor epochs.  Each UBO/SSBO binding gets a host-visible
     * arena; the epoch selects a 256-byte-aligned slice through a dynamic
     * offset, so UBOs can change between draws in one command buffer. */
    VkDescriptorPool dpool_draw;
    VkBuffer dyn_buf[2][GFX_REFLECT_MAX_BINDINGS];
    VkDeviceMemory dyn_mem[2][GFX_REFLECT_MAX_BINDINGS];
    size_t dyn_cap[2][GFX_REFLECT_MAX_BINDINGS];
    size_t dyn_stride[2][GFX_REFLECT_MAX_BINDINGS];
    GfxVertexLayout input;
    VkVertexInputBindingDescription vib[16];
    VkVertexInputAttributeDescription via[16];
    uint32_t nb, na;
    struct {
        uint64_t sig;
        GfxPipelineState state;
        VkPipeline pipe;
    } variants[64];
    unsigned nvariants;
};

static int vk_make_buffer_usage(uint32_t size, VkBufferUsageFlags usage,
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

static int vk_make_buffer(uint32_t size, VkBuffer *buf, VkDeviceMemory *mem) {
    return vk_make_buffer_usage(size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                buf, mem);
}

static int vk_make_sampler(VkSampler *sampler) {
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    return vk_ok(vkCreateSampler(g.dev, &sci, NULL, sampler),
                 "vkCreateSampler(reflection)");
}

struct GfxSampler {
    VkSampler sampler;
    GfxSamplerDesc desc;
};

static VkFilter vk_nvn_filter(uint32_t f) {
    return f == 1 || f == 3 || f == 5 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

static VkSamplerMipmapMode vk_nvn_mipmap(uint32_t f) {
    return f >= 4 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

static VkSamplerAddressMode vk_nvn_wrap(uint32_t w) {
    switch (w) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 7: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

static VkCompareOp vk_nvn_compare(uint32_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case 8:
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

GfxSampler *gfx_backend_sampler_create(const GfxSamplerDesc *d) {
    if (!d) return NULL;
    pthread_mutex_lock(&g_lock);
    if (!g.ready) { pthread_mutex_unlock(&g_lock); return NULL; }
    GfxSampler *s = calloc(1, sizeof *s);
    if (!s) { pthread_mutex_unlock(&g_lock); return NULL; }
    s->desc = *d;
    float max_lod = d->lod_max > 0.0f ? d->lod_max : VK_LOD_CLAMP_NONE;
    float min_lod = d->lod_min > 0.0f ? d->lod_min : 0.0f;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = vk_nvn_filter(d->mag_filter),
        .minFilter = vk_nvn_filter(d->min_filter),
        .mipmapMode = vk_nvn_mipmap(d->min_filter),
        .addressModeU = vk_nvn_wrap(d->wrap_s),
        .addressModeV = vk_nvn_wrap(d->wrap_t),
        .addressModeW = vk_nvn_wrap(d->wrap_r),
        .mipLodBias = d->lod_bias,
        .minLod = min_lod,
        .maxLod = max_lod,
        .compareEnable = d->compare_enable ? VK_TRUE : VK_FALSE,
        .compareOp = vk_nvn_compare(d->compare_op),
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    if (!vk_ok(vkCreateSampler(g.dev, &sci, NULL, &s->sampler), "vkCreateSampler(nvn sampler)")) {
        free(s);
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    pthread_mutex_unlock(&g_lock);
    return s;
}

void gfx_backend_sampler_destroy(GfxSampler *s) {
    if (!s) return;
    pthread_mutex_lock(&g_lock);
    if (g.dev && s->sampler) vkDestroySampler(g.dev, s->sampler, NULL);
    pthread_mutex_unlock(&g_lock);
    free(s);
}

static VkFormat vk_reflect_texel_format(const GfxBindingSpec *spec, int storage) {
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

static int vk_make_buffer_view(uint32_t size, int storage, VkFormat format, VkBuffer *buf,
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

static int vk_make_storage_image(uint32_t w, uint32_t h, VkFormat format,
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

static void vk_spirv_move_set(uint8_t *code, size_t size, uint32_t from, uint32_t to) {
    if (!code || size < 20 || (size & 3)) return;
    uint32_t *w = (uint32_t *)code;
    for (size_t i = 5; i < size / 4;) {
        uint32_t op = w[i] & 0xffffu, wc = w[i] >> 16;
        if (!wc || i + wc > size / 4) break;
        if (op == 71 && wc >= 4 && w[i + 2] == 34 && w[i + 3] == from) w[i + 3] = to;
        i += wc;
    }
}

static VkDescriptorType reflect_vk_type(GfxBindingKind kind) {
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

static VkBlendFactor vk_nvn_blend_factor(uint32_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 16: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 17: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 18: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 19: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp vk_nvn_blend_op(uint32_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    case 1:
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp vk_nvn_depth_op(uint32_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case 8: return VK_COMPARE_OP_ALWAYS;
    case 2:
    default: return VK_COMPARE_OP_LESS;
    }
}

static VkStencilOp vk_nvn_stencil_op(uint32_t o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    case 1:
    default: return VK_STENCIL_OP_KEEP;
    }
}

static uint64_t reflect_state_sig(const GfxPipelineState *st) {
    const uint8_t *p = (const uint8_t *)st;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof *st; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

static int reflect_create_pipeline(GfxReflectedPipeline *p, const GfxPipelineState *st,
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
    return vk_ok(vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &gpci, NULL, out),
                 "vkCreateGraphicsPipelines(reflected variant)");
}

static VkPipeline reflect_pipeline_for_state(GfxReflectedPipeline *p, const GfxPipelineState *st) {
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

static GfxReflectedPipeline *reflect_pipeline_create_impl(
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
            vk_log("ERROR reflected pipeline: attachment %u format %d is not color",
                   i, (int)cf);
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

static int reflect_find_spec(GfxReflectedPipeline *p, uint32_t set, uint32_t binding) {
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

static int reflect_write_buffer_locked(GfxReflectedPipeline *p, uint32_t set, uint32_t binding,
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

static int reflect_write_buffer(GfxReflectedPipeline *p, uint32_t set, uint32_t binding,
                                uint32_t offset, const void *data, size_t size, int storage) {
    if (!p || !data || !size) return 0;
    pthread_mutex_lock(&g_lock);
    int ok = reflect_write_buffer_locked(p, set, binding, offset, data, size, storage);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int gfx_backend_reflected_write_uniform(GfxReflectedPipeline *p, uint32_t set,
                                        uint32_t binding, uint32_t offset,
                                        const void *data, size_t size) {
    return reflect_write_buffer(p, set, binding, offset, data, size, 0);
}

int gfx_backend_reflected_write_storage(GfxReflectedPipeline *p, uint32_t set,
                                        uint32_t binding, uint32_t offset,
                                        const void *data, size_t size) {
    return reflect_write_buffer(p, set, binding, offset, data, size, 1);
}

static int reflect_set_for_stage(uint32_t stage) {
    return stage == 0 ? 0 : stage == 1 ? 1 : -1;
}

/* Find a reflected binding by the NVN stage/slot the guest bound.  The
 * .res.json slot is authoritative; a missing slot (-1) falls back to the
 * binding number used by the built-in test shaders. */
static int reflect_find_slot_locked(GfxReflectedPipeline *p, int set, uint32_t slot,
                                    int want_texture) {
    if (set < 0 || set > 1) return -1;
    int fallback = -1;
    for (uint32_t i = 0; i < p->nspec[set]; i++) {
        const GfxBindingSpec *s = &p->spec[set][i];
        int texture_kind = s->kind == GFX_BIND_COMBINED_IMAGE_SAMPLER ||
                           s->kind == GFX_BIND_SAMPLED_IMAGE;
        if (want_texture ? !texture_kind : s->kind != GFX_BIND_UBO) continue;
        if (s->nvn_slot == (int32_t)slot) return (int)i;
        if (fallback < 0 && (s->nvn_slot < 0 || s->nvn_slot == 0) && s->binding == slot)
            fallback = (int)i;
    }
    return fallback;
}

/* The guest binds the whole NVN range, which is often larger than the
 * UBO/SSBO allocation the reflection derived from the shader.  Copy what fits
 * and report the truncation instead of dropping the binding entirely. */
static int reflect_write_slot_locked(GfxReflectedPipeline *p, int set, int i,
                                     const void *data, size_t size, int storage) {
    if (i < 0) return 0;
    int32_t idx = p->bind_buf[set][i];
    if (idx < 0) return 0;
    size_t cap = (size_t)p->buf_size[idx];
    if (size > cap) {
        static unsigned truncated;
        if (truncated++ < 8)
            vk_log("reflected slot copy truncated binding=%u size=%zu cap=%zu",
                   p->spec[set][i].binding, size, cap);
        size = cap;
    }
    if (!size) return 0;
    return reflect_write_buffer_locked(p, set, p->spec[set][i].binding, 0, data, size, storage);
}

int gfx_backend_reflected_bind_ubo_slot(GfxReflectedPipeline *p, uint32_t stage,
                                        uint32_t slot, const void *data, size_t size) {
    if (!p || !data || !size) return 0;
    int set = reflect_set_for_stage(stage);
    if (set < 0) return 0;
    pthread_mutex_lock(&g_lock);
    int i = reflect_find_slot_locked(p, set, slot, 0);
    int ok = reflect_write_slot_locked(p, set, i, data, size, 0);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int gfx_backend_reflected_bind_storage_slot(GfxReflectedPipeline *p, uint32_t stage,
                                            uint32_t slot, const void *data, size_t size) {
    if (!p || !data || !size) return 0;
    int set = reflect_set_for_stage(stage);
    if (set < 0) return 0;
    pthread_mutex_lock(&g_lock);
    int found = -1;
    for (uint32_t i = 0; i < p->nspec[set]; i++) {
        if (p->spec[set][i].kind != GFX_BIND_SSBO) continue;
        if (p->spec[set][i].nvn_slot == (int32_t)slot) { found = (int)i; break; }
    }
    int ok = reflect_write_slot_locked(p, set, found, data, size, 1);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int gfx_backend_reflected_bind_texture_slot(GfxReflectedPipeline *p, uint32_t stage,
                                            uint32_t slot, const GfxTexture *texture) {
    if (!p || !texture) return 0;
    int set = reflect_set_for_stage(stage);
    if (set < 0) return 0;
    pthread_mutex_lock(&g_lock);
    int i = reflect_find_slot_locked(p, set, slot, 1);
    uint32_t binding = i >= 0 ? p->spec[set][i].binding : 0;
    pthread_mutex_unlock(&g_lock);
    if (i < 0) return 0;
    return gfx_backend_reflected_bind_texture(p, (uint32_t)set, binding, texture);
}

GfxReflectedPipeline *gfx_backend_reflected_builtin_texture_pipeline_create(void) {
    GfxBindingSpec ps = {
        .set = 1, .binding = 0, .kind = GFX_BIND_COMBINED_IMAGE_SAMPLER, .count = 1,
    };
    return gfx_backend_reflected_pipeline_create(
        (const uint8_t *)triangle_tex_vert_spv, triangle_tex_vert_spv_size,
        (const uint8_t *)triangle_tex_frag_spv, triangle_tex_frag_spv_size,
        gfx_backend_vk_format_for_nvn(0x25), NULL, 0, &ps, 1);
}

int gfx_backend_reflected_texture_selftest(void) {
    static const uint8_t checker[16] = {
        255, 0, 0, 255,   0, 255, 0, 255,
        0, 0, 255, 255,   255, 255, 255, 255,
    };
    GfxOffscreenTarget *t = gfx_backend_offscreen_create(1280, 720, 0x25);
    GfxTexture *tex = gfx_backend_texture_create_rgba8(2, 2, checker);
    GfxReflectedPipeline *p = t && tex ? gfx_backend_reflected_builtin_texture_pipeline_create() : NULL;
    int ok = 0;
    if (p && gfx_backend_reflected_bind_texture(p, 1, 0, tex)) {
        for (int i = 0; i < 10; i++)
            if (!gfx_backend_reflected_draw(p, t, 0.05f, 0.08f, 0.12f, 1.0f)) break;
        uint8_t center[4] = {0}, corner[4] = {0};
        gfx_backend_offscreen_read_pixel(t, 640, 360, center);
        gfx_backend_offscreen_read_pixel(t, 0, 0, corner);
        ok = center[0] > 80 && center[0] < 180 &&
             center[1] > 80 && center[1] < 180 &&
             center[2] > 80 && center[2] < 180 && corner[0] < 40;
        vk_log("reflected texture selftest center=%02x%02x%02x%02x corner=%02x%02x%02x%02x",
               center[0], center[1], center[2], center[3],
               corner[0], corner[1], corner[2], corner[3]);
    }
    if (p) gfx_backend_reflected_pipeline_destroy(p);
    if (tex) gfx_backend_texture_destroy(tex);
    if (t) gfx_backend_offscreen_destroy(t);
    return ok;
}

int gfx_backend_reflected_ubo_selftest(void) {
    GfxBindingSpec ps = {
        .set = 1, .binding = 0, .kind = GFX_BIND_UBO, .count = 1,
    };
    GfxOffscreenTarget *t = gfx_backend_offscreen_create(1280, 720, 0x25);
    GfxReflectedPipeline *p = t ? gfx_backend_reflected_pipeline_create(
        (const uint8_t *)triangle_vert_spv, triangle_vert_spv_size,
        (const uint8_t *)reflect_ubo_frag_spv, reflect_ubo_frag_spv_size,
        gfx_backend_vk_format_for_nvn(0x25), NULL, 0, &ps, 1) : NULL;
    float color[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    int ok = 0;
    if (p && gfx_backend_reflected_write_uniform(p, 1, 0, 0, color, sizeof color)) {
        for (int i = 0; i < 10; i++)
            if (!gfx_backend_reflected_draw(p, t, 0.05f, 0.08f, 0.12f, 1.0f)) break;
        uint8_t center[4] = {0};
        gfx_backend_offscreen_read_pixel(t, 640, 360, center);
        ok = center[0] == 0x40 && center[1] == 0x80 && center[2] == 0xbf && center[3] == 0xff;
        vk_log("reflected ubo selftest center=%02x%02x%02x%02x", center[0], center[1],
               center[2], center[3]);
    }
    if (p) gfx_backend_reflected_pipeline_destroy(p);
    if (t) gfx_backend_offscreen_destroy(t);
    return ok;
}

static VkPrimitiveTopology vk_reflect_topology(uint32_t primitive) {
    /* NVNdrawPrimitive (matches the macOS NVN mapping): 0 points, 1 lines,
     * 3 line strip, 4 triangles, 5 triangle strip, 7 quads (triangle list).
     * The reflected pipeline is created with TRIANGLE_LIST, which covers the
     * first real frames; non-triangle types still draw with that topology and
     * are logged by the caller. */
    switch (primitive) {
    case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case 4:
    case 7:
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

static int vk_reflect_ensure_buffer(VkBuffer *buf, VkDeviceMemory *mem, size_t *cap,
                                    size_t need, VkBufferUsageFlags usage) {
    if (need < 1) need = 1;
    if (*buf && *cap >= need) return 1;
    if (*buf) vkDestroyBuffer(g.dev, *buf, NULL);
    if (*mem) vkFreeMemory(g.dev, *mem, NULL);
    *buf = VK_NULL_HANDLE;
    *mem = VK_NULL_HANDLE;
    size_t size = (need + 255) & ~(size_t)255;
    if (!vk_make_buffer_usage((uint32_t)size, usage, buf, mem)) return 0;
    *cap = size;
    return 1;
}

static int vk_reflect_upload(VkDeviceMemory mem, size_t offset, const void *data, size_t size) {
    if (!data || !size) return 0;
    void *mapped = NULL;
    if (!vk_ok(vkMapMemory(g.dev, mem, offset, size, 0, &mapped), "vkMapMemory(reflected stream)"))
        return 0;
    memcpy(mapped, data, size);
    vkUnmapMemory(g.dev, mem);
    return 1;
}

static VkIndexType vk_reflect_index_type(uint32_t type) {
    /* NVN: 0 u8, 1 u16, 2 u32.  u8 is widened to u16 because the base Vulkan
     * 1.0 path does not require VK_EXT_index_type_uint8. */
    return type == 2 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
}

/* ---- per-draw descriptor epochs ---------------------------------------- */

#define REFLECT_ALIGN 256
static size_t reflect_align(size_t v) { return (v + REFLECT_ALIGN - 1) & ~(size_t)(REFLECT_ALIGN - 1); }

typedef struct {
    size_t ubo_off[2][GFX_REFLECT_MAX_BINDINGS];
    size_t ssbo_off[2][GFX_REFLECT_MAX_BINDINGS];
    uint8_t dyn[2][GFX_REFLECT_MAX_BINDINGS];
    const GfxTexture *tex[2][GFX_REFLECT_MAX_BINDINGS];
    const GfxSampler *samp[2][GFX_REFLECT_MAX_BINDINGS];
} ReflectEpochState;

/* Find the reflected binding for an NVN stage/slot.  The .res.json nvn_slot is
 * authoritative; the built-in test shaders fall back to binding == slot. */
static int reflect_find_bind_spec(GfxReflectedPipeline *p, uint32_t stage, uint32_t slot,
                                  uint32_t kind) {
    int set = stage == 0 ? 0 : stage == 1 ? 1 : -1;
    if (set < 0) return -1;
    int fallback = -1;
    for (uint32_t i = 0; i < p->nspec[set]; i++) {
        const GfxBindingSpec *sp = &p->spec[set][i];
        int match = kind == GFX_DESC_UBO ? sp->kind == GFX_BIND_UBO
                  : kind == GFX_DESC_SSBO ? sp->kind == GFX_BIND_SSBO
                  : (sp->kind == GFX_BIND_COMBINED_IMAGE_SAMPLER ||
                     sp->kind == GFX_BIND_SAMPLED_IMAGE);
        if (!match) continue;
        if (sp->nvn_slot == (int32_t)slot) return (int)i;
        if (fallback < 0 && (sp->nvn_slot <= 0) && sp->binding == slot) fallback = (int)i;
    }
    return fallback;
}

static int reflect_arena_ensure(GfxReflectedPipeline *p, int set, int idx, size_t need) {
    if (need < REFLECT_ALIGN) need = REFLECT_ALIGN;
    if (!vk_reflect_ensure_buffer(&p->dyn_buf[set][idx], &p->dyn_mem[set][idx],
                                  &p->dyn_cap[set][idx], need,
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        vk_log("ERROR reflected arena set=%d idx=%d size=%zu", set, idx, need);
        return 0;
    }
    return 1;
}

static void reflect_write_epoch_set(GfxReflectedPipeline *p, uint32_t s, VkDescriptorSet dset,
                                    const ReflectEpochState *st) {
    VkDescriptorImageInfo ii[GFX_REFLECT_MAX_BINDINGS];
    for (uint32_t i = 0; i < p->nspec[s]; i++) {
        const GfxBindingSpec *sp = &p->spec[s][i];
        uint32_t count = sp->count ? sp->count : GFX_REFLECT_RUNTIME_ARRAY;
        if (count > GFX_REFLECT_MAX_BINDINGS) count = GFX_REFLECT_MAX_BINDINGS;
        VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = dset, .dstBinding = sp->binding, .descriptorCount = count,
            .descriptorType = reflect_vk_type(sp->kind) };
        if (sp->kind == GFX_BIND_UBO || sp->kind == GFX_BIND_SSBO) {
            int use_dyn = st->dyn[s][i] && p->dyn_buf[s][i];
            int32_t b = use_dyn ? (int32_t)i : p->bind_buf[s][i];
            if (b < 0) continue;
            VkBuffer buffer = use_dyn ? p->dyn_buf[s][i] : p->buf[b];
            VkDeviceSize range = use_dyn ? (VkDeviceSize)p->dyn_stride[s][i] : VK_WHOLE_SIZE;
            VkDescriptorBufferInfo bi = { .buffer = buffer, .offset = 0, .range = range };
            w.descriptorCount = 1;
            w.pBufferInfo = &bi;
            vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
        } else if (sp->kind == GFX_BIND_SAMPLER) {
            int32_t si = p->bind_sampler[s][i];
            VkSampler sampler = si >= 0 ? p->sampler[si] : VK_NULL_HANDLE;
            for (uint32_t k = 0; k < count; k++)
                ii[k] = (VkDescriptorImageInfo){ .sampler = sampler, .imageView = VK_NULL_HANDLE,
                                                 .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED };
            w.pImageInfo = ii;
            vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
        } else if (sp->kind == GFX_BIND_SAMPLED_IMAGE ||
                   sp->kind == GFX_BIND_COMBINED_IMAGE_SAMPLER) {
            const GfxTexture *t = st->tex[s][i];
            if (!t && p->bind_tex[s][i] >= 0) t = p->tex[p->bind_tex[s][i]];
            if (!t) continue;
            VkSampler sampler = VK_NULL_HANDLE;
            if (sp->kind == GFX_BIND_COMBINED_IMAGE_SAMPLER) {
                sampler = st->samp[s][i] ? st->samp[s][i]->sampler : t->sampler;
            }
            for (uint32_t k = 0; k < count; k++)
                ii[k] = (VkDescriptorImageInfo){ .sampler = sampler, .imageView = t->view,
                                                 .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            w.pImageInfo = ii;
            vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
        } else if (sp->kind == GFX_BIND_STORAGE_IMAGE) {
            int32_t ii_idx = p->bind_img[s][i];
            if (ii_idx < 0) continue;
            for (uint32_t k = 0; k < count; k++)
                ii[k] = (VkDescriptorImageInfo){ .sampler = VK_NULL_HANDLE,
                                                 .imageView = p->img_view[ii_idx],
                                                 .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
            w.pImageInfo = ii;
            vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
        } else {
            int32_t vi = p->bind_view[s][i];
            if (vi < 0) continue;
            VkBufferView views[GFX_REFLECT_MAX_BINDINGS];
            for (uint32_t k = 0; k < count; k++) views[k] = p->buf_view[vi];
            w.pTexelBufferView = views;
            vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
        }
    }
}

static void reflect_apply_draw_state(VkCommandBuffer cmd, GfxReflectedPipeline *p,
                                     const GfxPipelineState *st) {
    VkPipeline pipe = reflect_pipeline_for_state(p, st);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe ? pipe : p->pipe);
    float zero[4] = { 0, 0, 0, 0 };
    vkCmdSetBlendConstants(cmd, st ? st->blend_color : zero);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, st ? st->stencil_ref : 0);
}

static void reflect_bind_epoch_sets(VkCommandBuffer cmd, GfxReflectedPipeline *p,
                                    const VkDescriptorSet sets[2],
                                    const ReflectEpochState *st) {
    uint32_t offsets[2 * GFX_REFLECT_MAX_BINDINGS];
    uint32_t n = 0;
    for (uint32_t s = 0; s < 2; s++)
        for (uint32_t i = 0; i < p->nspec[s]; i++) {
            if (p->spec[s][i].kind != GFX_BIND_UBO && p->spec[s][i].kind != GFX_BIND_SSBO)
                continue;
            size_t off = p->spec[s][i].kind == GFX_BIND_UBO ? st->ubo_off[s][i]
                                                           : st->ssbo_off[s][i];
            offsets[n++] = (uint32_t)off;
        }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 2, sets,
                            n, n ? offsets : NULL);
}


int gfx_backend_reflected_execute(GfxReflectedPipeline *p, GfxOffscreenTarget *t,
                                  const GfxCmd *cmds, uint32_t count,
                                  const float default_clear[4]) {
    return gfx_backend_reflected_execute_ex(p, t, cmds, count, default_clear, NULL, 0);
}

static int reflect_layout_matches_target(const GfxRenderTargetLayout *l,
                                         const GfxOffscreenTarget *t) {
    if (!l || !t || l->color_count != t->rt_layout.color_count) return 0;
    for (uint32_t i = 0; i < l->color_count; i++)
        if (l->color_formats[i] != (int)t->color_format[i]) return 0;
    if (l->depth_format != (int)t->depth_format) return 0;
    return 1;
}

int gfx_backend_reflected_execute_ex(GfxReflectedPipeline *p, GfxOffscreenTarget *t,
                                     const GfxCmd *cmds, uint32_t count,
                                     const float default_clear[4],
                                     const GfxVertexStream *streams, uint32_t nstreams) {
    if (!p || !t) return 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready || !reflect_layout_matches_target(&p->rt_layout, t)) {
        vk_log("ERROR reflected execute: target layout does not match pipeline "
               "(colors=%u/%u depth=%d/%d)",
               t->rt_layout.color_count, p->rt_layout.color_count,
               (int)t->depth_format, p->rt_layout.depth_format);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    uint32_t ncolor = p->rt_layout.color_count;
    int has_depth = p->rt_layout.depth_format != 0;
    uint32_t attach_count = ncolor + (has_depth ? 1u : 0u);
    VkImageView views[GFX_MAX_COLOR_TARGETS + 1];
    for (uint32_t i = 0; i < ncolor; i++) views[i] = t->color_view[i];
    if (has_depth) views[ncolor] = t->depth_view;
    int fb_dirty = !p->fb || p->fb_w != t->width || p->fb_h != t->height ||
                   p->fb_attach_count != attach_count;
    for (uint32_t i = 0; !fb_dirty && i < attach_count; i++)
        if (p->fb_views[i] != views[i]) fb_dirty = 1;
    if (fb_dirty) {
        if (p->fb) vkDestroyFramebuffer(g.dev, p->fb, NULL);
        p->fb = VK_NULL_HANDLE;
        VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = p->pass, .attachmentCount = attach_count, .pAttachments = views,
            .width = t->width, .height = t->height, .layers = 1 };
        if (!vk_ok(vkCreateFramebuffer(g.dev, &fci, NULL, &p->fb), "vkCreateFramebuffer(reflected)")) {
            pthread_mutex_unlock(&g_lock); return 0;
        }
        memcpy(p->fb_views, views, attach_count * sizeof views[0]);
        p->fb_attach_count = attach_count;
        p->fb_view = views[0]; p->fb_w = t->width; p->fb_h = t->height;
    }

    /* Pre-scan descriptor binds: one arena slice per change, and one set pair
     * per epoch.  UBO/SSBO slices are selected by dynamic offsets, so two
     * draws in the same command buffer can see different constant data. */
    uint32_t changes[2][GFX_REFLECT_MAX_BINDINGS];
    memset(changes, 0, sizeof changes);
    uint32_t nbinds = 0;
    for (uint32_t i = 0; i < count; i++) {
        const GfxCmd *c = &cmds[i];
        if (c->kind != GFX_CMD_BIND_DESC) continue;
        nbinds++;
        if (c->u.bind.kind != GFX_DESC_UBO && c->u.bind.kind != GFX_DESC_SSBO) continue;
        int idx = reflect_find_bind_spec(p, c->u.bind.stage, c->u.bind.slot, c->u.bind.kind);
        if (idx < 0) continue;
        int set = c->u.bind.stage == 0 ? 0 : 1;
        size_t stride = reflect_align(c->u.bind.size ? c->u.bind.size : 1);
        if (stride > p->dyn_stride[set][idx]) p->dyn_stride[set][idx] = stride;
        changes[set][idx]++;
    }
    for (uint32_t s = 0; s < 2; s++)
        for (uint32_t i = 0; i < p->nspec[s]; i++) {
            if (!changes[s][i]) continue;
            size_t stride = p->dyn_stride[s][i] ? p->dyn_stride[s][i] : REFLECT_ALIGN;
            size_t need = stride * (changes[s][i] + 1);
            if (need > (64u << 20) || !reflect_arena_ensure(p, (int)s, (int)i, need)) {
                vk_log("ERROR reflected arena too large/alloc failed set=%u binding=%u need=%zu",
                       s, p->spec[s][i].binding, need);
                changes[s][i] = 0;
            }
        }
    if (nbinds) vkResetDescriptorPool(g.dev, p->dpool_draw, 0);
    ReflectEpochState *st = calloc(1, sizeof *st);
    if (!st) { pthread_mutex_unlock(&g_lock); return 0; }
    for (uint32_t s = 0; s < 2; s++)
        for (uint32_t i = 0; i < p->nspec[s]; i++)
            st->dyn[s][i] = changes[s][i] ? 1 : 0;
    VkDescriptorSet cur_sets[2] = { p->dset[0], p->dset[1] };
    uint32_t cur_epochs = 0;
    uint32_t used[2][GFX_REFLECT_MAX_BINDINGS];
    memset(used, 0, sizeof used);

    /* Upload the guest stream ranges before recording.  These buffers are
     * host-visible and are rebuilt only when the bound range grows. */
    for (uint32_t s = 0; s < nstreams; s++) {
        uint32_t binding = streams[s].binding;
        if (binding >= 16 || !streams[s].data || !streams[s].size) continue;
        if (!vk_reflect_ensure_buffer(&p->vbuf[binding], &p->vbuf_mem[binding],
                                      &p->vbuf_cap[binding], streams[s].size,
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
            !vk_reflect_upload(p->vbuf_mem[binding], 0, streams[s].data, streams[s].size)) {
            vk_log("ERROR reflected vertex stream upload binding=%u size=%zu", binding, streams[s].size);
            free(st);
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
    }

    size_t *index_offsets = calloc(count ? count : 1, sizeof *index_offsets);
    if (!index_offsets) { free(st); pthread_mutex_unlock(&g_lock); return 0; }
    size_t index_total = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (cmds[i].kind != GFX_CMD_DRAW_INDEXED) continue;
        const GfxCmd *c = &cmds[i];
        uint32_t it = c->u.draw_indexed.index_type;
        size_t src = c->u.draw_indexed.index_size;
        if (!src) src = (size_t)c->u.draw_indexed.count * (it == 2 ? 4 : it == 1 ? 2 : 1);
        size_t dst = it == 0 ? (size_t)c->u.draw_indexed.count * 2 : src;
        index_offsets[i] = (index_total + 3) & ~(size_t)3;
        index_total = index_offsets[i] + dst;
    }
    if (index_total) {
        if (!vk_reflect_ensure_buffer(&p->index_buf, &p->index_mem, &p->index_cap,
                                      index_total, VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) {
            free(index_offsets);
            free(st);
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        for (uint32_t i = 0; i < count; i++) {
            if (cmds[i].kind != GFX_CMD_DRAW_INDEXED) continue;
            const GfxCmd *c = &cmds[i];
            uint32_t it = c->u.draw_indexed.index_type;
            size_t src = c->u.draw_indexed.index_size;
            if (!src) src = (size_t)c->u.draw_indexed.count * (it == 2 ? 4 : it == 1 ? 2 : 1);
            if (it == 0) {
                size_t n = c->u.draw_indexed.count;
                uint16_t *wide = malloc(n * 2);
                if (!wide) { free(index_offsets); free(st); pthread_mutex_unlock(&g_lock); return 0; }
                const uint8_t *in = c->u.draw_indexed.indices;
                for (size_t k = 0; k < n; k++) wide[k] = in[k];
                int ok = vk_reflect_upload(p->index_mem, index_offsets[i], wide, n * 2);
                free(wide);
                if (!ok) { free(index_offsets); free(st); pthread_mutex_unlock(&g_lock); return 0; }
            } else if (!vk_reflect_upload(p->index_mem, index_offsets[i],
                                          c->u.draw_indexed.indices, src)) {
                free(index_offsets);
                free(st);
                pthread_mutex_unlock(&g_lock);
                return 0;
            }
        }
    }

    /* Create state variants before recording; some drivers dislike pipeline
     * creation inside an active render pass. */
    for (uint32_t i = 0; i < count; i++) {
        const GfxPipelineState *ps = NULL;
        if (cmds[i].kind == GFX_CMD_DRAW && cmds[i].u.draw.has_state)
            ps = &cmds[i].u.draw.state;
        else if (cmds[i].kind == GFX_CMD_DRAW_INDEXED && cmds[i].u.draw_indexed.has_state)
            ps = &cmds[i].u.draw_indexed.state;
        if (ps) (void)reflect_pipeline_for_state(p, ps);
    }

    float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
    if (default_clear) memcpy(clear, default_clear, sizeof clear);
    int first_is_clear = count && cmds[0].kind == GFX_CMD_CLEAR_COLOR;
    if (first_is_clear) memcpy(clear, cmds[0].u.clear.rgba, sizeof clear);

    VkCommandBuffer cmd = g.cmd;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(reflected-execute)")) {
        free(index_offsets);
        free(st);
        pthread_mutex_unlock(&g_lock); return 0;
    }
    VkClearValue cv[GFX_MAX_COLOR_TARGETS + 1];
    for (uint32_t i = 0; i < ncolor; i++)
        cv[i] = (VkClearValue){ .color = { .float32 = { clear[0], clear[1], clear[2], clear[3] } } };
    if (has_depth)
        cv[ncolor] = (VkClearValue){ .depthStencil = { 1.0f, 0 } };
    VkRenderPassBeginInfo rp = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = p->pass, .framebuffer = p->fb,
        .renderArea = { { 0, 0 }, { t->width, t->height } },
        .clearValueCount = attach_count, .pClearValues = cv };
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipe);
    {
        const float zero[4] = { 0, 0, 0, 0 };
        vkCmdSetBlendConstants(cmd, zero);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    }
    for (uint32_t s = 0; s < nstreams; s++) {
        uint32_t binding = streams[s].binding;
        if (binding >= 16 || !streams[s].data || !streams[s].size) continue;
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, binding, 1, &p->vbuf[binding], &offset);
    }

    VkViewport viewport = { 0, 0, (float)t->width, (float)t->height, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, { t->width, t->height } };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    for (uint32_t i = 0; i < count; i++) {
        const GfxCmd *c = &cmds[i];
        switch (c->kind) {
        case GFX_CMD_CLEAR_COLOR: {
            if (i == 0 && first_is_clear) break;   /* already the pass load value */
            VkClearAttachment ca = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                     .colorAttachment = 0,
                                     .clearValue = { .color = { .float32 = {
                                         c->u.clear.rgba[0], c->u.clear.rgba[1],
                                         c->u.clear.rgba[2], c->u.clear.rgba[3] } } } };
            VkClearRect cr = { { { 0, 0 }, { t->width, t->height } }, 0, 1 };
            vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
            break;
        }
        case GFX_CMD_CLEAR_DEPTH_STENCIL: {
            VkClearAttachment ca = { .clearValue = { .depthStencil = {
                c->u.clear_depth.depth, (uint32_t)c->u.clear_depth.stencil } } };
            if (c->u.clear_depth.dmask) ca.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
            if (c->u.clear_depth.smask) ca.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            if (ca.aspectMask) {
                VkClearRect cr = { { { 0, 0 }, { t->width, t->height } }, 0, 1 };
                vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
            }
            break;
        }
        case GFX_CMD_SET_VIEWPORT:
            viewport = (VkViewport){ (float)c->u.rect.x, (float)c->u.rect.y,
                                     (float)c->u.rect.width, (float)c->u.rect.height, 0, 1 };
            if (c->u.rect.width > 0 && c->u.rect.height > 0)
                vkCmdSetViewport(cmd, 0, 1, &viewport);
            break;
        case GFX_CMD_SET_SCISSOR:
            if (c->u.rect.width > 0 && c->u.rect.height > 0) {
                scissor = (VkRect2D){ { c->u.rect.x, c->u.rect.y },
                                      { (uint32_t)c->u.rect.width, (uint32_t)c->u.rect.height } };
                vkCmdSetScissor(cmd, 0, 1, &scissor);
            }
            break;
        case GFX_CMD_BIND_DESC: {
            const GfxCmd *b = c;
            int idx = reflect_find_bind_spec(p, b->u.bind.stage, b->u.bind.slot, b->u.bind.kind);
            if (idx < 0) break;
            int set = b->u.bind.stage == 0 ? 0 : 1;
            if (b->u.bind.kind == GFX_DESC_UBO || b->u.bind.kind == GFX_DESC_SSBO) {
                if (!st->dyn[set][idx] || !p->dyn_buf[set][idx]) break;
                size_t stride = p->dyn_stride[set][idx];
                used[set][idx]++;
                size_t off = stride * used[set][idx];
                if (off + b->u.bind.size > p->dyn_cap[set][idx] ||
                    !vk_reflect_upload(p->dyn_mem[set][idx], off, b->u.bind.data, b->u.bind.size)) {
                    vk_log("ERROR reflected binding upload set=%d slot=%u size=%zu",
                           set, b->u.bind.slot, b->u.bind.size);
                    break;
                }
                if (b->u.bind.kind == GFX_DESC_UBO) st->ubo_off[set][idx] = off;
                else st->ssbo_off[set][idx] = off;
            } else {
                st->tex[set][idx] = b->u.bind.texture;
                st->samp[set][idx] = b->u.bind.sampler;
            }
            if (cur_epochs < GFX_DESC_MAX_EPOCHS) {
                VkDescriptorSetLayout layouts[2] = { p->dsl[0], p->dsl[1] };
                VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                    .descriptorPool = p->dpool_draw, .descriptorSetCount = 2, .pSetLayouts = layouts };
                VkDescriptorSet ns[2];
                if (vkAllocateDescriptorSets(g.dev, &ai, ns) == VK_SUCCESS) {
                    cur_sets[0] = ns[0]; cur_sets[1] = ns[1];
                    reflect_write_epoch_set(p, 0, ns[0], st);
                    reflect_write_epoch_set(p, 1, ns[1], st);
                    cur_epochs++;
                } else {
                    vk_log("ERROR reflected epoch set allocation failed at %u", cur_epochs);
                }
            } else {
                static unsigned epoch_full_logged;
                if (epoch_full_logged++ < 4)
                    vk_log("ERROR reflected descriptor epoch limit (%u) reached; reusing last set",
                           GFX_DESC_MAX_EPOCHS);
            }
            break;
        }
        case GFX_CMD_DRAW: {
            uint32_t prim = c->u.draw.primitive;
            if (vk_reflect_topology(prim) != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                vk_log("reflected execute: primitive %u uses the TRIANGLE_LIST pipeline", prim);
            reflect_bind_epoch_sets(cmd, p, cur_sets, st);
            reflect_apply_draw_state(cmd, p, c->u.draw.has_state ? &c->u.draw.state : NULL);
            vkCmdDraw(cmd, c->u.draw.count, 1, c->u.draw.first, 0);
            break;
        }
        case GFX_CMD_DRAW_INDEXED: {
            uint32_t prim = c->u.draw_indexed.primitive;
            if (vk_reflect_topology(prim) != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                vk_log("reflected execute: indexed primitive %u uses the TRIANGLE_LIST pipeline", prim);
            reflect_bind_epoch_sets(cmd, p, cur_sets, st);
            reflect_apply_draw_state(cmd, p, c->u.draw_indexed.has_state
                                              ? &c->u.draw_indexed.state : NULL);
            vkCmdBindIndexBuffer(cmd, p->index_buf, index_offsets[i],
                                 vk_reflect_index_type(c->u.draw_indexed.index_type));
            vkCmdDrawIndexed(cmd, c->u.draw_indexed.count, 1, 0,
                             (int32_t)c->u.draw_indexed.base_vertex, 0);
            break;
        }
        default:
            break;
        }
    }
    vkCmdEndRenderPass(cmd);
    for (uint32_t i = 0; i < ncolor; i++) {
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { t->width, t->height, 1 },
        };
        vkCmdCopyImageToBuffer(cmd, t->color_image[i], VK_IMAGE_LAYOUT_GENERAL,
                               t->color_readback[i], 1, &region);
        VkBufferMemoryBarrier bb = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = t->color_readback[i], .offset = 0, .size = VK_WHOLE_SIZE };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, NULL, 1, &bb, 0, NULL);
    }
    if (!vk_submit_and_wait(cmd, "reflected-execute")) {
        free(index_offsets);
        free(st);
        pthread_mutex_unlock(&g_lock); return 0;
    }
    t->layout_state = VK_IMAGE_LAYOUT_GENERAL;
    for (uint32_t i = 0; i < ncolor; i++) {
        void *mapped = NULL;
        if (!vk_ok(vkMapMemory(g.dev, t->color_readback_mem[i], 0, VK_WHOLE_SIZE, 0, &mapped),
                   "vkMapMemory(reflected-execute)")) {
            free(index_offsets);
            free(st);
            pthread_mutex_unlock(&g_lock); return 0;
        }
        memcpy(t->color_pixels[i], mapped, t->color_size[i]);
        vkUnmapMemory(g.dev, t->color_readback_mem[i]);
    }
    free(index_offsets);
    free(st);
    pthread_mutex_unlock(&g_lock);
    return 1;
}

int gfx_backend_reflected_draw(GfxReflectedPipeline *p, GfxOffscreenTarget *t,
                               float r, float g_, float b, float a) {
    const float clear[4] = { r, g_, b, a };
    GfxCmd cmd = { .kind = GFX_CMD_DRAW };
    cmd.u.draw.primitive = 4;
    cmd.u.draw.first = 0;
    cmd.u.draw.count = 3;
    return gfx_backend_reflected_execute(p, t, &cmd, 1, clear);
}

static GfxTexturedPipeline *g_builtin_textured;
GfxTexturedPipeline *gfx_backend_builtin_textured_pipeline(void) {
    if (!g_builtin_textured)
        g_builtin_textured = gfx_backend_textured_pipeline_create(VK_FORMAT_R8G8B8A8_UNORM);
    return g_builtin_textured;
}

int gfx_backend_offscreen_write_ppm_index(const GfxOffscreenTarget *t, uint32_t index,
                                          const char *path) {
    if (!t || !path || index >= t->rt_layout.color_count ||
        t->color_bpp[index] != 4 || !t->color_pixels[index]) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) { vk_log("ERROR screenshot open %s", path); return 0; }
    fprintf(f, "P6\n%u %u\n255\n", t->width, t->height);
    size_t n = (size_t)t->width * t->height;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = t->color_pixels[index] + i * 4;
        fwrite(p, 1, 3, f);
    }
    int ok = ferror(f) == 0;
    fclose(f);
    if (ok) vk_log("screenshot %s (%ux%u attachment=%u, %zu bytes)", path, t->width,
                   t->height, index, n * 3);
    else vk_log("ERROR screenshot write %s", path);
    return ok;
}

int gfx_backend_offscreen_write_ppm(const GfxOffscreenTarget *t, const char *path) {
    return gfx_backend_offscreen_write_ppm_index(t, 0, path);
}

uint64_t gfx_backend_offscreen_hash(const GfxOffscreenTarget *t) {
    if (!t || !t->color_pixels[0]) return 0;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < t->color_size[0]; i++) {
        h ^= t->color_pixels[0][i];
        h *= 1099511628211ull;
    }
    return h;
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


/* ------------------------------------------------------------------ */
/* First-frame backend selftests (game-data-free). */

int gfx_backend_texture_formats_selftest(void);
int gfx_backend_mrt_selftest(void);

static int vk_selftest_read_match(GfxTexture *t, uint32_t level, uint32_t layer,
                                  const uint8_t *expect, size_t n, const char *what) {
    uint8_t *got = malloc(n ? n : 1);
    if (!got) return 0;
    int ok = gfx_backend_texture_read_pixels(t, level, layer, got, n);
    if (!ok) {
        vk_log("texture formats selftest: %s readback failed", what);
        free(got);
        return 0;
    }
    if (memcmp(got, expect, n)) {
        size_t i = 0;
        while (i + 1 < n && got[i] == expect[i]) i++;
        vk_log("texture formats selftest: %s mismatch at byte %zu got=%02x want=%02x",
               what, i, got[i], expect[i]);
        ok = 0;
    } else {
        vk_log("texture formats selftest: %s OK (%zu bytes)", what, n);
    }
    free(got);
    return ok;
}

int gfx_backend_texture_formats_selftest(void) {
    if (!gfx_backend_vulkan_init()) {
        vk_log("texture formats selftest: Vulkan unavailable");
        return 0;
    }
    int ok = 1;

    /* 2D array, two mip levels, two layers.  Level 1 has 2x2 texels. */
    {
        enum { W = 4, H = 4 };
        uint8_t l0a[W * H * 4], l0b[W * H * 4], l1a[2 * 2 * 4], l1b[2 * 2 * 4];
        for (int i = 0; i < W * H; i++) {
            l0a[i * 4 + 0] = 0x10; l0a[i * 4 + 1] = 0x20;
            l0a[i * 4 + 2] = 0x30; l0a[i * 4 + 3] = 0xff;
            l0b[i * 4 + 0] = 0x40; l0b[i * 4 + 1] = 0x50;
            l0b[i * 4 + 2] = 0x60; l0b[i * 4 + 3] = 0xff;
        }
        for (int i = 0; i < 2 * 2; i++) {
            l1a[i * 4 + 0] = 0x70; l1a[i * 4 + 1] = 0x80;
            l1a[i * 4 + 2] = 0x90; l1a[i * 4 + 3] = 0xff;
            l1b[i * 4 + 0] = 0xa0; l1b[i * 4 + 1] = 0xb0;
            l1b[i * 4 + 2] = 0xc0; l1b[i * 4 + 3] = 0xff;
        }
        GfxTextureDesc d = {
            .target = GFX_TEXTURE_2D_ARRAY, .vk_format = VK_FORMAT_R8G8B8A8_UNORM,
            .width = W, .height = H, .depth = 1, .mip_levels = 2, .array_layers = 2,
            .samples = 1,
        };
        GfxTextureSubresource s[4] = {
            { 0, 0, l0a, sizeof l0a }, { 0, 1, l0b, sizeof l0b },
            { 1, 0, l1a, sizeof l1a }, { 1, 1, l1b, sizeof l1b },
        };
        GfxTexture *t = gfx_backend_texture_create_desc(&d, s, 4);
        if (!t) {
            vk_log("texture formats selftest: 2D array/mips create failed");
            ok = 0;
        } else {
            ok &= vk_selftest_read_match(t, 1, 1, l1b, sizeof l1b, "2D array mip1 layer1");
            ok &= vk_selftest_read_match(t, 0, 1, l0b, sizeof l0b, "2D array mip0 layer1");
            gfx_backend_texture_destroy(t);
        }
    }

    /* Cube: six 2x2 faces with distinct colours. */
    {
        enum { F = 6, N = 2 * 2 * 4 };
        uint8_t faces[F][N];
        for (int f = 0; f < F; f++)
            for (int i = 0; i < 2 * 2; i++) {
                faces[f][i * 4 + 0] = (uint8_t)(0x20 + f * 0x20);
                faces[f][i * 4 + 1] = (uint8_t)(0x10 + f * 0x10);
                faces[f][i * 4 + 2] = (uint8_t)(0xf0 - f * 0x10);
                faces[f][i * 4 + 3] = 0xff;
            }
        GfxTextureDesc d = {
            .target = GFX_TEXTURE_CUBE, .vk_format = VK_FORMAT_R8G8B8A8_UNORM,
            .width = 2, .height = 2, .depth = 1, .mip_levels = 1, .array_layers = 6,
            .samples = 1,
        };
        GfxTextureSubresource s[F];
        for (int f = 0; f < F; f++)
            s[f] = (GfxTextureSubresource){ 0, (uint32_t)f, faces[f], N };
        GfxTexture *t = gfx_backend_texture_create_desc(&d, s, F);
        if (!t) {
            vk_log("texture formats selftest: cube create failed");
            ok = 0;
        } else {
            ok &= vk_selftest_read_match(t, 0, 4, faces[4], N, "cube face4");
            gfx_backend_texture_destroy(t);
        }
    }

    /* 3D texture: two 2x2 slices, verify the first slice round-trips. */
    {
        uint8_t vol[2 * 2 * 2 * 4];
        for (int z = 0; z < 2; z++)
            for (int i = 0; i < 2 * 2; i++) {
                int o = (z * 4 + i) * 4;
                vol[o + 0] = z ? 0x30 : 0x80;
                vol[o + 1] = z ? 0x60 : 0x40;
                vol[o + 2] = z ? 0x90 : 0x20;
                vol[o + 3] = 0xff;
            }
        GfxTextureDesc d = {
            .target = GFX_TEXTURE_3D, .vk_format = VK_FORMAT_R8G8B8A8_UNORM,
            .width = 2, .height = 2, .depth = 2, .mip_levels = 1, .array_layers = 1,
            .samples = 1,
        };
        GfxTextureSubresource s = { 0, 0, vol, sizeof vol };
        GfxTexture *t = gfx_backend_texture_create_desc(&d, &s, 1);
        if (!t) {
            vk_log("texture formats selftest: 3D create failed");
            ok = 0;
        } else {
            ok &= vk_selftest_read_match(t, 0, 0, vol, sizeof vol, "3D volume");
            gfx_backend_texture_destroy(t);
        }
    }

    /* sRGB upload/readback must preserve the encoded bytes. */
    {
        uint8_t px[2 * 2 * 4];
        for (int i = 0; i < 2 * 2; i++) {
            px[i * 4 + 0] = 0x7f; px[i * 4 + 1] = 0x3a;
            px[i * 4 + 2] = 0xc4; px[i * 4 + 3] = 0xff;
        }
        GfxTextureDesc d = {
            .target = GFX_TEXTURE_2D, .vk_format = VK_FORMAT_R8G8B8A8_SRGB,
            .width = 2, .height = 2, .depth = 1, .mip_levels = 1, .array_layers = 1,
            .samples = 1,
        };
        GfxTextureSubresource s = { 0, 0, px, sizeof px };
        GfxTexture *t = gfx_backend_texture_create_desc(&d, &s, 1);
        if (!t) {
            vk_log("texture formats selftest: sRGB create failed");
            ok = 0;
        } else {
            ok &= vk_selftest_read_match(t, 0, 0, px, sizeof px, "RGBA8 sRGB");
            gfx_backend_texture_destroy(t);
        }
    }

    /* Extra uncompressed formats from the NVN/Ryujinx mapping tables. */
    {
        static const struct { VkFormat fmt; const char *name; } extra[] = {
            { VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F" },
            { VK_FORMAT_R32G32B32A32_SFLOAT, "RGBA32F" },
            { VK_FORMAT_B10G11R11_UFLOAT_PACK32, "R11G11B10F" },
            { VK_FORMAT_D32_SFLOAT, "D32F" },
        };
        for (unsigned i = 0; i < sizeof extra / sizeof extra[0]; i++) {
            VkFormatFeatureFlags ef = 0;
            if (!vk_format_can_texture(extra[i].fmt, &ef) ||
                !(ef & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) {
                vk_log("texture formats selftest: %s unsupported, skipped", extra[i].name);
                continue;
            }
            uint32_t bpp = (uint32_t)vk_format_info(extra[i].fmt).bpp;
            uint8_t px[2 * 2 * 16];
            if (!bpp || bpp > 16) continue;
            size_t n = (size_t)2 * 2 * bpp;
            for (size_t b = 0; b < n; b++) px[b] = (uint8_t)(0x23 * (b + 1));
            GfxTextureDesc d = {
                .target = GFX_TEXTURE_2D, .vk_format = (uint32_t)extra[i].fmt,
                .width = 2, .height = 2, .depth = 1, .mip_levels = 1, .array_layers = 1,
                .samples = 1,
            };
            GfxTextureSubresource s = { 0, 0, px, n };
            GfxTexture *t = gfx_backend_texture_create_desc(&d, &s, 1);
            if (!t) {
                vk_log("texture formats selftest: %s create failed", extra[i].name);
                ok = 0;
                continue;
            }
            char label[32];
            snprintf(label, sizeof label, "%s", extra[i].name);
            ok &= vk_selftest_read_match(t, 0, 0, px, n, label);
            gfx_backend_texture_destroy(t);
        }
    }

    /* Block compressed formats the device actually advertises.  BC is not
     * mandatory on Android/Adreno; skip (not fail) when unavailable. */
    {
        static const struct { VkFormat fmt; const char *name; } bc[] = {
            { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1" },
            { VK_FORMAT_BC3_UNORM_BLOCK, "BC3" },
            { VK_FORMAT_BC4_UNORM_BLOCK, "BC4" },
            { VK_FORMAT_BC5_UNORM_BLOCK, "BC5" },
            { VK_FORMAT_BC7_UNORM_BLOCK, "BC7" },
        };
        unsigned tested = 0, skipped = 0;
        for (unsigned i = 0; i < sizeof bc / sizeof bc[0]; i++) {
            VkFormatFeatureFlags bcf = 0;
            if (!vk_format_can_texture(bc[i].fmt, &bcf) ||
                !(bcf & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) {
                skipped++;
                continue;
            }
            uint8_t block[16];
            uint32_t bytes = (uint32_t)vk_format_info(bc[i].fmt).bpp;
            if (!bytes || bytes > sizeof block) { skipped++; continue; }
            for (uint32_t b = 0; b < bytes; b++) block[b] = (uint8_t)(0x11 * (b + 1));
            GfxTextureDesc d = {
                .target = GFX_TEXTURE_2D, .vk_format = (uint32_t)bc[i].fmt,
                .width = 4, .height = 4, .depth = 1, .mip_levels = 1, .array_layers = 1,
                .samples = 1,
            };
            GfxTextureSubresource s = { 0, 0, block, bytes };
            GfxTexture *t = gfx_backend_texture_create_desc(&d, &s, 1);
            if (!t) {
                vk_log("texture formats selftest: %s create failed", bc[i].name);
                ok = 0;
                continue;
            }
            char label[32];
            snprintf(label, sizeof label, "%s block", bc[i].name);
            ok &= vk_selftest_read_match(t, 0, 0, block, bytes, label);
            tested++;
            gfx_backend_texture_destroy(t);
        }
        vk_log("texture formats selftest: BC tested=%u skipped=%u", tested, skipped);
    }

    vk_log("texture formats selftest %s", ok ? "PASS" : "FAIL");
    return ok;
}

int gfx_backend_mrt_selftest(void) {
    if (!gfx_backend_vulkan_init()) {
        vk_log("MRT selftest: Vulkan unavailable");
        return 0;
    }
    GfxRenderTargetLayout layout = { 0 };
    layout.color_count = 2;
    layout.color_formats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    layout.color_formats[1] = VK_FORMAT_R8G8B8A8_UNORM;
    layout.depth_format = (int)g.depth_format;
    GfxOffscreenTarget *t = gfx_backend_offscreen_create_layout(128, 128, &layout);
    if (!t) {
        vk_log("MRT selftest: 2-color+depth target creation failed");
        return 0;
    }
    GfxReflectedPipeline *p = gfx_backend_reflected_pipeline_create_layout(
        (const uint8_t *)triangle_mrt_vert_spv, triangle_mrt_vert_spv_size,
        (const uint8_t *)triangle_mrt_frag_spv, triangle_mrt_frag_spv_size,
        &layout, NULL, 0, NULL, 0, NULL);
    if (!p) {
        vk_log("MRT selftest: pipeline creation failed");
        gfx_backend_offscreen_destroy(t);
        return 0;
    }

    const float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
    GfxCmd cmds[3] = { 0 };
    cmds[0].kind = GFX_CMD_CLEAR_COLOR;
    memcpy(cmds[0].u.clear.rgba, clear, sizeof clear);
    cmds[1].kind = GFX_CMD_CLEAR_DEPTH_STENCIL;
    cmds[1].u.clear_depth.depth = 1.0f;
    cmds[1].u.clear_depth.dmask = 1;
    cmds[1].u.clear_depth.smask = 1;
    cmds[2].kind = GFX_CMD_DRAW;
    cmds[2].u.draw.primitive = 4;
    cmds[2].u.draw.count = 3;
    int drew = 0;
    for (int i = 0; i < 10; i++) {
        if (!gfx_backend_reflected_execute(p, t, cmds, 3, clear)) break;
        drew++;
    }
    uint8_t c0[4] = { 0 }, c1[4] = { 0 };
    gfx_backend_offscreen_read_pixel_index(t, 0, 64, 64, c0);
    gfx_backend_offscreen_read_pixel_index(t, 1, 64, 64, c1);
    int ok = drew && c0[0] > 200 && c0[1] > 40 && c0[1] < 90 && c0[2] < 40 &&
             c1[0] < 60 && c1[1] > 180 && c1[2] > 20 && c1[2] < 80;
    vk_log("MRT selftest %s draws=%d attach0=%02x%02x%02x%02x attach1=%02x%02x%02x%02x "
           "depth=%s colors=%u",
           ok ? "PASS" : "FAIL", drew, c0[0], c0[1], c0[2], c0[3],
           c1[0], c1[1], c1[2], c1[3],
           vk_format_name((VkFormat)layout.depth_format),
           gfx_backend_offscreen_color_count(t));
    const char *shot = getenv("TOTK_ANDROID_SCREENSHOT");
    if (shot && *shot) {
        char path[1024];
        snprintf(path, sizeof path, "%s-mrt0.ppm", shot);
        gfx_backend_offscreen_write_ppm_index(t, 0, path);
        snprintf(path, sizeof path, "%s-mrt1.ppm", shot);
        gfx_backend_offscreen_write_ppm_index(t, 1, path);
    }
    GfxFence *fence = gfx_backend_fence_create();
    int fence_ok = fence && !gfx_backend_fence_wait(fence, 0);
    if (fence_ok) {
        gfx_backend_fence_signal(fence);
        fence_ok = gfx_backend_fence_wait(fence, 1000000);
    }
    fence_ok = fence_ok && gfx_backend_queue_idle();
    vk_log("MRT selftest fence %s", fence_ok ? "OK" : "FAIL");
    gfx_backend_fence_destroy(fence);
    ok = ok && fence_ok;
    gfx_backend_reflected_pipeline_destroy(p);
    gfx_backend_offscreen_destroy(t);
    return ok;
}

int gfx_backend_selftest(void) {
    if (!gfx_backend_vulkan_init()) {
        vk_log("ERROR Vulkan backend unavailable");
        return 0;
    }
    if (gfx_backend_vk_format_for_nvn(0x25) != VK_FORMAT_R8G8B8A8_UNORM ||
        gfx_backend_vk_format_for_nvn(0x38) != VK_FORMAT_R8G8B8A8_SRGB ||
        gfx_backend_vk_format_for_nvn(0x35) != VK_FORMAT_D24_UNORM_S8_UINT) {
        vk_log("ERROR NVN->Vk format table mismatch");
        return 0;
    }
    GfxOffscreenTarget *t = gfx_backend_offscreen_create(1280, 720, 0x25);
    if (!t) {
        vk_log("ERROR offscreen target creation failed");
        return 0;
    }
    if (!gfx_backend_offscreen_clear(t, 0.25f, 0.5f, 0.75f, 1.0f)) {
        gfx_backend_offscreen_destroy(t);
        return 0;
    }
    uint8_t px[4] = {0};
    if (!gfx_backend_offscreen_read_pixel(t, 0, 0, px)) {
        gfx_backend_offscreen_destroy(t);
        return 0;
    }
    int ok = px[0] >= 62 && px[0] <= 66 && px[1] >= 126 && px[1] <= 130 &&
             px[2] >= 189 && px[2] <= 193 && px[3] >= 253;
    vk_log("selftest %s 1280x720 RGBA8 px=%02x%02x%02x%02x device='%s'",
           ok ? "PASS" : "FAIL", px[0], px[1], px[2], px[3], g.props.deviceName);
    gfx_backend_offscreen_destroy(t);
    return ok;
}
