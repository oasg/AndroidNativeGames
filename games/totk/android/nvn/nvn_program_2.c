/* NVN module: program_2. */
#include "nvn_internal.h"

void nvn_pipe_ubo_set(GfxReflectedPipeline *rp,
                             const GfxBindingSpec *vss, uint32_t nvs,
                             const GfxBindingSpec *pss, uint32_t nps) {
    if (!rp) return;
    pthread_mutex_lock(&g_pipe_ubo_lock);
    nvn_pipe_ubo_t *e = NULL;
    for (unsigned i = 0; i < g_pipe_ubo_count; i++)
        if (g_pipe_ubo[i].rp == rp) { e = &g_pipe_ubo[i]; break; }
    if (!e && g_pipe_ubo_count < sizeof g_pipe_ubo / sizeof g_pipe_ubo[0])
        e = &g_pipe_ubo[g_pipe_ubo_count++];
    if (e) {
        memset(e, 0, sizeof *e);
        e->rp = rp;
        const GfxBindingSpec *src[2] = { vss, pss };
        uint32_t cnt[2] = { nvs, nps };
        for (int st = 0; st < 2; st++) {
            /* cbuf slot 0 is NVN's driver "support buffer" whenever the shader
             * also has a game cbuf (c1..); the guest binds game UBOs through
             * BindUniformBuffer indices.  A shader whose only UBO is c0 (the
             * synthetic probes) keeps c0 as its game UBO. */
            int has_game = 0;
            for (uint32_t k = 0; k < cnt[st]; k++)
                if (src[st][k].kind == GFX_BIND_UBO && src[st][k].nvn_slot > 0) has_game = 1;
            for (uint32_t k = 0; k < cnt[st] && e->nubo[st] < 16; k++) {
                if (src[st][k].kind != GFX_BIND_UBO) continue;
                if (src[st][k].nvn_slot < 0) continue;
                if (has_game && src[st][k].nvn_slot == 0) continue;
                e->ubo_slot[st][e->nubo[st]++] = src[st][k].nvn_slot;
            }
            for (uint32_t k = 0; k < cnt[st] && e->ntex[st] < 16; k++) {
                if (src[st][k].kind != GFX_BIND_COMBINED_IMAGE_SAMPLER &&
                    src[st][k].kind != GFX_BIND_SAMPLED_IMAGE) continue;
                e->tex_slot[st][e->ntex[st]++] = src[st][k].nvn_slot;
            }
        }
    }
    pthread_mutex_unlock(&g_pipe_ubo_lock);
}

const nvn_pipe_ubo_t *nvn_pipe_ubo_get(GfxReflectedPipeline *rp) {
    const nvn_pipe_ubo_t *found = NULL;
    pthread_mutex_lock(&g_pipe_ubo_lock);
    for (unsigned i = 0; i < g_pipe_ubo_count; i++)
        if (g_pipe_ubo[i].rp == rp) { found = &g_pipe_ubo[i]; break; }
    pthread_mutex_unlock(&g_pipe_ubo_lock);
    return found;
}
