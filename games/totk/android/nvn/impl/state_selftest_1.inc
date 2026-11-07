/* runtime_nvn_state_selftest: implementation phase 1. */
    g_nvn_probe = 1;
    if (frames < 1) frames = 1;
    if (frames > 60) frames = 60;
    void *nw = runtime_native_window_create(1280, 720, 0x25, 0xB1);
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
    win_t w;
    win_builder_t wb;
    memset(&w, 0, sizeof w);
    memset(&wb, 0, sizeof wb);
    wb.dev = &d; wb.native = nw; wb.ntex = 1; wb.tex[0] = (void *)(uintptr_t)1;
    if (!nvnWindowInitialize(&w, &wb)) return 0;
    cb_t cb;
    memset(&cb, 0, sizeof cb);
    if (!nvnCommandBufferInitialize(&cb, &d)) return 0;

    int ok = 1;
    uint8_t center[4] = {0};

    /* 1. Uniform buffer bound through the command buffer. */
    {
        const uint8_t *vs = NULL, *ps = NULL;
        size_t vs_size = 0, ps_size = 0;
        gfx_backend_builtin_ubo_modules(&vs, &vs_size, &ps, &ps_size);
        prog_t p;
        memset(&p, 0, sizeof p);
        nvn_probe_pair_t pair;
        memset(&pair, 0, sizeof pair);
        if (!nvnProgramInitialize(&p, &d) ||
            !nvn_probe_pair_load(&p, vs, vs_size, ps, ps_size, NULL,
                                 "{\"cbufs\":[{\"binding\":0,\"slot\":0}],"
                                 "\"sbufs\":[],\"textures\":[],\"images\":[]}",
                                 0x60, &pair)) {
            ok = 0;
        } else {
            float color[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
            for (int i = 0; i < frames; i++) {
                nvn_probe_cb_reset(&cb);
                cb.tex_pool = NULL;
                cb.draw_count = 0;
                nvnCommandBufferBeginRecording(&cb);
                nvnCommandBufferBindProgram(&cb, &p, -1);
                nvnCommandBufferSetWindow(&cb, &w);
                const float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
                nvnCommandBufferClearColor(&cb, 0, clear, 0xf);
                nvnCommandBufferSetViewport(&cb, 0, 0, 1280, 720);
                nvnCommandBufferSetScissor(&cb, 0, 0, 1280, 720);
                nvnCommandBufferBindUniformBuffer(&cb, 1, 0,
                                                  (uint64_t)(uintptr_t)color, sizeof color);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
                uint64_t h = nvnCommandBufferEndRecording(&cb);
                nvnQueueSubmitCommands(&q, 1, &h);
                nvnQueuePresentTexture(&q, &w, 0);
            }
            runtime_native_window_read_pixel(nw, 640, 360, center);
            ok &= nvn_probe_check("ubo", center, 0x3c, 0x44, 0x7c, 0x84, 0xbb, 0xc3);
        }
        nvnProgramFinalize(&p);
        nvn_probe_pair_free(&pair);
    }

    /* 1b. Two UBO epochs in one command buffer: the first draw must keep the
     * red constants even after the second draw binds blue. */
    {
        const uint8_t *vs = NULL, *ps = NULL;
        size_t vs_size = 0, ps_size = 0;
        gfx_backend_builtin_ubo_modules(&vs, &vs_size, &ps, &ps_size);
        prog_t p;
        memset(&p, 0, sizeof p);
        nvn_probe_pair_t pair;
        memset(&pair, 0, sizeof pair);
        if (!nvnProgramInitialize(&p, &d)
            || !nvn_probe_pair_load(&p, vs, vs_size, ps, ps_size, NULL,
                                    "{\"cbufs\":[{\"binding\":0,\"slot\":0}],"
                                    "\"sbufs\":[],\"textures\":[],\"images\":[]}",
                                    0x70, &pair)) {
            ok = 0;
        } else {
            float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
            float blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
            for (int i = 0; i < frames; i++) {
                nvn_probe_cb_reset(&cb);
                cb.tex_pool = NULL;
                cb.draw_count = 0;
                nvnCommandBufferBeginRecording(&cb);
                nvnCommandBufferBindProgram(&cb, &p, -1);
                nvnCommandBufferSetWindow(&cb, &w);
                const float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
                nvnCommandBufferClearColor(&cb, 0, clear, 0xf);
                nvnCommandBufferSetViewport(&cb, 0, 0, 1280, 720);
                nvnCommandBufferSetScissor(&cb, 0, 0, 1280, 720);
                nvnCommandBufferBindUniformBuffer(&cb, 1, 0, (uint64_t)(uintptr_t)red, sizeof red);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
                nvnCommandBufferBindUniformBuffer(&cb, 1, 0, (uint64_t)(uintptr_t)blue, sizeof blue);
                nvnCommandBufferSetViewport(&cb, 0, 0, 640, 720);
                nvnCommandBufferSetScissor(&cb, 0, 0, 640, 720);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
                uint64_t h = nvnCommandBufferEndRecording(&cb);
                nvnQueueSubmitCommands(&q, 1, &h);
                nvnQueuePresentTexture(&q, &w, 0);
            }
            uint8_t left[4] = { 0 };
            runtime_native_window_read_pixel(nw, 320, 360, left);
            runtime_native_window_read_pixel(nw, 760, 360, center);
            ok &= nvn_probe_check("orderL", left, 0x00, 0x0f, 0x00, 0x0f, 0xf0, 0xff);
            ok &= nvn_probe_check("orderR", center, 0xf0, 0xff, 0x00, 0x0f, 0x00, 0x0f);
        }
        nvnProgramFinalize(&p);
        nvn_probe_pair_free(&pair);
    }

    /* 2. Guest texture bound through the command buffer's texture pool. */
    {
        const uint8_t *vs = NULL, *ps = NULL;
        size_t vs_size = 0, ps_size = 0;
        gfx_backend_builtin_texture_modules(&vs, &vs_size, &ps, &ps_size);
        uint8_t *pool_mem = calloc(1, 0x10000);
        pool_t pool;
        pool_builder_t pb;
        tex_builder_t tb;
        tex_t tex;
        tex_pool_t tp;
        memset(&pool, 0, sizeof pool);
        memset(&pb, 0, sizeof pb);
        memset(&tex, 0, sizeof tex);
        memset(&tp, 0, sizeof tp);
        int have_tex = 0;
        if (pool_mem) {
            pb.dev = &d;
            pb.mem = pool_mem;
            pb.size = 0x10000;
            pb.flags = 1;
            have_tex = nvnMemoryPoolInitialize(&pool, &pb) != 0;
        }
        if (have_tex) {
            nvnTextureBuilderSetDefaults(&tb);
            tb.dev = &d;
            nvnTextureBuilderSetSize2D(&tb, 2, 2);
            nvnTextureBuilderSetFormat(&tb, 0x25);
            nvnTextureBuilderSetLevels(&tb, 1);
            nvnTextureBuilderSetStorage(&tb, &pool, 0);
            have_tex = nvnTextureInitialize(&tex, &tb) != 0;
        }
        const uint8_t checker[16] = {
            255, 0, 255, 255,   255, 0, 255, 255,
            255, 0, 255, 255,   255, 0, 255, 255,
        };
        if (have_tex) {
            int sw = runtime_guest_texture_swizzle(tex.mem, checker, 8, tex.d.target, tex.d.format,
                                                   tex.d.flags, tex.d.samples, tex.d.width,
                                                   tex.d.height, tex.d.depth, tex.d.levels,
                                                   tex.d.stride, tex.d.pkg_block_h);
            int up = sw == 0 ? runtime_nvn_texture_upload_guest(&tex) : 0;
            have_tex = sw == 0 && up && nvn_texture_gfx(&tex) != NULL;
        }
        if (have_tex)
            have_tex = nvnTexturePoolInitialize(&tp, &pool, 0, 4) != 0;
        if (have_tex) nvnTexturePoolRegisterTexture(&tp, 1, &tex, NULL);
        prog_t p;
        memset(&p, 0, sizeof p);
        nvn_probe_pair_t pair;
        memset(&pair, 0, sizeof pair);
        if (!have_tex || !nvnProgramInitialize(&p, &d) ||
            !nvn_probe_pair_load(&p, vs, vs_size, ps, ps_size, NULL,
                                 "{\"cbufs\":[],\"sbufs\":[],"
                                 "\"textures\":[{\"binding\":0,\"slot\":0,\"type\":3,"
                                 "\"format\":\"R8G8B8A8Unorm\",\"array\":1,"
                                 "\"separate\":false,\"flags\":0}],\"images\":[]}",
                                 0x80, &pair)) {
            ok = 0;
        } else {
            for (int i = 0; i < frames; i++) {
                nvn_probe_cb_reset(&cb);
                cb.draw_count = 0;
                nvnCommandBufferBeginRecording(&cb);
                nvnCommandBufferBindProgram(&cb, &p, -1);
                nvnCommandBufferSetWindow(&cb, &w);
                nvnCommandBufferSetTexturePool(&cb, &tp);
                const float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
                nvnCommandBufferClearColor(&cb, 0, clear, 0xf);
                nvnCommandBufferSetViewport(&cb, 0, 0, 1280, 720);
                nvnCommandBufferSetScissor(&cb, 0, 0, 1280, 720);
                nvnCommandBufferBindTexture(&cb, 1, 0, 1);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
                uint64_t h = nvnCommandBufferEndRecording(&cb);
                nvnQueueSubmitCommands(&q, 1, &h);
                nvnQueuePresentTexture(&q, &w, 0);
            }
            runtime_native_window_read_pixel(nw, 640, 360, center);
            ok &= nvn_probe_check("texture", center, 0xf0, 0xff, 0x00, 0x0f, 0xf0, 0xff);
        }
        nvnProgramFinalize(&p);
        nvn_probe_pair_free(&pair);
        if (have_tex) {
            nvnTexturePoolFinalize(&tp);
            nvnTextureFinalize(&tex);
        }
        free(pool_mem);
    }
