/* Definitions of the state declared in nvn_state.h. */
#include "nvn_state.h"
/* Evidence counters for the title/UI input trace. */
unsigned nvn_tex_writes;
unsigned nvn_buf_maps;
unsigned nvn_cb_copy_b2b;
unsigned nvn_cb_copy_b2t;
unsigned nvn_cb_draw_texture_calls;
unsigned nvn_cb_clear_texture_calls;
unsigned nvn_cb_copy_t2b_calls;
unsigned nvn_cb_dispatch_calls;
unsigned nvn_cb_copy_t2t_calls;
unsigned nvn_cb_blit_calls;
nvn_global_ubo_t g_global_ubo[6][16];
pthread_mutex_t g_global_ubo_lock = PTHREAD_MUTEX_INITIALIZER;
/* Same queue-scoped table for storage buffers.  The compute path needs it for
 * the same reason: the engine binds the per-frame SSBO table on a state-only
 * recording and the dispatch buffer carries only what it changed. */
nvn_global_ubo_t g_global_ssbo[6][16];
pthread_mutex_t g_global_ssbo_lock = PTHREAD_MUTEX_INITIALIZER;
const char *g_last_api = "none";
char g_last_proc[80] = "none";
uint32_t nvn_fail_count;
/* Live NVN texture objects.  The window path needs the guest texture's
 * dimensions/format, but a guest-provided pointer must never be dereferenced
 * unless nvnTextureInitialize produced it: the registry gives us a safe
 * lookup (some callers pass small integers as placeholder textures). */
tex_t **g_tex_objs;
unsigned g_tex_obj_count;
unsigned g_tex_obj_cap;
pthread_mutex_t g_tex_obj_lock = PTHREAD_MUTEX_INITIALIZER;
nvn_global_tex_t g_global_tex[6][128];
pthread_mutex_t g_global_tex_lock = PTHREAD_MUTEX_INITIALIZER;
/* Queue-scoped image binds, mirroring the texture table.  A compute dispatch
 * recorded on a later command buffer still sees an image bound earlier, the
 * same way NVN queue state behaves. */
nvn_global_tex_t g_global_img[6][16];
pthread_mutex_t g_global_img_lock = PTHREAD_MUTEX_INITIALIZER;
tex_bridge_t *g_tex_bridges;
unsigned g_tex_bridge_count;
unsigned g_tex_bridge_cap;
pthread_mutex_t g_tex_bridge_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
unsigned nvn_upload_traces;
/* Set when the compositor samples a target that no segment drew to.  The L2
 * frame dump uses it to capture the title/clear state instead of waiting for a
 * scene pass that does not exist yet. */
unsigned nvn_empty_sampled;
tex_pool_map_t g_tex_pool_map[NVN_POOL_MAP_MAX];
int32_t g_tex_buckets[NVN_POOL_BUCKETS];
int32_t g_tex_free;
unsigned g_tex_pool_map_count;
unsigned g_tex_pool_full;
unsigned g_tex_pool_max;
pool_epoch_t g_pool_epochs[NVN_POOL_EPOCH_MAX];
unsigned g_pool_epoch_count;
pthread_mutex_t g_pool_epoch_lock = PTHREAD_MUTEX_INITIALIZER;
nvn_prog_ubo_t g_prog_ubos[NVN_PROG_UBO_MAX];
unsigned g_prog_ubo_count;
uint64_t g_prog_ubo_age;
pthread_mutex_t g_prog_ubo_lock = PTHREAD_MUTEX_INITIALIZER;
unsigned nvn_gpu_submits;
unsigned nvn_gpu_presents;
unsigned nvn_submit_trace;
unsigned nvn_map_trace;
/* Explicit CPU/GPU ordering.  The backend submits and waits synchronously, so
 * the moment a submit/present returns the GPU has finished; nvn_gpu_drain
 * makes that a checked precondition instead of an accident, and every CPU
 * reader (buffer/pool map, fence wait) goes through it. */
int nvn_gpu_pending_work;
/* Mark the host copy of a guest texture stale after a CPU-side texel write. */
/* Mask producer diagnostics (Lane 4 gate34): the two R8 masks sampled by the
 * 0x7a scene passes.  Counts target/sample binds and reports whether any
 * producer path touched them (RT association, CPU write, upload). */
unsigned g_mask_binds;
unsigned g_mask_rt;
unsigned g_mask_writes;
/* Texture/sampler pools are queue state: the engine may set the pool on one
 * command buffer and bind texture handles on another.  Remember the last pool
 * bound on any command buffer and inherit it for a command buffer that has not
 * set a pool of its own. */
const tex_pool_t *g_last_tex_pool;
const samp_pool_t *g_last_samp_pool;
pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
uint64_t nvn_cb_serial;
cb_t **g_cbs;
unsigned g_ncbs;
unsigned g_cbs_cap;
cb_meta_t g_cb_meta[NVN_CB_META_MAX];
pthread_mutex_t g_cb_meta_lock = PTHREAD_MUTEX_INITIALIZER;
nvn_state_g_orphans_t g_orphans[NVN_ORPHAN_MAX];
unsigned g_orphan_count;
unsigned g_orphan_next;
nvn_cbacct_t g_cbacct;
pthread_mutex_t g_cbacct_lock = PTHREAD_MUTEX_INITIALIZER;
unsigned nvn_nvn_draws;
int g_nvn_probe;
/* synthetic selftest shaders are not module-backed */
unsigned nvn_record_count;
/* BeginRecording calls, for the first-frame trace */
unsigned nvn_vtx_trace;
unsigned nvn_rt_trace;
unsigned nvn_tex_trace;
unsigned nvn_draw_trace;
unsigned nvn_ubo_trace;
unsigned nvn_draw_rec_trace;
unsigned nvn_vaset_trace;
unsigned nvn_copy_trace;
/* Defined with the scene-present state below; the eviction policy protects the
 * target currently routed to present(). */
GfxOffscreenTarget *g_last_scene_rt;
GfxOffscreenTarget *g_present_rt;
nvn_cb_target_t g_cb_targets[64];
pthread_mutex_t g_cb_target_lock = PTHREAD_MUTEX_INITIALIZER;
nvn_retired_t g_retired[NVN_RETIRED_MAX];
unsigned nvn_scene_queued;
unsigned nvn_scene_submits;
unsigned nvn_scene_rendered;
uint32_t g_last_scene_w;
uint32_t g_last_scene_h;
/* Which guest texture/color0/layout produced g_last_scene_rt.  The window pass
 * often samples an alias object of the RT's texture, so keep the resource
 * identity for the association fallback and for present routing. */
const tex_t *g_last_scene_tex;
const void *g_last_scene_color0;
GfxRenderTargetLayout g_last_scene_layout;
pthread_mutex_t g_prog_lock = PTHREAD_MUTEX_INITIALIZER;
uint64_t g_retired_age;
pthread_mutex_t g_retired_lock = PTHREAD_MUTEX_INITIALIZER;
unsigned nvn_ui_seg_logs;
/* --- Per-draw vertex-stream snapshots -----------------------------------
 * The guest records draws into a recycled ring buffer and only replays the
 * retired handle later.  The segment's bind state keeps the *address* of the
 * last draw's stream, but the bytes are already gone.  Snapshot each small
 * bound stream at draw time (the same idea as index_data above); the segment
 * replay merges the per-draw copies into one stream and offsets every draw. */
size_t g_vb_snap_live;
pthread_mutex_t g_vb_snap_lock = PTHREAD_MUTEX_INITIALIZER;
unsigned g_shader_cache_hits;
unsigned g_shader_cache_misses;
unsigned g_pipelines_created;
prog_state_slot_t *g_prog_states[PROG_STATE_BUCKETS];
nvn_pipe_ubo_t g_pipe_ubo[128];
unsigned g_pipe_ubo_count;
pthread_mutex_t g_pipe_ubo_lock = PTHREAD_MUTEX_INITIALIZER;
nvn_ubo_dec_t g_ubo_dec[2][16];
nvn_gpu_stats_t g_gpu_stats;
