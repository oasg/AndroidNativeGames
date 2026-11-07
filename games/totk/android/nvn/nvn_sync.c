/* NVN module: sync. */
#include "nvn_internal.h"

uint32_t nvnSyncInitialize(sync_t *s, const nvn_dev_t *d) { nvn_api("nvnSyncInitialize");
    if (!s || !nvn_obj(d, NVN_MAGIC('D', 'E', 'V', 0), "nvnSyncInitialize(device)")) return 0;
    memset(s, 0, sizeof *s);
    s->magic = NVN_MAGIC('S', 'Y', 'N', 'C');
    return 1;
}

void nvnSyncFinalize(sync_t *s) {
    if (!s) return;
    if (s->fence) gfx_backend_fence_destroy((GfxFence *)s->fence);
    s->fence = NULL;
    s->magic = 0;
}

void nvnSyncSetDebugLabel(sync_t *s, const char *l) { (void)s; (void)l; }

/* Signal only after the queue reports completion: a fence is never reported
 * signaled ahead of the GPU work submitted before it. */
int nvn_sync_complete(sync_t *s, const char *what) {
    if (!s || !s->fence) return 0;
    if (s->state) return 1;
    if (!nvn_gpu_drain(what)) return 0;
    gfx_backend_fence_signal((GfxFence *)s->fence);
    s->state = 1;
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] %s signaled after GPU completion (sync=%p)\n", what, (void *)s);
    return 1;
}

int nvnSyncWait(const sync_t *s, uint64_t timeout) {
    if (!s || s->magic != NVN_MAGIC('S', 'Y', 'N', 'C')) {
        nvn_fail("nvnSyncWait", "invalid sync");
        return NVN_SYNC_WAIT_FAILED;
    }
    if (s->state) return NVN_SYNC_WAIT_ALREADY_SIGNALED;
    if (!s->fence) return NVN_SYNC_WAIT_ALREADY_SIGNALED;  /* never fenced */
    int r = gfx_backend_fence_wait((GfxFence *)s->fence, timeout);
    if (r > 0) {
        ((sync_t *)s)->state = 1;   /* the guest owns the mutable sync object */
        return NVN_SYNC_WAIT_CONDITION_SATISFIED;
    }
    if (r == 0) return NVN_SYNC_WAIT_TIMEOUT_EXPIRED;
    return NVN_SYNC_WAIT_FAILED;
}

uint32_t nvnSyncInitializeFromFencedGLSync(sync_t *s, const nvn_dev_t *d, const void *gl) {
    (void)gl;
    return nvnSyncInitialize(s, d);
}

void *nvnSyncCreateGLSync(const sync_t *s) { (void)s; return NULL; }

void nvnEventBuilderSetDefaults(ev_builder_t *b) { if (b) memset(b, 0, sizeof *b); }

void nvnEventBuilderSetStorage(ev_builder_t *b, const pool_t *p, int64_t off) {
    if (b) { b->pool = (pool_t *)p; b->off = off; }
}

void nvnEventBuilderGetStorage(const ev_builder_t *b, void **pool, int64_t *off) {
    if (pool) *pool = b ? b->pool : NULL;
    if (off) *off = b ? b->off : 0;
}

void *nvnEventBuilderGetMemoryPool(const ev_builder_t *b) { return b ? b->pool : NULL; }

int64_t nvnEventBuilderGetMemoryOffset(const ev_builder_t *b) { return b ? b->off : 0; }

uint32_t nvnEventInitialize(ev_t *e, const ev_builder_t *b) { nvn_api("nvnEventInitialize");
    if (!e || !b) { nvn_fail("nvnEventInitialize", "null event/builder"); return 0; }
    if (!nvn_obj(b->pool, NVN_MAGIC('P', 'O', 'O', 'L'), "nvnEventInitialize(pool)")) return 0;
    if (b->off < 0 || (uint64_t)b->off + 4 > b->pool->size || !b->pool->mem) {
        nvn_fail("nvnEventInitialize", "event storage is not mapped");
        return 0;
    }
    memset(e, 0, sizeof *e);
    e->magic = NVN_MAGIC('E', 'V', 'N', 'T');
    e->pool = b->pool;
    e->off = b->off;
    e->val = (volatile uint32_t *)(b->pool->mem + b->off);
    return 1;
}

void nvnEventFinalize(ev_t *e) { if (e) e->magic = 0; }

uint32_t nvnEventGetValue(const ev_t *e) {
    if (!e || e->magic != NVN_MAGIC('E', 'V', 'N', 'T') || !e->val) return 0;
    return *e->val;
}

void nvnEventSignal(ev_t *e, int mode, uint32_t v) {
    if (!e || e->magic != NVN_MAGIC('E', 'V', 'N', 'T') || !e->val) {
        nvn_fail("nvnEventSignal", "invalid event");
        return;
    }
    if (mode == NVN_EVENT_SIGNAL_MODE_ADD) *e->val += v;
    else *e->val = v;
}

void *nvnEventGetMemoryPool(const ev_t *e) { return e ? e->pool : NULL; }

int64_t nvnEventGetMemoryOffset(const ev_t *e) { return e ? e->off : 0; }
