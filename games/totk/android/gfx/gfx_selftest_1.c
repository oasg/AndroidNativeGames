/* Module: selftest_1. */
#include "gfx_internal.h"

int gfx_backend_surface_target_selftest(void) {
    if (!gfx_backend_vulkan_init()) {
        vk_log("surface target selftest: Vulkan unavailable");
        return 0;
    }
    int ok = 1;

    GfxOffscreenTarget *rgba = gfx_backend_offscreen_create(64, 64, 0x25);
    if (!rgba) {
        vk_log("surface target selftest: RGBA8 target creation failed");
        return 0;
    }
    GfxTrianglePipeline *tri = gfx_backend_builtin_triangle_pipeline();
    if (!tri || !gfx_backend_triangle_draw(tri, rgba, 0.0f, 0.0f, 0.0f, 1.0f)) {
        vk_log("surface target selftest: RGBA8 triangle draw failed");
        gfx_backend_offscreen_destroy(rgba);
        return 0;
    }

    uint8_t *px = NULL;
    pthread_mutex_lock(&g_lock);
    int probe_ok = vk_present_probe_locked(rgba, GFX_SURFACE_SCALE_FIT, &px);
    pthread_mutex_unlock(&g_lock);
    if (!probe_ok || !px) {
        vk_log("surface target selftest: RGBA8 fit copy failed");
        ok = 0;
    } else {
        uint8_t centre[4], bar[4], upper[4], lower[4];
        probe_pixel(px, 48, 32, centre);
        probe_pixel(px, 4, 32, bar);
        probe_pixel(px, 36, 20, upper);
        probe_pixel(px, 36, 44, lower);
        int content = centre[0] > 200 && centre[1] > 40 && centre[1] < 130 && centre[2] < 70;
        int letterbox = bar[0] < 24 && bar[1] < 24 && bar[2] < 24;
        int upright = upper[0] > 200 && lower[0] < 24 && lower[1] < 24 && lower[2] < 24;
        vk_log("surface target selftest RGBA8 fit centre=%02x%02x%02x bar=%02x%02x%02x "
               "upper=%02x%02x%02x lower=%02x%02x%02x content=%s letterbox=%s upright=%s",
               centre[0], centre[1], centre[2], bar[0], bar[1], bar[2],
               upper[0], upper[1], upper[2], lower[0], lower[1], lower[2],
               content ? "OK" : "BAD", letterbox ? "OK" : "BAD", upright ? "OK" : "BAD");
        ok = ok && content && letterbox && upright;
        free(px);
        px = NULL;
    }

    GfxOffscreenTarget *hdr = gfx_backend_offscreen_create(64, 64, 0x3F);
    if (!hdr) {
        vk_log("surface target selftest: R11G11B10F target creation failed");
        ok = 0;
    } else {
        if (!gfx_backend_offscreen_clear(hdr, 0.0f, 1.0f, 0.0f, 1.0f)) {
            vk_log("surface target selftest: R11G11B10F clear failed");
            ok = 0;
        } else {
            /* Stretch: conversion path must fill the whole surface. */
            pthread_mutex_lock(&g_lock);
            probe_ok = vk_present_probe_locked(hdr, GFX_SURFACE_SCALE_STRETCH, &px);
            pthread_mutex_unlock(&g_lock);
            if (!probe_ok || !px) {
                vk_log("surface target selftest: R11G11B10F stretch copy failed");
                ok = 0;
            } else {
                uint8_t centre[4], edge[4];
                probe_pixel(px, 48, 32, centre);
                probe_pixel(px, 4, 32, edge);
                int filled = centre[1] > 200 && edge[1] > 200 &&
                             centre[0] < 40 && edge[0] < 40;
                vk_log("surface target selftest R11 stretch centre=%02x%02x%02x "
                       "edge=%02x%02x%02x %s", centre[0], centre[1], centre[2],
                       edge[0], edge[1], edge[2], filled ? "OK" : "BAD");
                ok = ok && filled;
                free(px);
                px = NULL;
            }
            /* Fit: converted target must still letterbox. */
            pthread_mutex_lock(&g_lock);
            probe_ok = vk_present_probe_locked(hdr, GFX_SURFACE_SCALE_FIT, &px);
            pthread_mutex_unlock(&g_lock);
            if (!probe_ok || !px) {
                vk_log("surface target selftest: R11G11B10F fit copy failed");
                ok = 0;
            } else {
                uint8_t centre[4], edge[4];
                probe_pixel(px, 48, 32, centre);
                probe_pixel(px, 4, 32, edge);
                int fitted = centre[1] > 200 && edge[0] < 24 && edge[1] < 24 &&
                             edge[2] < 24;
                vk_log("surface target selftest R11 fit centre=%02x%02x%02x "
                       "edge=%02x%02x%02x %s", centre[0], centre[1], centre[2],
                       edge[0], edge[1], edge[2], fitted ? "OK" : "BAD");
                ok = ok && fitted;
                free(px);
                px = NULL;
            }
        }
        gfx_backend_offscreen_destroy(hdr);
    }

    if (gfx_backend_surface_ready()) {
        if (!gfx_backend_surface_present_target(rgba, GFX_SURFACE_SCALE_FIT)) {
            vk_log("surface target selftest: real surface present failed");
            ok = 0;
        } else {
            vk_log("surface target selftest: presented RGBA8 target to ANativeWindow");
        }
    } else {
        vk_log("surface target selftest: no ANativeWindow attached; surface present skipped");
    }
    gfx_backend_offscreen_destroy(rgba);
    vk_log("surface target selftest %s", ok ? "PASS" : "FAIL");
    return ok;
}

/* Focused regression for the save/title UI texture: 512x512 BC4_UNORM with a
 * full 10-level mip chain, including the sub-block mips (2x2 and 1x1), with
 * real block data uploaded and every level read back byte-exact. */
int gfx_backend_bc4_mips_selftest(void) {
    if (!gfx_backend_vulkan_init()) {
        vk_log("BC4 mips selftest: Vulkan unavailable");
        return 0;
    }
    if (gfx_backend_vk_format_bpp(VK_FORMAT_BC4_UNORM_BLOCK) != 8) {
        vk_log("BC4 mips selftest: bpp=%d, expected 8-byte blocks",
               gfx_backend_vk_format_bpp(VK_FORMAT_BC4_UNORM_BLOCK));
        return 0;
    }
    enum { W = 512, H = 512, LEVELS = 10 };
    GfxTextureDesc desc = {
        .target = GFX_TEXTURE_2D,
        .vk_format = VK_FORMAT_BC4_UNORM_BLOCK,
        .width = W, .height = H, .depth = 1,
        .mip_levels = LEVELS, .array_layers = 1, .samples = 1,
    };
    uint8_t *data[LEVELS] = { 0 };
    GfxTextureSubresource subs[LEVELS];
    size_t sizes[LEVELS] = { 0 };
    int ok = 1;
    for (uint32_t l = 0; l < LEVELS; l++) {
        sizes[l] = vk_texture_slice_size(&desc, l);
        if (!sizes[l]) { ok = 0; break; }
        data[l] = malloc(sizes[l]);
        if (!data[l]) { ok = 0; break; }
        for (size_t i = 0; i < sizes[l]; i++)
            data[l][i] = (uint8_t)(0x31u * (l + 1) + (uint8_t)i * 7u);
        subs[l] = (GfxTextureSubresource){ l, 0, data[l], sizes[l] };
    }
    GfxTexture *t = ok ? gfx_backend_texture_create_desc(&desc, subs, LEVELS) : NULL;
    if (!t) {
        vk_log("BC4 mips selftest: 512x512 mips=10 create failed");
        ok = 0;
    } else {
        for (uint32_t l = 0; l < LEVELS && ok; l++) {
            uint8_t *got = malloc(sizes[l] ? sizes[l] : 1);
            if (!got) { ok = 0; break; }
            if (!gfx_backend_texture_read_pixels(t, l, 0, got, sizes[l])) {
                vk_log("BC4 mips selftest: read level %u (%zu bytes) failed", l, sizes[l]);
                ok = 0;
            } else if (memcmp(got, data[l], sizes[l])) {
                size_t i = 0;
                while (i + 1 < sizes[l] && got[i] == data[l][i]) i++;
                vk_log("BC4 mips selftest: level %u mismatch at byte %zu got=%02x want=%02x",
                       l, i, got[i], data[l][i]);
                ok = 0;
            }
            free(got);
        }
        vk_log("BC4 mips selftest: %s 512x512 mips=%u sizes=%zu,%zu,%zu,%zu",
               ok ? "PASS" : "FAIL", LEVELS, sizes[0], sizes[1], sizes[8], sizes[9]);
        gfx_backend_texture_destroy(t);
    }
    /* A standalone small BC4 texture also exercises the 1-block case. */
    if (ok) {
        uint8_t block[8];
        for (unsigned i = 0; i < sizeof block; i++) block[i] = (uint8_t)(0xa5u ^ (i * 3));
        GfxTextureDesc d4 = {
            .target = GFX_TEXTURE_2D, .vk_format = VK_FORMAT_BC4_UNORM_BLOCK,
            .width = 4, .height = 4, .depth = 1, .mip_levels = 1,
            .array_layers = 1, .samples = 1,
        };
        GfxTextureSubresource s4 = { 0, 0, block, sizeof block };
        GfxTexture *t4 = gfx_backend_texture_create_desc(&d4, &s4, 1);
        uint8_t got[8] = { 0 };
        if (!t4 || !gfx_backend_texture_read_pixels(t4, 0, 0, got, sizeof got) ||
            memcmp(got, block, sizeof block)) {
            vk_log("BC4 mips selftest: 4x4 block round-trip failed");
            ok = 0;
        }
        if (t4) gfx_backend_texture_destroy(t4);
    }
    for (uint32_t l = 0; l < LEVELS; l++) free(data[l]);
    vk_log("BC4 mips selftest %s", ok ? "PASS" : "FAIL");
    return ok;
}

int gfx_backend_selftest(void) {
    if (!gfx_backend_vulkan_init()) {
        vk_log("ERROR Vulkan backend unavailable");
        return 0;
    }
    if (gfx_backend_vk_format_for_nvn(0x25) != VK_FORMAT_R8G8B8A8_UNORM ||
        gfx_backend_vk_format_for_nvn(0x38) != VK_FORMAT_R8G8B8A8_SRGB ||
        gfx_backend_vk_format_for_nvn(0x35) != VK_FORMAT_D24_UNORM_S8_UINT) {
        vk_log("ERROR NVN->Vk format table mismatch");
        return 0;
    }
    GfxOffscreenTarget *t = gfx_backend_offscreen_create(1280, 720, 0x25);
    if (!t) {
        vk_log("ERROR offscreen target creation failed");
        return 0;
    }
    if (!gfx_backend_offscreen_clear(t, 0.25f, 0.5f, 0.75f, 1.0f)) {
        gfx_backend_offscreen_destroy(t);
        return 0;
    }
    uint8_t px[4] = {0};
    if (!gfx_backend_offscreen_read_pixel(t, 0, 0, px)) {
        gfx_backend_offscreen_destroy(t);
        return 0;
    }
    int ok = px[0] >= 62 && px[0] <= 66 && px[1] >= 126 && px[1] <= 130 &&
             px[2] >= 189 && px[2] <= 193 && px[3] >= 253;
    vk_log("selftest %s 1280x720 RGBA8 px=%02x%02x%02x%02x device='%s'",
           ok ? "PASS" : "FAIL", px[0], px[1], px[2], px[3], g.props.deviceName);
    gfx_backend_offscreen_destroy(t);
    return ok;
}

/* Draw a fullscreen triangle that samples a known BC4 texture.  Vulkan's
 * block-compressed view semantics require sample == (R, 0, 0, 1); a channel
 * swap or a zero return here would explain the zero UI chain. */
int bc4_sample_selftest_size(uint32_t size) {
    uint32_t grid = size / 4;
    if (!grid) return 0;
    size_t bytes = (size_t)grid * grid * 8;
    uint8_t *blocks = calloc(1, bytes);
    if (!blocks) return 0;
    for (uint32_t by = 0; by < grid; by++) {
        for (uint32_t bx = 0; bx < grid; bx++) {
            uint8_t *b = blocks + ((size_t)by * grid + bx) * 8;
            bc4_probe_constant_block(b, bx < grid / 2 ? 64 : 192);
        }
    }
    uint32_t sbx = grid / 2, sby = grid / 2;
    bc4_probe_half_block(blocks + ((size_t)sby * grid + sbx) * 8, 0, 255);

    GfxTextureDesc d = {
        .target = GFX_TEXTURE_2D,
        .vk_format = VK_FORMAT_BC4_UNORM_BLOCK,
        .width = size, .height = size, .depth = 1,
        .mip_levels = 1, .array_layers = 1, .samples = 1,
    };
    GfxTextureSubresource sub = { 0, 0, blocks, bytes };
    GfxTexture *tex = gfx_backend_texture_create_desc(&d, &sub, 1);
    GfxOffscreenTarget *t = tex ? gfx_backend_offscreen_create(size, size, 0x25) : NULL;
    GfxTexturedPipeline *p = t ? textured_pipeline_create_bc4(VK_FORMAT_R8G8B8A8_UNORM) : NULL;
    int ok = 0;
    if (p && gfx_backend_textured_draw(p, tex, t, 0.0f, 1.0f, 0.0f, 1.0f)) {
        uint32_t left_x = size / 8, mid_y = size / 2;
        uint32_t right_x = size - size / 8;
        uint32_t sx = sbx * 4 + 1, sy = sby * 4;
        ok = bc4_probe_expect(t, left_x, mid_y, 64, "left half") &&
             bc4_probe_expect(t, right_x, mid_y, 192, "right half") &&
             bc4_probe_expect(t, sx, sy, 0, "index 0") &&
             bc4_probe_expect(t, sx, sy + 2, 255, "index 1");
        if (ok)
            vk_log("BC4 sample %ux%u PASS: (R,0,0,1) left=64 right=192 "
                   "index0=0 index1=255", size, size);
    } else {
        vk_log("ERROR BC4 sample %ux%u: pipeline or draw failed", size, size);
    }
    if (p) gfx_backend_textured_pipeline_destroy(p);
    if (t) gfx_backend_offscreen_destroy(t);
    if (tex) gfx_backend_texture_destroy(tex);
    free(blocks);
    return ok;
}

int gfx_backend_bc4_sample_selftest(void) {
    if (!gfx_backend_vulkan_init()) return 0;
    int ok64 = bc4_sample_selftest_size(64);
    int ok96 = bc4_sample_selftest_size(96);
    vk_log("BC4 sampled draw %s (64x64 %s, 96x96 %s)",
           ok64 && ok96 ? "PASS" : "FAIL", ok64 ? "PASS" : "FAIL",
           ok96 ? "PASS" : "FAIL");
    return ok64 && ok96;
}
