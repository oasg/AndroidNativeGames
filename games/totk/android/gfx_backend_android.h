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
typedef struct GfxTexture GfxTexture;

/* Backend availability is explicit: a failed Vulkan path never reports a fake
 * success.  `gfx_backend_vulkan_init` is idempotent and thread-safe. */
int gfx_backend_vulkan_init(void);
void gfx_backend_vulkan_shutdown(void);
int gfx_backend_vulkan_available(void);
const char *gfx_backend_name(void);

/* NVN format id -> Vulkan VkFormat.  Returns VK_FORMAT_UNDEFINED (0) for an
 * unsupported format; callers must treat that as an error. */
int gfx_backend_vk_format_for_nvn(uint32_t nvn_format);
/* Ryujinx TextureFormat enum (the string in .res.json images[].format) ->
 * Vulkan format.  Unknown/unsupported returns VK_FORMAT_UNDEFINED (0). */
int gfx_backend_vk_format_for_texture_format(uint32_t texture_format);
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
/* The built-in triangle SPIR-V pair (repository-owned test shader) so the NVN
 * cache probe can populate a synthetic cache entry without game data. */
void gfx_backend_builtin_triangle_modules(const uint8_t **vs, size_t *vs_size,
                                          const uint8_t **ps, size_t *ps_size);
void gfx_backend_builtin_texture_modules(const uint8_t **vs, size_t *vs_size,
                                         const uint8_t **ps, size_t *ps_size);
void gfx_backend_builtin_ubo_modules(const uint8_t **vs, size_t *vs_size,
                                     const uint8_t **ps, size_t *ps_size);
void gfx_backend_builtin_attr_modules(const uint8_t **vs, size_t *vs_size,
                                      const uint8_t **ps, size_t *ps_size);

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
typedef enum {
    GFX_BIND_UBO = 0,
    GFX_BIND_SSBO,
    GFX_BIND_SAMPLER,
    GFX_BIND_SAMPLED_IMAGE,
    GFX_BIND_COMBINED_IMAGE_SAMPLER,
    GFX_BIND_STORAGE_IMAGE,
    GFX_BIND_UNIFORM_TEXEL_BUFFER,
    GFX_BIND_STORAGE_TEXEL_BUFFER
} GfxBindingKind;

/* A descriptor binding as declared by the translated SPIR-V, plus the NVN
 * source information from the matching .res.json entry.  `kind` and `count`
 * are the SPIR-V view; `nvn_*` is only used to resolve the runtime resource. */
typedef struct {
    uint32_t set;
    uint32_t binding;
    GfxBindingKind kind;
    uint32_t count;

    int32_t  nvn_slot;        /* cbuf/sbuf slot, texture slot when known */
    int32_t  nvn_cbuf_slot;   /* constant bank holding the NVN handle/address */
    int32_t  nvn_cb_offset;   /* word offset in that bank (sbufs cb_offset) */
    uint32_t nvn_handle;      /* translator fake handle, not the runtime handle */
    uint32_t nvn_type;        /* Ryujinx SamplerType value */
    uint32_t nvn_format;      /* Ryujinx TextureFormat value */
    uint32_t nvn_flags;       /* BufferUsageFlags / TextureUsageFlags */
    int      separate;        /* separate texture + sampler binding */
} GfxBindingSpec;

/* Vertex input derived from the NVN attribute/stream state.  Attribute
 * locations are the NVN attribute indices; bindings are the NVN stream
 * indices.  `format` is a VkFormat. */
typedef struct {
    uint32_t location;
    uint32_t binding;
    int format;
    uint32_t offset;
} GfxVertexAttribute;

typedef struct {
    uint32_t binding;
    uint32_t stride;
    uint32_t input_rate;      /* 0 vertex, 1 instance */
} GfxVertexBinding;

typedef struct {
    uint32_t nbindings, nattributes;
    GfxVertexBinding bindings[16];
    GfxVertexAttribute attributes[16];
} GfxVertexLayout;

/* Offline reflection helpers shared by the probes and the NVN program bridge.
 * They are implemented in loader_android.c; `json_path` is the .res.json and
 * `spv_path` the translated module.  SPIR-V wins for descriptor type/count. */
unsigned gfx_reflect_full(const char *json_path, const char *spv_path, uint32_t target_set,
                          GfxBindingSpec *out, unsigned max);
int gfx_spirv_move_set(uint8_t *code, size_t size, uint32_t from, uint32_t to);

typedef struct GfxReflectedPipeline GfxReflectedPipeline;
GfxReflectedPipeline *gfx_backend_reflected_pipeline_create(const uint8_t *vs, size_t vs_size,
                                                            const uint8_t *ps, size_t ps_size,
                                                            int vk_format,
                                                            const GfxBindingSpec *vs_specs, uint32_t nvs,
                                                            const GfxBindingSpec *ps_specs, uint32_t nps);
GfxReflectedPipeline *gfx_backend_reflected_pipeline_create_ex(
                                                            const uint8_t *vs, size_t vs_size,
                                                            const uint8_t *ps, size_t ps_size,
                                                            int vk_format,
                                                            const GfxBindingSpec *vs_specs, uint32_t nvs,
                                                            const GfxBindingSpec *ps_specs, uint32_t nps,
                                                            const GfxVertexLayout *input);
void gfx_backend_reflected_pipeline_destroy(GfxReflectedPipeline *pipeline);
int gfx_backend_reflected_draw(GfxReflectedPipeline *pipeline, GfxOffscreenTarget *target,
                               float r, float g, float b, float a);

/* Command-buffer translation, ordered the way the guest records it:
 * clear colour, viewport, scissor, draw.  Descriptor/resource binds are still
 * applied through the reflected pipeline's sets (dummy resources until the
 * per-draw upload bridge lands).  `default_clear` is used when the list has no
 * GFX_CMD_CLEAR_COLOR, matching the window's default clear. */
typedef enum {
    GFX_CMD_CLEAR_COLOR = 1,
    GFX_CMD_SET_VIEWPORT,
    GFX_CMD_SET_SCISSOR,
    GFX_CMD_DRAW,
    GFX_CMD_DRAW_INDEXED
} GfxCmdKind;

typedef struct {
    uint32_t kind;
    union {
        struct { float rgba[4]; uint32_t mask; } clear;
        struct { int32_t x, y, width, height; } rect;
        struct { uint32_t primitive, first, count; } draw;
        struct {
            uint32_t primitive, count, index_type, base_vertex;
            const void *indices;
            size_t index_size;
        } draw_indexed;
    } u;
} GfxCmd;

typedef struct {
    uint32_t binding;
    const void *data;
    size_t size;
} GfxVertexStream;

int gfx_backend_reflected_execute(GfxReflectedPipeline *pipeline, GfxOffscreenTarget *target,
                                  const GfxCmd *cmds, uint32_t count,
                                  const float default_clear[4]);
int gfx_backend_reflected_execute_ex(GfxReflectedPipeline *pipeline, GfxOffscreenTarget *target,
                                     const GfxCmd *cmds, uint32_t count,
                                     const float default_clear[4],
                                     const GfxVertexStream *streams, uint32_t nstreams);

/* Bind a resource by its NVN stage/slot rather than by the SPIR-V binding.
 * The reflected pipeline maps the slot through the .res.json metadata. */
int gfx_backend_reflected_bind_ubo_slot(GfxReflectedPipeline *pipeline, uint32_t stage,
                                        uint32_t slot, const void *data, size_t size);
int gfx_backend_reflected_bind_storage_slot(GfxReflectedPipeline *pipeline, uint32_t stage,
                                            uint32_t slot, const void *data, size_t size);
int gfx_backend_reflected_bind_texture_slot(GfxReflectedPipeline *pipeline, uint32_t stage,
                                            uint32_t slot, const GfxTexture *texture);
/* Replace the dummy resource of one reflected binding at runtime.  The binding
 * kind and count come from the pipeline's SPIR-V signature. */
int gfx_backend_reflected_bind_texture(GfxReflectedPipeline *pipeline, uint32_t set,
                                       uint32_t binding, const GfxTexture *texture);
int gfx_backend_reflected_write_uniform(GfxReflectedPipeline *pipeline, uint32_t set,
                                        uint32_t binding, uint32_t offset,
                                        const void *data, size_t size);
int gfx_backend_reflected_write_storage(GfxReflectedPipeline *pipeline, uint32_t set,
                                        uint32_t binding, uint32_t offset,
                                        const void *data, size_t size);
/* Built-in reflected texture test: repository shaders + a real VkTexture
 * bound after pipeline creation.  No game data. */
int gfx_backend_reflected_texture_selftest(void);
/* New pipeline from the repository's triangle_tex shaders; the caller owns it. */
GfxReflectedPipeline *gfx_backend_reflected_builtin_texture_pipeline_create(void);
/* Built-in reflected UBO test: writes a colour into a reflected UBO after
 * pipeline creation, then draws it.  No game data. */
int gfx_backend_reflected_ubo_selftest(void);

/* Minimal sampled-texture path: RGBA8 texture -> VkImage/VkSampler and a
 * textured triangle pipeline.  This is the 2B.3 NVN texture bridge's Vulkan
 * half; the NVN object wraps it. */
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
