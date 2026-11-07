/* Internal module API. */
#ifndef TOTK_ANDROID_GFX_API_0_H
#define TOTK_ANDROID_GFX_API_0_H
#include "gfx_state.h"

/* Defined below; forward declaration so the present/triangle pipelines use the
 * same large-stack compile path. */
VkResult vk_create_graphics_pipelines_bigstack(
        VkPipelineCache cache, const VkGraphicsPipelineCreateInfo *ci, VkPipeline *out);

/* ------------------------------------------------------------------ */
/* First-frame backend selftests (game-data-free). */

int gfx_backend_texture_formats_selftest(void);

int gfx_backend_mrt_selftest(void);

int gfx_backend_bc4_mips_selftest(void);

void vk_present_cache_destroy_locked(void);

int vk_choose_surface_format(VkSurfaceKHR surface, VkFormat *out_format, VkColorSpaceKHR *out_space);

int vk_create_android_surface_locked(void *window);

int vk_create_swapchain_locked(void);

int vk_recreate_swapchain_locked(void);

int gfx_backend_surface_ready(void);

int vk_present_format_is_shader_src(VkFormat f);

VkFilter vk_present_filter(VkFormat src);

int vk_present_blit_ok(VkFormat src, VkFormat dst, VkFilter *out_filter);

VkRect2D vk_present_rect(const GfxOffscreenTarget *t, VkExtent2D extent, uint32_t scale_mode);

GfxPresentPipe *vk_present_pipe_get_locked(VkFormat src, VkFormat dst, VkImageLayout final_layout);

int vk_present_target_locked(VkCommandBuffer cmd, GfxOffscreenTarget *t, VkImage dst, VkImageView dst_view, VkImageLayout dst_old, VkFormat dst_format, VkExtent2D extent, uint32_t scale_mode, VkImageLayout dst_final, VkFramebuffer *out_fb);

int vk_surface_acquire_locked(uint32_t *out_idx, int *out_suboptimal);

int gfx_backend_surface_present_target(const GfxOffscreenTarget *target, uint32_t scale_mode);

int gfx_backend_surface_present(float r, float g_, float b, float a);

void vk_log(const char *fmt, ...);

int vk_ok(VkResult r, const char *what);

int vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, uint32_t *out);

void vk_destroy_swapchain_images_locked(void);

void vk_destroy_swapchain_locked(void);

void vk_reset_locked(void);

int vk_init_locked(void);

int gfx_backend_vulkan_init(void);

int gfx_backend_vulkan_available(void);

void gfx_backend_vulkan_shutdown(void);

const char *gfx_backend_name(void);

int gfx_backend_negative_viewport_supported(void);

void vk_log_bad_color_attachment(const char *what, uint32_t attachment, VkFormat fmt, const GfxRenderTargetLayout *layout, uint32_t width, uint32_t height, unsigned *counter);

int vk_submit_and_wait(VkCommandBuffer cmd, const char *what);

GfxFence *gfx_backend_fence_create(void);

void gfx_backend_fence_signal(GfxFence *fence);

int gfx_backend_fence_wait(GfxFence *fence, uint64_t timeout_ns);

void gfx_backend_fence_destroy(GfxFence *fence);

int gfx_backend_queue_idle(void);

double vk_now_ms(void);

VkFormatInfo vk_format_info(VkFormat f);

int gfx_backend_vk_format_for_nvn(uint32_t f);

int gfx_backend_vk_format_for_texture_format(uint32_t f);

int gfx_backend_vk_format_bpp(int vk_format);

const char *vk_format_name(VkFormat f);

int vk_format_is_color_target(VkFormat f);

int vk_format_is_depth(VkFormat f);

int vk_format_is_compressed(VkFormat f);

int vk_format_block(VkFormat f, uint32_t *bw, uint32_t *bh, uint32_t *bytes);

void vk_rt_dim_hint_record(int format, uint32_t width, uint32_t height);

int vk_rt_dim_hint_lookup(int format, uint32_t *width, uint32_t *height);

VkImageAspectFlags vk_format_copy_aspect(VkFormat f);

VkImageAspectFlags vk_format_view_aspect(VkFormat f);

VkSampleCountFlagBits vk_sample_count(uint32_t samples);

int vk_format_can_texture(VkFormat f, VkFormatFeatureFlags *out_features);

int vk_format_can_blit(VkFormat f, int dst);

int vk_image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout, VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage);

int vk_image_barrier_range(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout, VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage, VkImageAspectFlags aspect, uint32_t base_level, uint32_t level_count, uint32_t base_layer, uint32_t layer_count);

void vk_mip_extent(const GfxTextureDesc *d, uint32_t level, uint32_t *w, uint32_t *h, uint32_t *depth);

size_t vk_texture_slice_size(const GfxTextureDesc *d, uint32_t level);

GfxTexture *texture_create_desc_locked(const GfxTextureDesc *desc, const GfxTextureSubresource *subs, uint32_t count);

GfxTexture *texture_create_rgba8_locked(uint32_t width, uint32_t height, const uint8_t *pixels);

GfxTexture *gfx_backend_texture_create_rgba8(uint32_t width, uint32_t height, const uint8_t *pixels);

GfxTexture *gfx_backend_texture_create_desc(const GfxTextureDesc *desc, const GfxTextureSubresource *subresources, uint32_t count);

int texture_read_pixels_locked(const GfxTexture *t, uint32_t level, uint32_t layer, void *dst, size_t size);

int gfx_backend_texture_read_pixels(const GfxTexture *texture, uint32_t level, uint32_t layer, void *dst, size_t size);

void gfx_backend_texture_destroy(GfxTexture *t);

GfxTexturedPipeline *gfx_backend_textured_pipeline_create(int vk_format);

void gfx_backend_textured_pipeline_destroy(GfxTexturedPipeline *p);

int gfx_backend_textured_draw(GfxTexturedPipeline *p, GfxTexture *tex, GfxOffscreenTarget *t, float r, float g_, float b, float a);

void vk_texture_level_extent(const GfxTexture *t, uint32_t level, uint32_t *w, uint32_t *h, uint32_t *d);

int vk_texture_transfer_check(const GfxTexture *t, uint32_t level, uint32_t layer, const char *what, int need_dst);

int vk_texture_region_check_blocks(const GfxTexture *t, int32_t x, int32_t y, int32_t z, uint32_t w, uint32_t h, const char *what);

int vk_texture_packed_geometry(VkFormat fmt, uint32_t w, uint32_t h, size_t *row_bytes, uint32_t *rows, uint32_t *bpp, uint32_t *bw, uint32_t *bh);

int gfx_backend_texture_copy(GfxTexture *dst, uint32_t dst_level, uint32_t dst_layer, int32_t dst_x, int32_t dst_y, int32_t dst_z, const GfxTexture *src, uint32_t src_level, uint32_t src_layer, int32_t src_x, int32_t src_y, int32_t src_z, uint32_t width, uint32_t height, uint32_t depth);

int gfx_backend_texture_clear(GfxTexture *dst, uint32_t level, uint32_t layer, const float rgba[4], int32_t x, int32_t y, uint32_t width, uint32_t height);

int gfx_backend_texture_upload(GfxTexture *dst, uint32_t level, uint32_t layer, int32_t x, int32_t y, uint32_t width, uint32_t height, const void *data, size_t size);

int gfx_backend_texture_download(const GfxTexture *src, uint32_t level, uint32_t layer, int32_t x, int32_t y, uint32_t width, uint32_t height, void *dst, size_t size);

int vk_create_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf, VkDeviceMemory *mem);

int vk_create_device_image(VkImageType type, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t mips, uint32_t layers, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImage *image, VkDeviceMemory *mem);

int gfx_backend_attach_android_surface(void *window, uint32_t width, uint32_t height, uint32_t nvn_format);

void gfx_backend_detach_android_surface(void);

int vk_make_buffer_usage(uint32_t size, VkBufferUsageFlags usage, VkBuffer *buf, VkDeviceMemory *mem);

int vk_make_buffer(uint32_t size, VkBuffer *buf, VkDeviceMemory *mem);

int vk_make_buffer_view(uint32_t size, int storage, VkFormat format, VkBuffer *buf, VkDeviceMemory *mem, VkBufferView *view);

int vk_make_storage_image(uint32_t w, uint32_t h, VkFormat format, VkImage *img, VkDeviceMemory *mem, VkImageView *view);

int gfx_backend_validate_spirv(const uint8_t *code, size_t size, const char *name);

int vk_clamp_axis(int32_t *pos, uint32_t *len, uint32_t extent, uint32_t *cut);

int vk_clamp_region(int32_t *x, int32_t *y, int32_t *z, uint32_t *w, uint32_t *h, uint32_t *d, uint32_t ew, uint32_t eh, uint32_t ed, uint32_t *cx, uint32_t *cy, uint32_t *cz);

uint8_t vk_unorm8(float v);

int vk_pack_clear_texel(VkFormat fmt, const float rgba[4], uint8_t *out, uint32_t *bpp);

VkResult vk_create_compute_pipeline_bigstack( VkPipelineCache cache, const VkComputePipelineCreateInfo *ci, VkPipeline *out);

float vk_decode_ufloat11(uint32_t v);

float vk_decode_ufloat10(uint32_t v);

void vk_decode_b10g11r11(uint32_t v, float out[3]);

GfxOffscreenTarget *vk_offscreen_create_layout_locked(uint32_t width, uint32_t height, const GfxRenderTargetLayout *layout);

GfxOffscreenTarget *gfx_backend_offscreen_create_layout(uint32_t width, uint32_t height, const GfxRenderTargetLayout *layout);

GfxOffscreenTarget *gfx_backend_offscreen_create(uint32_t width, uint32_t height, uint32_t nvn_format);

void gfx_backend_offscreen_destroy(GfxOffscreenTarget *t);

int gfx_backend_offscreen_clear(GfxOffscreenTarget *t, float r, float g_, float b, float a);

int gfx_backend_offscreen_read_pixel_index(const GfxOffscreenTarget *t, uint32_t index, uint32_t x, uint32_t y, uint8_t out[4]);

int gfx_backend_offscreen_read_pixel(GfxOffscreenTarget *t, uint32_t x, uint32_t y, uint8_t out[4]);

uint32_t gfx_backend_offscreen_color_count(const GfxOffscreenTarget *t);

const GfxRenderTargetLayout *gfx_backend_offscreen_layout(const GfxOffscreenTarget *t);

int gfx_backend_offscreen_present(GfxOffscreenTarget *t);

uint32_t gfx_backend_offscreen_width(const GfxOffscreenTarget *t);

uint32_t gfx_backend_offscreen_height(const GfxOffscreenTarget *t);

const uint8_t *gfx_backend_offscreen_pixels(const GfxOffscreenTarget *t, size_t *size);

const char *gfx_backend_offscreen_format_name(const GfxOffscreenTarget *t);

int gfx_backend_offscreen_write_ppm_index(const GfxOffscreenTarget *t, uint32_t index, const char *path);

int gfx_backend_offscreen_write_ppm(const GfxOffscreenTarget *t, const char *path);

uint64_t gfx_backend_offscreen_hash(const GfxOffscreenTarget *t);

void *vk_pipe_compile_thread(void *arg);

VkResult vk_create_graphics_pipelines_bigstack( VkPipelineCache cache, const VkGraphicsPipelineCreateInfo *ci, VkPipeline *out);

int vk_shader_module(const uint8_t *code, size_t size, const char *what, VkShaderModule *out);

GfxTrianglePipeline *triangle_create_impl(const uint8_t *vs, size_t vs_size, const uint8_t *ps, size_t ps_size, int vk_format, const GfxUboBinding *ubos, uint32_t nubo);

GfxTrianglePipeline *gfx_backend_triangle_pipeline_create(const uint8_t *vs, size_t vs_size, const uint8_t *ps, size_t ps_size, int vk_format);

GfxTrianglePipeline *gfx_backend_triangle_pipeline_create_ubo(const uint8_t *vs, size_t vs_size, const uint8_t *ps, size_t ps_size, int vk_format, const GfxUboBinding *ubos, uint32_t nubo);

int gfx_backend_triangle_write_ubo(GfxTrianglePipeline *p, uint32_t binding, uint32_t offset, const void *data, size_t size);

void gfx_backend_triangle_pipeline_destroy(GfxTrianglePipeline *p);

int gfx_backend_triangle_draw(GfxTrianglePipeline *p, GfxOffscreenTarget *t, float r, float g_, float b, float a);

GfxTrianglePipeline *gfx_backend_totk_vs_pipeline_create(const uint8_t *vs, size_t vs_size, int vk_format);

GfxTrianglePipeline *gfx_backend_builtin_triangle_pipeline(void);

void gfx_backend_builtin_triangle_modules(const uint8_t **vs, size_t *vs_size, const uint8_t **ps, size_t *ps_size);

void gfx_backend_builtin_texture_modules(const uint8_t **vs, size_t *vs_size, const uint8_t **ps, size_t *ps_size);

void gfx_backend_builtin_ubo_modules(const uint8_t **vs, size_t *vs_size, const uint8_t **ps, size_t *ps_size);

void gfx_backend_builtin_attr_modules(const uint8_t **vs, size_t *vs_size, const uint8_t **ps, size_t *ps_size);

void gfx_backend_builtin_depth_modules(const uint8_t **vs, size_t *vs_size, const uint8_t **ps, size_t *ps_size);

GfxTexturedPipeline *gfx_backend_builtin_textured_pipeline(void);

GfxTexturedPipeline *textured_pipeline_create_impl(const uint8_t *vs, size_t vs_size, const uint8_t *ps, size_t ps_size, int vk_format);

GfxTexturedPipeline *textured_pipeline_create_bc4(int vk_format);

int vk_make_sampler(VkSampler *sampler);

VkFilter vk_nvn_filter(uint32_t f);

VkSamplerMipmapMode vk_nvn_mipmap(uint32_t f);

VkSamplerAddressMode vk_nvn_wrap(uint32_t w);

VkCompareOp vk_nvn_compare(uint32_t f);

GfxSampler *gfx_backend_sampler_create(const GfxSamplerDesc *d);

void gfx_backend_sampler_destroy(GfxSampler *s);

VkBlendFactor vk_nvn_blend_factor(uint32_t f);

VkBlendOp vk_nvn_blend_op(uint32_t e);

VkCompareOp vk_nvn_depth_op(uint32_t f);

VkStencilOp vk_nvn_stencil_op(uint32_t o);

VkFormat vk_reflect_texel_format(const GfxBindingSpec *spec, int storage);

void vk_spirv_move_set(uint8_t *code, size_t size, uint32_t from, uint32_t to);

VkDescriptorType reflect_vk_type(GfxBindingKind kind);

uint64_t reflect_state_sig(const GfxPipelineState *st);

int reflect_create_pipeline(GfxReflectedPipeline *p, const GfxPipelineState *st, VkPipeline *out);

VkPipeline reflect_pipeline_for_state(GfxReflectedPipeline *p, const GfxPipelineState *st);

GfxReflectedPipeline *gfx_backend_reflected_pipeline_create(const uint8_t *vs, size_t vs_size, const uint8_t *ps, size_t ps_size, int vk_format, const GfxBindingSpec *vs_specs, uint32_t nvs, const GfxBindingSpec *ps_specs, uint32_t nps);

GfxReflectedPipeline *reflect_pipeline_create_impl( const uint8_t *vs, size_t vs_size, const uint8_t *ps, size_t ps_size, const GfxRenderTargetLayout *layout, const GfxBindingSpec *vs_specs, uint32_t nvs, const GfxBindingSpec *ps_specs, uint32_t nps, const GfxVertexLayout *input);

GfxReflectedPipeline *gfx_backend_reflected_pipeline_create_ex( const uint8_t *vs, size_t vs_size, const uint8_t *ps, size_t ps_size, int vk_format, const GfxBindingSpec *vs_specs, uint32_t nvs, const GfxBindingSpec *ps_specs, uint32_t nps, const GfxVertexLayout *input);

GfxReflectedPipeline *gfx_backend_reflected_pipeline_create_layout( const uint8_t *vs, size_t vs_size, const uint8_t *ps, size_t ps_size, const GfxRenderTargetLayout *layout, const GfxBindingSpec *vs_specs, uint32_t nvs, const GfxBindingSpec *ps_specs, uint32_t nps, const GfxVertexLayout *input);

void gfx_backend_reflected_pipeline_destroy(GfxReflectedPipeline *p);

int reflect_find_spec(GfxReflectedPipeline *p, uint32_t set, uint32_t binding);

int gfx_backend_reflected_bind_texture(GfxReflectedPipeline *p, uint32_t set, uint32_t binding, const GfxTexture *texture);

int reflect_write_buffer_locked(GfxReflectedPipeline *p, uint32_t set, uint32_t binding, uint32_t offset, const void *data, size_t size, int storage);

int reflect_write_buffer(GfxReflectedPipeline *p, uint32_t set, uint32_t binding, uint32_t offset, const void *data, size_t size, int storage);

int gfx_backend_reflected_write_uniform(GfxReflectedPipeline *p, uint32_t set, uint32_t binding, uint32_t offset, const void *data, size_t size);

int gfx_backend_reflected_write_storage(GfxReflectedPipeline *p, uint32_t set, uint32_t binding, uint32_t offset, const void *data, size_t size);

int reflect_set_for_stage(uint32_t stage);

int reflect_find_slot_locked(GfxReflectedPipeline *p, int set, uint32_t slot, int want_texture);

int reflect_write_slot_locked(GfxReflectedPipeline *p, int set, int i, const void *data, size_t size, int storage);

int gfx_backend_reflected_bind_ubo_slot(GfxReflectedPipeline *p, uint32_t stage, uint32_t slot, const void *data, size_t size);

int gfx_backend_reflected_bind_storage_slot(GfxReflectedPipeline *p, uint32_t stage, uint32_t slot, const void *data, size_t size);

int gfx_backend_reflected_bind_texture_slot(GfxReflectedPipeline *p, uint32_t stage, uint32_t slot, const GfxTexture *texture);

GfxReflectedPipeline *gfx_backend_reflected_builtin_texture_pipeline_create(void);

VkPrimitiveTopology vk_reflect_topology(uint32_t primitive);

int vk_reflect_ensure_buffer(VkBuffer *buf, VkDeviceMemory *mem, size_t *cap, size_t need, VkBufferUsageFlags usage);

int vk_reflect_upload(VkDeviceMemory mem, size_t offset, const void *data, size_t size);

VkIndexType vk_reflect_index_type(uint32_t type);

size_t reflect_align(size_t v);

int reflect_find_bind_spec(GfxReflectedPipeline *p, uint32_t stage, uint32_t slot, uint32_t kind);

int reflect_arena_ensure(GfxReflectedPipeline *p, int set, int idx, size_t need);

void reflect_write_epoch_set(GfxReflectedPipeline *p, uint32_t s, VkDescriptorSet dset, const ReflectEpochState *st);

void reflect_apply_draw_state(VkCommandBuffer cmd, GfxReflectedPipeline *p, const GfxPipelineState *st);

void reflect_bind_epoch_sets(VkCommandBuffer cmd, GfxReflectedPipeline *p, const VkDescriptorSet sets[2], const ReflectEpochState *st);

int gfx_backend_reflected_execute(GfxReflectedPipeline *p, GfxOffscreenTarget *t, const GfxCmd *cmds, uint32_t count, const float default_clear[4]);

int reflect_layout_matches_target(const GfxRenderTargetLayout *l, const GfxOffscreenTarget *t);

int gfx_backend_reflected_execute_ex(GfxReflectedPipeline *p, GfxOffscreenTarget *t, const GfxCmd *cmds, uint32_t count, const float default_clear[4], const GfxVertexStream *streams, uint32_t nstreams);

int gfx_backend_reflected_draw(GfxReflectedPipeline *p, GfxOffscreenTarget *t, float r, float g_, float b, float a);

int gfx_backend_reflected_texture_selftest(void);

int gfx_backend_reflected_ubo_selftest(void);

int vk_selftest_read_match(GfxTexture *t, uint32_t level, uint32_t layer, const uint8_t *expect, size_t n, const char *what);

int gfx_backend_texture_formats_selftest(void);

int gfx_backend_mrt_selftest(void);

int gfx_backend_surface_target_selftest(void);

int gfx_backend_bc4_mips_selftest(void);

int gfx_backend_selftest(void);

int bc4_sample_selftest_size(uint32_t size);

int gfx_backend_bc4_sample_selftest(void);

int vk_present_probe_locked(GfxOffscreenTarget *t, uint32_t scale_mode, uint8_t **out_pixels);

void probe_pixel(const uint8_t *pixels, uint32_t x, uint32_t y, uint8_t out[4]);

uint32_t vk_compute_probe_value(uint32_t x, uint32_t y, const uint8_t *tex, uint32_t seed);

void vk_compute_probe_fill_texture(uint8_t *pixels, uint32_t w, uint32_t h);

int gfx_backend_compute_probe(const char *work_dir);

void vk_copy_probe_fill(uint8_t *p, uint32_t w, uint32_t h, uint32_t seed);

void bc4_probe_constant_block(uint8_t *b, uint8_t v);

void bc4_probe_half_block(uint8_t *b, uint8_t lo, uint8_t hi);

int bc4_probe_expect(GfxOffscreenTarget *t, uint32_t x, uint32_t y, uint8_t r, const char *what);

int gfx_backend_copy_probe(const char *work_dir);

int vk_compute_desc_type(GfxBindingKind kind, VkDescriptorType *out);

uint32_t vk_compute_binding_count(const GfxBindingSpec *s);

VkDeviceSize vk_compute_align_up(VkDeviceSize v, VkDeviceSize a);

int vk_compute_buffer_create(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf, VkDeviceMemory *mem, void **mapped);

void vk_compute_binding_release(GfxComputeBinding *b);

void *vk_compute_compile_thread(void *arg);

int vk_compute_find_spec(const GfxComputePipeline *p, uint32_t set, uint32_t binding);

void vk_compute_pipeline_destroy_locked(GfxComputePipeline *p);

GfxComputePipeline *gfx_backend_compute_pipeline_create(const uint8_t *cs, size_t cs_size, const GfxBindingSpec *specs, uint32_t nspec);

void gfx_backend_compute_pipeline_destroy(GfxComputePipeline *pipeline);

int gfx_backend_compute_bind_buffer(GfxComputePipeline *pipeline, uint32_t set, uint32_t binding, const void *data, size_t size);

int gfx_backend_compute_bind_texture(GfxComputePipeline *pipeline, uint32_t set, uint32_t binding, const GfxTexture *texture);

int gfx_backend_compute_write_buffer(GfxComputePipeline *pipeline, uint32_t set, uint32_t binding, uint32_t offset, const void *data, size_t size);

int gfx_backend_compute_read_buffer(GfxComputePipeline *pipeline, uint32_t set, uint32_t binding, void *dst, size_t size);

int gfx_backend_compute_dispatch(GfxComputePipeline *pipeline, uint32_t group_x, uint32_t group_y, uint32_t group_z);

#endif
