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
