/* Internal NVN module API. */
#ifndef TOTK_ANDROID_NVN_API_3_H
#define TOTK_ANDROID_NVN_API_3_H
#include "nvn_state.h"
#include "nvn_api_2.h"

void nvnVertexAttribStateGetFormat(const void *s, int *format, ptrdiff_t *offset);

int nvnVertexAttribStateGetStreamIndex(const void *s);

void nvnVertexStreamStateSetDefaults(void *s);

void nvnVertexStreamStateSetStride(void *s, ptrdiff_t stride);

void nvnVertexStreamStateSetDivisor(void *s, int divisor);

ptrdiff_t nvnVertexStreamStateGetStride(const void *s);

int nvnVertexStreamStateGetDivisor(const void *s);

char nvn_attr_class(uint32_t f);

uint64_t nvn_variant_sig(const char *v);

void nvn_variant_suffix(prog_t *p, const cb_t *cb, char *out, size_t cap);

uint64_t nvn_vertex_signature(const GfxVertexLayout *layout);

nvn_vfmt_t nvn_vfmt_of(int f);

int nvn_float_vk_format(int comps);

void nvn_build_vertex_layout(const nvn_bind_state_t *b, GfxVertexLayout *out, uint64_t *sig);

uint32_t nvn_fix_index_range(GfxCmd *cmds, uint32_t count, const nvn_bind_state_t *b, const char *where);

uint32_t nvn_build_vertex_streams(const nvn_bind_state_t *b, GfxVertexStream *out, uint32_t max, void **owned, uint32_t max_owned, uint32_t *nowned, uint32_t draw_verts);

const uint8_t *nvn_draw_vb_find(const nvn_cmd_t *c, int stream, size_t *size);

unsigned nvn_merge_draw_vertex_snapshots(const nvn_cmd_t *cmds, uint32_t ncmds, nvn_bind_state_t *bs, GfxCmd *translated, uint32_t tcount, uint32_t tw, uint32_t th, uint8_t *out_data[16], size_t out_size[16]);

void nvn_bind_state_merge(nvn_bind_state_t *d, const nvn_bind_state_t *s);

void nvn_remap_texture_bind_cmds(GfxCmd *cmds, uint32_t n, GfxReflectedPipeline *rp);

void nvn_inherit_ubo_slots(nvn_bind_state_t *b, GfxReflectedPipeline *rp);

void nvn_apply_bind_state(prog_t *p, GfxReflectedPipeline *rp, const nvn_bind_state_t *b, cb_t *cb);

void nvn_state_to_gfx(const nvn_render_state_t *rs, GfxPipelineState *o);

int nvn_bs_samples_ui(const nvn_bind_state_t *b, cb_t *view, char *out, size_t cap);

uint32_t nvn_translate_commands(const cb_t *cb, GfxCmd *out, uint32_t max, GfxOffscreenTarget *target, float clear[4], int *has_clear);

void nvn_level_extent(const tex_t *t, uint32_t level, uint32_t *w, uint32_t *h);

void nvn_region_resolve(int32_t x, int32_t y, uint32_t *w, uint32_t *h, uint32_t lw, uint32_t lh);

int nvn_exec_clear_texture(const nvn_cmd_t *cmd);

int nvn_exec_copy_t2t(const nvn_cmd_t *cmd);

int nvn_exec_copy_b2t(const nvn_cmd_t *cmd);

int nvn_exec_copy_t2b(const nvn_cmd_t *cmd);

int nvn_exec_dispatch(const nvn_cmd_t *cmd);

void nvn_exec_gpu_commands(const nvn_cmd_t *cmds, uint32_t ncmds, const char *where);

int nvn_target_first_nz(GfxOffscreenTarget *t, uint32_t *first, uint8_t center[4]);

GfxComputePipeline *nvn_compute_pipeline_get(prog_shader_state_t *s, GfxBindingSpec *out, uint32_t *nout);

int nvn_copy_probe(const char *work_dir);

int nvn_compute_probe(const char *work_dir);

void nvn_trace_dispatch_compute(void *cv, int x, int y, int z);

void nvn_trace_dispatch_indirect(void *c, void *a);

void nvn_trace_clear_texture(void *cv, void *t, void *v, void *r, void *color, int mask);

void nvn_trace_clear_texturei(void *cv, void *t, void *v, void *r, void *color, int mask);

void nvn_trace_clear_textureui(void *cv, void *t, void *v, void *r, void *color, int mask);

void nvn_trace_copy_b2t(void *cv, void *buf, void *t, void *v, void *r, int flags);

void nvn_trace_copy_t2t(void *cv, const void *src, const void *sv, const void *sr, const void *dst, const void *dv, const void *dr, int flags);

void nvn_trace_blit(void *cv, ...);

void nvn_trace_copy_t2b(void *cv, void *t, void *v, void *r, void *buf, int flags);

void nvn_trace_cb_fence(void *c, void *sync, int condition, int flags);

void nvn_trace_cb_wait_sync(void *c, void *sync);

void nvn_trace_cb_signal_event(void *c, void *event, int value);

void nvn_trace_cb_wait_event(void *c, void *event, int value, int flags);

void nvn_trace_barrier(void *c, int bits);

void nvn_trace_render_enable_cond(void *c, int mode, void *addr);

uint32_t nvnSyncInitialize(sync_t *s, const nvn_dev_t *d);

void nvnSyncFinalize(sync_t *s);

void nvnSyncSetDebugLabel(sync_t *s, const char *l);

int nvn_sync_complete(sync_t *s, const char *what);

int nvnSyncWait(const sync_t *s, uint64_t timeout);

uint32_t nvnSyncInitializeFromFencedGLSync(sync_t *s, const nvn_dev_t *d, const void *gl);

void *nvnSyncCreateGLSync(const sync_t *s);

void nvnEventBuilderSetDefaults(ev_builder_t *b);

void nvnEventBuilderSetStorage(ev_builder_t *b, const pool_t *p, int64_t off);

void nvnEventBuilderGetStorage(const ev_builder_t *b, void **pool, int64_t *off);

void *nvnEventBuilderGetMemoryPool(const ev_builder_t *b);

int64_t nvnEventBuilderGetMemoryOffset(const ev_builder_t *b);

uint32_t nvnEventInitialize(ev_t *e, const ev_builder_t *b);

void nvnEventFinalize(ev_t *e);

uint32_t nvnEventGetValue(const ev_t *e);

void nvnEventSignal(ev_t *e, int mode, uint32_t v);

void *nvnEventGetMemoryPool(const ev_t *e);

int64_t nvnEventGetMemoryOffset(const ev_t *e);

void nvn_zero_call(const char *name);

void nvn_view_level_layer(const void *view, uint32_t *level, uint32_t *layer);

uint64_t nvn_zero64(void);

void *nvn_lookup(const char *name);

void *nvnDeviceGetProcAddress(const nvn_dev_t *d, const char *name);

void *runtime_nvn_bootstrap(const char *name);

#endif
