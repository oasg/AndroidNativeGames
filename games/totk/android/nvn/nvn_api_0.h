/* Internal NVN module API. */
#ifndef TOTK_ANDROID_NVN_API_0_H
#define TOTK_ANDROID_NVN_API_0_H
#include "nvn_state.h"

extern int runtime_native_window_present(void *p);

extern void runtime_native_window_set_present_target(void *p, void *target);

extern void runtime_native_window_destroy(void *p);

extern void *runtime_native_window_create(uint32_t width, uint32_t height,
                                          uint32_t format, uint64_t layer_id);

extern int runtime_native_window_info(const void *p, uint32_t *width, uint32_t *height,
                                      uint32_t *format, void **buffer, size_t *size);

extern int runtime_native_window_valid(const void *p);

extern int runtime_native_window_draw_triangle(void *p);

extern int runtime_native_window_draw_triangle_pipeline(void *p, void *pipeline);

extern int runtime_native_window_draw_textured_triangle(void *p, void *pipeline, void *texture);

extern int runtime_native_window_draw_reflected_pipeline(void *p, void *pipeline);

extern int runtime_native_window_execute_reflected(void *p, void *pipeline,
                                                   const GfxCmd *cmds, uint32_t count,
                                                   const float default_clear[4]);

extern int runtime_native_window_execute_reflected_ex(void *p, void *pipeline,
                                                      const GfxCmd *cmds, uint32_t count,
                                                      const float default_clear[4],
                                                      const GfxVertexStream *streams,
                                                      uint32_t nstreams);

extern int runtime_native_window_clear_rgba(void *p, const float rgba[4]);

extern int runtime_native_window_read_pixel(void *p, uint32_t x, uint32_t y, uint8_t out[4]);

extern int runtime_native_window_screenshot(void *p, const char *path);

extern GfxOffscreenTarget *runtime_native_window_present_target(const void *p);

void nvn_process_submit_commands(const uint64_t *cmds, int n);

void nvn_warm_recording(void *cb);

int nvn_gpu_drain(const char *what);

void nvn_host_rt_clear(void);

void nvn_scene_submit_pending(void);

int nvn_submit_scene_snapshot(uint64_t handle, uint32_t seg_index);

int nvn_is_empty_target(const void *tex);

int nvn_trace_inputs(void);

int nvn_trace_composite(void);

int nvn_trace_save_ui(void);

uint32_t nvn_first_nonzero(const uint8_t *p, size_t n, size_t cap);

int nvn_guest_range_ok(uint64_t addr, size_t size);

void nvn_pool_forget(const void *pool);

int nvn_ui_seg_want(void);

void nvn_cbacct_log(const char *why);

void nvn_pipe_ubo_set(GfxReflectedPipeline *rp,
                             const GfxBindingSpec *vss, uint32_t nvs,
                             const GfxBindingSpec *pss, uint32_t nps);

void nvn_cmd_payload_free(nvn_cmd_t *cmd);

int nvn_vb_snap_reserve(size_t n);

void nvn_vb_snap_release(size_t n);

uint32_t nvn_cb_index_vertex_count(const nvn_cmd_t *cmd);

void nvn_cb_snapshot_vertex_buffers(nvn_cmd_t *cmd, const nvn_bind_state_t *b,
                                           uint32_t need_verts);

void nvn_cb_snapshot_free(nvn_cmd_t *cmd);

/* Recorded GPU transfer/compute entry points (defined with the proc-address
 * shims at the end of the file) and the program-state refcount helper. */
void nvn_trace_dispatch_compute(void *c, int x, int y, int z);

void nvn_trace_clear_texture(void *c, void *t, void *v, void *r, void *color, int mask);

void nvn_trace_copy_b2t(void *c, void *buf, void *t, void *v, void *r, int flags);

void nvn_trace_copy_t2t(void *c, const void *src, const void *sv, const void *sr,
                               const void *dst, const void *dv, const void *dr, int flags);

void nvn_trace_copy_t2b(void *c, void *t, void *v, void *r, void *buf, int flags);

void nvn_prog_ref(prog_shader_state_t *s);

/* Diagnostic crash context for the retired-pass investigation.  Enabled only
 * by TOTK_ANDROID_HOST_CRASH=1; the loader owns the production observer. */
void runtime_guest_frame_report(uint64_t fp, uint64_t lr);

int nvn_trace(void);

extern int runtime_thread_probe(uint32_t handle, int *wkind, uint64_t *waddr,
                                uint32_t wh[4], int *nwh, int *last_svc,
                                uint64_t *last_lr, uint64_t *last_fp,
                                uint64_t *nsvc, int *exited);

void nvn_mask_trace(const tex_t *t, const char *what);

uint32_t nvnSyncInitialize(sync_t *s, const nvn_dev_t *d);

void nvnSyncFinalize(sync_t *s);

void nvn_frame_submit_trace(int n, const uint64_t *cmds);

void nvn_frame_ubo_trace(int n, const uint64_t *cmds);

void nvn_frame_report_orphans(void);

int nvnQueueFenceSync(queue_t *q, sync_t *s, int condition, int flags);

int nvnSyncWait(const sync_t *s, uint64_t timeout);

/* Host-side round-trip check used by probe-vi: the native-window pointer the
 * VI layer hands out must survive builder -> window -> getter unchanged. */
extern int runtime_native_window_clear(void *p, uint32_t rgba);

/* Guest storage -> linear -> Vulkan for every mip/layer of the shapes the
 * first frame uses.  Guest storage stays the source of truth: the upload is
 * lazy (first bind) and unsupported target/format combinations are explicit
 * failures, never a silent RGBA8 substitute. */
extern int runtime_guest_texture_deswizzle(const void *storage, void *linear, size_t pitch,
    int target, int format, int flags, int samples, uint32_t width, uint32_t height,
    uint32_t depth, uint32_t levels, uint32_t stride, int pkg_block_h);

extern int runtime_guest_texture_swizzle(void *storage, const void *linear, size_t pitch,
    int target, int format, int flags, int samples, uint32_t width, uint32_t height,
    uint32_t depth, uint32_t levels, uint32_t stride, int pkg_block_h);

extern int runtime_guest_texture_deswizzle_at(const void *storage, void *linear, size_t pitch,
    int target, int format, int flags, int samples, uint32_t width, uint32_t height,
    uint32_t depth, uint32_t levels, uint32_t stride, int pkg_block_h,
    unsigned level, unsigned layer, unsigned z, unsigned z_count);

extern int runtime_guest_texture_level_geom(int target, int format, int flags, int samples,
    uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t stride,
    int pkg_block_h, unsigned level, uint32_t *row_bytes, uint32_t *rows,
    uint32_t *slices, uint32_t *bpb);

void nvn_texture_mark_dirty(const tex_t *t);

void nvn_cb_trace_record_vb(cb_t *c, const char *what, uint32_t count);

int nvn_ui_seg_want(void);

void nvn_warm_scene(cb_t *c);

void nvn_seg_prepare(nvn_seg_t *segs, uint32_t nseg, cb_t *c);

void prog_state_free_locked(prog_shader_state_t *s);

void nvn_prog_release(prog_shader_state_t *s);

int nvn_bs_samples_ui(const nvn_bind_state_t *b, cb_t *view,
                             char *out, size_t cap);

GfxReflectedPipeline *nvn_program_cached_pipeline(prog_t *p, uint32_t nvn_format,
                                                         const GfxRenderTargetLayout *layout,
                                                         int legacy_window_target,
                                                         const GfxVertexLayout *input,
                                                         uint64_t vertex_sig,
                                                         const char *variant,
                                                         int allow_create);

/* probe-nvn-retired-stress: repeatedly retire and submit multi-target CBs
 * with target/alias churn.  Exercises the retired snapshot, host-RT cache,
 * texture bridge, queue drain and pipeline reuse without the game. */
int nvn_seg_probe_tex(tex_t *t, uint32_t w, uint32_t h);

/* probe-nvn-sparse-targets: an NVN MRT array may leave inactive slots null
 * while later slots stay active.  The resolver must preserve the active slot
 * indices and substitute dummy attachments for the gaps. */
int nvn_seg_probe_tex(tex_t *t, uint32_t w, uint32_t h);

void nvn_render_state_defaults(nvn_render_state_t *r);

nvn_fmt_t nvn_fmt_of(int f);

uint32_t nvn_mip(uint32_t s, unsigned l);

uint32_t nvn_blk(uint32_t s, uint32_t b);

unsigned nvn_pick_tile(uint32_t gobs);

unsigned nvn_adj_tile(unsigned shift, uint32_t unit, uint32_t dim);

void nvn_shape(const tex_desc *d, unsigned *dpl, int *layered, uint32_t *w, uint32_t *h, uint32_t *z);

void nvn_samples_xy(int s, uint32_t *sx, uint32_t *sy);

int nvn_tex_linear(const tex_desc *d);

uint32_t nvn_linear_stride(const tex_desc *d);

uint64_t nvn_level_offset(const tex_desc *d, unsigned level, unsigned dpl, uint32_t w, uint32_t h, uint32_t z, unsigned tileH, unsigned tileD, nvn_fmt_t t);

uint64_t nvn_layer_bytes(const tex_desc *d, unsigned dpl, int layered, uint32_t w, uint32_t h, uint32_t z, unsigned tileH, unsigned tileD, nvn_fmt_t t);

void nvn_host_tex_layout(const tex_desc *d, tex_layout *o);

uint64_t nvn_host_tex_level_offset(const tex_desc *d, unsigned level);

void nvn_global_ubo_set(int stage, int index, uint64_t address, size_t size);

int nvn_global_ubo_get(int stage, int index, uint64_t *address, size_t *size);

void nvn_global_ssbo_set(int stage, int index, uint64_t address, size_t size);

int nvn_global_ssbo_get(int stage, int index, uint64_t *address, size_t *size);

void nvn_prog_ubo_set(const prog_t *p, int stage, int index, uint64_t address, size_t size);

int nvn_prog_ubo_get(const prog_t *p, int stage, int index, uint64_t *address, size_t *size);

void nvn_prog_ubo_forget(const prog_t *p);

GfxReflectedPipeline *prog_pipe_find(prog_shader_state_t *s, int fmt, uint64_t layout_sig, uint64_t vertex_sig, uint64_t variant_sig);

void prog_pipe_add(prog_shader_state_t *s, GfxReflectedPipeline *pipe, int fmt, uint64_t layout_sig, uint64_t vertex_sig, uint64_t variant_sig);

void prog_pipe_clear(prog_shader_state_t *s);

void prog_stage_clear(prog_shader_state_t *s, unsigned stage);

void prog_state_clear_locked(prog_shader_state_t *s);

unsigned prog_state_hash(const prog_t *p);

prog_shader_state_t *prog_state_find(prog_t *p);

prog_shader_state_t *prog_state_install(prog_t *p, prog_shader_state_t *s);

prog_shader_state_t *prog_state_remove(prog_t *p);

prog_shader_state_t *prog_state_ensure(prog_t *p);

prog_host_t prog_host_view(prog_t *p);

void prog_host_set(prog_t *p, void *gfx_pipeline, void *gfx_textured_pipeline, void *gfx_texture, GfxReflectedPipeline *reflected, uint32_t textured);

uint32_t nvnProgramInitialize(prog_t *p, const nvn_dev_t *d);

void prog_state_free_locked(prog_shader_state_t *s);

void nvn_prog_release(prog_shader_state_t *s);

void nvn_prog_ref(prog_shader_state_t *s);

void nvnProgramFinalize(prog_t *p);

void nvnProgramSetDebugLabel(prog_t *p, const char *l);

uint32_t nvn_program_set_shaders(prog_t *p, int n, const void *shaders, size_t stride, const char *what);

uint32_t nvnProgramSetShaders(prog_t *p, int n, const void *shaders);

uint32_t nvnProgramSetShadersExt(prog_t *p, int n, const void *shaders);

int nvn_read_file(const char *path, uint8_t **out, size_t *size);

int runtime_nvn_program_load_spirv(void *prog, const char *vs_path, const char *ps_path);

int runtime_nvn_program_load_spirv_reflect(void *prog, const char *vs_path, const char *ps_path, const char *vs_res, const char *ps_res);

int runtime_nvn_program_bind_texture(void *prog, uint32_t set, uint32_t binding, const void *texture);

int runtime_nvn_program_write_uniform(void *prog, uint32_t set, uint32_t binding, uint32_t offset, const void *data, size_t size);

int runtime_nvn_program_write_storage(void *prog, uint32_t set, uint32_t binding, uint32_t offset, const void *data, size_t size);

void nvnProgramSetSampleShading(prog_t *p, int v);

int nvnProgramGetSubtileSize(const prog_t *p);

void nvnProgramSetSubroutineLinkage(prog_t *p, int n, const void *l);

GfxReflectedPipeline *nvn_program_cached_pipeline(prog_t *p, uint32_t nvn_format, const GfxRenderTargetLayout *layout, int legacy_window_target, const GfxVertexLayout *input, uint64_t vertex_sig, const char *variant, int allow_create);

void nvn_pipe_ubo_set(GfxReflectedPipeline *rp, const GfxBindingSpec *vss, uint32_t nvs, const GfxBindingSpec *pss, uint32_t nps);

const nvn_pipe_ubo_t *nvn_pipe_ubo_get(GfxReflectedPipeline *rp);

int nvn_crash_read_mem(uintptr_t addr, void *out, size_t n);

uint64_t nvn_crash_u64(uintptr_t addr);

void nvn_crash_x8_dump(uint64_t x8);

void nvn_host_crash_raw(int sig, siginfo_t *si, void *ctx);

void nvn_install_host_crash_late(void);

void nvn_api(const char *what);

const char *runtime_nvn_last_api(void);

const char *runtime_nvn_last_proc(void);

void nvn_fail(const char *what, const char *detail);

int nvn_trace(void);

int nvn_trace_frame(void);

int nvn_trace_first(void);

int nvn_obj(const void *p, uint32_t magic, const char *what);

void nvn_dump_vertex_stream(const char *tag, const GfxVertexLayout *l, const nvn_bind_state_t *bv, int maxv);

void nvn_orphan_add(const cb_t *c, const cb_meta_t *m);

void nvn_cbacct_log(const char *why);

void nvn_cbacct_bump(int which);

int nvn_orphan_seen(uint64_t handle);

void nvn_trace_bad_target(const char *what, uint32_t index, const void *p);

int nvn_trace_ui_segs(void);

__attribute__((unused)) static void nvn_trace_tex(const char *what, const tex_t *t);

void nvn_dump_shader(const RuntimeShaderIdentity *id, const void *control, const void *code);

int nvn_guest_range_ok(uint64_t addr, size_t size);

int nvn_trace_inputs(void);

int nvn_trace_save_ui(void);

int nvn_trace_composite(void);

uint32_t nvn_first_nonzero(const uint8_t *p, size_t n, size_t cap);

void nvn_trace_segment_inputs(const char *tag, cb_t *view, const nvn_bind_state_t *bs, prog_t *p, GfxReflectedPipeline *rp);

void nvn_trace_draw_instanced(void *c, int p, int t, int count, void *idx, int bv, int bi, int inst);

void nvn_trace_draw_indirect(void *c, void *a);

void nvn_trace_draw_arrays_indirect(void *c, void *a);

void nvn_trace_draw_texture(void *c, uint64_t h, const void *src, const void *dst);

void nvn_tex_registry_add(tex_t *t);

void nvn_tex_registry_del(tex_t *t);

const tex_t *nvn_tex_registry_find(const void *p);

void nvn_global_tex_set(int stage, int slot, uint64_t handle, const tex_t *tex);

int nvn_global_tex_get(int stage, int slot, uint64_t *handle, const tex_t **tex);

void nvn_global_img_set(int stage, int slot, uint64_t handle, const tex_t *tex);

int nvn_global_img_get(int stage, int slot, uint64_t *handle, const tex_t **tex);

int nvn_tex_same_resource(const tex_t *a, const tex_t *b);

tex_bridge_t *nvn_tex_bridge(const tex_t *t, int create);

void nvn_tex_set_rt(const tex_t *t, GfxOffscreenTarget *rt, const GfxRenderTargetLayout *layout, uint32_t w, uint32_t h);

void nvn_tex_forget_rt(GfxOffscreenTarget *rt);

void nvnTextureBuilderSetDefaults(tex_builder_t *b);

void nvnTextureBuilderSetDevice(tex_builder_t *b, void *d);

void nvnTextureBuilderSetFlags(tex_builder_t *b, int f);

void nvnTextureBuilderSetTarget(tex_builder_t *b, int t);

void nvnTextureBuilderSetWidth(tex_builder_t *b, int v);

void nvnTextureBuilderSetHeight(tex_builder_t *b, int v);

void nvnTextureBuilderSetDepth(tex_builder_t *b, int v);

void nvnTextureBuilderSetSize1D(tex_builder_t *b, int w);

void nvnTextureBuilderSetSize2D(tex_builder_t *b, int w, int h);

void nvnTextureBuilderSetSize3D(tex_builder_t *b, int w, int h, int d);

void nvnTextureBuilderSetLevels(tex_builder_t *b, int v);

void nvnTextureBuilderSetFormat(tex_builder_t *b, int f);

void nvnTextureBuilderSetSamples(tex_builder_t *b, int s);

void nvnTextureBuilderSetSwizzle(tex_builder_t *b, int r, int g, int bl, int a);

void nvnTextureBuilderSetDepthStencilMode(tex_builder_t *b, int m);

void nvnTextureBuilderSetStorage(tex_builder_t *b, void *p, ptrdiff_t off);

void nvnTextureBuilderSetPackagedTextureData(tex_builder_t *b, const void *data);

void nvnTextureBuilderSetPackagedTextureLayout(tex_builder_t *b, const void *layout);

void nvnTextureBuilderSetStride(tex_builder_t *b, ptrdiff_t s);

void nvnTextureBuilderSetGLTextureName(tex_builder_t *b, uint32_t n);

size_t nvnTextureBuilderGetStorageSize(const tex_builder_t *b);

size_t nvnTextureBuilderGetStorageAlignment(const tex_builder_t *b);

ptrdiff_t nvnTextureBuilderGetViewOffset(const tex_builder_t *b, const uint8_t *v);

const void *nvnTextureBuilderGetPackagedTextureLayout(const tex_builder_t *b);

int nvnTextureBuilderGetRawStorageClass(const tex_builder_t *b);

int nvnTextureBuilderGetStorageClass(const tex_builder_t *b);

int nvnTextureBuilderGetFlags(const tex_builder_t *b);

int nvnTextureBuilderGetTarget(const tex_builder_t *b);

int nvnTextureBuilderGetWidth(const tex_builder_t *b);

int nvnTextureBuilderGetHeight(const tex_builder_t *b);

int nvnTextureBuilderGetDepth(const tex_builder_t *b);

int nvnTextureBuilderGetLevels(const tex_builder_t *b);

int nvnTextureBuilderGetFormat(const tex_builder_t *b);

int nvnTextureBuilderGetSamples(const tex_builder_t *b);

void nvnTextureBuilderGetSwizzle(const tex_builder_t *b, int *r, int *g, int *bl, int *a);

int nvnTextureBuilderGetDepthStencilMode(const tex_builder_t *b);

const void *nvnTextureBuilderGetPackagedTextureData(const tex_builder_t *b);

ptrdiff_t nvnTextureBuilderGetStride(const tex_builder_t *b);

void nvnTextureBuilderGetSparseTileLayout(const tex_builder_t *b, void *out);

uint32_t nvnTextureBuilderGetGLTextureName(const tex_builder_t *b);

size_t nvnTextureBuilderGetZCullStorageSize(const tex_builder_t *b);

void *nvnTextureBuilderGetMemoryPool(const tex_builder_t *b);

ptrdiff_t nvnTextureBuilderGetMemoryOffset(const tex_builder_t *b);

#endif
