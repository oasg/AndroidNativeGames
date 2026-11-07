/* Type and object layouts. */
#ifndef TOTK_ANDROID_GFX_TYPES_0_H
#define TOTK_ANDROID_GFX_TYPES_0_H
#include "gfx_prelude.h"

/* Vulkan backend for the Android TOTK port.
 *
 * This is a 2B.1 bring-up backend, not a renderer replacement yet.  It owns the
 * Android Vulkan instance/device/queue, an offscreen VkImage render target and
 * an optional VkSwapchainKHR for a real ANativeWindow.  The NVN object layer in
 * nvn/ keeps the object graph, format/layout semantics and submission
 * order; this file only turns the selected resources into Vulkan work.
 *
 * Errors are explicit.  If Vulkan is unavailable, `gfx_backend_vulkan_init`
 * returns 0 and the caller can select the software path; when the caller asks
 * for Vulkan specifically, no fake success object is produced.
 */
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#endif


#ifdef __ANDROID__
#endif


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

    /* Needed to rebuild a lost/out-of-date swapchain without another attach
     * call from the app shell.  `android_window` is the ANativeWindow* the
     * surface was created from. */
    void *android_window;
    uint32_t attach_width, attach_height;
    uint32_t attach_format;
    int sc_suboptimal;
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



/* Cached fullscreen-present pipeline, keyed by (target format, swapchain
 * format, final layout).  At most one surface exists at a time, so a handful
 * of entries covers the RGBA8 and R11G11B10F targets plus the selftest image. */
typedef struct {
    int used;
    VkFormat src_format, dst_format;
    VkImageLayout final_layout;
    VkRenderPass pass;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkSampler sampler;
} GfxPresentPipe;



/* Recent render-target dimensions, recorded when an offscreen layout is
 * requested.  The frozen reflected-pipeline API carries only the format, so
 * this lets a rejected attachment report the size the caller asked for. */
typedef struct {
    int format;
    uint32_t width, height;
} VkRtDimHint;



/* Submission fences.  Every backend submission above is synchronous, so a
 * fence can only become signaled once the queue has actually drained.  We
 * still wait on the queue in signal()/wait() so a future async submit path
 * cannot report completion early. */
struct GfxFence {
    int signaled;
};




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



/* Adreno's LLVM shader compiler needs more than the 1 MiB host stack that a
 * guest thread often provides.  vkCreateGraphicsPipelines runs the compiler on
 * the calling thread, so compile on a dedicated large-stack host thread.  This
 * is what the BC4 save/title frame hit at libllvm-qgl+0xebb514 (stack guard
 * page below sp), not the texture upload path. */
typedef struct {
    VkPipelineCache cache;
    const VkGraphicsPipelineCreateInfo *ci;
    VkPipeline *out;
    VkResult result;
} VkPipeCompileJob;




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
    int transfer_dst_ok;
    int storage_ok;
};



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



struct GfxSampler {
    VkSampler sampler;
    GfxSamplerDesc desc;
};



typedef struct {
    size_t ubo_off[2][GFX_REFLECT_MAX_BINDINGS];
    size_t ssbo_off[2][GFX_REFLECT_MAX_BINDINGS];
    uint8_t dyn[2][GFX_REFLECT_MAX_BINDINGS];
    const GfxTexture *tex[2][GFX_REFLECT_MAX_BINDINGS];
    const GfxSampler *samp[2][GFX_REFLECT_MAX_BINDINGS];
} ReflectEpochState;



/* ------------------------------------------------------------------ */
/* Compute pipelines and dispatch.                                     */

typedef struct {
    int bound;
    GfxBindingKind kind;
    VkBuffer buffer;
    VkDeviceMemory mem;
    void *mapped;
    VkDeviceSize capacity;
    const GfxTexture *texture;
    VkImageLayout layout;
} GfxComputeBinding;



struct GfxComputePipeline {
    VkShaderModule cs;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool pool;
    VkDescriptorSet dset;
    VkPipelineLayout layout;
    VkPipeline pipe;
    uint32_t nspec;
    GfxBindingSpec spec[GFX_COMPUTE_MAX_BINDINGS];
    GfxComputeBinding bind[GFX_COMPUTE_MAX_BINDINGS];
};



typedef struct {
    VkPipelineCache cache;
    const VkComputePipelineCreateInfo *ci;
    VkPipeline *out;
    VkResult result;
} VkComputeCompileJob;



/* ------------------------------------------------------------------ */
/* Repository-owned probes.                                            */

typedef struct {
    uint32_t width, height, seed, pad;
    float scale[4];
} GfxComputeProbeParams;

#endif
