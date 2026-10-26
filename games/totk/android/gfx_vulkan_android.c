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
    VkImage image;
    VkDeviceMemory image_mem;
    VkImageView view;
    VkBuffer readback;
    VkDeviceMemory readback_mem;
    VkFormat format;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    VkImageLayout layout;
    uint8_t *pixels;
    size_t pixels_size;
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
    case VK_FORMAT_R8G8_UNORM: return (VkFormatInfo){f, "R8G8_UNORM", 2};
    case VK_FORMAT_R8G8B8_UNORM: return (VkFormatInfo){f, "R8G8B8_UNORM", 3};
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
    case VK_FORMAT_R16_SFLOAT: return (VkFormatInfo){f, "R16_SFLOAT", 2};
    case VK_FORMAT_R16G16_UNORM: return (VkFormatInfo){f, "R16G16_UNORM", 4};
    case VK_FORMAT_R16G16_SFLOAT: return (VkFormatInfo){f, "R16G16_SFLOAT", 4};
    case VK_FORMAT_R16G16B16A16_UNORM: return (VkFormatInfo){f, "R16G16B16A16_UNORM", 8};
    case VK_FORMAT_R16G16B16A16_SFLOAT: return (VkFormatInfo){f, "R16G16B16A16_SFLOAT", 8};
    case VK_FORMAT_R32_SFLOAT: return (VkFormatInfo){f, "R32_SFLOAT", 4};
    case VK_FORMAT_R32G32_SFLOAT: return (VkFormatInfo){f, "R32G32_SFLOAT", 8};
    case VK_FORMAT_R32G32B32A32_SFLOAT: return (VkFormatInfo){f, "R32G32B32A32_SFLOAT", 16};
    case VK_FORMAT_D16_UNORM: return (VkFormatInfo){f, "D16_UNORM", 2};
    case VK_FORMAT_X8_D24_UNORM_PACK32: return (VkFormatInfo){f, "X8_D24_UNORM", 4};
    case VK_FORMAT_D24_UNORM_S8_UINT: return (VkFormatInfo){f, "D24_UNORM_S8", 4};
    case VK_FORMAT_D32_SFLOAT: return (VkFormatInfo){f, "D32_SFLOAT", 4};
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return (VkFormatInfo){f, "D32_SFLOAT_S8", 8};
    case VK_FORMAT_S8_UINT: return (VkFormatInfo){f, "S8_UINT", 1};
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return (VkFormatInfo){f, "BC1_RGBA_UNORM", 8};
    case VK_FORMAT_BC3_UNORM_BLOCK: return (VkFormatInfo){f, "BC3_UNORM", 16};
    case VK_FORMAT_BC5_UNORM_BLOCK: return (VkFormatInfo){f, "BC5_UNORM", 16};
    case VK_FORMAT_BC7_UNORM_BLOCK: return (VkFormatInfo){f, "BC7_UNORM", 16};
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
    case 0x6D: return VK_FORMAT_R8G8B8A8_UNORM;
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

GfxOffscreenTarget *gfx_backend_offscreen_create(uint32_t width, uint32_t height,
                                                  uint32_t nvn_format) {
    pthread_mutex_lock(&g_lock);
    if (!vk_init_locked()) { pthread_mutex_unlock(&g_lock); return NULL; }
    if (!width || !height || width > 16384 || height > 16384) {
        vk_log("ERROR offscreen size %ux%u", width, height);
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
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
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g.phys, fmt, &fp);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                               VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                               VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((fp.optimalTilingFeatures & need) != need) {
        vk_log("ERROR format %s lacks color/transfer feature (optimal=%#x)", info.name,
               fp.optimalTilingFeatures);
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    GfxOffscreenTarget *t = calloc(1, sizeof *t);
    if (!t) { pthread_mutex_unlock(&g_lock); return NULL; }
    t->format = fmt;
    t->width = width;
    t->height = height;
    t->bpp = info.bpp;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    t->pixels_size = (size_t)width * height * t->bpp;
    t->pixels = calloc(1, t->pixels_size ? t->pixels_size : 1);
    if (!t->pixels) goto fail;

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (!vk_ok(vkCreateImage(g.dev, &ici, NULL, &t->image), "vkCreateImage(offscreen)")) goto fail;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.dev, t->image, &req);
    uint32_t mt = 0;
    if (!vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mt) &&
        !vk_mem_type(req.memoryTypeBits, 0, &mt)) {
        vk_log("ERROR no image memory type");
        goto fail;
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = mt,
    };
    if (!vk_ok(vkAllocateMemory(g.dev, &mai, NULL, &t->image_mem), "vkAllocateMemory(image)")) goto fail;
    if (!vk_ok(vkBindImageMemory(g.dev, t->image, t->image_mem, 0), "vkBindImageMemory")) goto fail;
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = t->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    if (!vk_ok(vkCreateImageView(g.dev, &vci, NULL, &t->view), "vkCreateImageView")) goto fail;

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = t->pixels_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (!vk_ok(vkCreateBuffer(g.dev, &bci, NULL, &t->readback), "vkCreateBuffer(readback)")) goto fail;
    vkGetBufferMemoryRequirements(g.dev, t->readback, &req);
    if (!vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &mt)) {
        vk_log("ERROR no host-visible readback memory type");
        goto fail;
    }
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mt;
    if (!vk_ok(vkAllocateMemory(g.dev, &mai, NULL, &t->readback_mem), "vkAllocateMemory(readback)")) goto fail;
    if (!vk_ok(vkBindBufferMemory(g.dev, t->readback, t->readback_mem, 0), "vkBindBufferMemory")) goto fail;

    pthread_mutex_unlock(&g_lock);
    vk_log("offscreen %ux%u nvn=%#x vk=%s(%d) bpp=%u image=%p",
           width, height, nvn_format, info.name, (int)fmt, t->bpp, (void *)t->image);
    return t;

fail:
    gfx_backend_offscreen_destroy(t);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

void gfx_backend_offscreen_destroy(GfxOffscreenTarget *t) {
    if (!t) return;
    /* destroy is also called from the create failure path while g_lock is held;
     * keep it lock-free and only touch per-target objects. */
    if (g.dev) {
        if (t->readback) vkDestroyBuffer(g.dev, t->readback, NULL);
        if (t->readback_mem) vkFreeMemory(g.dev, t->readback_mem, NULL);
        if (t->view) vkDestroyImageView(g.dev, t->view, NULL);
        if (t->image) vkDestroyImage(g.dev, t->image, NULL);
        if (t->image_mem) vkFreeMemory(g.dev, t->image_mem, NULL);
    }
    free(t->pixels);
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
    if (t->layout != VK_IMAGE_LAYOUT_GENERAL) {
        vk_image_barrier(cmd, t->image, t->layout, VK_IMAGE_LAYOUT_GENERAL, 0,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         t->layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                                : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);
    }
    VkClearColorValue color = { .float32 = { r, g_, b, a } };
    VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1,
    };
    vkCmdClearColorImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                              .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { t->width, t->height, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, t->readback, 1, &region);
    VkBufferMemoryBarrier bb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = t->readback,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &bb, 0, NULL);
    if (!vk_submit_and_wait(cmd, "clear")) { pthread_mutex_unlock(&g_lock); return 0; }
    t->layout = VK_IMAGE_LAYOUT_GENERAL;

    void *mapped = NULL;
    if (!vk_ok(vkMapMemory(g.dev, t->readback_mem, 0, VK_WHOLE_SIZE, 0, &mapped),
               "vkMapMemory(readback)")) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    memcpy(t->pixels, mapped, t->pixels_size);
    vkUnmapMemory(g.dev, t->readback_mem);
    pthread_mutex_unlock(&g_lock);
    if (getenv("TOTK_ANDROID_TRACE_VK"))
        vk_log("clear %ux%u color=(%.3f,%.3f,%.3f,%.3f) first=%02x%02x%02x%02x",
               t->width, t->height, r, g_, b, a,
               t->pixels[0], t->pixels[1], t->pixels[2], t->pixels[3]);
    return 1;
}

int gfx_backend_offscreen_read_pixel(GfxOffscreenTarget *t, uint32_t x, uint32_t y,
                                     uint8_t out[4]) {
    if (!t || !out || x >= t->width || y >= t->height) return 0;
    size_t off = ((size_t)y * t->width + x) * t->bpp;
    if (off + 4 > t->pixels_size) return 0;
    memcpy(out, t->pixels + off, 4);
    return 1;
}

int gfx_backend_offscreen_present(GfxOffscreenTarget *t) {
    if (!t) return 0;
    t->presents++;
    vk_log("present #%u offscreen %ux%u %s", t->presents, t->width, t->height,
           vk_format_name(t->format));
    return 1;
}

uint32_t gfx_backend_offscreen_width(const GfxOffscreenTarget *t) { return t ? t->width : 0; }
uint32_t gfx_backend_offscreen_height(const GfxOffscreenTarget *t) { return t ? t->height : 0; }
const uint8_t *gfx_backend_offscreen_pixels(const GfxOffscreenTarget *t, size_t *size) {
    if (size) *size = t ? t->pixels_size : 0;
    return t ? t->pixels : NULL;
}
const char *gfx_backend_offscreen_format_name(const GfxOffscreenTarget *t) {
    return t ? vk_format_name(t->format) : "none";
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
    if (!g.ready || t->format != p->format || t->bpp != 4) {
        vk_log("ERROR triangle draw: format/bpp mismatch (pipeline=%d target=%d bpp=%u)",
               (int)p->format, (int)t->format, t->bpp);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (!p->fb || p->fb_view != t->view || p->fb_w != t->width || p->fb_h != t->height) {
        if (p->fb) vkDestroyFramebuffer(g.dev, p->fb, NULL);
        VkImageView view = t->view;
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
        p->fb_view = t->view;
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
    vkCmdCopyImageToBuffer(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, t->readback, 1, &region);
    VkBufferMemoryBarrier bb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = t->readback,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &bb, 0, NULL);
    if (!vk_submit_and_wait(cmd, "triangle")) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    t->layout = VK_IMAGE_LAYOUT_GENERAL;
    void *mapped = NULL;
    if (!vk_ok(vkMapMemory(g.dev, t->readback_mem, 0, VK_WHOLE_SIZE, 0, &mapped),
               "vkMapMemory(triangle)")) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    memcpy(t->pixels, mapped, t->pixels_size);
    vkUnmapMemory(g.dev, t->readback_mem);
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


struct GfxTexture {
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    VkSampler sampler;
    uint32_t width, height;
    VkImageLayout layout;
};

/* Callers must hold g_lock.  Kept separate from the public wrapper so the
 * reflected pipeline (which already holds the lock) can create its fallback
 * textures without deadlocking on the non-recursive g_lock. */
static GfxTexture *texture_create_rgba8_locked(uint32_t width, uint32_t height,
                                               const uint8_t *pixels) {
    if (!width || !height || width > 8192 || height > 8192 || !pixels) return NULL;
    if (!vk_init_locked()) return NULL;
    GfxTexture *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->width = width; t->height = height;
    size_t bytes = (size_t)width * height * 4;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { width, height, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (!vk_ok(vkCreateImage(g.dev, &ici, NULL, &t->image), "vkCreateImage(texture)")) goto fail;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.dev, t->image, &req);
    uint32_t mt = 0;
    if (!vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mt) &&
        !vk_mem_type(req.memoryTypeBits, 0, &mt)) goto fail;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = mt };
    if (!vk_ok(vkAllocateMemory(g.dev, &mai, NULL, &t->mem), "vkAllocateMemory(texture)")) goto fail;
    if (!vk_ok(vkBindImageMemory(g.dev, t->image, t->mem, 0), "vkBindImageMemory(texture)")) goto fail;
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = t->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (!vk_ok(vkCreateImageView(g.dev, &vci, NULL, &t->view), "vkCreateImageView(texture)")) goto fail;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0,
    };
    if (!vk_ok(vkCreateSampler(g.dev, &sci, NULL, &t->sampler), "vkCreateSampler(texture)")) goto fail;

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stage_mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (!vk_ok(vkCreateBuffer(g.dev, &bci, NULL, &staging), "vkCreateBuffer(texture staging)")) goto fail;
    vkGetBufferMemoryRequirements(g.dev, staging, &req);
    if (!vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &mt)) goto fail_stage;
    mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
    if (!vk_ok(vkAllocateMemory(g.dev, &mai, NULL, &stage_mem), "vkAllocateMemory(texture staging)")) goto fail_stage;
    if (!vk_ok(vkBindBufferMemory(g.dev, staging, stage_mem, 0), "vkBindBufferMemory(texture staging)")) goto fail_stage;
    void *mapped = NULL;
    if (!vk_ok(vkMapMemory(g.dev, stage_mem, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory(texture)")) goto fail_stage;
    memcpy(mapped, pixels, bytes);
    vkUnmapMemory(g.dev, stage_mem);

    VkCommandBuffer cmd = g.cmd;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(texture)")) goto fail_stage;
    VkBufferMemoryBarrier bmb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = staging, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 1, &bmb, 0, NULL);
    vk_image_barrier(cmd, t->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {
        .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { 0, 0, 0 }, .imageExtent = { width, height, 1 },
    };
    vkCmdCopyBufferToImage(cmd, staging, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    vk_image_barrier(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (!vk_submit_and_wait(cmd, "texture upload")) goto fail_stage;
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkDestroyBuffer(g.dev, staging, NULL);
    vkFreeMemory(g.dev, stage_mem, NULL);
    vk_log("texture %ux%u RGBA8 image=%p", width, height, (void *)t->image);
    return t;

fail_stage:
    if (staging) vkDestroyBuffer(g.dev, staging, NULL);
    if (stage_mem) vkFreeMemory(g.dev, stage_mem, NULL);
fail:
    gfx_backend_texture_destroy(t);
    return NULL;
}

GfxTexture *gfx_backend_texture_create_rgba8(uint32_t width, uint32_t height,
                                             const uint8_t *pixels) {
    pthread_mutex_lock(&g_lock);
    GfxTexture *t = texture_create_rgba8_locked(width, height, pixels);
    pthread_mutex_unlock(&g_lock);
    return t;
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
    if (!g.ready || t->format != p->format || t->bpp != 4) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (!p->fb || p->fb_view != t->view || p->fb_w != t->width || p->fb_h != t->height) {
        if (p->fb) vkDestroyFramebuffer(g.dev, p->fb, NULL);
        VkImageView view = t->view;
        VkFramebufferCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = p->pass, .attachmentCount = 1, .pAttachments = &view,
            .width = t->width, .height = t->height, .layers = 1,
        };
        if (!vk_ok(vkCreateFramebuffer(g.dev, &fci, NULL, &p->fb), "vkCreateFramebuffer(tex)")) {
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        p->fb_view = t->view; p->fb_w = t->width; p->fb_h = t->height;
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
    vkCmdCopyImageToBuffer(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, t->readback, 1, &region);
    VkBufferMemoryBarrier bb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = t->readback, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &bb, 0, NULL);
    if (!vk_submit_and_wait(cmd, "textured triangle")) {
        pthread_mutex_unlock(&g_lock); return 0;
    }
    t->layout = VK_IMAGE_LAYOUT_GENERAL;
    void *mapped = NULL;
    if (!vk_ok(vkMapMemory(g.dev, t->readback_mem, 0, VK_WHOLE_SIZE, 0, &mapped),
               "vkMapMemory(tex)")) {
        pthread_mutex_unlock(&g_lock); return 0;
    }
    memcpy(t->pixels, mapped, t->pixels_size);
    vkUnmapMemory(g.dev, t->readback_mem);
    pthread_mutex_unlock(&g_lock);
    return 1;
}


struct GfxReflectedPipeline {
    VkShaderModule vs, ps;
    VkDescriptorSetLayout dsl[2];
    VkDescriptorPool dpool;
    VkDescriptorSet dset[2];
    VkPipelineLayout layout;
    VkRenderPass pass;
    VkPipeline pipe;
    VkFormat format;
    VkFramebuffer fb;
    uint32_t fb_w, fb_h;
    VkImageView fb_view;
    GfxTexture *tex[16];
    unsigned ntex;
    VkImage img[16];
    VkDeviceMemory img_mem[16];
    VkImageView img_view[16];
    unsigned nimg;
    VkBuffer buf[16];
    VkDeviceMemory buf_mem[16];
    unsigned nbuf;
};

static int vk_make_buffer(uint32_t size, VkBuffer *buf, VkDeviceMemory *mem) {
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
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

static int vk_make_storage_image(uint32_t w, uint32_t h, VkImage *img, VkDeviceMemory *mem,
                                 VkImageView *view) {
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
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
        .image = *img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    return vk_ok(vkCreateImageView(g.dev, &vci, NULL, view), "vkCreateImageView(reflection storage)");
}

GfxReflectedPipeline *gfx_backend_reflected_pipeline_create(const uint8_t *vs, size_t vs_size,
                                                            const uint8_t *ps, size_t ps_size,
                                                            int vk_format,
                                                            const GfxBindingSpec *vs_specs, uint32_t nvs,
                                                            const GfxBindingSpec *ps_specs, uint32_t nps) {
    pthread_mutex_lock(&g_lock);
    if (!vk_init_locked()) { pthread_mutex_unlock(&g_lock); return NULL; }
    VkFormat fmt = (VkFormat)vk_format;
    GfxReflectedPipeline *p = calloc(1, sizeof *p);
    if (fmt == VK_FORMAT_UNDEFINED) {
        vk_log("ERROR reflected pipeline: undefined format");
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    if (!p) { pthread_mutex_unlock(&g_lock); return NULL; }
    p->format = fmt;
    if (!vk_shader_module(vs, vs_size, "vkCreateShaderModule(reflected vs)", &p->vs) ||
        !vk_shader_module(ps, ps_size, "vkCreateShaderModule(reflected ps)", &p->ps))
        goto fail;

    const GfxBindingSpec *sets[2] = { vs_specs, ps_specs };
    uint32_t counts[2] = { nvs, nps };
    VkDescriptorPoolSize pool[4] = {0};
    uint32_t npool = 0;
    for (unsigned s = 0; s < 2; s++) {
        if (counts[s] > 16) goto fail;
        VkDescriptorSetLayoutBinding b[16];
        for (uint32_t i = 0; i < counts[s]; i++) {
            VkDescriptorType dt = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            if (sets[s][i].kind == GFX_BIND_SSBO) dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            else if (sets[s][i].kind == GFX_BIND_TEXTURE) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            else if (sets[s][i].kind == GFX_BIND_IMAGE) dt = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b[i] = (VkDescriptorSetLayoutBinding){ .binding = sets[s][i].binding,
                .descriptorType = dt, .descriptorCount = sets[s][i].count ? sets[s][i].count : 1,
                .stageFlags = s == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT };
            int found = -1;
            for (uint32_t j = 0; j < npool; j++) if (pool[j].type == dt) found = (int)j;
            if (found < 0) { pool[npool].type = dt; found = (int)npool++; }
            pool[found].descriptorCount += b[i].descriptorCount;
        }
        VkDescriptorSetLayoutCreateInfo dlci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .bindingCount = counts[s], .pBindings = b };
        if (!vk_ok(vkCreateDescriptorSetLayout(g.dev, &dlci, NULL, &p->dsl[s]), "vkCreateDescriptorSetLayout(reflected)")) goto fail;
    }
    if (!npool) { pool[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; pool[0].descriptorCount = 0; npool = 1; }
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                        .maxSets = 2, .poolSizeCount = npool, .pPoolSizes = pool };
    if (!vk_ok(vkCreateDescriptorPool(g.dev, &dpci, NULL, &p->dpool), "vkCreateDescriptorPool(reflected)")) goto fail;
    for (unsigned s = 0; s < 2; s++) {
        VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                             .descriptorPool = p->dpool, .descriptorSetCount = 1, .pSetLayouts = &p->dsl[s] };
        if (!vk_ok(vkAllocateDescriptorSets(g.dev, &dsai, &p->dset[s]), "vkAllocateDescriptorSets(reflected)")) goto fail;
    }

    VkWriteDescriptorSet writes[32];
    uint32_t nw = 0;
    VkDescriptorBufferInfo binfo[16];
    VkDescriptorImageInfo iinfo[16][4];
    for (unsigned s = 0; s < 2; s++) {
        for (uint32_t i = 0; i < counts[s]; i++) {
            uint32_t count = sets[s][i].count ? sets[s][i].count : 1;
            if (count > 4) goto fail;
            VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = p->dset[s], .dstBinding = sets[s][i].binding,
                                       .descriptorCount = count };
            if (sets[s][i].kind == GFX_BIND_UBO || sets[s][i].kind == GFX_BIND_SSBO) {
                if (p->nbuf >= 16) goto fail;
                uint32_t sz = sets[s][i].kind == GFX_BIND_UBO ? 0x10000 : 0x1000;
                if (!vk_make_buffer(sz, &p->buf[p->nbuf], &p->buf_mem[p->nbuf])) goto fail;
                binfo[i] = (VkDescriptorBufferInfo){ .buffer = p->buf[p->nbuf], .offset = 0, .range = VK_WHOLE_SIZE };
                w.descriptorType = sets[s][i].kind == GFX_BIND_UBO ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                                  : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w.pBufferInfo = &binfo[i];
                p->nbuf++;
            } else if (sets[s][i].kind == GFX_BIND_TEXTURE) {
                static const uint8_t checker[16] = { 255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255 };
                GfxTexture *tex = texture_create_rgba8_locked(2, 2, checker);
                if (!tex) goto fail;
                p->tex[p->ntex++] = tex;
                for (uint32_t k = 0; k < count; k++)
                    iinfo[i][k] = (VkDescriptorImageInfo){ .sampler = tex->sampler, .imageView = tex->view,
                                                           .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.pImageInfo = iinfo[i];
            } else {
                if (p->nimg >= 16) goto fail;
                if (!vk_make_storage_image(2, 2, &p->img[p->nimg], &p->img_mem[p->nimg], &p->img_view[p->nimg])) goto fail;
                for (uint32_t k = 0; k < count; k++)
                    iinfo[i][k] = (VkDescriptorImageInfo){ .sampler = VK_NULL_HANDLE,
                                                           .imageView = p->img_view[p->nimg],
                                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
                w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                w.pImageInfo = iinfo[i];
                p->nimg++;
            }
            writes[nw++] = w;
        }
    }
    if (nw) vkUpdateDescriptorSets(g.dev, nw, writes, 0, NULL);

    VkAttachmentDescription att = { .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = 1, .pColorAttachments = &ref };
    VkSubpassDependency dep = { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };
    VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub,
        .dependencyCount = 1, .pDependencies = &dep };
    if (!vk_ok(vkCreateRenderPass(g.dev, &rpci, NULL, &p->pass), "vkCreateRenderPass(reflected)")) goto fail;
    VkDescriptorSetLayout layouts[2] = { p->dsl[0], p->dsl[1] };
    VkPipelineLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 2, .pSetLayouts = layouts };
    if (!vk_ok(vkCreatePipelineLayout(g.dev, &lci, NULL, &p->layout), "vkCreatePipelineLayout(reflected)")) goto fail;
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = p->vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = p->ps, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rast = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .blendEnable = VK_FALSE, .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rast, .pMultisampleState = &ms,
        .pColorBlendState = &cb, .pDynamicState = &ds, .layout = p->layout, .renderPass = p->pass };
    if (!vk_ok(vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &gpci, NULL, &p->pipe),
               "vkCreateGraphicsPipelines(reflected)")) goto fail;
    pthread_mutex_unlock(&g_lock);
    vk_log("reflected pipeline ready vs=%u ps=%u bindings format=%s", nvs, nps, vk_format_name(fmt));
    return p;
fail:
    gfx_backend_reflected_pipeline_destroy(p);
    pthread_mutex_unlock(&g_lock);
    return NULL;
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
        for (unsigned i = 0; i < p->nbuf; i++) {
            if (p->buf[i]) vkDestroyBuffer(g.dev, p->buf[i], NULL);
            if (p->buf_mem[i]) vkFreeMemory(g.dev, p->buf_mem[i], NULL);
        }
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

int gfx_backend_reflected_draw(GfxReflectedPipeline *p, GfxOffscreenTarget *t,
                               float r, float g_, float b, float a) {
    if (!p || !t) return 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready || t->format != p->format || t->bpp != 4) { pthread_mutex_unlock(&g_lock); return 0; }
    if (!p->fb || p->fb_view != t->view || p->fb_w != t->width || p->fb_h != t->height) {
        if (p->fb) vkDestroyFramebuffer(g.dev, p->fb, NULL);
        VkImageView view = t->view;
        VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = p->pass, .attachmentCount = 1, .pAttachments = &view,
            .width = t->width, .height = t->height, .layers = 1 };
        if (!vk_ok(vkCreateFramebuffer(g.dev, &fci, NULL, &p->fb), "vkCreateFramebuffer(reflected)")) {
            pthread_mutex_unlock(&g_lock); return 0;
        }
        p->fb_view = t->view; p->fb_w = t->width; p->fb_h = t->height;
    }
    VkCommandBuffer cmd = g.cmd;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(reflected)")) {
        pthread_mutex_unlock(&g_lock); return 0;
    }
    VkClearValue clear = { .color = { .float32 = { r, g_, b, a } } };
    VkRenderPassBeginInfo rp = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = p->pass, .framebuffer = p->fb,
        .renderArea = { { 0, 0 }, { t->width, t->height } }, .clearValueCount = 1, .pClearValues = &clear };
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipe);
    VkDescriptorSet sets[2] = { p->dset[0], p->dset[1] };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 2, sets, 0, NULL);
    VkViewport viewport = { 0, 0, (float)t->width, (float)t->height, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, { t->width, t->height } };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { t->width, t->height, 1 } };
    vkCmdCopyImageToBuffer(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, t->readback, 1, &region);
    VkBufferMemoryBarrier bb = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = t->readback, .offset = 0, .size = VK_WHOLE_SIZE };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &bb, 0, NULL);
    if (!vk_submit_and_wait(cmd, "reflected")) { pthread_mutex_unlock(&g_lock); return 0; }
    t->layout = VK_IMAGE_LAYOUT_GENERAL;
    void *mapped = NULL;
    if (!vk_ok(vkMapMemory(g.dev, t->readback_mem, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory(reflected)")) {
        pthread_mutex_unlock(&g_lock); return 0;
    }
    memcpy(t->pixels, mapped, t->pixels_size);
    vkUnmapMemory(g.dev, t->readback_mem);
    pthread_mutex_unlock(&g_lock);
    return 1;
}

static GfxTexturedPipeline *g_builtin_textured;
GfxTexturedPipeline *gfx_backend_builtin_textured_pipeline(void) {
    if (!g_builtin_textured)
        g_builtin_textured = gfx_backend_textured_pipeline_create(VK_FORMAT_R8G8B8A8_UNORM);
    return g_builtin_textured;
}

int gfx_backend_offscreen_write_ppm(const GfxOffscreenTarget *t, const char *path) {
    if (!t || !path || t->bpp != 4 || !t->pixels) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) { vk_log("ERROR screenshot open %s", path); return 0; }
    fprintf(f, "P6\n%u %u\n255\n", t->width, t->height);
    size_t n = (size_t)t->width * t->height;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = t->pixels + i * 4;
        fwrite(p, 1, 3, f);
    }
    int ok = ferror(f) == 0;
    fclose(f);
    if (ok) vk_log("screenshot %s (%ux%u, %zu bytes)", path, t->width, t->height, n * 3);
    else vk_log("ERROR screenshot write %s", path);
    return ok;
}

uint64_t gfx_backend_offscreen_hash(const GfxOffscreenTarget *t) {
    if (!t || !t->pixels) return 0;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < t->pixels_size; i++) {
        h ^= t->pixels[i];
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
