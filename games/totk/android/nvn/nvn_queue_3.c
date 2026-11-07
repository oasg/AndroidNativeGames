/* NVN module: queue_3. */
#include "nvn_internal.h"

int nvnQueueFenceSync(queue_t *q, sync_t *s, int condition, int flags) { nvn_api("nvnQueueFenceSync");
    if (nvn_trace_frame())
        fprintf(stderr, "[totk-frame] fence thread=%#x queue=%p sync=%p cond=%d\n",
                runtime_thread_current_handle(), (void *)q, (void *)s, condition);
    (void)flags;
    if (!nvn_obj(q, NVN_MAGIC('Q', 'U', 'E', 'U'), "nvnQueueFenceSync(queue)")) return 0;
    if (!nvn_obj(s, NVN_MAGIC('S', 'Y', 'N', 'C'), "nvnQueueFenceSync(sync)")) return 0;
    if (s->fence) {
        gfx_backend_fence_destroy((GfxFence *)s->fence);
        s->fence = NULL;
    }
    s->state = 0;
    s->condition = condition;
    s->submit_generation = nvn_gpu_submits;
    s->fence = gfx_backend_fence_create();
    if (!s->fence) {
        nvn_fail("nvnQueueFenceSync", "backend fence creation failed");
        return 0;
    }
    s->fence_generation++;
    return nvn_sync_complete(s, "nvnQueueFenceSync");
}
