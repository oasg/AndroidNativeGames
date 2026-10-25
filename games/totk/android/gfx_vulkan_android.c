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

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
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
    va_list ap;
    va_start(ap, fmt);
    fputs("[totk-vk] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
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
