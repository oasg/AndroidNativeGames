/* NVN module: probe_0. */
#include "nvn_internal.h"

int runtime_nvn_submit_selftest(void) {
    void *nw = runtime_native_window_create(1280, 720, 0x25, 0x77);
    if (!nw) return 0;
    dev_builder_t db;
    nvn_dev_t d;
    queue_builder_t qb;
    queue_t q;
    win_builder_t wb;
    win_t w;
    memset(&db, 0, sizeof db);
    memset(&d, 0, sizeof d);
    if (!nvnDeviceInitialize(&d, &db)) return 0;
    memset(&qb, 0, sizeof qb);
    qb.dev = &d;
    qb.flags = 1;
    qb.cmd_sz = 0x10000;
    qb.ctl_sz = 0x1000;
    qb.mem_sz = 0x2000;
    memset(&q, 0, sizeof q);
    if (!nvnQueueInitialize(&q, &qb)) return 0;
    memset(&wb, 0, sizeof wb);
    wb.dev = &d;
    wb.native = nw;
    wb.ntex = 1;
    wb.tex[0] = (void *)(uintptr_t)1;
    memset(&w, 0, sizeof w);
    if (!nvnWindowInitialize(&w, &wb)) return 0;
    uint64_t handle = 0x100000001ull;
    nvnQueueSubmitCommands(&q, 1, &handle);
    if (!runtime_native_window_clear(nw, 0xff102030u)) return 0;
    nvnQueuePresentTexture(&q, &w, 0);
    if (q.submits != 1 || w.presents != 1) return 0;
    nvnWindowFinalize(&w);
    return 1;
}

int runtime_nvn_window_selftest(void *nw) {
    win_builder_t b;
    win_t w;
    memset(&b, 0, sizeof b);
    memset(&w, 0, sizeof w);
    b.native = nw;
    b.ntex = 1;
    b.tex[0] = (void *)(uintptr_t)1;
    if (!nvnWindowInitialize(&w, &b)) return 0;
    if (nvnWindowGetNativeWindow(&w) != nw) return 0;
    if (nvnWindowGetNumTextures(&w) != 1) return 0;
    nvnWindowFinalize(&w);
    return 1;
}

int runtime_nvn_draw_selftest(const char *screenshot, int frames,
                              const char *vs_path, const char *ps_path, int expect_visible) {
    if (frames < 1) frames = 1;
    if (frames > 60) frames = 60;
    void *nw = runtime_native_window_create(1280, 720, 0x25, 0x88);
    if (!nw) return 0;
    nvn_dev_t d;
    dev_builder_t db;
    memset(&d, 0, sizeof d);
    memset(&db, 0, sizeof db);
    if (!nvnDeviceInitialize(&d, &db)) return 0;
    queue_t q;
    queue_builder_t qb;
    memset(&q, 0, sizeof q);
    memset(&qb, 0, sizeof qb);
    qb.dev = &d;
    qb.flags = 1;
    qb.cmd_sz = 0x10000;
    qb.ctl_sz = 0x1000;
    qb.mem_sz = 0x2000;
    if (!nvnQueueInitialize(&q, &qb)) return 0;
    prog_t p;
    memset(&p, 0, sizeof p);
    if (!nvnProgramInitialize(&p, &d)) return 0;
    if (vs_path && *vs_path && ps_path && *ps_path) {
        if (!runtime_nvn_program_load_spirv(&p, vs_path, ps_path)) return 0;
    } else {
        GfxTrianglePipeline *bp = gfx_backend_builtin_triangle_pipeline();
        prog_host_set(&p, bp, NULL, NULL, NULL, 0);
        if (!bp) return 0;
    }
    win_t w;
    win_builder_t wb;
    memset(&w, 0, sizeof w);
    memset(&wb, 0, sizeof wb);
    wb.dev = &d;
    wb.native = nw;
    wb.ntex = 1;
    wb.tex[0] = (void *)(uintptr_t)1;
    if (!nvnWindowInitialize(&w, &wb)) return 0;
    cb_t cb;
    if (!nvnCommandBufferInitialize(&cb, &d)) return 0;
    for (int i = 0; i < frames; i++) {
        cb.prog = &p;
        cb.win = &w;
        nvnCommandBufferBeginRecording(&cb);
        nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
        uint64_t handle = nvnCommandBufferEndRecording(&cb);
        nvnQueueSubmitCommands(&q, 1, &handle);
        nvnQueuePresentTexture(&q, &w, 0);
    }
    nvnCommandBufferFinalize(&cb);
    nvnWindowFinalize(&w);
    uint8_t center[4] = {0}, corner[4] = {0};
    runtime_native_window_read_pixel(nw, 640, 360, center);
    runtime_native_window_read_pixel(nw, 0, 0, corner);
    if (screenshot && *screenshot) runtime_native_window_screenshot(nw, screenshot);
    int ok = expect_visible ? (center[0] > 200 && center[1] > 60 && center[1] < 100 &&
                               center[2] < 40 && corner[0] < 40) : 1;
    if (!expect_visible)
        fprintf(stderr, "[totk-nvn] translated SPIR-V draw executed; pixel visibility not asserted\n");
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] NVN draw selftest center=%02x%02x%02x%02x corner=%02x%02x%02x%02x\n",
                center[0], center[1], center[2], center[3], corner[0], corner[1], corner[2], corner[3]);
    return ok;
}

/* Regression check for the NVN command-buffer submission registry.  The guest
 * keeps many command buffers alive at once and recycles their storage after
 * nvnCommandBufferFinalize, so a submitted handle must never resolve to a
 * finalized or recycled object. */
int runtime_nvn_cb_lifetime_selftest(void) {
    enum { LIVE = 96 };
    void *nw = runtime_native_window_create(1280, 720, 0x25, 0xC8);
    if (!nw) return 0;
    nvn_dev_t d;
    dev_builder_t db;
    memset(&d, 0, sizeof d);
    memset(&db, 0, sizeof db);
    if (!nvnDeviceInitialize(&d, &db)) { runtime_native_window_destroy(nw); return 0; }
    queue_t q;
    queue_builder_t qb;
    memset(&q, 0, sizeof q);
    memset(&qb, 0, sizeof qb);
    qb.dev = &d; qb.flags = 1; qb.cmd_sz = 0x10000; qb.ctl_sz = 0x1000; qb.mem_sz = 0x2000;
    if (!nvnQueueInitialize(&q, &qb)) { runtime_native_window_destroy(nw); return 0; }
    prog_t p;
    memset(&p, 0, sizeof p);
    if (!nvnProgramInitialize(&p, &d)) { runtime_native_window_destroy(nw); return 0; }
    prog_host_set(&p, gfx_backend_builtin_triangle_pipeline(), NULL, NULL, NULL, 0);
    win_t w;
    win_builder_t wb;
    memset(&w, 0, sizeof w);
    memset(&wb, 0, sizeof wb);
    wb.dev = &d; wb.native = nw; wb.ntex = 1; wb.tex[0] = (void *)(uintptr_t)1;
    if (!nvnWindowInitialize(&w, &wb)) { runtime_native_window_destroy(nw); return 0; }
    cb_t *cbs = calloc(LIVE, sizeof *cbs);
    if (!cbs) { runtime_native_window_destroy(nw); return 0; }

    /* More live command buffers than the old fixed table held (64). */
    unsigned before = nvn_nvn_draws;
    for (int i = 0; i < LIVE; i++) {
        if (!nvnCommandBufferInitialize(&cbs[i], &d)) continue;
        cbs[i].prog = &p;
        cbs[i].win = &w;
        nvnCommandBufferBeginRecording(&cbs[i]);
        nvnCommandBufferDrawArrays(&cbs[i], NVN_DRAW_TRIANGLES, 0, 3);
        uint64_t handle = nvnCommandBufferEndRecording(&cbs[i]);
        nvnQueueSubmitCommands(&q, 1, &handle);
        nvnQueuePresentTexture(&q, &w, 0);
    }
    unsigned live_draws = nvn_nvn_draws - before;
    int ok = live_draws == LIVE;

    /* A finalized handle is dead: resubmitting it must not touch the object. */
    uint64_t stale = cbs[0].handle;
    nvnCommandBufferFinalize(&cbs[0]);
    before = nvn_nvn_draws;
    nvnQueueSubmitCommands(&q, 1, &stale);
    unsigned stale_draws = nvn_nvn_draws - before;
    ok &= stale_draws == 0;

    /* The same storage is reusable and must draw again. */
    if (!nvnCommandBufferInitialize(&cbs[0], &d)) ok = 0;
    cbs[0].prog = &p;
    cbs[0].win = &w;
    nvnCommandBufferBeginRecording(&cbs[0]);
    nvnCommandBufferDrawArrays(&cbs[0], NVN_DRAW_TRIANGLES, 0, 3);
    uint64_t again = nvnCommandBufferEndRecording(&cbs[0]);
    nvnQueueSubmitCommands(&q, 1, &again);
    int reused = nvn_nvn_draws == before + 1;
    ok &= reused;

    for (int i = 0; i < LIVE; i++) nvnCommandBufferFinalize(&cbs[i]);
    fprintf(stderr, "[totk-nvn] cb lifetime selftest live=%u/%u stale_draws=%u reuse=%d ok=%d\n",
            live_draws, (unsigned)LIVE, stale_draws, reused, ok);
    free(cbs);
    nvnWindowFinalize(&w);
    nvnProgramFinalize(&p);
    runtime_native_window_destroy(nw);
    return ok;
}

/* Regression check for NVN command-handle lifetime: a recorded draw list must
 * survive nvnCommandBufferFinalize and remain executable through
 * nvnCommandBufferCallCommands.  The guest command-buffer object is clobbered
 * after finalize to prove the replay uses an immutable snapshot, not stale
 * guest memory. */
int runtime_nvn_cb_call_selftest(void) {
    void *nw = runtime_native_window_create(640, 480, 0x25, 0xCF);
    if (!nw) return 0;
    nvn_dev_t d;
    dev_builder_t db;
    queue_t q;
    queue_builder_t qb;
    memset(&d, 0, sizeof d);
    memset(&db, 0, sizeof db);
    memset(&q, 0, sizeof q);
    memset(&qb, 0, sizeof qb);
    int ok = nvnDeviceInitialize(&d, &db) != 0;
    qb.dev = &d; qb.flags = 1; qb.cmd_sz = 0x10000; qb.ctl_sz = 0x1000; qb.mem_sz = 0x2000;
    ok &= nvnQueueInitialize(&q, &qb) != 0;
    prog_t p;
    memset(&p, 0, sizeof p);
    ok &= nvnProgramInitialize(&p, &d) != 0;
    GfxTrianglePipeline *bp = gfx_backend_builtin_triangle_pipeline();
    prog_host_set(&p, bp, NULL, NULL, NULL, 0);
    ok &= bp != NULL;
    win_t w;
    win_builder_t wb;
    memset(&w, 0, sizeof w);
    memset(&wb, 0, sizeof wb);
    wb.dev = &d; wb.native = nw; wb.ntex = 1; wb.tex[0] = (void *)(uintptr_t)1;
    ok &= nvnWindowInitialize(&w, &wb) != 0;

    cb_t src;
    memset(&src, 0, sizeof src);
    ok &= nvnCommandBufferInitialize(&src, &d) != 0;
    src.prog = &p;
    src.win = &w;
    nvnCommandBufferBeginRecording(&src);
    for (int i = 0; i < 6; i++) nvnCommandBufferDrawArrays(&src, NVN_DRAW_TRIANGLES, 0, 3);
    uint64_t handle = nvnCommandBufferEndRecording(&src);
    unsigned src_cmds = src.ncmds;
    nvnCommandBufferFinalize(&src);
    /* Command memory is gone; clobber the guest object to prove the replay
     * cannot depend on it. */
    memset(&src, 0xaa, sizeof src);

    cb_t dst;
    memset(&dst, 0, sizeof dst);
    ok &= nvnCommandBufferInitialize(&dst, &d) != 0;
    dst.prog = &p;
    dst.win = &w;
    nvnCommandBufferBeginRecording(&dst);
    nvnCommandBufferCallCommands(&dst, 1, &handle);
    unsigned draws = 0;
    for (uint32_t i = 0; i < dst.ncmds; i++)
        if (dst.cmds[i].kind == NVN_CMD_DRAW) draws++;
    uint64_t dh = nvnCommandBufferEndRecording(&dst);
    unsigned before = nvn_nvn_draws;
    nvnQueueSubmitCommands(&q, 1, &dh);
    int executed = nvn_nvn_draws > before;
    uint8_t center[4] = {0};
    runtime_native_window_read_pixel(nw, 320, 240, center);
    int visible = center[0] > 200 && center[1] > 60 && center[1] < 100 && center[2] < 40;
    ok &= src_cmds == 6 && draws == 6 && dst.draw_count == 18 && executed && visible;
    fprintf(stderr, "[totk-nvn] cb-call probe src_cmds=%u merged_draws=%u merged_cmds=%u"
                    " draw_count=%d executed=%d center=%02x%02x%02x%02x ok=%d\n",
            src_cmds, draws, dst.ncmds, dst.draw_count, executed,
            center[0], center[1], center[2], center[3], ok);
    nvnCommandBufferFinalize(&dst);
    nvnWindowFinalize(&w);
    nvnProgramFinalize(&p);
    runtime_native_window_destroy(nw);
    return ok;
}

int runtime_nvn_pool_lifetime_selftest(void) {
    enum { POOLS = 40, PER_POOL = 1000 };
    tex_pool_t pools[POOLS];
    pool_t mem;
    tex_t tex;
    memset(&mem, 0, sizeof mem);
    mem.magic = NVN_MAGIC('P', 'O', 'O', 'L');
    memset(&tex, 0, sizeof tex);
    tex.magic = NVN_MAGIC('T', 'E', 'X', 0);
    int ok = 1;
    g_tex_pool_max = g_tex_pool_map_count;
    for (unsigned g = 0; g < POOLS; g++) {
        if (!nvnTexturePoolInitialize(&pools[g], &mem, 0, 100000)) { ok = 0; break; }
        for (unsigned i = 0; i < PER_POOL; i++)
            nvnTexturePoolRegisterTexture(&pools[g], (int)(g * PER_POOL + i), &tex, NULL);
        if (nvn_tex_pool_lookup(&pools[g], (int)(g * PER_POOL + PER_POOL - 1)) != &tex) ok = 0;
        nvn_pool_epoch_tick();
        nvn_pool_epoch_tick();
    }
    /* The current pool resolves; a pool unused for many epochs is pruned. */
    if (nvn_tex_pool_lookup(&pools[POOLS - 1], (int)((POOLS - 1) * PER_POOL)) != &tex) ok = 0;
    if (nvn_tex_pool_lookup(&pools[0], 0) != NULL) ok = 0;
    /* Same pool object, next generation: old ids must not resolve. */
    if (!nvnTexturePoolInitialize(&pools[0], &mem, 0, 100000)) ok = 0;
    nvnTexturePoolRegisterTexture(&pools[0], 7, &tex, NULL);
    if (nvn_tex_pool_lookup(&pools[0], 7) != &tex) ok = 0;
    if (!nvnTexturePoolInitialize(&pools[0], &mem, 0, 100000)) ok = 0;
    if (nvn_tex_pool_lookup(&pools[0], 7) != NULL) ok = 0;
    if (runtime_nvn_tex_pool_regs() > NVN_POOL_MAP_MAX) ok = 0;
    if (runtime_nvn_tex_pool_full() != 0) ok = 0;
    for (unsigned g = 0; g < POOLS; g++) nvnTexturePoolFinalize(&pools[g]);
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] pool lifetime selftest %s regs=%u max=%u full=%u\n",
                ok ? "PASS" : "FAIL", runtime_nvn_tex_pool_regs(),
                runtime_nvn_tex_pool_max(), runtime_nvn_tex_pool_full());
    return ok;
}

int runtime_nvn_bind_selftest(void) {    tex_t tex;
    samp_t samp;
    tex_pool_t tp;
    samp_pool_t sp;
    cb_t cb;
    memset(&tex, 0, sizeof tex);
    memset(&samp, 0, sizeof samp);
    memset(&tp, 0, sizeof tp);
    memset(&sp, 0, sizeof sp);
    memset(&cb, 0, sizeof cb);
    tex.magic = NVN_MAGIC('T', 'E', 'X', 0);
    samp.magic = NVN_MAGIC('S', 'A', 'M', 'P');
    tp.magic = NVN_MAGIC('T', 'P', 'O', 'L'); tp.n = 4;
    sp.magic = NVN_MAGIC('S', 'P', 'O', 'L'); sp.n = 4;
    cb.magic = NVN_MAGIC('C', 'M', 'D', 'B');
    nvn_tex_pool_add(&tp, 3, &tex);
    nvn_samp_pool_add(&sp, 2, &samp);
    nvnCommandBufferSetTexturePool(&cb, &tp);
    nvnCommandBufferSetSamplerPool(&cb, &sp);

    uint64_t h = nvnDeviceGetTextureHandle(NULL, 3, 2);
    int ok = (h & 0xfffffu) == 3 && ((h >> 20) & 0xfffu) == 2;
    ok &= nvn_tex_pool_lookup(&tp, 3) == &tex;
    ok &= nvn_samp_pool_lookup(&sp, 2) == &samp;
    nvnCommandBufferBindTexture(&cb, 5, 0, h);
    nvnCommandBufferBindTexture(&cb, 5, 1, nvnDeviceGetSeparateTextureHandle(NULL, 3));
    nvnCommandBufferBindImage(&cb, 5, 0, nvnDeviceGetImageHandle(NULL, 3));
    nvnCommandBufferBindUniformBuffer(&cb, 0, 4, 0x1000, 0x100);
    nvnCommandBufferBindStorageBuffer(&cb, 5, 0, 0x2000, 0x100);
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] bind selftest handle=%#llx ok=%d\n",
                (unsigned long long)h, ok);
    nvn_tex_pool_forget(&tp);
    nvn_samp_pool_forget(&sp);
    return ok;
}

int runtime_nvn_draw_reflect_selftest(const char *screenshot, int frames,
                                      const char *vs_path, const char *ps_path,
                                      const char *vs_res, const char *ps_res) {
    if (frames < 1) frames = 1;
    if (frames > 60) frames = 60;
    void *nw = runtime_native_window_create(1280, 720, 0x25, 0xA1);
    if (!nw) return 0;
    nvn_dev_t d;
    dev_builder_t db;
    memset(&d, 0, sizeof d);
    memset(&db, 0, sizeof db);
    if (!nvnDeviceInitialize(&d, &db)) return 0;
    queue_t q;
    queue_builder_t qb;
    memset(&q, 0, sizeof q);
    memset(&qb, 0, sizeof qb);
    qb.dev = &d; qb.flags = 1; qb.cmd_sz = 0x10000; qb.ctl_sz = 0x1000; qb.mem_sz = 0x2000;
    if (!nvnQueueInitialize(&q, &qb)) return 0;
    prog_t p;
    memset(&p, 0, sizeof p);
    if (!nvnProgramInitialize(&p, &d)) return 0;
    if (!runtime_nvn_program_load_spirv_reflect(&p, vs_path, ps_path, vs_res, ps_res)) return 0;
    win_t w;
    win_builder_t wb;
    memset(&w, 0, sizeof w);
    memset(&wb, 0, sizeof wb);
    wb.dev = &d; wb.native = nw; wb.ntex = 1; wb.tex[0] = (void *)(uintptr_t)1;
    if (!nvnWindowInitialize(&w, &wb)) return 0;
    cb_t cb;
    memset(&cb, 0, sizeof cb);
    if (!nvnCommandBufferInitialize(&cb, &d)) return 0;
    for (int i = 0; i < frames; i++) {
        cb.prog = &p;
        cb.win = &w;
        nvnCommandBufferBeginRecording(&cb);
        nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
        uint64_t handle = nvnCommandBufferEndRecording(&cb);
        nvnQueueSubmitCommands(&q, 1, &handle);
        nvnQueuePresentTexture(&q, &w, 0);
    }
    nvnCommandBufferFinalize(&cb);
    nvnWindowFinalize(&w);
    uint8_t center[4] = {0}, corner[4] = {0};
    runtime_native_window_read_pixel(nw, 640, 360, center);
    runtime_native_window_read_pixel(nw, 0, 0, corner);
    if (screenshot && *screenshot) runtime_native_window_screenshot(nw, screenshot);
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] reflected draw selftest center=%02x%02x%02x%02x corner=%02x%02x%02x%02x\n",
                center[0], center[1], center[2], center[3], corner[0], corner[1], corner[2], corner[3]);
    nvnProgramFinalize(&p);
    return 1;
}

void nvn_probe_pair_free(nvn_probe_pair_t *pair) {
    if (!pair) return;
    for (unsigned i = 0; i < 2; i++) {
        free(pair->controls[i]);
        pair->controls[i] = NULL;
    }
}

/* Build a synthetic NVN control/code pair, store the translated modules under
 * the resulting keys, and bind the program.  The key derivation is the real
 * runtime path; only the control/code bytes are synthetic. */
/* Install a probe shader stage directly from its SPIR-V bytes.  The probe's
 * synthetic NVN control block is not in a guest mapping, which the shader
 * cache's pointer validation rejects; the probe only needs the host state. */
int nvn_probe_install_stage(prog_t *p, unsigned stage, const uint8_t *spv,
                                   size_t size, const char *res,
                                   const RuntimeShaderIdentity *id) {
    if (!p || stage >= RUNTIME_SHADER_STAGES || !spv || !size || !id) return 0;
    pthread_mutex_lock(&g_prog_lock);
    prog_shader_state_t *s = prog_state_find(p);
    if (!s) {
        s = calloc(1, sizeof *s);
        if (s) s->owner = p;
        if (s && !prog_state_install(p, s)) { free(s); s = NULL; }
    }
    if (!s) { pthread_mutex_unlock(&g_prog_lock); return 0; }
    prog_stage_clear(s, stage);
    RuntimeShaderModule mod;
    if (runtime_shader_cache_load(runtime_shader_cache_root(), id, &mod)) {
        s->spv[stage] = mod.spv;
        s->spv_size[stage] = mod.spv_size;
        snprintf(s->spv_path[stage], sizeof s->spv_path[stage], "%s", mod.spv_path);
        snprintf(s->res_path[stage], sizeof s->res_path[stage], "%s", mod.res_path);
        mod.spv = NULL;
        runtime_shader_cache_module_free(&mod);
    } else {
        s->spv[stage] = malloc(size);
        if (!s->spv[stage]) { pthread_mutex_unlock(&g_prog_lock); return 0; }
        memcpy(s->spv[stage], spv, size);
        s->spv_size[stage] = size;
        snprintf(s->res_path[stage], sizeof s->res_path[stage], "%s", res ? res : "");
    }
    snprintf(s->key[stage], sizeof s->key[stage], "%s", id->key);
    s->id[stage] = *id;
    s->loaded |= 1u << stage;
    pthread_mutex_unlock(&g_prog_lock);
    return 1;
}
