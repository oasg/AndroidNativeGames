/* NVN module: replay_1. */
#include "nvn_internal.h"

/* Map the NVN descriptor state of one dispatch onto the reflected SPIR-V
 * bindings, then run it.  Every required binding must resolve; a missing one
 * fails the dispatch instead of running it with a dummy resource. */
int nvn_exec_dispatch(const nvn_cmd_t *cmd) {
    prog_shader_state_t *s = cmd->u.dispatch.pstate;
    const nvn_dispatch_snap_t *ds = cmd->u.dispatch.bind;
    const nvn_bind_state_t *b = ds ? &ds->state : NULL;
    if (!s || !b) {
        g_gpu_stats.dispatch_fail++;
        fprintf(stderr, "[totk-nvn] DispatchCompute %ux%ux%u: no retained program/bind"
                        " snapshot\n", cmd->u.dispatch.x, cmd->u.dispatch.y,
                cmd->u.dispatch.z);
        return 0;
    }
    GfxBindingSpec specs[64];
    uint32_t nspecs = 0;
    GfxComputePipeline *cp = nvn_compute_pipeline_get(s, specs, &nspecs);
    if (!cp) {
        g_gpu_stats.dispatch_fail++;
        fprintf(stderr, "[totk-nvn] DispatchCompute %ux%ux%u: program=%p has no compute"
                        " SPIR-V in the cache\n", cmd->u.dispatch.x, cmd->u.dispatch.y,
                cmd->u.dispatch.z, cmd->u.dispatch.prog);
        return 0;
    }
    int ok = 1;
    unsigned bound_buffers = 0, bound_tex = 0;
    for (uint32_t i = 0; i < nspecs && ok; i++) {
        const GfxBindingSpec *sp = &specs[i];
        if (sp->kind == GFX_BIND_UBO || sp->kind == GFX_BIND_SSBO) {
            int slot = sp->nvn_slot;
            if (slot < 0 || slot >= 16) {
                fprintf(stderr, "[totk-nvn] compute binding %u (%s) has no NVN slot\n",
                        sp->binding, sp->kind == GFX_BIND_UBO ? "ubo" : "ssbo");
                ok = 0;
                break;
            }
            const uint64_t addr = sp->kind == GFX_BIND_UBO
                                ? b->ubo[5][slot].address : b->ssbo[5][slot].address;
            const size_t size = sp->kind == GFX_BIND_UBO
                              ? b->ubo[5][slot].size : b->ssbo[5][slot].size;
            if (!addr || !size) {
                fprintf(stderr, "[totk-nvn] compute binding %u (%s slot %d) not bound on the"
                                " compute stage\n", sp->binding,
                        sp->kind == GFX_BIND_UBO ? "ubo" : "ssbo", slot);
                ok = 0;
                break;
            }
            if (!gfx_backend_compute_bind_buffer(cp, sp->set, sp->binding,
                                                 (const void *)(uintptr_t)addr, size)) {
                fprintf(stderr, "[totk-nvn] compute bind buffer binding=%u slot=%d size=%zu"
                                " failed\n", sp->binding, slot, size);
                ok = 0;
                break;
            }
            bound_buffers++;
            if (nvn_gpu_trace())
                fprintf(stderr, "[totk-nvn] compute bind %s binding=%u slot=%d addr=%#llx"
                                " size=%zu\n", sp->kind == GFX_BIND_UBO ? "ubo" : "ssbo",
                        sp->binding, slot, (unsigned long long)addr, size);
        } else if (sp->kind == GFX_BIND_SAMPLED_IMAGE ||
                   sp->kind == GFX_BIND_COMBINED_IMAGE_SAMPLER ||
                   sp->kind == GFX_BIND_STORAGE_IMAGE) {
            int slot = sp->nvn_slot;
            const void *tp = NULL;
            if (slot >= 0 && slot < 16) tp = ds->img_tex[slot];
            if (!tp && slot >= 0 && slot < 128) tp = ds->tex_tex[slot];
            if (!tp && slot >= 0 && slot < 16) {
                /* Bound on an earlier command buffer: queue-scoped image state. */
                uint64_t gh = 0;
                const tex_t *gt = NULL;
                if (nvn_global_img_get(5, slot, &gh, &gt) && gt) tp = gt;
            }
            if (!tp && slot >= 0 && slot < 128) {
                uint64_t gh = 0;
                const tex_t *gt = NULL;
                if (nvn_global_tex_get(5, slot, &gh, &gt) && gt) tp = gt;
            }
            const tex_t *t = nvn_tex_registry_find(tp);            if (!t) {
                fprintf(stderr, "[totk-nvn] compute binding %u (%s slot %d) has no texture\n",
                        sp->binding, sp->kind == GFX_BIND_STORAGE_IMAGE ? "image" : "texture",
                        slot);
                ok = 0;
                break;
            }
            GfxTexture *g = nvn_texture_gfx_ready(t);
            if (!g) {
                fprintf(stderr, "[totk-nvn] compute binding %u: texture %p has no host image\n",
                        sp->binding, (const void *)t);
                ok = 0;
                break;
            }
            if (!gfx_backend_compute_bind_texture(cp, sp->set, sp->binding, g)) {
                fprintf(stderr, "[totk-nvn] compute bind texture binding=%u slot=%d failed\n",
                        sp->binding, slot);
                ok = 0;
                break;
            }
            bound_tex++;
        } else {
            fprintf(stderr, "[totk-nvn] compute binding %u kind=%d is not supported\n",
                    sp->binding, (int)sp->kind);
            ok = 0;
        }
    }
    if (!ok) {
        g_gpu_stats.dispatch_fail++;
        return 0;
    }
    if (!gfx_backend_compute_dispatch(cp, cmd->u.dispatch.x, cmd->u.dispatch.y,
                                      cmd->u.dispatch.z)) {
        g_gpu_stats.dispatch_fail++;
        fprintf(stderr, "[totk-nvn] DispatchCompute %ux%ux%u failed (bindings=%u buffers=%u"
                        " textures=%u)\n", cmd->u.dispatch.x, cmd->u.dispatch.y,
                cmd->u.dispatch.z, nspecs, bound_buffers, bound_tex);
        return 0;
    }
    /* The guest owns buffer memory: publish SSBO results back so later draws
     * (and the guest itself) observe what the shader wrote. */
    for (uint32_t i = 0; i < nspecs; i++) {
        const GfxBindingSpec *sp = &specs[i];
        if (sp->kind != GFX_BIND_SSBO) continue;
        int slot = sp->nvn_slot;
        if (slot < 0 || slot >= 16) continue;
        uint64_t addr = b->ssbo[5][slot].address;
        size_t size = b->ssbo[5][slot].size;
        if (!addr || !size) continue;
        if (!gfx_backend_compute_read_buffer(cp, sp->set, sp->binding,
                                             (void *)(uintptr_t)addr, size))
            fprintf(stderr, "[totk-nvn] compute readback binding=%u slot=%d size=%zu failed\n",
                    sp->binding, slot, size);
    }
    g_gpu_stats.dispatch_ok++;
    if (nvn_gpu_trace() || nvn_trace_frame())
        fprintf(stderr, "[totk-nvn] DispatchCompute %ux%ux%u program=%p bindings=%u"
                        " buffers=%u textures=%u\n", cmd->u.dispatch.x, cmd->u.dispatch.y,
                cmd->u.dispatch.z, cmd->u.dispatch.prog, nspecs, bound_buffers, bound_tex);
    return 1;
}

void nvn_exec_gpu_commands(const nvn_cmd_t *cmds, uint32_t ncmds,
                                  const char *where) {
    if (!cmds || !ncmds) return;
    for (uint32_t i = 0; i < ncmds; i++) {
        const nvn_cmd_t *c = &cmds[i];
        switch (c->kind) {
        case NVN_CMD_CLEAR_TEXTURE: nvn_exec_clear_texture(c); break;
        case NVN_CMD_COPY_TEXTURE_TO_TEXTURE: nvn_exec_copy_t2t(c); break;
        case NVN_CMD_COPY_BUFFER_TO_TEXTURE: nvn_exec_copy_b2t(c); break;
        case NVN_CMD_COPY_TEXTURE_TO_BUFFER: nvn_exec_copy_t2b(c); break;
        case NVN_CMD_DISPATCH: nvn_exec_dispatch(c); break;
        default: break;
        }
    }
    nvn_gpu_stats_log(where);
}

int nvn_target_first_nz(GfxOffscreenTarget *t, uint32_t *first, uint8_t center[4]) {
    if (first) *first = UINT32_MAX;
    if (center) memset(center, 0, 4);
    if (!t) return 0;
    size_t n = 0;
    const uint8_t *px = gfx_backend_offscreen_pixels(t, &n);
    if (first && px) *first = nvn_first_nonzero(px, n, 1u << 20);
    uint32_t w = gfx_backend_offscreen_width(t);
    uint32_t h = gfx_backend_offscreen_height(t);
    if (center && w && h && !gfx_backend_offscreen_read_pixel_index(t, 0, w / 2, h / 2, center))
        memset(center, 0, 4);
    return px != NULL;
}
