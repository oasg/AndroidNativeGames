/* loader_main: implementation phase 2. */
    if (!strcmp(argv[2], "probe-vulkan-texture")) {
        GfxOffscreenTarget *t = gfx_backend_offscreen_create(1280, 720, 0x25);
        GfxTexturedPipeline *p = gfx_backend_builtin_textured_pipeline();
        static const uint8_t checker[16] = {
            255, 0, 0, 255,   0, 255, 0, 255,
            0, 0, 255, 255,   255, 255, 255, 255,
        };
        GfxTexture *tex = gfx_backend_texture_create_rgba8(2, 2, checker);
        if (!t || !p || !tex) {
            fprintf(stderr, "[totk-vk] ERROR texture probe setup\n");
            if (tex) gfx_backend_texture_destroy(tex);
            if (t) gfx_backend_offscreen_destroy(t);
            return 1;
        }
        for (int i = 0; i < 10; i++)
            if (!gfx_backend_textured_draw(p, tex, t, 0.05f, 0.08f, 0.12f, 1.0f)) {
                fprintf(stderr, "[totk-vk] ERROR textured draw %d\n", i);
                return 1;
            }
        uint8_t center[4] = {0}, corner[4] = {0};
        gfx_backend_offscreen_read_pixel(t, 640, 360, center);
        gfx_backend_offscreen_read_pixel(t, 0, 0, corner);
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/texture.ppm", argv[1]);
        int shot_ok = gfx_backend_offscreen_write_ppm(t, shot);
        int ok = center[0] > 80 && center[0] < 180 && center[1] > 80 && center[1] < 180 &&
                 center[2] > 80 && center[2] < 180 && corner[0] < 40;
        printf("{\"status\":\"%s\",\"host\":\"android\",\"probe\":\"vulkan-texture\","
               "\"center\":\"%02x%02x%02x%02x\",\"corner\":\"%02x%02x%02x%02x\","
               "\"screenshot\":\"%s\",\"screenshot_ok\":%s}\n",
               ok ? "pass" : "fail", center[0], center[1], center[2], center[3],
               corner[0], corner[1], corner[2], corner[3], shot, shot_ok ? "true" : "false");
        gfx_backend_texture_destroy(tex);
        gfx_backend_offscreen_destroy(t);
        return ok ? 0 : 1;
    }
    if (!strcmp(argv[2], "probe-shader-cache")) {
        const char *root = getenv("TOTK_ANDROID_SHADER_CACHE");
        if (!root || !*root) root = runtime_shader_cache_root();
        static const char *const stages[] = { "vs", "ps", "gs", "tcs", "tes", "cs" };
        unsigned total = 0, failed = 0, pairs = 0;
        for (unsigned s = 0; s < sizeof stages / sizeof stages[0]; s++) {
            char dir[PATH_MAX];
            snprintf(dir, sizeof dir, "%s/%s", root, stages[s]);
            DIR *d = opendir(dir);
            if (!d) continue;
            struct dirent *ent;
            while ((ent = readdir(d))) {
                const char *name = ent->d_name;
                size_t n = strlen(name);
                if (n != 64 + 4 || strcmp(name + 64, ".spv")) continue;
                int key_ok = 1;
                for (size_t i = 0; i < 64; i++) {
                    char c = name[i];
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { key_ok = 0; break; }
                }
                if (!key_ok) { failed++; continue; }
                char path[PATH_MAX];
                snprintf(path, sizeof path, "%s/%s", dir, name);
                int fd = open(path, O_RDONLY);
                if (fd < 0) { failed++; continue; }
                struct stat st;
                if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (off_t)(16 << 20)) {
                    close(fd); failed++; continue;
                }
                uint8_t *buf = malloc((size_t)st.st_size);
                ssize_t got = buf ? read(fd, buf, (size_t)st.st_size) : -1;
                close(fd);
                if (!buf || got != st.st_size) { free(buf); failed++; continue; }
                total++;
                if (!gfx_backend_validate_spirv(buf, (size_t)got, name)) failed++;
                free(buf);
                char res[PATH_MAX];
                snprintf(res, sizeof res, "%.*s.res.json", (int)(strlen(path) - 4), path);
                if (access(res, R_OK) == 0) pairs++;
            }
            closedir(d);
        }
        if (!total) {
            fprintf(stderr, "[totk-spv] ERROR no keyed .spv modules under %s\n", root);
            return 1;
        }
        printf("{\"status\":\"%s\",\"host\":\"android\",\"probe\":\"shader-cache\","
               "\"root\":\"%s\",\"modules\":%u,\"reflections\":%u,\"failed\":%u}\n",
               failed ? "fail" : "pass", root, total, pairs, failed);
        return failed ? 1 : 0;
    }
    if (!strcmp(argv[2], "probe-spirv")) {
        const char *dir = getenv("TOTK_ANDROID_SPV_DIR");
        if (!dir) dir = "/data/local/tmp/totk-android/spv";
        DIR *d = opendir(dir);
        if (!d) { fprintf(stderr, "[totk-spv] ERROR cannot open %s\n", dir); return 1; }
        unsigned total = 0, failed = 0;
        struct dirent *ent;
        while ((ent = readdir(d))) {
            size_t n = strlen(ent->d_name);
            if (n < 5 || strcmp(ent->d_name + n - 4, ".spv")) continue;
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
            int fd = open(path, O_RDONLY);
            if (fd < 0) { fprintf(stderr, "[totk-spv] ERROR open %s\n", path); failed++; continue; }
            struct stat st;
            if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (off_t)(16 << 20)) {
                fprintf(stderr, "[totk-spv] ERROR size %s\n", path); close(fd); failed++; continue;
            }
            uint8_t *buf = malloc((size_t)st.st_size);
            ssize_t got = buf ? read(fd, buf, (size_t)st.st_size) : -1;
            close(fd);
            if (!buf || got != st.st_size) { fprintf(stderr, "[totk-spv] ERROR read %s\n", path); free(buf); failed++; continue; }
            total++;
            if (!gfx_backend_validate_spirv(buf, (size_t)got, ent->d_name)) failed++;
            free(buf);
        }
        closedir(d);
        if (!total) { fprintf(stderr, "[totk-spv] ERROR no .spv files in %s\n", dir); return 1; }
        printf("{\"status\":\"%s\",\"host\":\"android\",\"probe\":\"spirv\",\"modules\":%u,\"failed\":%u}\n",
               failed ? "fail" : "pass", total, failed);
        return failed ? 1 : 0;
    }
    if (!strcmp(argv[2], "probe-vulkan")) {
        if (!gfx_backend_selftest()) { fprintf(stderr, "[totk-vk] selftest FAIL\n"); return 1; }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"vulkan-offscreen\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-vulkan-texture-formats")) {
        int gfx_backend_texture_formats_selftest(void);
        if (!gfx_backend_texture_formats_selftest()) {
            fprintf(stderr, "[totk-vk] texture formats selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"vulkan-texture-formats\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-vulkan-bc4-mips")) {
        int gfx_backend_bc4_mips_selftest(void);
        if (!gfx_backend_bc4_mips_selftest()) {
            fprintf(stderr, "[totk-vk] BC4 mips selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"vulkan-bc4-mips\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-vulkan-mrt")) {
        int gfx_backend_mrt_selftest(void);
        if (!gfx_backend_mrt_selftest()) {
            fprintf(stderr, "[totk-vk] MRT selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"vulkan-mrt\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-vulkan-compute"))
        return gfx_backend_compute_probe(argv[1]);
    if (!strcmp(argv[2], "probe-vulkan-copy"))
        return gfx_backend_copy_probe(argv[1]);
    if (!strcmp(argv[2], "probe-vulkan-surface-target")) {
        if (!gfx_backend_surface_target_selftest()) {
            fprintf(stderr, "[totk-vk] surface target selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"vulkan-surface-target\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-texture")) {
        int runtime_nvn_textured_draw_selftest(const char *screenshot, int frames);
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/nvn-texture.ppm", argv[1]);
        const char *fs = getenv("TOTK_ANDROID_NVN_DRAW_FRAMES");
        int frames = fs && *fs ? atoi(fs) : 60;
        if (!runtime_nvn_textured_draw_selftest(shot, frames)) {
            fprintf(stderr, "[totk-nvn] NVN texture draw selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-texture\","
               "\"frames\":%d,\"screenshot\":\"%s\"}\n", frames, shot);
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-draw")) {
        int runtime_nvn_draw_selftest(const char *screenshot, int frames,
                                      const char *vs_path, const char *ps_path, int expect_visible);
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/nvn-draw.ppm", argv[1]);
        const char *fs = getenv("TOTK_ANDROID_NVN_DRAW_FRAMES");
        int frames = fs && *fs ? atoi(fs) : 60;
        const char *vs = getenv("TOTK_ANDROID_SPV_VS");
        const char *ps = getenv("TOTK_ANDROID_SPV_PS");
        int visible = !(vs && *vs && ps && *ps);
        if (!runtime_nvn_draw_selftest(shot, frames, vs, ps, visible)) {
            fprintf(stderr, "[totk-nvn] NVN draw selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-draw\","
               "\"frames\":%d,\"visible\":%s,\"screenshot\":\"%s\"}\n",
               frames, visible ? "true" : "false", shot);
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-reflect")) {
        int runtime_nvn_draw_reflect_selftest(const char *screenshot, int frames,
                                              const char *vs_path, const char *ps_path,
                                              const char *vs_res, const char *ps_res);
        const char *vs = getenv("TOTK_ANDROID_SPV_VS");
        const char *ps = getenv("TOTK_ANDROID_SPV_PS");
        const char *vres = getenv("TOTK_ANDROID_REFL_VS");
        const char *pres = getenv("TOTK_ANDROID_REFL_PS");
        if (!vs || !ps || !vres || !pres) {
            fprintf(stderr, "[totk-nvn] ERROR probe-nvn-reflect needs SPV_VS/SPV_PS/REFL_VS/REFL_PS\n");
            return 1;
        }
        if (!reflect_check_interface(vs, ps) && !getenv("TOTK_ANDROID_ALLOW_INTERFACE_MISMATCH"))
            return 1;
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/nvn-reflect.ppm", argv[1]);
        const char *fs = getenv("TOTK_ANDROID_NVN_DRAW_FRAMES");
        int frames = fs && *fs ? atoi(fs) : 60;
        if (!runtime_nvn_draw_reflect_selftest(shot, frames, vs, ps, vres, pres)) {
            fprintf(stderr, "[totk-nvn] NVN reflected draw selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-reflect\","
               "\"frames\":%d,\"screenshot\":\"%s\"}\n", frames, shot);
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-texture-reflect")) {
        int runtime_nvn_texture_bind_selftest(const char *screenshot, int frames);
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/nvn-texture-reflect.ppm", argv[1]);
        const char *fs = getenv("TOTK_ANDROID_NVN_DRAW_FRAMES");
        int frames = fs && *fs ? atoi(fs) : 2;
        if (!runtime_nvn_texture_bind_selftest(shot, frames)) {
            fprintf(stderr, "[totk-nvn] NVN textured reflected draw selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-texture-reflect\","
               "\"frames\":%d,\"screenshot\":\"%s\"}\n", frames, shot);
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-state")) {
        int runtime_nvn_state_selftest(const char *screenshot, int frames);
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/nvn-state.ppm", argv[1]);
        const char *fs = getenv("TOTK_ANDROID_NVN_DRAW_FRAMES");
        int frames = fs && *fs ? atoi(fs) : 2;
        if (!getenv("TOTK_ANDROID_SHADER_CACHE")) {
            static char cache[PATH_MAX];
            snprintf(cache, sizeof cache, "%s/shader-cache", argv[1]);
            setenv("TOTK_ANDROID_SHADER_CACHE", cache, 1);
        }
        if (!runtime_nvn_state_selftest(shot, frames)) {
            fprintf(stderr, "[totk-nvn] NVN state selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-state\","
               "\"frames\":%d,\"shader_cache\":\"%s\",\"screenshot\":\"%s\"}\n",
               frames, getenv("TOTK_ANDROID_SHADER_CACHE"), shot);
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-frame")) {
        int runtime_nvn_frame_selftest(const char *screenshot, int frames);
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/nvn-frame.ppm", argv[1]);
        const char *fs = getenv("TOTK_ANDROID_NVN_DRAW_FRAMES");
        int frames = fs && *fs ? atoi(fs) : 2;
        if (!getenv("TOTK_ANDROID_SHADER_CACHE")) {
            static char cache[PATH_MAX];
            snprintf(cache, sizeof cache, "%s/shader-cache", argv[1]);
            setenv("TOTK_ANDROID_SHADER_CACHE", cache, 1);
        }
        if (!runtime_nvn_frame_selftest(shot, frames)) {
            fprintf(stderr, "[totk-nvn] NVN frame (shader cache) selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-frame\","
               "\"frames\":%d,\"shader_cache\":\"%s\",\"screenshot\":\"%s\"}\n",
               frames, getenv("TOTK_ANDROID_SHADER_CACHE"), shot);
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-bind")) {
        int runtime_nvn_bind_selftest(void);
        if (!runtime_nvn_bind_selftest()) {
            fprintf(stderr, "[totk-nvn] NVN bind selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-bind\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-submit")) {
        int runtime_nvn_submit_selftest(void);
        if (!runtime_nvn_submit_selftest()) { fprintf(stderr, "[totk-nvn] submit selftest FAIL\n"); return 1; }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-submit-present\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-cb-lifetime")) {
        int runtime_nvn_cb_lifetime_selftest(void);
        if (!runtime_nvn_cb_lifetime_selftest()) {
            fprintf(stderr, "[totk-nvn] command-buffer lifetime selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-cb-lifetime\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-mrt")) {
        int runtime_nvn_mrt_selftest(const char *screenshot, int frames);
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/nvn-mrt.ppm", argv[1]);
        const char *fs = getenv("TOTK_ANDROID_NVN_DRAW_FRAMES");
        int frames = fs && *fs ? atoi(fs) : 2;
        if (!runtime_nvn_mrt_selftest(shot, frames)) {
            fprintf(stderr, "[totk-nvn] NVN MRT selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-mrt\","
               "\"frames\":%d,\"color0\":\"%s.c0.ppm\",\"color1\":\"%s.c1.ppm\"}\n",
               frames, shot, shot);
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-segment-targets")) {
        int runtime_nvn_segment_selftest(const char *screenshot, int frames);
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/nvn-segment.ppm", argv[1]);
        const char *fs = getenv("TOTK_ANDROID_NVN_DRAW_FRAMES");
        int frames = fs && *fs ? atoi(fs) : 1;
        if (!runtime_nvn_segment_selftest(shot, frames)) {
            fprintf(stderr, "[totk-nvn] NVN segment targets selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-segment-targets\","
               "\"frames\":%d,\"color0\":\"%s.c0.ppm\",\"color1\":\"%s.c1.ppm\"}\n",
               frames, shot, shot);
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-retired-stress")) {
        int runtime_nvn_retired_stress_selftest(int iterations);
        const char *it = getenv("TOTK_ANDROID_STRESS_ITERS");
        int iterations = it && *it ? atoi(it) : 500;
        if (!runtime_nvn_retired_stress_selftest(iterations)) {
            fprintf(stderr, "[totk-nvn] retired stress selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\","
               "\"probe\":\"nvn-retired-stress\",\"iterations\":%d}\n",
               iterations);
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-prog-abi")) {
        int runtime_nvn_prog_abi_selftest(void);
        if (!runtime_nvn_prog_abi_selftest()) {
            fprintf(stderr, "[totk-nvn] NVNprogram ABI selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\","
               "\"probe\":\"nvn-prog-abi\",\"size\":112,"
               "\"sd2_data\":40,\"sd2_control\":48,\"serial\":104}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-sparse-targets")) {
        int runtime_nvn_sparse_target_selftest(void);
        if (!runtime_nvn_sparse_target_selftest()) {
            fprintf(stderr, "[totk-nvn] sparse MRT target selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\","
               "\"probe\":\"nvn-sparse-targets\",\"colors\":4,"
               "\"dummy_slot\":2}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-fence")) {
        int runtime_nvn_fence_selftest(void);
        if (!runtime_nvn_fence_selftest()) {
            fprintf(stderr, "[totk-nvn] NVN fence/map ordering selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-fence\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-compute"))
        return nvn_compute_probe(argv[1]);
    if (!strcmp(argv[2], "probe-nvn-copy"))
        return nvn_copy_probe(argv[1]);
    if (!strcmp(argv[2], "probe-nvn-vertex-state")) {
        int runtime_nvn_vertex_state_selftest(void);
        if (!runtime_nvn_vertex_state_selftest()) {
            fprintf(stderr, "[totk-nvn] NVN vertex-state/pool-inheritance selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-vertex-state\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-cb-call")) {
        int runtime_nvn_cb_call_selftest(void);
        if (!runtime_nvn_cb_call_selftest()) {
            fprintf(stderr, "[totk-nvn] NVN finalized-handle CallCommands selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-cb-call\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-pool-lifetime")) {
        int runtime_nvn_pool_lifetime_selftest(void);
        unsigned runtime_nvn_tex_pool_max(void);
        if (!runtime_nvn_pool_lifetime_selftest()) {
            fprintf(stderr, "[totk-nvn] pool lifetime selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-pool-lifetime\","
               "\"registrations\":40000,\"max_entries\":%u}\n", runtime_nvn_tex_pool_max());
        return 0;
    }
    if (!strcmp(argv[2], "probe-vi")) {
        int runtime_vi_selftest(void);
        if (!runtime_vi_selftest()) { fprintf(stderr, "[totk-vi] selftest FAIL\n"); return 1; }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"vi-native-window\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvdrv")) {
        int runtime_nvdrv_selftest(void);
        if (!runtime_nvdrv_selftest()) { fprintf(stderr, "[totk-nvdrv] selftest FAIL\n"); return 1; }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvdrv\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-tls")) { tls_probe_mode = 1; return boot_sdk(); }
    if (!strcmp(argv[2], "boot-sdk")) return boot_sdk();
    runtime_fail("unknown mode");
