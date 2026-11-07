/* Internal NVN module API. */
#ifndef TOTK_ANDROID_NVN_API_1_H
#define TOTK_ANDROID_NVN_API_1_H
#include "nvn_state.h"
#include "nvn_api_0.h"

void *nvnTextureBuilderGetDevice(const tex_builder_t *b);

uint32_t nvnTextureInitialize(tex_t *t, const tex_builder_t *b);

int nvn_texture_store_bridge(tex_t *t, GfxTexture *g);

void nvn_texture_bridge_drop(const tex_t *t);

int runtime_nvn_texture_upload_rgba8(void *texture, const uint8_t *pixels, uint32_t width, uint32_t height);

int nvn_texture_gfx_target(int target);

uint32_t nvn_texture_gfx_layers(const tex_desc *d);

int runtime_nvn_texture_upload_guest(void *texture);

void *nvn_texture_gfx(const tex_t *t);

void nvn_upload_trace_result(int on, const tex_t *t, const char *what, GfxTexture *g);

int nvn_tex_is_direct_mask(const tex_t *t);

uint64_t nvn_tex_mem_sum(const tex_t *t);

GfxTexture *nvn_texture_gfx_ready_locked(const tex_t *t);

GfxTexture *nvn_texture_gfx_ready(const tex_t *t);

void nvnTextureFinalize(tex_t *t);

void nvnTextureSetDebugLabel(tex_t *t, const char *l);

int nvnTextureGetStorageClass(const tex_t *t);

int nvnTextureGetFlags(const tex_t *t);

int nvnTextureGetTarget(const tex_t *t);

int nvnTextureGetWidth(const tex_t *t);

int nvnTextureGetHeight(const tex_t *t);

int nvnTextureGetDepth(const tex_t *t);

int nvnTextureGetLevels(const tex_t *t);

int nvnTextureGetFormat(const tex_t *t);

int nvnTextureGetSamples(const tex_t *t);

int nvnTextureGetStride(const tex_t *t);

void nvnTextureGetSwizzle(const tex_t *t, int *r, int *g, int *bl, int *a);

int nvnTextureGetDepthStencilMode(const tex_t *t);

int nvnTextureGetStorageSize(const tex_t *t);

int64_t nvnTextureGetViewOffset(const tex_t *t, const void *view);

uint64_t nvnTextureGetTextureAddress(const tex_t *t);

void nvnTextureGetSparseTileLayout(const tex_t *t, void *out);

void nvnTextureFlushTexels(const tex_t *t, const void *v, const void *r);

void nvnTextureInvalidateTexels(const tex_t *t, const void *v, const void *r);

void *nvnTextureGetMemoryPool(const tex_t *t);

int64_t nvnTextureGetMemoryOffset(const tex_t *t);

int nvnTextureGetRawStorageClass(const tex_t *t);

size_t nvnTextureGetZCullStorageSize(const tex_t *t);

uint64_t nvnTextureGetDebugID(const tex_t *t);

uint32_t nvnTextureCompare(const tex_t *a, const tex_t *b);

float nvn_half_to_float(uint16_t h);

void nvn_texture_mark_dirty(const tex_t *t);

void nvn_tex_copy(const tex_t *t, const void *view, const int *region, void *dst, const void *src, ptrdiff_t row, ptrdiff_t img, int to_dst);

void nvnTextureWriteTexels(const tex_t *t, const void *v, const int *r, const void *src);

void nvnTextureWriteTexelsStrided(const tex_t *t, const void *v, const int *r, const void *src, ptrdiff_t row, ptrdiff_t img);

void nvnTextureReadTexels(const tex_t *t, const void *v, const int *r, void *dst);

void nvnTextureReadTexelsStrided(const tex_t *t, const void *v, const int *r, void *dst, ptrdiff_t row, ptrdiff_t img);

void nvnTextureViewSetDefaults(uint8_t *v);

void nvnTextureViewSetLevels(uint8_t *v, int base, int n);

void nvnTextureViewSetLayers(uint8_t *v, int min, int n);

void nvnTextureViewSetFormat(uint8_t *v, int f);

void nvnTextureViewSetSwizzle(uint8_t *v, int r, int g, int bl, int a);

void nvnTextureViewSetDepthStencilMode(uint8_t *v, int m);

void nvnTextureViewSetTarget(uint8_t *v, int t);

uint32_t nvnTextureViewGetLevels(const uint8_t *v, int *base, int *n);

uint32_t nvnTextureViewGetLayers(const uint8_t *v, int *min, int *n);

uint32_t nvnTextureViewGetFormat(const uint8_t *v, int *f);

uint32_t nvnTextureViewGetSwizzle(const uint8_t *v, int *r, int *g, int *bl, int *a);

uint32_t nvnTextureViewGetDepthStencilMode(const uint8_t *v, int *m);

uint32_t nvnTextureViewGetTarget(const uint8_t *v, int *t);

uint32_t nvnTextureViewCompare(const uint8_t *a, const uint8_t *b);

int tex_pool_valid(const tex_pool_t *p);

uint32_t tex_pool_sig(const tex_pool_t *p);

__attribute__((unused)) static void tex_pool_unlink_locked(int32_t idx);

void tex_pool_prune_locked(void);

unsigned runtime_nvn_tex_pool_regs(void);

unsigned runtime_nvn_tex_pool_full(void);

unsigned runtime_nvn_tex_pool_max(void);

uint32_t nvnTexturePoolInitialize(tex_pool_t *p, const pool_t *pool, ptrdiff_t off, int n);

void nvnTexturePoolFinalize(tex_pool_t *p);

void nvnTexturePoolSetDebugLabel(tex_pool_t *p, const char *l);

void nvnTexturePoolRegisterTexture(const tex_pool_t *p, int id, const tex_t *t, const void *v);

void nvnTexturePoolRegisterImage(const tex_pool_t *p, int id, const tex_t *t, const void *v);

void *nvnTexturePoolGetMemoryPool(const tex_pool_t *p);

int64_t nvnTexturePoolGetMemoryOffset(const tex_pool_t *p);

int nvnTexturePoolGetSize(const tex_pool_t *p);

const char *nvn_tex_target_name(int target);

int nvn_tex_is_ui(const tex_t *t);

size_t nvn_texel_bytes(const tex_t *t);

int nvn_tex_is_rt(const tex_t *t, GfxOffscreenTarget **rt, GfxRenderTargetLayout *layout, uint32_t *rw, uint32_t *rh);

int runtime_nvn_textured_draw_selftest(const char *screenshot, int frames);

int runtime_nvn_texture_bind_selftest(const char *screenshot, int frames);

void pool_epoch_set(const void *p);

uint32_t pool_epoch_get(const void *p);

void nvnMemoryPoolBuilderSetDefaults(pool_builder_t *b);

void nvnMemoryPoolBuilderSetDevice(pool_builder_t *b, void *d);

void nvnMemoryPoolBuilderSetStorage(pool_builder_t *b, void *m, size_t n);

void nvnMemoryPoolBuilderSetFlags(pool_builder_t *b, int f);

void *nvnMemoryPoolBuilderGetMemory(const pool_builder_t *b);

size_t nvnMemoryPoolBuilderGetSize(const pool_builder_t *b);

int nvnMemoryPoolBuilderGetFlags(const pool_builder_t *b);

void *nvnMemoryPoolBuilderGetDevice(const pool_builder_t *b);

uint32_t nvnMemoryPoolInitialize(pool_t *p, const pool_builder_t *b);

void nvnMemoryPoolFinalize(pool_t *p);

void nvnMemoryPoolSetDebugLabel(pool_t *p, const char *l);

void *nvnMemoryPoolMap(const pool_t *p);

void nvnMemoryPoolFlushMappedRange(const pool_t *p, ptrdiff_t off, size_t n);

void nvnMemoryPoolInvalidateMappedRange(const pool_t *p, ptrdiff_t off, size_t n);

uint64_t nvnMemoryPoolGetBufferAddress(const pool_t *p);

uint32_t nvnMemoryPoolMapVirtual(pool_t *p, int n, const void *reqs);

size_t nvnMemoryPoolGetSize(const pool_t *p);

int nvnMemoryPoolGetFlags(const pool_t *p);

unsigned pool_home(const void *p, int id);

int pool_old(const tex_pool_t *p);

void pool_map_init_locked(void);

void nvn_tex_pool_forget(const tex_pool_t *p);

void nvn_tex_pool_add(const tex_pool_t *p, int id, const tex_t *t);

const tex_t *nvn_tex_pool_lookup(const tex_pool_t *p, int id);

void nvn_samp_pool_forget(const samp_pool_t *p);

void nvn_samp_pool_add(const samp_pool_t *p, int id, const samp_t *s);

const samp_t *nvn_samp_pool_lookup(const samp_pool_t *p, int id);

void nvn_pool_epoch_tick(void);

const tex_pool_t *nvn_tex_pool_for(const cb_t *c);

const samp_pool_t *nvn_samp_pool_for(const cb_t *c);

void nvn_pool_forget(const void *pool);

void nvnDeviceBuilderSetDefaults(dev_builder_t *b);

void nvnDeviceBuilderSetFlags(dev_builder_t *b, int flags);

uint32_t nvnDeviceBuilderGetFlags(const dev_builder_t *b);

uint32_t nvnDeviceInitialize(nvn_dev_t *d, const dev_builder_t *b);

void nvnDeviceFinalize(nvn_dev_t *d);

void nvnDeviceSetDebugLabel(nvn_dev_t *d, const char *label);

void nvnDeviceGetInteger(const nvn_dev_t *d, int what, int *out);

uint64_t nvnDeviceGetCurrentTimestampInNanoseconds(const nvn_dev_t *d);

uint64_t nvnDeviceGetTimestampInNanoseconds(const nvn_dev_t *d, const void *counter);

uint64_t nvnDeviceGetTextureHandle(const nvn_dev_t *d, int index, int sampler);

uint64_t nvnDeviceGetTexelFetchHandle(const nvn_dev_t *d, int index);

uint64_t nvnDeviceGetImageHandle(const nvn_dev_t *d, int index);

uint64_t nvnDeviceGetSeparateTextureHandle(const nvn_dev_t *d, int index);

uint64_t nvnDeviceGetSeparateSamplerHandle(const nvn_dev_t *d, int index);

uint32_t nvnDeviceIsExternalDebuggerAttached(const nvn_dev_t *d);

void nvnDeviceInstallDebugCallback(nvn_dev_t *d, const void *cb, void *data, uint32_t enable);

int nvnDeviceGenerateDebugDomainId(const nvn_dev_t *d, const char *name);

void nvnDeviceSetWindowOriginMode(nvn_dev_t *d, int mode);

int nvnDeviceGetWindowOriginMode(const nvn_dev_t *d);

void nvnDeviceSetDepthMode(nvn_dev_t *d, int mode);

int nvnDeviceGetDepthMode(const nvn_dev_t *d);

uint32_t nvnDeviceRegisterFastClearColor(nvn_dev_t *d, const float *c, int f);

uint32_t nvnDeviceRegisterFastClearColori(nvn_dev_t *d, const int *c, int f);

uint32_t nvnDeviceRegisterFastClearColorui(nvn_dev_t *d, const uint32_t *c, int f);

uint32_t nvnDeviceRegisterFastClearDepth(nvn_dev_t *d, float v);

void nvnDeviceSetIntermediateShaderCache(nvn_dev_t *d, int v);

void nvnDeviceApplyDeferredFinalizes(nvn_dev_t *d, int v);

void nvnDeviceFinalizeCommandHandle(nvn_dev_t *d, uint64_t h);

void nvnDeviceWalkDebugDatabase(const nvn_dev_t *d, int t, void *cb, void *u);

void nvnDeviceSetAutomaticCheckpointsEnable(nvn_dev_t *d, int v);

void nvnDeviceSetAutomaticCheckpointsMask(nvn_dev_t *d, uint64_t v);

void nvnDeviceSetAutomaticCheckpointsSamplingInterval(nvn_dev_t *d, int v);

void nvnDeviceSetAutomaticCheckpointsByDebugGroup(nvn_dev_t *d, int v);

void nvnDeviceSetCheckpointCallStacksEnable(nvn_dev_t *d, int v);

void nvnDeviceSetCheckpointMaxCallStacksDepth(nvn_dev_t *d, int v);

void nvnQueueBuilderSetDefaults(queue_builder_t *b);

void nvnQueueBuilderSetDevice(queue_builder_t *b, void *d);

void nvnQueueBuilderSetFlags(queue_builder_t *b, int f);

void nvnQueueBuilderSetCommandMemorySize(queue_builder_t *b, size_t n);

void nvnQueueBuilderSetComputeMemorySize(queue_builder_t *b, size_t n);

void nvnQueueBuilderSetControlMemorySize(queue_builder_t *b, size_t n);

void nvnQueueBuilderSetCommandFlushThreshold(queue_builder_t *b, size_t n);

void nvnQueueBuilderSetQueuePriority(queue_builder_t *b, int p);

int nvnQueueBuilderGetQueuePriority(const queue_builder_t *b);

size_t nvnQueueBuilderGetQueueMemorySize(const queue_builder_t *b);

void nvnQueueBuilderSetQueueMemory(queue_builder_t *b, void *m, size_t n);

void *nvnQueueBuilderGetMemory(const queue_builder_t *b);

size_t nvnQueueBuilderGetMemorySize(const queue_builder_t *b);

void *nvnQueueBuilderGetDevice(const queue_builder_t *b);

int nvnQueueBuilderGetFlags(const queue_builder_t *b);

size_t nvnQueueBuilderGetCommandMemorySize(const queue_builder_t *b);

size_t nvnQueueBuilderGetComputeMemorySize(const queue_builder_t *b);

size_t nvnQueueBuilderGetControlMemorySize(const queue_builder_t *b);

size_t nvnQueueBuilderGetCommandFlushThreshold(const queue_builder_t *b);

uint32_t nvnQueueInitialize(queue_t *q, const queue_builder_t *b);

void nvnQueueFinalize(queue_t *q);

void nvnQueueSetDebugLabel(queue_t *q, const char *l);

int nvn_gpu_drain(const char *what);

void nvnQueueSubmitCommands(queue_t *q, int n, const uint64_t *cmds);

void nvnQueueFlush(queue_t *q);

void nvnQueueFinish(queue_t *q);

void nvnQueuePresentTexture(queue_t *q, win_t *w, int index);

int nvnQueueGetError(queue_t *q, void *info);

size_t nvnQueueGetTotalCommandMemoryUsed(queue_t *q);

size_t nvnQueueGetTotalControlMemoryUsed(queue_t *q);

size_t nvnQueueGetTotalComputeMemoryUsed(queue_t *q);

void nvnQueueResetMemoryUsageCounts(queue_t *q);

int nvnQueueWaitSync(queue_t *q, const sync_t *s);

int nvnQueueAcquireTexture(queue_t *q, win_t *w, int *index);

void nvn_frame_target_hist(const cb_t *c, const void *chosen);

void nvn_frame_draw_trace(cb_t *c, const char *what, int count);

void nvn_frame_ubo_trace(int n, const uint64_t *cmds);

void nvn_frame_submit_trace(int n, const uint64_t *cmds);

void nvn_frame_report_orphans(void);

void nvn_warm_recording(void *vc);

void nvn_warm_scene(cb_t *c);

int nvn_gpu_trace(void);

void nvn_gpu_report(const char *what, const nvn_cmd_t *cmd, const char *why);

void nvn_gpu_stats_log(const char *where);

int nvn_submit_scene_snapshot(uint64_t handle, uint32_t seg_index);

void nvn_process_submit_commands(const uint64_t *cmds, int n);

int nvnQueueFenceSync(queue_t *q, sync_t *s, int condition, int flags);

void nvnWindowBuilderSetDefaults(win_builder_t *b);

void nvnWindowBuilderSetDevice(win_builder_t *b, void *d);

void nvnWindowBuilderSetNativeWindow(win_builder_t *b, NvnNativeWindow w);

void nvnWindowBuilderSetTextures(win_builder_t *b, int n, void *const *t);

void nvnWindowBuilderSetPresentInterval(win_builder_t *b, int v);

void nvnWindowBuilderSetNumActiveTextures(win_builder_t *b, int n);

void *nvnWindowBuilderGetNativeWindow(const win_builder_t *b);

int nvnWindowBuilderGetPresentInterval(const win_builder_t *b);

int nvnWindowBuilderGetNumTextures(const win_builder_t *b);

void *nvnWindowBuilderGetTexture(const win_builder_t *b, int i);

void *nvnWindowBuilderGetDevice(const win_builder_t *b);

int nvnWindowBuilderGetNumActiveTextures(const win_builder_t *b);

void nvn_window_register(win_t *w);

void nvn_window_unregister(win_t *w);

win_t *nvn_window_for_texture(const void *texture);

void nvn_window_guest_desc(const win_builder_t *b, uint32_t *width, uint32_t *height, uint32_t *format);

uint32_t nvnWindowInitialize(win_t *w, const win_builder_t *b);

void nvnWindowFinalize(win_t *w);

void nvnWindowSetDebugLabel(win_t *w, const char *l);

int nvnWindowAcquireTexture(win_t *w, sync_t *s, int *index);

void *nvnWindowGetNativeWindow(const win_t *w);

int nvnWindowGetPresentInterval(const win_t *w);

void nvnWindowSetPresentInterval(win_t *w, int v);

void nvnWindowSetCrop(win_t *w, int x, int y, int ww, int hh);

void nvnWindowGetCrop(const win_t *w, int *out);

void nvnWindowSetNumActiveTextures(win_t *w, int n);

int nvnWindowGetNumActiveTextures(const win_t *w);

int nvnWindowGetNumTextures(const win_t *w);

void nvn_window_layout(const win_t *w, GfxRenderTargetLayout *layout, uint32_t *width, uint32_t *height);

uint32_t nvn_window_format(const win_t *w);

int nvn_window_mirror_target(win_t *w, GfxOffscreenTarget *target);

const tex_t *nvn_window_sampled_tex(const nvn_bind_state_t *bs, cb_t *view);

GfxOffscreenTarget *nvn_window_present_rt(const nvn_bind_state_t *bs, cb_t *view, const tex_t **tex_out);

int nvn_window_frame_ppm(win_t *w, const char *path);

int runtime_nvn_submit_selftest(void);

int runtime_nvn_window_selftest(void *nw);

int runtime_nvn_draw_selftest(const char *screenshot, int frames, const char *vs_path, const char *ps_path, int expect_visible);

int runtime_nvn_cb_lifetime_selftest(void);

#endif
