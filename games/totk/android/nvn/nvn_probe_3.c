/* NVN module: probe_3. */
#include "nvn_internal.h"

/* ------------------------------------------------------------------ */
/* Lane 2 probes: NVN texture copy/clear recording+replay and compute  */

/* 32x32 RGBA8 NVN texture with no guest storage; the host image is created
 * directly from linear pixels so the probe never depends on block-linear
 * deswizzling. */
int nvn_probe_make_texture(tex_t *t, uint32_t w, uint32_t h, uint32_t format,
                                  const uint8_t *pixels) {
    tex_builder_t tb;
    memset(&tb, 0, sizeof tb);
    nvnTextureBuilderSetDefaults(&tb);
    tb.d.target = NVN_TARGET_2D;
    tb.d.format = (int)format;
    tb.d.width = w;
    tb.d.height = h;
    tb.d.depth = 1;
    tb.d.levels = 1;
    tb.d.samples = 1;
    if (!nvnTextureInitialize(t, &tb)) return 0;
    if (pixels) {
        if (!runtime_nvn_texture_upload_rgba8(t, pixels, w, h)) return 0;
    }
    return 1;
}

void nvn_probe_fill(uint8_t *p, uint32_t w, uint32_t h, uint32_t seed) {
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *q = p + ((size_t)y * w + x) * 4;
            q[0] = (uint8_t)(seed + x * 3u + y * 5u);
            q[1] = (uint8_t)(seed * 3u + x * 7u + y * 11u);
            q[2] = (uint8_t)(seed * 5u + x * 13u + y * 17u);
            q[3] = (uint8_t)(0x80u + (x + y) % 0x7fu);
        }
}

int runtime_nvn_frame_selftest(const char *screenshot, int frames) {
    g_nvn_probe = 1;
    if (frames < 1) frames = 1;
    if (frames > 60) frames = 60;
    void *nw = runtime_native_window_create(1280, 720, 0x25, 0xF1);
    if (!nw) return 0;

    /* Synthetic NVN control/code pairs give the cache real keys without game
     * data.  The translated modules are the repository's triangle pair. */
    const uint8_t *vs = NULL, *ps = NULL;
    size_t vs_size = 0, ps_size = 0;
    gfx_backend_builtin_triangle_modules(&vs, &vs_size, &ps, &ps_size);
    if (!vs || !ps) { runtime_native_window_destroy(nw); return 0; }
    const char res_json[] =
        "{\"cbufs\":[],\"sbufs\":[],\"textures\":[],\"images\":[]}";
    int miss = getenv("TOTK_ANDROID_SHADER_CACHE_MISS") != NULL;
    int clear_only = getenv("TOTK_ANDROID_CLEAR_ONLY") != NULL;
    RuntimeShaderIdentity ids[2];
    uint8_t *shader_entries = calloc(2, 16);
    void *shader_controls[2] = {0};
    if (!shader_entries) { runtime_native_window_destroy(nw); return 0; }
    for (unsigned stage = 0; stage < 2; stage++) {
        uint8_t *control = calloc(1, 0x880);
        uint8_t *code = calloc(1, 0x100);
        if (!control || !code) { free(control); free(code); runtime_native_window_destroy(nw); return 0; }
        uint32_t declared = 0x880, c1 = 0, length = 0x100, st = stage;
        memcpy(control + 0x20, &declared, sizeof declared);
        memcpy(control + 0x700, &c1, sizeof c1);
        memcpy(control + 0x704, &length, sizeof length);
        memcpy(control + 0x714, &st, sizeof st);
        memset(code, (uint8_t)((stage ? 0x22 : 0x11) ^ (miss ? 0x33 : 0)), length);
        if (!runtime_shader_identity(control, code, &ids[stage])) {
            fprintf(stderr, "[totk-nvn] ERROR synthetic shader identity stage=%u\n", stage);
            free(control); free(code); runtime_native_window_destroy(nw); return 0;
        }
        if (!miss && !runtime_shader_cache_store(runtime_shader_cache_root(), &ids[stage],
                                                 stage ? ps : vs, stage ? ps_size : vs_size,
                                                 res_json)) {
            fprintf(stderr, "[totk-nvn] ERROR write shader cache root=%s\n",
                    runtime_shader_cache_root());
            free(control); free(code); runtime_native_window_destroy(nw); return 0;
        }
        fprintf(stderr, "[totk-nvn] frame probe cache %s %s code=%u%s\n",
                runtime_shader_stage_name(stage), ids[stage].key, length,
                miss ? " (miss test, not stored)" : "");
        /* NVNshaderData in memory: {uint64 data, void *control}. */
        uint8_t *entry = shader_entries + stage * 16;
        uint64_t addr = (uint64_t)(uintptr_t)code;
        memcpy(entry, &addr, 8);
        memcpy(entry + 8, &control, 8);
        shader_controls[stage] = control;
    }

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
    if (!nvnProgramSetShaders(&p, 2, shader_entries)) return 0;

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
    memset(&cb, 0, sizeof cb);
    if (!nvnCommandBufferInitialize(&cb, &d)) return 0;
    for (int i = 0; i < frames; i++) {
        nvnCommandBufferBeginRecording(&cb);
        nvnCommandBufferSetProgram(&cb, &p);
        nvnCommandBufferSetWindow(&cb, &w);
        const float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
        nvnCommandBufferClearColor(&cb, 0, clear, 0xf);
        nvnCommandBufferSetViewport(&cb, 0, 0, 1280, 720);
        nvnCommandBufferSetScissor(&cb, 0, 0, 1280, 720);
        if (!clear_only) nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
        uint64_t handle = nvnCommandBufferEndRecording(&cb);
        nvnQueueSubmitCommands(&q, 1, &handle);
        nvnQueuePresentTexture(&q, &w, 0);
    }
    nvnCommandBufferFinalize(&cb);
    uint8_t center[4] = {0}, corner[4] = {0};
    runtime_native_window_read_pixel(nw, 640, 360, center);
    runtime_native_window_read_pixel(nw, 0, 0, corner);
    if (screenshot && *screenshot) runtime_native_window_screenshot(nw, screenshot);
    int ok = center[0] > 200 && center[1] > 60 && center[1] < 100 && center[2] < 40 &&
             corner[0] < 40;
    if (miss) {
        /* No cached pipeline: the frame is the recorded clear, not a draw. */
        ok = center[0] < 40 && center[1] < 40 && center[2] < 40 &&
             center[0] == corner[0] && center[1] == corner[1] && center[2] == corner[2];
    }
    if (clear_only) {
        ok = center[0] < 40 && center[1] < 40 && center[2] < 40 &&
             center[0] == corner[0] && center[1] == corner[1] && center[2] == corner[2];
    }
    fprintf(stderr, "[totk-nvn] frame selftest center=%02x%02x%02x%02x corner=%02x%02x%02x%02x ok=%d%s\n",
            center[0], center[1], center[2], center[3],
            corner[0], corner[1], corner[2], corner[3], ok,
            miss ? " (cache miss fallback)" : clear_only ? " (clear-only)" : "");

    nvnWindowFinalize(&w);
    nvnProgramFinalize(&p);
    free(shader_entries);
    for (unsigned stage = 0; stage < 2; stage++) free(shader_controls[stage]);
    return ok;
}

int nvn_probe_px_near(const uint8_t a[4], const float c[4], int tol) {
    for (int i = 0; i < 4; i++) {
        int want = (int)(c[i] * 255.0f + 0.5f);
        if (a[i] < want - tol || a[i] > want + tol) return 0;
    }
    return 1;
}

/* probe-nvn-mrt: two color attachments plus depth.  Clear, draw, then read
 * both attachments back through the layout-aware readback API.  Also checks
 * that the same program produces distinct pipelines for a 1-target and a
 * 2-target layout (the MRT layout is part of the pipeline key). */
int runtime_nvn_mrt_selftest(const char *screenshot, int frames) {
    g_nvn_probe = 1;
    if (frames < 1) frames = 1;
    if (frames > 16) frames = 16;
    int color_fmt = gfx_backend_vk_format_for_nvn(0x25);   /* RGBA8 */
    int depth_fmt = gfx_backend_vk_format_for_nvn(0x35);   /* D24S8 */
    if (!color_fmt || !depth_fmt) {
        fprintf(stderr, "[totk-nvn] mrt probe: format mapping unavailable\n");
        return 0;
    }
    GfxRenderTargetLayout mrt;
    memset(&mrt, 0, sizeof mrt);
    mrt.color_count = 2;
    mrt.color_formats[0] = color_fmt;
    mrt.color_formats[1] = color_fmt;
    mrt.depth_format = depth_fmt;
    GfxRenderTargetLayout single;
    memset(&single, 0, sizeof single);
    single.color_count = 1;
    single.color_formats[0] = color_fmt;

    GfxOffscreenTarget *target = gfx_backend_offscreen_create_layout(320, 240, &mrt);
    if (!target) {
        fprintf(stderr, "[totk-nvn] mrt probe: offscreen layout creation failed\n");
        return 0;
    }
    int ok = gfx_backend_offscreen_color_count(target) == 2;
    const GfxRenderTargetLayout *got = gfx_backend_offscreen_layout(target);
    ok &= got && got->color_count == 2 && got->depth_format == depth_fmt &&
          got->color_formats[1] == color_fmt;

    const uint8_t *vs = NULL, *ps = NULL;
    size_t vs_size = 0, ps_size = 0;
    gfx_backend_builtin_triangle_modules(&vs, &vs_size, &ps, &ps_size);
    if (!vs || !ps) { gfx_backend_offscreen_destroy(target); return 0; }
    GfxReflectedPipeline *mrt_pipe =
        gfx_backend_reflected_pipeline_create_layout(vs, vs_size, ps, ps_size, &mrt,
                                                     NULL, 0, NULL, 0, NULL);
    GfxReflectedPipeline *single_pipe =
        gfx_backend_reflected_pipeline_create_layout(vs, vs_size, ps, ps_size, &single,
                                                     NULL, 0, NULL, 0, NULL);
    if (!mrt_pipe || !single_pipe) {
        fprintf(stderr, "[totk-nvn] mrt probe: pipeline creation failed (mrt=%p single=%p)\n",
                (void *)mrt_pipe, (void *)single_pipe);
        ok = 0;
    } else {
        int distinct = mrt_pipe != single_pipe;
        fprintf(stderr, "[totk-nvn] mrt probe pipelines mrt=%p single=%p distinct=%d\n",
                (void *)mrt_pipe, (void *)single_pipe, distinct);
        ok &= distinct;
    }
    if (ok && mrt_pipe) {
        const float clear[4] = { 0.10f, 0.20f, 0.40f, 1.0f };
        for (int f = 0; f < frames; f++) {
            GfxCmd cmds[8];
            memset(cmds, 0, sizeof cmds);
            uint32_t nc = 0;
            cmds[nc].kind = GFX_CMD_CLEAR_COLOR;
            memcpy(cmds[nc].u.clear.rgba, clear, sizeof clear);
            cmds[nc].u.clear.mask = 0xf;
            nc++;
            cmds[nc].kind = GFX_CMD_CLEAR_DEPTH_STENCIL;
            cmds[nc].u.clear_depth.depth = 1.0f;
            cmds[nc].u.clear_depth.dmask = 1;
            cmds[nc].u.clear_depth.stencil = 0;
            cmds[nc].u.clear_depth.smask = 0xff;
            nc++;
            cmds[nc].kind = GFX_CMD_SET_RENDER_TARGETS;
            cmds[nc].u.set_targets.target = target;
            nc++;
            cmds[nc].kind = GFX_CMD_SET_VIEWPORT;
            cmds[nc].u.rect.x = 0; cmds[nc].u.rect.y = 0;
            cmds[nc].u.rect.width = 320; cmds[nc].u.rect.height = 240;
            nc++;
            cmds[nc].kind = GFX_CMD_SET_SCISSOR;
            cmds[nc].u.rect.x = 0; cmds[nc].u.rect.y = 0;
            cmds[nc].u.rect.width = 320; cmds[nc].u.rect.height = 240;
            nc++;
            cmds[nc].kind = GFX_CMD_DRAW;
            cmds[nc].u.draw.primitive = NVN_DRAW_TRIANGLES;
            cmds[nc].u.draw.first = 0;
            cmds[nc].u.draw.count = 3;
            cmds[nc].u.draw.has_state = 0;
            nc++;
            if (!gfx_backend_reflected_execute_ex(mrt_pipe, target, cmds, nc, clear, NULL, 0)) {
                fprintf(stderr, "[totk-nvn] mrt probe: execute failed\n");
                ok = 0;
                break;
            }
        }
    }

    uint8_t c0[4] = {0}, c1[4] = {0}, k0[4] = {0}, k1[4] = {0};
    if (ok) {
        ok &= gfx_backend_offscreen_read_pixel_index(target, 0, 160, 120, c0) != 0;
        ok &= gfx_backend_offscreen_read_pixel_index(target, 1, 160, 120, c1) != 0;
        ok &= gfx_backend_offscreen_read_pixel_index(target, 0, 4, 4, k0) != 0;
        ok &= gfx_backend_offscreen_read_pixel_index(target, 1, 4, 4, k1) != 0;
    }
    /* Attachment 0 carries the triangle (bright red-ish); attachment 1 was only
     * cleared, so it must hold the pass clear colour. */
    int drawn = c0[0] > 180 && c0[1] < 120 && c0[2] < 120;
    int cleared1 = nvn_probe_px_near(c1, (const float[4]){ 0.10f, 0.20f, 0.40f, 1.0f }, 6);
    int cleared1_corner = nvn_probe_px_near(k1, (const float[4]){ 0.10f, 0.20f, 0.40f, 1.0f }, 6);
    ok &= drawn && cleared1 && cleared1_corner;
    fprintf(stderr, "[totk-nvn] mrt probe color0=%02x%02x%02x%02x color1=%02x%02x%02x%02x"
                    " corner1=%02x%02x%02x%02x drawn=%d cleared1=%d\n",
            c0[0], c0[1], c0[2], c0[3], c1[0], c1[1], c1[2], c1[3],
            k1[0], k1[1], k1[2], k1[3], drawn, cleared1);

    if (screenshot && *screenshot) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s.c0.ppm", screenshot);
        if (!gfx_backend_offscreen_write_ppm_index(target, 0, path)) ok = 0;
        snprintf(path, sizeof path, "%s.c1.ppm", screenshot);
        if (!gfx_backend_offscreen_write_ppm_index(target, 1, path)) ok = 0;
    }
    if (mrt_pipe) gfx_backend_reflected_pipeline_destroy(mrt_pipe);
    if (single_pipe) gfx_backend_reflected_pipeline_destroy(single_pipe);
    gfx_backend_offscreen_destroy(target);
    fprintf(stderr, "[totk-nvn] mrt selftest %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* probe-nvn-fence: submit -> fence -> map/read ordering.  A fence may only be
 * signaled after the queue reports completion, and a mapped buffer must see
 * the storage only after that completion. */
int runtime_nvn_fence_selftest(void) {
    void *nw = runtime_native_window_create(320, 240, 0x25, 0xE1);
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

    uint8_t *pool_mem = calloc(1, 0x10000);
    pool_t pool;
    pool_builder_t pb;
    buf_t buf;
    buf_builder_t bb;
    memset(&pool, 0, sizeof pool);
    memset(&pb, 0, sizeof pb);
    memset(&buf, 0, sizeof buf);
    memset(&bb, 0, sizeof bb);
    if (pool_mem) {
        pb.dev = &d; pb.mem = pool_mem; pb.size = 0x10000; pb.flags = 1;
        ok &= nvnMemoryPoolInitialize(&pool, &pb) != 0;
        bb.dev = &d; bb.pool = &pool; bb.off = 0; bb.size = 0x100;
        ok &= nvnBufferInitialize(&buf, &bb) != 0;
    } else {
        ok = 0;
    }
    /* CPU writes before the submit are the data the post-map reader checks. */
    const uint8_t pattern[8] = { 0xde, 0xad, 0xbe, 0xef, 0x01, 0x23, 0x45, 0x67 };
    if (ok) {
        uint8_t *m = nvnBufferMap(&buf);
        if (!m) ok = 0; else memcpy(m, pattern, sizeof pattern);
    }

    win_t w;
    win_builder_t wb;
    memset(&w, 0, sizeof w);
    memset(&wb, 0, sizeof wb);
    wb.dev = &d; wb.native = nw; wb.ntex = 1; wb.tex[0] = (void *)(uintptr_t)1;
    if (ok) ok = nvnWindowInitialize(&w, &wb) != 0;
    cb_t cb;
    memset(&cb, 0, sizeof cb);
    if (ok) ok = nvnCommandBufferInitialize(&cb, &d) != 0;

    sync_t fence_sync;
    memset(&fence_sync, 0, sizeof fence_sync);
    if (ok) ok = nvnSyncInitialize(&fence_sync, &d) != 0;

    /* Submit a real frame (clear through the host target). */
    if (ok) {
        cb.win = &w;
        nvnCommandBufferBeginRecording(&cb);
        const float clear[4] = { 0.15f, 0.35f, 0.55f, 1.0f };
        nvnCommandBufferClearColor(&cb, 0, clear, 0xf);
        uint64_t h = nvnCommandBufferEndRecording(&cb);
        nvnQueueSubmitCommands(&q, 1, &h);
        nvnQueuePresentTexture(&q, &w, 0);
    }

    int fence_ok = 0, wait_ok = 0, queue_wait_ok = 0;
    if (ok) {
        fence_ok = nvnQueueFenceSync(&q, &fence_sync, 0 /* NVN_SYNC_CONDITION_ALL_GPU_WORK */, 0) != 0;
        int r = nvnSyncWait(&fence_sync, 1000000000ull);
        wait_ok = r == NVN_SYNC_WAIT_CONDITION_SATISFIED || r == NVN_SYNC_WAIT_ALREADY_SIGNALED;
        if (r == NVN_SYNC_WAIT_TIMEOUT_EXPIRED)
            fprintf(stderr, "[totk-nvn] fence probe: SyncWait timed out\n");
        queue_wait_ok = nvnQueueWaitSync(&q, &fence_sync) != 0;
    }

    int map_ok = 0, pattern_ok = 0;
    if (ok) {
        uint8_t *m = nvnBufferMap(&buf);
        map_ok = m != NULL;
        if (m) pattern_ok = memcmp(m, pattern, sizeof pattern) == 0;
        uint8_t *pm = nvnMemoryPoolMap(&pool);
        map_ok = map_ok && pm != NULL && pm == (uint8_t *)m;
    }

    int acquire_ok = 0, acquire_sync_ok = 0;
    int index = -1;
    if (ok) {
        acquire_ok = nvnQueueAcquireTexture(&q, &w, &index) == 0 && index >= 0 && index < w.ntex;
        sync_t acq;
        memset(&acq, 0, sizeof acq);
        if (nvnSyncInitialize(&acq, &d)) {
            if (nvnWindowAcquireTexture(&w, &acq, &index) == 0) {
                int r = nvnSyncWait(&acq, 1000000000ull);
                acquire_sync_ok = r == NVN_SYNC_WAIT_CONDITION_SATISFIED ||
                                  r == NVN_SYNC_WAIT_ALREADY_SIGNALED;
            }
            nvnSyncFinalize(&acq);
        }
    }
    uint8_t px[4] = {0};
    if (ok) runtime_native_window_read_pixel(nw, 160, 120, px);

    /* A guest window texture (nvnWindowBuilderSetTextures) must drive the host
     * target size/format instead of the 1920x1080 RGBA8 fallback. */
    int window_layout_ok = 0;
    if (ok) {
        tex_builder_t tb;
        tex_t wtex;
        memset(&wtex, 0, sizeof wtex);
        nvnTextureBuilderSetDefaults(&tb);
        tb.dev = &d;
        nvnTextureBuilderSetSize2D(&tb, 64, 64);
        nvnTextureBuilderSetFormat(&tb, 0x25);
        nvnTextureBuilderSetLevels(&tb, 1);
        nvnTextureBuilderSetStorage(&tb, &pool, 0x100);
        win_t gw;
        win_builder_t wb2;
        memset(&gw, 0, sizeof gw);
        memset(&wb2, 0, sizeof wb2);
        wb2.dev = &d;
        wb2.native = NULL;              /* force the host to create the target */
        wb2.ntex = 1;
        wb2.tex[0] = &wtex;
        if (nvnTextureInitialize(&wtex, &tb) && nvnWindowInitialize(&gw, &wb2)) {
            uint32_t gwid = 0, ghei = 0, gfmt = 0;
            if (runtime_native_window_info(nvnWindowGetNativeWindow(&gw),
                                           &gwid, &ghei, &gfmt, NULL, NULL))
                window_layout_ok = gwid == 64 && ghei == 64 && gfmt == 0x25;
            nvnWindowFinalize(&gw);
        }
        nvnTextureFinalize(&wtex);
    }

    ok = ok && fence_ok && wait_ok && queue_wait_ok && map_ok && pattern_ok &&
         acquire_ok && acquire_sync_ok && window_layout_ok;
    fprintf(stderr, "[totk-nvn] fence probe submit=%llu fence=%d wait=%d queue-wait=%d"
                    " map=%d pattern=%d acquire=%d acquire-sync=%d win-layout=%d"
                    " center=%02x%02x%02x%02x ok=%d\n",
            (unsigned long long)q.submits, fence_ok, wait_ok, queue_wait_ok, map_ok,
            pattern_ok, acquire_ok, acquire_sync_ok, window_layout_ok,
            px[0], px[1], px[2], px[3], ok);

    nvnSyncFinalize(&fence_sync);
    nvnCommandBufferFinalize(&cb);
    nvnWindowFinalize(&w);
    nvnMemoryPoolFinalize(&pool);
    free(pool_mem);
    runtime_native_window_destroy(nw);
    return ok;
}
