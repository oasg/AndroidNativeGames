/* NVN object/state layouts. */
#ifndef TOTK_ANDROID_NVN_TYPES_1_H
#define TOTK_ANDROID_NVN_TYPES_1_H
#include "nvn_types_0.h"


typedef struct { const samp_t *samp; GfxSampler *gfx; } samp_bridge_t;


typedef struct {
    pool_t *pool;
    int64_t off;
    int32_t n;
    uint32_t magic;
    uint32_t gen, epoch;
} samp_pool_t;



/* NVN texture/sampler pool registrations.  The pool object itself is
 * guest-sized (0x20 bytes); the host map is generation-aware and bounded. */
typedef struct {
    const tex_pool_t *pool;
    const tex_t *tex;
    uint32_t id, gen, sig, age;
    int32_t next;
} tex_pool_map_t;


typedef struct {
    const samp_pool_t *pool;
    const samp_t *samp;
    uint32_t id, gen, age;
    int32_t next;
} samp_pool_map_t;


/* NVN pool objects are guest memory and must stay untouched after init: the
 * engine keeps its own links next to the header.  Keep the host last-use epoch
 * in a side table keyed by the pool pointer. */
typedef struct { const void *p; uint32_t epoch; } pool_epoch_t;



/* Guest NVNprogram ABI (matches
 * games/totk/third_party/d2r_gfx/gfx/nvn/nvn_obj.h): the shader data/control
 * pairs are interleaved 16-byte entries.  Host-private pipeline/texture state
 * lives in prog_shader_state_t, not in this guest-visible object. */
typedef struct { uint64_t data; const void *control; } nvn_shader_data_t;


typedef struct {
    uint32_t magic, nstage;
    nvn_shader_data_t sd[6];
    uint64_t serial;
} prog_t;


_Static_assert(sizeof(prog_t) <= 0xC0, "NVNprogram guest ABI size");


_Static_assert(offsetof(prog_t, sd) == 8, "NVNprogram sd offset");


_Static_assert(offsetof(prog_t, serial) == 0x68, "NVNprogram serial offset");



/* Per-program uniform-buffer history.  NVN cbuf binds are queue-scoped, but
 * different programs reuse the same NVN index for different constant banks
 * (the window program's c3 ortho vs a preceding pass's projection buffer at
 * index 0).  Remember, for each program, the last buffer bound while that
 * program was current, so a program can reclaim its own cbuf even after
 * another program overwrote the index.  Host-only; guest memory untouched. */
typedef struct {
    const prog_t *prog;
    uint32_t stage, index;
    uint64_t address;
    size_t size;
    uint64_t age;
} nvn_prog_ubo_t;



typedef struct {
    uint32_t magic, state;    /* 0 pending, 1 signaled (GPU completion seen) */
    void *fence;              /* GfxFence* from the backend contract */
    uint64_t fence_generation;
    uint64_t submit_generation;
    int32_t condition;        /* NVNsyncCondition recorded by QueueFenceSync */
    uint32_t pad;
} sync_t;


typedef struct { pool_t *pool; int64_t off; } ev_builder_t;


typedef struct { uint32_t magic, pad; volatile uint32_t *val; pool_t *pool; int64_t off; } ev_t;



/* The guest allocates the NVN reserved sizes; the host layouts must fit. */
_Static_assert(sizeof(queue_t) <= NVN_QUEUE_SIZE, "queue object too large");


_Static_assert(sizeof(win_t) <= NVN_WINDOW_SIZE, "window object too large");


_Static_assert(sizeof(pool_t) <= NVN_MEMORY_POOL_SIZE, "memory pool object too large");


_Static_assert(sizeof(tex_t) <= NVN_TEXTURE_SIZE, "texture object too large");


_Static_assert(sizeof(samp_t) <= NVN_SAMPLER_SIZE, "sampler object too large");


_Static_assert(sizeof(buf_t) <= NVN_BUFFER_SIZE, "buffer object too large");


_Static_assert(sizeof(prog_t) <= NVN_PROGRAM_SIZE, "program object too large");


_Static_assert(sizeof(sync_t) <= NVN_SYNC_SIZE, "sync object too large");


_Static_assert(sizeof(ev_t) <= NVN_EVENT_SIZE, "event object too large");


_Static_assert(sizeof(tex_pool_t) <= NVN_TEXTURE_POOL_SIZE, "texture pool object too large");


_Static_assert(sizeof(samp_pool_t) <= NVN_SAMPLER_POOL_SIZE, "sampler pool object too large");


_Static_assert(sizeof(tex_builder_t) <= 0x80, "texture builder too large");


_Static_assert(sizeof(samp_builder_t) <= 0x60, "sampler builder too large");


_Static_assert(sizeof(queue_builder_t) <= 0x40, "queue builder too large");


_Static_assert(sizeof(win_builder_t) <= 0x40, "window builder too large");


_Static_assert(sizeof(pool_builder_t) <= 0x40, "pool builder too large");


_Static_assert(sizeof(buf_builder_t) <= 0x40, "buffer builder too large");


_Static_assert(sizeof(dev_builder_t) <= 0x40, "device builder too large");


_Static_assert(sizeof(ev_builder_t) <= 0x20, "event builder too large");



/* Texture storage layout (Tegra X1 block-linear).  The guest uses these
 * values to size and offset memory pools, so the calculation is kept
 * consistent even though nothing is submitted to a GPU. */
typedef struct { uint8_t bpb, bw, bh; } nvn_fmt_t;



/* ------------------------------------------------------------------ */
/* Command buffer                                                      */

typedef struct {
    uint32_t magic;
    uint32_t recording;
    void *dev;
    void *ctl;
    size_t ctl_size, ctl_used;
    void *cmd;
    size_t cmd_size, cmd_used;
    void *mem_cb, *mem_cb_data;
    void *prog;
    void *win;
    const tex_pool_t *tex_pool;
    const samp_pool_t *samp_pool;
    uint64_t serial, handle;
    int draw_first, draw_count;
    nvn_cmd_t *cmds;
    uint32_t ncmds, cmds_cap;
    void *bind_state;
} cb_t;



_Static_assert(sizeof(cb_t) <= NVN_COMMAND_BUFFER_SIZE, "command buffer object too large");



/* First-frame lifetime metadata kept out of cb_t (the guest object is only
 * NVN_COMMAND_BUFFER_SIZE bytes).  NVN command handles die when the guest
 * object is re-initialised, so remember the last few recorded-but-never-
 * submitted buffers and report the owning thread at present time. */
typedef struct {
    const cb_t *cb;
    uint64_t record_lr;
    uint32_t record_thread, record_commands, submit_count;
    /* Last render-target set recorded into this command buffer. */
    const void *rt_color0;
    uint32_t rt_nvn_format, rt_width, rt_height, rt_draws;
    uint64_t record_seq;
} cb_meta_t;


/* --- Command-buffer accounting (Lane 4 gate34) ----------------------
 * Distinguishes "the producer command was never recorded" from "it was
 * recorded on a buffer that never reached submit". */
typedef struct {
    unsigned recorded, finalized, submitted, retired, call_cmds, unsubmitted;
    unsigned with_draws, with_gpu, with_mask_target;
    unsigned peak_inflight;
} nvn_cbacct_t;



/* --- Host render targets (MRT + depth) ------------------------------ */
/* NVN binds a set of color targets plus depth; the host mirrors that with a
 * GfxOffscreenTarget created for the complete layout.  Targets are cached by
 * (size, layout) so a repeated frame reuses one image instead of churning
 * Vulkan objects. */
typedef struct {
    uint32_t width, height;
    GfxRenderTargetLayout layout;
    const void *owner;          /* guest colour-0 texture this image backs */
    GfxOffscreenTarget *gfx;
    uint64_t last_use;          /* LRU stamp: refreshed on every lookup */
} nvn_host_rt_t;



/* State captured at nvnCommandBufferDraw* time.  NVN records state changes in
 * command order; applying the command buffer's *final* state to an earlier
 * draw picks up later unbinds.  Keep the state and render-target snapshot that
 * were live when the draw was recorded. */
typedef struct {
    nvn_bind_state_t state;
    GfxRenderTargetLayout layout;
    uint32_t width, height;
    int have_state;
    int have_layout;
    int window_texture;
    const void *color0_tex;   /* guest NVN texture bound as color attachment 0 */
    uint32_t cmd_index;       /* index of the recorded draw command */
} nvn_draw_snap_t;



/* Per-command-buffer resolved render-target state.  Kept out of cb_t so the
 * guest's NVN_COMMAND_BUFFER_SIZE contract does not change. */
typedef struct {
    const cb_t *cb;
    GfxRenderTargetLayout layout;
    uint32_t width, height;
    int valid;
    int window_texture;   /* color 0 is a registered window swapchain texture */
    const void *color0_tex;   /* guest NVN texture bound as color attachment 0 */
    nvn_draw_snap_t *draws;
    uint32_t ndraws, draws_cap;
} nvn_cb_target_t;




/* One contiguous command subrange that draws to a single render-target set.
 * A retired recording is split at every nvnCommandBufferSetRenderTargets so a
 * buffer that clears a 1920x1080 target but draws only to small UI atlases is
 * never replayed into the wrong texture.  The first segment keeps the commands
 * before the first SetRenderTargets as a prefix (state binds recorded before
 * the target). */
typedef struct prog_shader_state prog_shader_state_t;


typedef struct {
    uint32_t first_cmd, ncmds;
    uint32_t draw_count;             /* vertices referenced by draws */
    uint32_t clear_count;            /* color-0 clear commands */
    uint32_t gpu_count;              /* dispatch / texture transfer commands */
    uint32_t clear_mask;
    float clear_rgba[4];
    const void *color0_tex;
    GfxRenderTargetLayout layout;
    uint32_t width, height;
    int have_layout, window_texture;
    nvn_draw_snap_t last;            /* state at the segment's last draw */
    int have_last;
    /* Pipeline/state captured while the guest program is still alive.  The
     * scene queue can render this segment several frames after retirement;
     * the guest may have finalized and reused the NVNprogram by then. */
    GfxReflectedPipeline *pipeline;
    prog_shader_state_t *pstate;
    char variant[192];
    int warm;
    /* Guest ring buffers can be recycled before the queued segment runs.
     * Large composites keep private copies of the vertex/UBO bytes. */
    uint8_t *vb_copy[16];
    size_t vb_copy_size[16];
    uint8_t *ubo_copy[2][16];
    size_t ubo_copy_size[2][16];
    /* First vertex bytes seen at prepare time; compared at replay to prove
     * whether the guest ring buffer was recycled in between. */
    uint8_t vb_head[16];
    int have_vb_head;
} nvn_seg_t;



/* --- Retired command-buffer recordings --------------------------------- */
/* NVN command handles stay valid after nvnCommandBufferFinalize until the
 * command memory is reused: the boot frame records a 6-vertex indexed draw,
 * finalizes the command buffer and then replays the handle with
 * nvnCommandBufferCallCommands every frame.  Keep an immutable deep copy of
 * the recording (commands including uniform-update payloads, bind state,
 * program/window/pools and the resolved targets) keyed by handle so the dead
 * handle remains executable.  The copy never aliases the guest's command
 * memory, so re-initialising the command buffer cannot mutate it. */
typedef struct {
    uint64_t handle;
    uint64_t age;
    uint64_t record_seq;                 /* LRU stamp, refreshed on every replay */
    int used;
    const cb_t *source;
    nvn_cmd_t *cmds;
    uint32_t ncmds;
    nvn_bind_state_t *bind;
    void *prog, *win, *dev;
    const tex_pool_t *tex_pool;
    const samp_pool_t *samp_pool;
    uint32_t draw_count;
    GfxRenderTargetLayout layout;
    uint32_t width, height;
    int have_layout, window_texture;
    const void *color0_tex;
    /* State live at the last recorded draw (same snapshot the recording
     * thread warmed the pipeline with). */
    nvn_draw_snap_t last;
    int have_last;
    /* Per-target segments (split at SetRenderTargets). */
    nvn_seg_t *seg;
    uint32_t nseg;
} nvn_retired_t;


/* Retired offscreen passes waiting to be rendered before the next window
 * compositor submit.  The scene worker retires its command buffer; the main
 * thread's submit drains this FIFO first so the compositor samples a scene
 * that was rendered in this frame. */
typedef struct {
    uint64_t handle;
    uint32_t seg;
    uint64_t seq;
    uint32_t prio;
    const void *target;   /* guest colour-0 texture, for the window's 0x3f lookup */
    uint32_t width, height, draws;   /* segment target shape / draw count */
} nvn_scene_item_t;



/* Program, sync, event                                                */

/* Host-only state for the offline SPIR-V cache.  The guest NVNprogram object
 * stays 0xC0 bytes; only this pointer is added to it. */
struct prog_shader_state {
    uint8_t *spv[RUNTIME_SHADER_STAGES];
    size_t spv_size[RUNTIME_SHADER_STAGES];
    char spv_path[RUNTIME_SHADER_STAGES][RUNTIME_SHADER_PATH_MAX];
    char res_path[RUNTIME_SHADER_STAGES][RUNTIME_SHADER_PATH_MAX];
    char key[RUNTIME_SHADER_STAGES][RUNTIME_SHADER_KEY_MAX + 1];
    RuntimeShaderIdentity id[RUNTIME_SHADER_STAGES];
    /* Bind-state variant (<key>-<suffix>.spv), loaded lazily at submit. */
    uint8_t *vspv[2];
    size_t vspv_size[2];
    char vspv_path[2][RUNTIME_SHADER_PATH_MAX];
    char vres_path[2][RUNTIME_SHADER_PATH_MAX];
    char variant[192];
    uint64_t variant_sig;
    unsigned loaded;                 /* bit per stage with a loaded module */
    GfxReflectedPipeline *pipeline;  /* most recently created pipeline */
    /* Stage-5 (compute) pipeline, built lazily from the cached SPIR-V. */
    GfxComputePipeline *compute_pipeline;
    GfxBindingSpec compute_specs[64];
    uint32_t compute_nspecs;
    int pipeline_format;
    uint64_t layout_sig;             /* render-target layout in the cache key */
    GfxRenderTargetLayout pipeline_layout;
    uint64_t vertex_sig;
    /* A recording can draw the same program into several target layouts
     * (scene + UI atlases), so keep one pipeline per layout/variant key. */
    GfxReflectedPipeline *pipes[NVN_PROG_PIPE_SLOTS];
    int pipe_fmt[NVN_PROG_PIPE_SLOTS];
    uint64_t pipe_layout_sig[NVN_PROG_PIPE_SLOTS];
    uint64_t pipe_vertex_sig[NVN_PROG_PIPE_SLOTS];
    uint64_t pipe_variant_sig[NVN_PROG_PIPE_SLOTS];
    unsigned npipes, pipe_next;
    void *owner;                     /* guest program this state belongs to */
    /* Host-private program resources (never stored in the guest NVNprogram). */
    void *gfx_pipeline;              /* basic triangle pipeline */
    void *gfx_textured_pipeline;     /* basic textured triangle pipeline */
    void *gfx_texture;               /* texture used by the textured pipeline */
    GfxReflectedPipeline *gfx_reflected_pipeline; /* full reflected pipeline */
    uint32_t textured;
    /* Reflection used to build the current pipeline, for the per-submit
     * slot->binding bridge (especially NVN uniform-buffer index vs shader
     * constant-bank slot). */
    GfxBindingSpec specs[2][64];
    uint32_t nspecs[2];
    unsigned refs;                   /* retired segments still using this state */
    int orphan;                      /* guest program finalized */
};



/* Host-side program -> shader-state map.  NVNprogram is an opaque 0xC0-byte
 * guest object; keeping host pointers inside it lets guest writes clobber
 * them (TOTK overwrites the low half of the old inline shader_state field
 * with 0x0003 before the first draw).  The map lives outside guest memory. */
typedef struct prog_state_slot {
    struct prog_state_slot *next;
    prog_t *program;
    prog_shader_state_t *state;
} prog_state_slot_t;



typedef struct {
    void *gfx_pipeline;
    void *gfx_textured_pipeline;
    void *gfx_texture;
    GfxReflectedPipeline *gfx_reflected_pipeline;
    uint32_t textured;
} prog_host_t;



/* Decode the recorded NVN attribute/stream state.  `va_st` is packed as
 * {fmt:8, offset:19, stream:5}; `vs_st` is {stride, divisor}. */
/* NVN *_UI2F / *_I2F vertex formats store unnormalised integers but the
 * shader reads floats.  Vulkan has no equivalent vertex-input conversion, so
 * the attribute is expanded into a private float stream (float bits reinterpret
 * the integer bytes otherwise, which collapses UI quads to near-zero size). */
typedef struct { uint8_t comps, bytes, is_signed, convert; } nvn_vfmt_t;



/* Apply the last UBO/SSBO/texture state recorded by the command buffer.  This
 * is a per-submit state model; NVN games normally bind then draw in order. */
/* Reflected UBO slots of the current pipeline, keyed by the pipeline pointer.
 * nvn_apply_bind_state runs on the submitting thread, where the guest program
 * object may already be recycled; never dereference prog_t there. */
typedef struct {
    GfxReflectedPipeline *rp;
    uint32_t nubo[2];
    int32_t ubo_slot[2][16];
    uint32_t ntex[2];
    int32_t tex_slot[2][16];
} nvn_pipe_ubo_t;



/* Diagnostic: which NVN uniform-buffer index/size actually fed each shader
 * cbuf slot in the most recent nvn_apply_bind_state.  The UI vertex shader
 * reads vp_c3 (slot 3) rows 1..6/33 and vp_c1 (slot 1), so the mapping has to
 * be observed rather than assumed. */
typedef struct { int32_t nvn; uint64_t addr; size_t size; int valid; const char *src; } nvn_ubo_dec_t;



/* ------------------------------------------------------------------ */
/* Recorded GPU transfer / compute replay                              */

typedef struct {
    unsigned clear_ok, clear_fail;
    unsigned copy_ok, copy_fail;
    unsigned upload_ok, upload_fail;
    unsigned download_ok, download_fail;
    unsigned dispatch_ok, dispatch_fail;
    unsigned rt_clear_ok, rt_unsupported;
    unsigned unsupported;
} nvn_gpu_stats_t;



/* --- Resource/vertex probe helpers --------------------------------------- */

typedef struct {
    uint8_t entries[2][16];
    void *controls[2];
} nvn_probe_pair_t;


typedef struct { const char *name; void (*fn)(void); } nvn_entry;

#endif
