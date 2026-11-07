/* runtime_nvn_state_selftest: implementation phase 2. */
    /* 2b. Combined texture handle selects the sampler-pool entry: the default
     * texture sampler is linear, so nearest must produce a corner texel. */
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
        samp_builder_t sb;
        samp_t samp;
        samp_pool_t sp;
        memset(&pool, 0, sizeof pool);
        memset(&pb, 0, sizeof pb);
        memset(&tex, 0, sizeof tex);
        memset(&tp, 0, sizeof tp);
        memset(&samp, 0, sizeof samp);
        memset(&sp, 0, sizeof sp);
        int have_tex = 0, have_samp = 0;
        if (pool_mem) {
            pb.dev = &d; pb.mem = pool_mem; pb.size = 0x10000; pb.flags = 1;
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
        static const uint8_t checker[16] = {
            255, 0, 0, 255,   0, 255, 0, 255,
            0, 0, 255, 255,   255, 255, 255, 255,
        };
        if (have_tex) {
            int sw = runtime_guest_texture_swizzle(tex.mem, checker, 8, tex.d.target, tex.d.format,
                                                   tex.d.flags, tex.d.samples, tex.d.width,
                                                   tex.d.height, tex.d.depth, tex.d.levels,
                                                   tex.d.stride, tex.d.pkg_block_h);
            have_tex = sw == 0 && runtime_nvn_texture_upload_guest(&tex) && nvn_texture_gfx(&tex);
        }
        if (have_tex) have_tex = nvnTexturePoolInitialize(&tp, &pool, 0, 4) != 0;
        if (have_tex) nvnTexturePoolRegisterTexture(&tp, 1, &tex, NULL);
        if (have_tex) {
            nvnSamplerBuilderSetDefaults(&sb);
            sb.dev = &d;
            nvnSamplerBuilderSetMinMagFilter(&sb, 0, 0);   /* NEAREST / NEAREST */
            nvnSamplerBuilderSetWrapMode(&sb, 7, 7, 7);    /* CLAMP_TO_EDGE */
            have_samp = nvnSamplerInitialize(&samp, &sb) != 0;
        }
        if (have_samp)
            have_samp = nvnSamplerPoolInitialize(&sp, &pool, 0x1000, 4) != 0;
        if (have_samp) nvnSamplerPoolRegisterSampler(&sp, 1, &samp);
        prog_t p;
        memset(&p, 0, sizeof p);
        nvn_probe_pair_t pair;
        memset(&pair, 0, sizeof pair);
        if (!have_tex || !have_samp || !nvnProgramInitialize(&p, &d) ||
            !nvn_probe_pair_load(&p, vs, vs_size, ps, ps_size, NULL,
                                 "{\"cbufs\":[],\"sbufs\":[],"
                                 "\"textures\":[{\"binding\":0,\"slot\":0,\"type\":3,"
                                 "\"format\":\"R8G8B8A8Unorm\",\"array\":1,"
                                 "\"separate\":false,\"flags\":0}],\"images\":[]}",
                                 0x90, &pair)) {
            ok = 0;
        } else {
            for (int i = 0; i < frames; i++) {
                nvn_probe_cb_reset(&cb);
                cb.draw_count = 0;
                nvnCommandBufferBeginRecording(&cb);
                nvnCommandBufferBindProgram(&cb, &p, -1);
                nvnCommandBufferSetWindow(&cb, &w);
                nvnCommandBufferSetTexturePool(&cb, &tp);
                nvnCommandBufferSetSamplerPool(&cb, &sp);
                const float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
                nvnCommandBufferClearColor(&cb, 0, clear, 0xf);
                nvnCommandBufferSetViewport(&cb, 0, 0, 1280, 720);
                nvnCommandBufferSetScissor(&cb, 0, 0, 1280, 720);
                nvnCommandBufferBindTexture(&cb, 1, 0, (1u << 20) | 1u);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
                uint64_t h = nvnCommandBufferEndRecording(&cb);
                nvnQueueSubmitCommands(&q, 1, &h);
                nvnQueuePresentTexture(&q, &w, 0);
            }
            runtime_native_window_read_pixel(nw, 640, 360, center);
            int nearest = (center[0] == 0 || center[0] == 255) &&
                          (center[1] == 0 || center[1] == 255) &&
                          (center[2] == 0 || center[2] == 255) &&
                          (center[0] + center[1] + center[2] >= 255);
            fprintf(stderr, "[totk-nvn] state probe sampler center=%02x%02x%02x%02x ok=%d\n",
                    center[0], center[1], center[2], center[3], nearest);
            ok &= nearest;
        }
        nvnProgramFinalize(&p);
        nvn_probe_pair_free(&pair);
        if (have_samp) nvnSamplerPoolFinalize(&sp);
        if (have_tex) {
            nvnTexturePoolFinalize(&tp);
            nvnTextureFinalize(&tex);
        }
        if (have_samp) nvnSamplerFinalize(&samp);
        free(pool_mem);
    }

    /* 2c. Depth test: a farther triangle must not overwrite a nearer one. */
    {
        const uint8_t *vs = NULL, *ps = NULL;
        size_t vs_size = 0, ps_size = 0;
        gfx_backend_builtin_depth_modules(&vs, &vs_size, &ps, &ps_size);
        struct ZVert { float x, y, z; };
        static const struct ZVert verts[9] = {
            { -0.65f, -0.65f, 0.50f }, { 0.65f, -0.65f, 0.50f }, { 0.0f, 0.65f, 0.50f },
            { -0.65f, -0.65f, 0.75f }, { 0.65f, -0.65f, 0.75f }, { 0.0f, 0.65f, 0.75f },
            { -0.65f, -0.65f, 0.25f }, { 0.65f, -0.65f, 0.25f }, { 0.0f, 0.65f, 0.25f },
        };
        uint32_t va[2] = {
            (uint32_t)0x16 | (0u << 8) | (0u << 27),
            (uint32_t)0x0A | (8u << 8) | (0u << 27),
        };
        uint32_t vstream[2] = { (uint32_t)sizeof(struct ZVert), 0 };
        nvn_ds_st ds;
        nvnDepthStencilStateSetDefaults(&ds);
        nvnDepthStencilStateSetDepthTestEnable(&ds, 1);
        nvnDepthStencilStateSetDepthWriteEnable(&ds, 1);
        nvnDepthStencilStateSetDepthFunc(&ds, 2);   /* LESS */
        prog_t p;
        memset(&p, 0, sizeof p);
        nvn_probe_pair_t pair;
        memset(&pair, 0, sizeof pair);
        if (!nvnProgramInitialize(&p, &d) ||
            !nvn_probe_pair_load(&p, vs, vs_size, ps, ps_size, NULL,
                                 "{\"cbufs\":[{\"binding\":0,\"slot\":0}],"
                                 "\"sbufs\":[],\"textures\":[],\"images\":[]}",
                                 0x30, &pair)) {
            ok = 0;
        } else {
            float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
            float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
            float blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
            for (int frame = 0; frame < 2; frame++) {
                nvn_probe_cb_reset(&cb);
                cb.tex_pool = NULL;
                cb.draw_count = 0;
                nvnCommandBufferBeginRecording(&cb);
                nvnCommandBufferBindProgram(&cb, &p, -1);
                nvnCommandBufferSetWindow(&cb, &w);
                const float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
                nvnCommandBufferClearColor(&cb, 0, clear, 0xf);
                nvnCommandBufferClearDepthStencil(&cb, 1.0f, 1, 0, 0);
                nvnCommandBufferSetViewport(&cb, 0, 0, 1280, 720);
                nvnCommandBufferSetScissor(&cb, 0, 0, 1280, 720);
                nvnCommandBufferBindVertexBuffer(&cb, 0, (uint64_t)(uintptr_t)verts, sizeof verts);
                nvnCommandBufferBindVertexAttribState(&cb, 2, va);
                nvnCommandBufferBindVertexStreamState(&cb, 1, vstream);
                nvnCommandBufferBindDepthStencilState(&cb, &ds);
                nvnCommandBufferBindUniformBuffer(&cb, 1, 0, (uint64_t)(uintptr_t)red, sizeof red);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
                nvnCommandBufferBindUniformBuffer(&cb, 1, 0, (uint64_t)(uintptr_t)
                                                  (frame ? blue : green),
                                                  sizeof red);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, frame ? 6 : 3, 3);
                uint64_t h = nvnCommandBufferEndRecording(&cb);
                nvnQueueSubmitCommands(&q, 1, &h);
                nvnQueuePresentTexture(&q, &w, 0);
                runtime_native_window_read_pixel(nw, 640, 360, center);
                if (frame == 0)
                    ok &= nvn_probe_check("depth", center, 0xf0, 0xff, 0x00, 0x0f, 0x00, 0x0f);
                else
                    ok &= nvn_probe_check("depthN", center, 0x00, 0x0f, 0x00, 0x0f, 0xf0, 0xff);
            }
        }
        nvnProgramFinalize(&p);
        nvn_probe_pair_free(&pair);
    }

    /* 2d. Blend applies only to the second draw (per-draw pipeline variant). */
    {
        const uint8_t *vs = NULL, *ps = NULL;
        size_t vs_size = 0, ps_size = 0;
        gfx_backend_builtin_ubo_modules(&vs, &vs_size, &ps, &ps_size);
        nvn_color_st color;
        nvn_blend_st blend;
        nvnColorStateSetDefaults(&color);
        nvnBlendStateSetDefaults(&blend);
        nvnColorStateSetBlendEnable(&color, 0, 1);
        nvnBlendStateSetBlendFunc(&blend, 5, 6, 5, 6);   /* SRC_ALPHA / 1-SRC_ALPHA */
        nvnBlendStateSetBlendEquation(&blend, 1, 1);     /* ADD */
        prog_t p;
        memset(&p, 0, sizeof p);
        nvn_probe_pair_t pair;
        memset(&pair, 0, sizeof pair);
        if (!nvnProgramInitialize(&p, &d) ||
            !nvn_probe_pair_load(&p, vs, vs_size, ps, ps_size, NULL,
                                 "{\"cbufs\":[{\"binding\":0,\"slot\":0}],"
                                 "\"sbufs\":[],\"textures\":[],\"images\":[]}",
                                 0x40, &pair)) {
            ok = 0;
        } else {
            float green[4] = { 0.0f, 1.0f, 0.0f, 0.5f };
            float red[4] = { 1.0f, 0.0f, 0.0f, 0.5f };
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
                nvnCommandBufferBindUniformBuffer(&cb, 1, 0, (uint64_t)(uintptr_t)green, sizeof green);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
                nvnCommandBufferBindColorState(&cb, &color);
                nvnCommandBufferBindBlendState(&cb, &blend);
                nvnCommandBufferSetBlendColor(&cb, red);
                nvnCommandBufferBindUniformBuffer(&cb, 1, 0, (uint64_t)(uintptr_t)red, sizeof red);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
                uint64_t h = nvnCommandBufferEndRecording(&cb);
                nvnQueueSubmitCommands(&q, 1, &h);
                nvnQueuePresentTexture(&q, &w, 0);
            }
            runtime_native_window_read_pixel(nw, 640, 360, center);
            ok &= nvn_probe_check("blend", center, 0x78, 0x88, 0x78, 0x88, 0x00, 0x08);
        }
        nvnProgramFinalize(&p);
        nvn_probe_pair_free(&pair);
    }

    /* 2e. Raster cull: the builtin triangle is CCW in NVN's Y-up clip space
     * (CW in Vulkan's Y-down space).  NVN front=CCW + cull BACK must keep it,
     * and cull FRONT must drop it; this pins the NVN->Vulkan winding mapping. */
    {
        const uint8_t *vs = NULL, *ps = NULL;
        size_t vs_size = 0, ps_size = 0;
        gfx_backend_builtin_ubo_modules(&vs, &vs_size, &ps, &ps_size);
        nvn_poly_st poly;
        nvnPolygonStateSetDefaults(&poly);
        nvnPolygonStateSetCullFace(&poly, 2);    /* BACK */
        nvnPolygonStateSetFrontFace(&poly, 1);   /* CCW */
        prog_t p;
        memset(&p, 0, sizeof p);
        nvn_probe_pair_t pair;
        memset(&pair, 0, sizeof pair);
        if (!nvnProgramInitialize(&p, &d) ||
            !nvn_probe_pair_load(&p, vs, vs_size, ps, ps_size, NULL,
                                 "{\"cbufs\":[{\"binding\":0,\"slot\":0}],"
                                 "\"sbufs\":[],\"textures\":[],\"images\":[]}",
                                 0x50, &pair)) {
            ok = 0;
        } else {
            float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
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
                nvnCommandBufferBindPolygonState(&cb, &poly);
                nvnCommandBufferBindUniformBuffer(&cb, 1, 0, (uint64_t)(uintptr_t)red, sizeof red);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
                uint64_t h = nvnCommandBufferEndRecording(&cb);
                nvnQueueSubmitCommands(&q, 1, &h);
                nvnQueuePresentTexture(&q, &w, 0);
            }
            runtime_native_window_read_pixel(nw, 640, 360, center);
            ok &= nvn_probe_check("cullB", center, 0xf0, 0xff, 0x00, 0x0f, 0x00, 0x0f);

            /* Front-face culling must remove the same NVN-CCW triangle. */
            nvnPolygonStateSetCullFace(&poly, 1);   /* FRONT */
            for (int i = 0; i < 1; i++) {
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
                nvnCommandBufferBindPolygonState(&cb, &poly);
                nvnCommandBufferBindUniformBuffer(&cb, 1, 0, (uint64_t)(uintptr_t)red, sizeof red);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
                uint64_t h2 = nvnCommandBufferEndRecording(&cb);
                nvnQueueSubmitCommands(&q, 1, &h2);
                nvnQueuePresentTexture(&q, &w, 0);
            }
            runtime_native_window_read_pixel(nw, 640, 360, center);
            ok &= nvn_probe_check("cullF", center, 0x0d, 0x14, 0x14, 0x1f, 0x1f, 0x24);
        }
        nvnProgramFinalize(&p);
        nvn_probe_pair_free(&pair);
    }

    /* 3. Guest vertex stream + attributes + indexed draw. */
    {
        const uint8_t *vs = NULL, *ps = NULL;
        size_t vs_size = 0, ps_size = 0;
        gfx_backend_builtin_attr_modules(&vs, &vs_size, &ps, &ps_size);
        struct Vertex { float x, y, u, v; };
        static const struct Vertex verts[3] = {
            { -0.65f, -0.65f, 0.0f, 0.0f },
            {  0.65f, -0.65f, 1.0f, 0.0f },
            {  0.0f,   0.65f, 0.5f, 1.0f },
        };
        static const uint16_t indices[3] = { 0, 1, 2 };
        uint32_t va[2] = {
            (uint32_t)0x16 | (0u << 8) | (0u << 27),
            (uint32_t)0x16 | (8u << 8) | (0u << 27),
        };
        uint32_t vstream[2] = { (uint32_t)sizeof(struct Vertex), 0 };
        prog_t p;
        memset(&p, 0, sizeof p);
        nvn_probe_pair_t pair;
        memset(&pair, 0, sizeof pair);
        if (!nvnProgramInitialize(&p, &d) ||
            !nvn_probe_pair_load(&p, vs, vs_size, ps, ps_size, NULL, NULL, 0xA0, &pair)) {
            ok = 0;
        } else {
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
                nvnCommandBufferBindVertexBuffer(&cb, 0, (uint64_t)(uintptr_t)verts, sizeof verts);
                nvnCommandBufferBindVertexAttribState(&cb, 2, va);
                nvnCommandBufferBindVertexStreamState(&cb, 1, vstream);
                nvnCommandBufferDrawElements(&cb, NVN_DRAW_TRIANGLES, 1, 3,
                                             (uint64_t)(uintptr_t)indices);
                uint64_t h = nvnCommandBufferEndRecording(&cb);
                nvnQueueSubmitCommands(&q, 1, &h);
                nvnQueuePresentTexture(&q, &w, 0);
            }
            runtime_native_window_read_pixel(nw, 640, 360, center);
            ok &= nvn_probe_check("attr", center, 0x70, 0x90, 0x70, 0x90, 0x30, 0x50);
        }
        nvnProgramFinalize(&p);
        nvn_probe_pair_free(&pair);
    }

    if (screenshot && *screenshot) runtime_native_window_screenshot(nw, screenshot);
    nvnCommandBufferFinalize(&cb);
    nvnWindowFinalize(&w);
    fprintf(stderr, "[totk-nvn] state selftest %s\n", ok ? "PASS" : "FAIL");
    return ok;
