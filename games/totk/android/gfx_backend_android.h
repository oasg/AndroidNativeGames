/* Android graphics backend selection.
 *
 * Phase 2B uses Vulkan for Android.  The NVN CPU object layer stays the source
 * of truth for object lifetime and command ordering; this interface is the
 * narrow boundary where NVN resources are turned into Vulkan objects.  The
 * macOS d2r_gfx Metal backend is not used here; only its NVN semantics,
 * format/layout tables and command-order rules are shared conceptually.
 */
#ifndef TOTK_ANDROID_GFX_BACKEND_H
#define TOTK_ANDROID_GFX_BACKEND_H

#include <stddef.h>
#include <stdint.h>

typedef struct GfxOffscreenTarget GfxOffscreenTarget;

/* Backend availability is explicit: a failed Vulkan path never reports a fake
 * success.  `gfx_backend_vulkan_init` is idempotent and thread-safe. */
int gfx_backend_vulkan_init(void);
void gfx_backend_vulkan_shutdown(void);
int gfx_backend_vulkan_available(void);
const char *gfx_backend_name(void);

/* NVN format id -> Vulkan VkFormat.  Returns VK_FORMAT_UNDEFINED (0) for an
 * unsupported format; callers must treat that as an error. */
int gfx_backend_vk_format_for_nvn(uint32_t nvn_format);
int gfx_backend_vk_format_bpp(int vk_format);

/* Offscreen render target.  The image is device-local; a host-visible
 * readback buffer is used for verification and for the software mirror used by
 * the existing HostNativeWindow API. */
GfxOffscreenTarget *gfx_backend_offscreen_create(uint32_t width, uint32_t height,
                                                  uint32_t nvn_format);
void gfx_backend_offscreen_destroy(GfxOffscreenTarget *target);
int gfx_backend_offscreen_clear(GfxOffscreenTarget *target,
                                float r, float g, float b, float a);
int gfx_backend_offscreen_read_pixel(GfxOffscreenTarget *target, uint32_t x, uint32_t y,
                                     uint8_t out[4]);
int gfx_backend_offscreen_present(GfxOffscreenTarget *target);
uint32_t gfx_backend_offscreen_width(const GfxOffscreenTarget *target);
uint32_t gfx_backend_offscreen_height(const GfxOffscreenTarget *target);
const uint8_t *gfx_backend_offscreen_pixels(const GfxOffscreenTarget *target,
                                            size_t *size);
const char *gfx_backend_offscreen_format_name(const GfxOffscreenTarget *target);
int gfx_backend_offscreen_write_ppm(const GfxOffscreenTarget *target, const char *path);
uint64_t gfx_backend_offscreen_hash(const GfxOffscreenTarget *target);

/* Minimal graphics pipeline used by the 2B.3 triangle milestone and by the
 * host NVN draw self-test.  The SPIR-V pair is compiled from the repository's
 * own triangle.vert/frag; translated game shaders can use the same path. */
typedef struct GfxTrianglePipeline GfxTrianglePipeline;
GfxTrianglePipeline *gfx_backend_triangle_pipeline_create(const uint8_t *vs, size_t vs_size,
                                                          const uint8_t *ps, size_t ps_size,
                                                          int vk_format);
void gfx_backend_triangle_pipeline_destroy(GfxTrianglePipeline *pipeline);
int gfx_backend_triangle_draw(GfxTrianglePipeline *pipeline, GfxOffscreenTarget *target,
                              float r, float g, float b, float a);
GfxTrianglePipeline *gfx_backend_builtin_triangle_pipeline(void);

typedef struct { uint32_t binding; uint32_t size; } GfxUboBinding;
GfxTrianglePipeline *gfx_backend_triangle_pipeline_create_ubo(const uint8_t *vs, size_t vs_size,
                                                              const uint8_t *ps, size_t ps_size,
                                                              int vk_format,
                                                              const GfxUboBinding *ubos, uint32_t nubo);
int gfx_backend_triangle_write_ubo(GfxTrianglePipeline *pipeline, uint32_t binding,
                                   uint32_t offset, const void *data, size_t size);
/* TOTK VS + built-in fragment shader, with the two cbuf UBOs the system
 * vertex shader reads (binding 1 = vp_c4, binding 2 = vp_c11). */
GfxTrianglePipeline *gfx_backend_totk_vs_pipeline_create(const uint8_t *vs, size_t vs_size,
                                                         int vk_format);

/* Reflected multi-descriptor pipeline.  Set 0 belongs to the vertex stage and
 * set 1 to the fragment stage (the translated PS SPIR-V is moved to set 1 so
 * independent NVN binding numbers cannot collide). */
typedef enum { GFX_BIND_UBO = 0, GFX_BIND_SSBO = 1, GFX_BIND_TEXTURE = 2, GFX_BIND_IMAGE = 3 } GfxBindingKind;
typedef struct { uint32_t binding; GfxBindingKind kind; uint32_t count; } GfxBindingSpec;
typedef struct GfxReflectedPipeline GfxReflectedPipeline;
GfxReflectedPipeline *gfx_backend_reflected_pipeline_create(const uint8_t *vs, size_t vs_size,
                                                            const uint8_t *ps, size_t ps_size,
                                                            int vk_format,
                                                            const GfxBindingSpec *vs_specs, uint32_t nvs,
                                                            const GfxBindingSpec *ps_specs, uint32_t nps);
void gfx_backend_reflected_pipeline_destroy(GfxReflectedPipeline *pipeline);
int gfx_backend_reflected_draw(GfxReflectedPipeline *pipeline, GfxOffscreenTarget *target,
                               float r, float g, float b, float a);

/* Minimal sampled-texture path: RGBA8 texture -> VkImage/VkSampler and a
 * textured triangle pipeline.  This is the 2B.3 NVN texture bridge's Vulkan
 * half; the NVN object wraps it. */
typedef struct GfxTexture GfxTexture;
typedef struct GfxTexturedPipeline GfxTexturedPipeline;
GfxTexture *gfx_backend_texture_create_rgba8(uint32_t width, uint32_t height,
                                             const uint8_t *pixels);
void gfx_backend_texture_destroy(GfxTexture *texture);
GfxTexturedPipeline *gfx_backend_textured_pipeline_create(int vk_format);
void gfx_backend_textured_pipeline_destroy(GfxTexturedPipeline *pipeline);
int gfx_backend_textured_draw(GfxTexturedPipeline *pipeline, GfxTexture *texture,
                              GfxOffscreenTarget *target,
                              float r, float g, float b, float a);
GfxTexturedPipeline *gfx_backend_builtin_textured_pipeline(void);

/* Android Surface path.  `window` is an ANativeWindow*.  A surface is optional:
 * offscreen targets work without it.  If creation fails, the caller keeps the
 * offscreen target and reports the explicit error. */
int gfx_backend_attach_android_surface(void *window, uint32_t width, uint32_t height,
                                       uint32_t nvn_format);
void gfx_backend_detach_android_surface(void);
int gfx_backend_surface_ready(void);
int gfx_backend_surface_present(float r, float g, float b, float a);

/* End-to-end host test: instance/device, 1280x720 offscreen RGBA8 target,
 * clear, GPU copy-back and pixel verification. */
int gfx_backend_selftest(void);

/* Validate a translated SPIR-V module with the real Android Vulkan driver by
 * creating a VkShaderModule.  This is the Android-side check of the offline
 * NVN -> SPIR-V chain; it rejects a bad magic/alignment before the driver. */
int gfx_backend_validate_spirv(const uint8_t *code, size_t size, const char *name);

#endif
