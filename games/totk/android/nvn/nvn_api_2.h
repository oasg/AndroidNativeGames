/* Internal NVN module API. */
#ifndef TOTK_ANDROID_NVN_API_2_H
#define TOTK_ANDROID_NVN_API_2_H
#include "nvn_state.h"
#include "nvn_api_1.h"

int runtime_nvn_cb_call_selftest(void);

int runtime_nvn_pool_lifetime_selftest(void);

int runtime_nvn_bind_selftest(void);

int runtime_nvn_draw_reflect_selftest(const char *screenshot, int frames, const char *vs_path, const char *ps_path, const char *vs_res, const char *ps_res);

void nvn_probe_pair_free(nvn_probe_pair_t *pair);

int nvn_probe_install_stage(prog_t *p, unsigned stage, const uint8_t *spv, size_t size, const char *res, const RuntimeShaderIdentity *id);

int nvn_probe_pair_load(prog_t *p, const uint8_t *vs, size_t vs_size, const uint8_t *ps, size_t ps_size, const char *vs_res, const char *ps_res, uint8_t tag, nvn_probe_pair_t *pair);

void nvn_probe_cb_reset(cb_t *cb);

int nvn_probe_check(const char *what, const uint8_t px[4], int r_lo, int r_hi, int g_lo, int g_hi, int b_lo, int b_hi);

int runtime_nvn_state_selftest(const char *screenshot, int frames);

int nvn_probe_make_texture(tex_t *t, uint32_t w, uint32_t h, uint32_t format, const uint8_t *pixels);

void nvn_probe_fill(uint8_t *p, uint32_t w, uint32_t h, uint32_t seed);

int runtime_nvn_frame_selftest(const char *screenshot, int frames);

int nvn_probe_px_near(const uint8_t a[4], const float c[4], int tol);

int runtime_nvn_mrt_selftest(const char *screenshot, int frames);

int runtime_nvn_fence_selftest(void);

int runtime_nvn_vertex_state_selftest(void);

int runtime_nvn_retired_stress_selftest(int iterations);

int runtime_nvn_prog_abi_selftest(void);

int runtime_nvn_sparse_target_selftest(void);

int runtime_nvn_segment_selftest(const char *screenshot, int frames);

void nvnBufferBuilderSetDefaults(buf_builder_t *b);

void nvnBufferBuilderSetDevice(buf_builder_t *b, void *d);

void nvnBufferBuilderSetStorage(buf_builder_t *b, void *p, ptrdiff_t off, size_t size);

void *nvnBufferBuilderGetDevice(const buf_builder_t *b);

void *nvnBufferBuilderGetMemoryPool(const buf_builder_t *b);

ptrdiff_t nvnBufferBuilderGetMemoryOffset(const buf_builder_t *b);

size_t nvnBufferBuilderGetSize(const buf_builder_t *b);

uint32_t nvnBufferInitialize(buf_t *b, const buf_builder_t *s);

void nvnBufferFinalize(buf_t *b);

void nvnBufferSetDebugLabel(buf_t *b, const char *l);

void *nvnBufferMap(const buf_t *b);

uint64_t nvnBufferGetAddress(const buf_t *b);

void nvnBufferFlushMappedRange(const buf_t *b, ptrdiff_t off, size_t n);

void nvnBufferInvalidateMappedRange(const buf_t *b, ptrdiff_t off, size_t n);

void *nvnBufferGetMemoryPool(const buf_t *b);

ptrdiff_t nvnBufferGetMemoryOffset(const buf_t *b);

size_t nvnBufferGetSize(const buf_t *b);

uint64_t nvnBufferGetDebugID(const buf_t *b);

GfxSampler *nvn_sampler_gfx(const samp_t *s);

void nvnSamplerBuilderSetDefaults(samp_builder_t *b);

void nvnSamplerBuilderSetDevice(samp_builder_t *b, void *d);

void nvnSamplerBuilderSetMinMagFilter(samp_builder_t *b, int min, int mag);

void nvnSamplerBuilderSetWrapMode(samp_builder_t *b, int s, int t, int r);

void nvnSamplerBuilderSetLodClamp(samp_builder_t *b, float lo, float hi);

void nvnSamplerBuilderSetLodBias(samp_builder_t *b, float v);

void nvnSamplerBuilderSetCompare(samp_builder_t *b, int mode, int func);

void nvnSamplerBuilderSetBorderColor(samp_builder_t *b, const float *c);

void nvnSamplerBuilderSetBorderColori(samp_builder_t *b, const int *c);

void nvnSamplerBuilderSetBorderColorui(samp_builder_t *b, const uint32_t *c);

void nvnSamplerBuilderSetMaxAnisotropy(samp_builder_t *b, float v);

void nvnSamplerBuilderSetReductionFilter(samp_builder_t *b, int v);

void nvnSamplerBuilderSetLodSnap(samp_builder_t *b, float v);

void *nvnSamplerBuilderGetDevice(const samp_builder_t *b);

int nvnSamplerBuilderGetMinMagFilter(const samp_builder_t *b, int *min, int *mag);

int nvnSamplerBuilderGetWrapMode(const samp_builder_t *b, int *s, int *t, int *r);

void nvnSamplerBuilderGetLodClamp(const samp_builder_t *b, float *lo, float *hi);

float nvnSamplerBuilderGetLodBias(const samp_builder_t *b);

void nvnSamplerBuilderGetCompare(const samp_builder_t *b, int *mode, int *func);

void nvnSamplerBuilderGetBorderColor(const samp_builder_t *b, float *c);

void nvnSamplerBuilderGetBorderColori(const samp_builder_t *b, int *c);

void nvnSamplerBuilderGetBorderColorui(const samp_builder_t *b, uint32_t *c);

float nvnSamplerBuilderGetMaxAnisotropy(const samp_builder_t *b);

int nvnSamplerBuilderGetReductionFilter(const samp_builder_t *b);

float nvnSamplerBuilderGetLodSnap(const samp_builder_t *b);

uint32_t nvnSamplerInitialize(samp_t *s, const samp_builder_t *b);

void nvnSamplerFinalize(samp_t *s);

void nvnSamplerSetDebugLabel(samp_t *s, const char *l);

int nvnSamplerGetMinMagFilter(const samp_t *s, int *min, int *mag);

int nvnSamplerGetWrapMode(const samp_t *s, int *x, int *y, int *z);

void nvnSamplerGetLodClamp(const samp_t *s, float *lo, float *hi);

float nvnSamplerGetLodBias(const samp_t *s);

void nvnSamplerGetCompare(const samp_t *s, int *mode, int *func);

void nvnSamplerGetBorderColor(const samp_t *s, float *c);

void nvnSamplerGetBorderColori(const samp_t *s, int *c);

void nvnSamplerGetBorderColorui(const samp_t *s, uint32_t *c);

float nvnSamplerGetMaxAnisotropy(const samp_t *s);

int nvnSamplerGetReductionFilter(const samp_t *s);

uint32_t nvnSamplerCompare(const samp_t *a, const samp_t *b);

uint64_t nvnSamplerGetDebugID(const samp_t *s);

int samp_pool_valid(const samp_pool_t *p);

uint32_t nvnSamplerPoolInitialize(samp_pool_t *p, const pool_t *pool, ptrdiff_t off, int n);

void nvnSamplerPoolFinalize(samp_pool_t *p);

void nvnSamplerPoolSetDebugLabel(samp_pool_t *p, const char *l);

void nvnSamplerPoolRegisterSampler(const samp_pool_t *p, int id, const samp_t *s);

void nvnSamplerPoolRegisterSamplerBuilder(const samp_pool_t *p, int id, const samp_builder_t *b);

void *nvnSamplerPoolGetMemoryPool(const samp_pool_t *p);

int64_t nvnSamplerPoolGetMemoryOffset(const samp_pool_t *p);

int nvnSamplerPoolGetSize(const samp_pool_t *p);

void nvn_mask_trace(const tex_t *t, const char *what);

cb_meta_t *nvn_cb_meta(const cb_t *c, int create);

int nvn_cb_mask_candidate(const cb_t *c, unsigned *kinds);

nvn_cb_target_t *nvn_cb_target(const cb_t *cb, int create);

void nvn_cb_target_forget(const cb_t *cb);

void nvn_cb_draw_snapshot(cb_t *c);

const nvn_draw_snap_t *nvn_cb_last_draw(const cb_t *cb);

const nvn_bind_state_t *nvn_cb_draw_state(const cb_t *cb);

uint32_t nvn_cb_segments(const cb_t *c, nvn_seg_t **out);

int nvn_cb_track(cb_t *c);

void nvn_cb_untrack(const cb_t *c);

nvn_cmd_t *nvn_cb_cmd(cb_t *c, uint32_t kind);

nvn_bind_state_t *nvn_cb_bind_state(cb_t *c);

void nvn_cb_state_event(cb_t *c);

void nvn_cb_desc_event(cb_t *c, uint32_t stage, uint32_t kind, uint32_t slot, uint64_t address, size_t size, uint64_t handle);

void nvn_cb_free_commands(cb_t *c);

int nvn_cmd_clone(nvn_cmd_t *dst, const nvn_cmd_t *src);

void nvn_cmd_payload_free(nvn_cmd_t *cmd);

void nvn_cb_retire(cb_t *c);

uint32_t nvnCommandBufferInitialize(cb_t *c, void *d);

void nvnCommandBufferFinalize(cb_t *c);

void nvnCommandBufferSetDebugLabel(cb_t *c, const char *l);

void nvnCommandBufferSetMemoryCallback(cb_t *c, void *cb);

void nvnCommandBufferSetMemoryCallbackData(cb_t *c, void *d);

void nvnCommandBufferSetControlMemoryCallbackEnabled(cb_t *c, int e);

void nvnCommandBufferSetCommandMemoryCallbackEnabled(cb_t *c, int e);

void nvnCommandBufferAddCommandMemory(cb_t *c, const pool_t *p, ptrdiff_t off, size_t n);

void nvnCommandBufferAddControlMemory(cb_t *c, void *m, size_t n);

size_t nvnCommandBufferGetControlMemorySize(const cb_t *c);

size_t nvnCommandBufferGetControlMemoryUsed(const cb_t *c);

size_t nvnCommandBufferGetCommandMemorySize(const cb_t *c);

size_t nvnCommandBufferGetCommandMemoryUsed(const cb_t *c);

void nvnCommandBufferBeginRecording(cb_t *c);

uint64_t nvnCommandBufferEndRecording(cb_t *c);

void nvnCommandBufferBindProgram(cb_t *c, const prog_t *p, int stage_mask);

void nvnCommandBufferSetProgram(cb_t *c, prog_t *p);

void nvnCommandBufferSetWindow(cb_t *c, win_t *w);

void nvnCommandBufferDrawArrays(cb_t *c, int mode, int first, int count);

void nvnCommandBufferDrawArraysInstanced(cb_t *c, int mode, int first, int count, int base_instance, int instances);

void nvnCommandBufferSetRenderEnable(cb_t *c, int e);

void nvnCommandBufferBindBlendState(cb_t *c, const void *s);

void nvnCommandBufferBindColorState(cb_t *c, const void *s);

void nvnCommandBufferBindChannelMaskState(cb_t *c, const void *s);

void nvnCommandBufferBindMultisampleState(cb_t *c, const void *s);

void nvnCommandBufferBindPolygonState(cb_t *c, const void *s);

void nvnCommandBufferBindDepthStencilState(cb_t *c, const void *s);

void nvnCommandBufferSetBlendColor(cb_t *c, const float *v);

void nvnCommandBufferSetStencilRef(cb_t *c, int face, int v);

void nvnCommandBufferSetStencilMask(cb_t *c, int face, int v);

void nvnCommandBufferSetStencilValueMask(cb_t *c, int face, int v);

void nvnCommandBufferClearDepthStencil(cb_t *c, float depth, uint32_t dmask, int stencil, int smask);

void nvnCommandBufferSetRenderTargets(cb_t *c, int n, const void *const *t, const void *const *views, const void *depth, const void *depth_view);

void nvnCommandBufferClearColor(cb_t *c, int i, const float *v, int mask);

void nvnCommandBufferClearColori(cb_t *c, int i, const int *v, int mask);

void nvnCommandBufferClearColorui(cb_t *c, int i, const uint32_t *v, int mask);

void nvnCommandBufferSetViewport(cb_t *c, int x, int y, int w, int h);

void nvnCommandBufferSetScissor(cb_t *c, int x, int y, int w, int h);

void nvnCommandBufferSetViewports(cb_t *c, int first, int n, const float *v);

void nvnCommandBufferSetScissors(cb_t *c, int first, int n, const int *v);

void nvnCommandBufferBindVertexBuffer(cb_t *c, int index, uint64_t address, size_t size);

void nvnCommandBufferBindVertexBuffers(cb_t *c, int first, int n, const void *ranges);

void nvnCommandBufferBindVertexAttribState(cb_t *c, int n, const void *states);

void nvnCommandBufferBindVertexStreamState(cb_t *c, int n, const void *states);

void nvnCommandBufferCopyBufferToBuffer(cb_t *c, uint64_t src, uint64_t dst, size_t n, int flags);

void nvnCommandBufferClearBuffer(cb_t *c, uint64_t dst, size_t n, uint32_t value);

cb_t *nvn_cb_by_handle(uint64_t h);

int nvn_cb_append_command(cb_t *dst, const nvn_cmd_t *src);

void nvn_cb_merge_commands(cb_t *dst, const cb_t *src, const char *what);

int nvn_cb_merge_retired(cb_t *dst, uint64_t handle, const char *what);

void nvnCommandBufferCallCommands(cb_t *c, int n, const uint64_t *handles);

void nvnCommandBufferCopyCommands(cb_t *c, int n, const uint64_t *handles);

void nvnCommandBufferSetPrimitiveRestart(cb_t *c, uint32_t enable, int index);

void nvn_cb_snapshot_indices(nvn_cmd_t *cmd, int type, int count, uint64_t index);

int nvn_vb_snap_reserve(size_t n);

void nvn_vb_snap_release(size_t n);

uint32_t nvn_cb_index_vertex_count(const nvn_cmd_t *cmd);

void nvn_cb_snapshot_vertex_buffers(nvn_cmd_t *cmd, const nvn_bind_state_t *b, uint32_t need_verts);

void nvn_cb_snapshot_free(nvn_cmd_t *cmd);

int nvn_cb_trace_record_vb_want(cb_t *c);

void nvn_cb_trace_record_vb(cb_t *c, const char *what, uint32_t count);

void nvnCommandBufferDrawElements(cb_t *c, int mode, int type, int count, uint64_t index);

void nvnCommandBufferDrawElementsBaseVertex(cb_t *c, int mode, int type, int count, uint64_t index, int base_vertex);

void nvnCommandBufferSetTexturePool(cb_t *c, const tex_pool_t *p);

void nvnCommandBufferSetSamplerPool(cb_t *c, const samp_pool_t *p);

void nvnCommandBufferUpdateUniformBuffer(cb_t *c, uint64_t buffer, size_t buffer_size, ptrdiff_t offset, size_t size, const void *data);

void nvnCommandBufferBindUniformBuffer(cb_t *c, int stage, int i, uint64_t address, size_t size);

void nvnCommandBufferBindUniformBuffers(cb_t *c, int stage, int first, int n, const void *ranges);

void nvnCommandBufferBindStorageBuffer(cb_t *c, int stage, int i, uint64_t address, size_t size);

void nvnCommandBufferBindStorageBuffers(cb_t *c, int stage, int first, int n, const void *ranges);

void nvnCommandBufferBindTexture(cb_t *c, int stage, int i, uint64_t handle);

void nvnCommandBufferBindTextures(cb_t *c, int stage, int first, int n, const uint64_t *handles);

void nvnCommandBufferBindImage(cb_t *c, int stage, int i, uint64_t handle);

void nvnCommandBufferBindImages(cb_t *c, int stage, int first, int n, const uint64_t *handles);

void nvn_cb_apply_uniform_updates(cb_t *cb);

void nvn_cb_clear_texture(cb_t *c, void *t, void *v, void *r, const float rgba[4], int mask, int integer, const char *what);

uint64_t nvn_layout_sig(const GfxRenderTargetLayout *l);

int nvn_layout_equal(const GfxRenderTargetLayout *a, const GfxRenderTargetLayout *b);

int nvn_layout_valid(const GfxRenderTargetLayout *l);

GfxOffscreenTarget *nvn_host_rt_get_owner(uint32_t width, uint32_t height, const GfxRenderTargetLayout *layout, const void *owner);

void nvn_host_rt_clear(void);

int nvn_rt_layout_from_targets(uint32_t n, void *const *tex, const void *const *views, const void *depth, const void *depth_view, GfxRenderTargetLayout *layout, uint32_t *width, uint32_t *height, const char **why);

void nvn_note_empty_target(const void *tex);

int nvn_is_empty_target(const void *tex);

int nvn_layout_matches_window(const win_t *w, const GfxRenderTargetLayout *layout, uint32_t width, uint32_t height);

int nvn_view_format(const void *view, int *format, uint32_t *base_layer, uint32_t *layers);

void nvnBlendStateSetDefaults(void *s);

void nvnBlendStateSetBlendTarget(void *s, int t);

void nvnBlendStateSetBlendFunc(void *s, int sr, int dr, int sa, int da);

void nvnBlendStateSetBlendEquation(void *s, int er, int ea);

void nvnColorStateSetDefaults(void *s);

void nvnColorStateSetBlendEnable(void *s, int i, uint32_t en);

void nvnChannelMaskStateSetDefaults(void *s);

void nvnChannelMaskStateSetChannelMask(void *s, int i, uint32_t r, uint32_t g, uint32_t b, uint32_t a);

void nvnMultisampleStateSetDefaults(void *s);

void nvnMultisampleStateSetMultisampleEnable(void *s, uint32_t e);

void nvnMultisampleStateSetSamples(void *s, int n);

void nvnMultisampleStateSetAlphaToCoverageEnable(void *s, uint32_t e);

void nvnMultisampleStateSetSampleLocationsEnable(void *s, uint32_t e);

void nvnMultisampleStateSetSampleLocations(void *s, int start, int n, const float *v);

void nvnPolygonStateSetDefaults(void *s);

void nvnPolygonStateSetCullFace(void *s, int f);

void nvnPolygonStateSetFrontFace(void *s, int f);

void nvnPolygonStateSetPolygonMode(void *s, int m);

void nvnPolygonStateSetPolygonOffsetEnables(void *s, int e);

void nvnDepthStencilStateSetDefaults(void *s);

void nvnDepthStencilStateSetDepthTestEnable(void *s, uint32_t e);

void nvnDepthStencilStateSetDepthWriteEnable(void *s, uint32_t e);

void nvnDepthStencilStateSetDepthFunc(void *s, int f);

void nvnDepthStencilStateSetStencilTestEnable(void *s, uint32_t e);

void nvnDepthStencilStateSetStencilFunc(void *s, int f, int fn);

void nvnDepthStencilStateSetStencilOp(void *s, int f, int fail, int zfail, int zpass);

void nvn_seg_fill(const cb_t *c, nvn_seg_t *s, uint32_t first, uint32_t end);

void nvn_scene_queue(uint64_t handle, uint32_t seg, uint64_t seq, uint32_t prio, const void *target, uint32_t width, uint32_t height, uint32_t draws);

unsigned nvn_scene_take(nvn_scene_item_t *out, unsigned max);

void nvn_retired_clear_locked(nvn_retired_t *e);

int nvn_ui_seg_want(void);

const char *nvn_seg_reason(const nvn_seg_t *sg, const nvn_retired_t *e);

int nvn_seg_is_ui(const nvn_seg_t *sg);

void nvn_retired_drop_cb(const cb_t *c);

void nvn_seg_prepare(nvn_seg_t *segs, uint32_t nseg, cb_t *c);

void nvn_scene_submit_sampled_target(const nvn_bind_state_t *bs, cb_t *view);

void nvn_scene_submit_pending(void);

int nvn_seg_probe_tex(tex_t *t, uint32_t w, uint32_t h);

void nvnVertexAttribStateSetDefaults(void *s);

void nvnVertexAttribStateSetFormat(void *s, int format, ptrdiff_t offset);

void nvnVertexAttribStateSetStreamIndex(void *s, int index);

#endif
