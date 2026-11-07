/* NVN module: texture_3. */
#include "nvn_internal.h"

int runtime_nvn_texture_bind_selftest(const char *screenshot, int frames) {
    if (frames < 1) frames = 1;
    if (frames > 60) frames = 60;
    void *nw = runtime_native_window_create(1280, 720, 0x25, 0xA2);
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

    uint8_t *pool_mem = calloc(1, 0x10000);
    if (!pool_mem) return 0;
    pool_t pool;
    pool_builder_t pb;
    memset(&pool, 0, sizeof pool);
    memset(&pb, 0, sizeof pb);
    pb.dev = &d; pb.mem = pool_mem; pb.size = 0x10000; pb.flags = 1;
    if (!nvnMemoryPoolInitialize(&pool, &pb)) { free(pool_mem); return 0; }
    tex_builder_t tb;
    tex_t tex;
    memset(&tex, 0, sizeof tex);
    nvnTextureBuilderSetDefaults(&tb);
    tb.dev = &d;
    nvnTextureBuilderSetSize2D(&tb, 2, 2);
    nvnTextureBuilderSetFormat(&tb, 0x25);
    nvnTextureBuilderSetLevels(&tb, 1);
    nvnTextureBuilderSetStorage(&tb, &pool, 0);
    if (!nvnTextureInitialize(&tex, &tb)) { free(pool_mem); return 0; }
    const uint8_t checker[16] = {
        255, 0, 0, 255,   0, 255, 0, 255,
        0, 0, 255, 255,   255, 255, 255, 255,
    };
    if (runtime_guest_texture_swizzle(tex.mem, checker, 8, tex.d.target, tex.d.format,
                                       tex.d.flags, tex.d.samples, tex.d.width, tex.d.height,
                                       tex.d.depth, tex.d.levels, tex.d.stride,
                                       tex.d.pkg_block_h)) { free(pool_mem); return 0; }
    if (!runtime_nvn_texture_upload_guest(&tex)) { free(pool_mem); return 0; }

    prog_t p;
    memset(&p, 0, sizeof p);
    if (!nvnProgramInitialize(&p, &d)) { free(pool_mem); return 0; }
    GfxReflectedPipeline *rp = gfx_backend_reflected_builtin_texture_pipeline_create();
    prog_host_set(&p, NULL, NULL, NULL, rp, 0);
    if (!rp) { free(pool_mem); return 0; }
    if (!runtime_nvn_program_bind_texture(&p, 1, 0, &tex)) {
        fprintf(stderr, "[totk-nvn] ERROR bind real texture to reflected PS\n");
        nvnProgramFinalize(&p); free(pool_mem); return 0;
    }

    win_t w;
    win_builder_t wb;
    memset(&w, 0, sizeof w);
    memset(&wb, 0, sizeof wb);
    wb.dev = &d; wb.native = nw; wb.ntex = 1; wb.tex[0] = (void *)(uintptr_t)1;
    if (!nvnWindowInitialize(&w, &wb)) {
        nvnProgramFinalize(&p); free(pool_mem); return 0;
    }
    cb_t cb;
    memset(&cb, 0, sizeof cb);
    if (!nvnCommandBufferInitialize(&cb, &d)) {
        nvnProgramFinalize(&p); free(pool_mem); return 0;
    }
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
    int visible = center[0] != corner[0] || center[1] != corner[1] ||
                  center[2] != corner[2] || center[3] != corner[3];
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] textured reflected selftest center=%02x%02x%02x%02x"
                        " corner=%02x%02x%02x%02x visible=%d\n",
                center[0], center[1], center[2], center[3],
                corner[0], corner[1], corner[2], corner[3], visible);
    nvnProgramFinalize(&p);
    free(pool_mem);
    return visible;
}
