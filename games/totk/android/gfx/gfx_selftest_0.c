/* Module: selftest_0. */
#include "gfx_internal.h"

int gfx_backend_reflected_texture_selftest(void) {
    const uint8_t checker[16] = {
        255, 0, 0, 255,   0, 255, 0, 255,
        0, 0, 255, 255,   255, 255, 255, 255,
    };
    GfxOffscreenTarget *t = gfx_backend_offscreen_create(1280, 720, 0x25);
    GfxTexture *tex = gfx_backend_texture_create_rgba8(2, 2, checker);
    GfxReflectedPipeline *p = t && tex ? gfx_backend_reflected_builtin_texture_pipeline_create() : NULL;
    int ok = 0;
    if (p && gfx_backend_reflected_bind_texture(p, 1, 0, tex)) {
        for (int i = 0; i < 10; i++)
            if (!gfx_backend_reflected_draw(p, t, 0.05f, 0.08f, 0.12f, 1.0f)) break;
        uint8_t center[4] = {0}, corner[4] = {0};
        gfx_backend_offscreen_read_pixel(t, 640, 360, center);
        gfx_backend_offscreen_read_pixel(t, 0, 0, corner);
        ok = center[0] > 80 && center[0] < 180 &&
             center[1] > 80 && center[1] < 180 &&
             center[2] > 80 && center[2] < 180 && corner[0] < 40;
        vk_log("reflected texture selftest center=%02x%02x%02x%02x corner=%02x%02x%02x%02x",
               center[0], center[1], center[2], center[3],
               corner[0], corner[1], corner[2], corner[3]);
    }
    if (p) gfx_backend_reflected_pipeline_destroy(p);
    if (tex) gfx_backend_texture_destroy(tex);
    if (t) gfx_backend_offscreen_destroy(t);
    return ok;
}

int gfx_backend_reflected_ubo_selftest(void) {
    GfxBindingSpec ps = {
        .set = 1, .binding = 0, .kind = GFX_BIND_UBO, .count = 1,
    };
    GfxOffscreenTarget *t = gfx_backend_offscreen_create(1280, 720, 0x25);
    GfxReflectedPipeline *p = t ? gfx_backend_reflected_pipeline_create(
        (const uint8_t *)triangle_vert_spv, triangle_vert_spv_size,
        (const uint8_t *)reflect_ubo_frag_spv, reflect_ubo_frag_spv_size,
        gfx_backend_vk_format_for_nvn(0x25), NULL, 0, &ps, 1) : NULL;
    float color[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    int ok = 0;
    if (p && gfx_backend_reflected_write_uniform(p, 1, 0, 0, color, sizeof color)) {
        for (int i = 0; i < 10; i++)
            if (!gfx_backend_reflected_draw(p, t, 0.05f, 0.08f, 0.12f, 1.0f)) break;
        uint8_t center[4] = {0};
        gfx_backend_offscreen_read_pixel(t, 640, 360, center);
        ok = center[0] == 0x40 && center[1] == 0x80 && center[2] == 0xbf && center[3] == 0xff;
        vk_log("reflected ubo selftest center=%02x%02x%02x%02x", center[0], center[1],
               center[2], center[3]);
    }
    if (p) gfx_backend_reflected_pipeline_destroy(p);
    if (t) gfx_backend_offscreen_destroy(t);
    return ok;
}

int vk_selftest_read_match(GfxTexture *t, uint32_t level, uint32_t layer,
                                  const uint8_t *expect, size_t n, const char *what) {
    uint8_t *got = malloc(n ? n : 1);
    if (!got) return 0;
    int ok = gfx_backend_texture_read_pixels(t, level, layer, got, n);
    if (!ok) {
        vk_log("texture formats selftest: %s readback failed", what);
        free(got);
        return 0;
    }
    if (memcmp(got, expect, n)) {
        size_t i = 0;
        while (i + 1 < n && got[i] == expect[i]) i++;
        vk_log("texture formats selftest: %s mismatch at byte %zu got=%02x want=%02x",
               what, i, got[i], expect[i]);
        ok = 0;
    } else {
        vk_log("texture formats selftest: %s OK (%zu bytes)", what, n);
    }
    free(got);
    return ok;
}

int gfx_backend_texture_formats_selftest(void) {
    if (!gfx_backend_vulkan_init()) {
        vk_log("texture formats selftest: Vulkan unavailable");
        return 0;
    }
    int ok = 1;

    /* 2D array, two mip levels, two layers.  Level 1 has 2x2 texels. */
    {
        enum { W = 4, H = 4 };
        uint8_t l0a[W * H * 4], l0b[W * H * 4], l1a[2 * 2 * 4], l1b[2 * 2 * 4];
        for (int i = 0; i < W * H; i++) {
            l0a[i * 4 + 0] = 0x10; l0a[i * 4 + 1] = 0x20;
            l0a[i * 4 + 2] = 0x30; l0a[i * 4 + 3] = 0xff;
            l0b[i * 4 + 0] = 0x40; l0b[i * 4 + 1] = 0x50;
            l0b[i * 4 + 2] = 0x60; l0b[i * 4 + 3] = 0xff;
        }
        for (int i = 0; i < 2 * 2; i++) {
            l1a[i * 4 + 0] = 0x70; l1a[i * 4 + 1] = 0x80;
            l1a[i * 4 + 2] = 0x90; l1a[i * 4 + 3] = 0xff;
            l1b[i * 4 + 0] = 0xa0; l1b[i * 4 + 1] = 0xb0;
            l1b[i * 4 + 2] = 0xc0; l1b[i * 4 + 3] = 0xff;
        }
        GfxTextureDesc d = {
            .target = GFX_TEXTURE_2D_ARRAY, .vk_format = VK_FORMAT_R8G8B8A8_UNORM,
            .width = W, .height = H, .depth = 1, .mip_levels = 2, .array_layers = 2,
            .samples = 1,
        };
        GfxTextureSubresource s[4] = {
            { 0, 0, l0a, sizeof l0a }, { 0, 1, l0b, sizeof l0b },
            { 1, 0, l1a, sizeof l1a }, { 1, 1, l1b, sizeof l1b },
        };
        GfxTexture *t = gfx_backend_texture_create_desc(&d, s, 4);
        if (!t) {
            vk_log("texture formats selftest: 2D array/mips create failed");
            ok = 0;
        } else {
            ok &= vk_selftest_read_match(t, 1, 1, l1b, sizeof l1b, "2D array mip1 layer1");
            ok &= vk_selftest_read_match(t, 0, 1, l0b, sizeof l0b, "2D array mip0 layer1");
            gfx_backend_texture_destroy(t);
        }
    }

    /* Cube: six 2x2 faces with distinct colours. */
    {
        enum { F = 6, N = 2 * 2 * 4 };
        uint8_t faces[F][N];
        for (int f = 0; f < F; f++)
            for (int i = 0; i < 2 * 2; i++) {
                faces[f][i * 4 + 0] = (uint8_t)(0x20 + f * 0x20);
                faces[f][i * 4 + 1] = (uint8_t)(0x10 + f * 0x10);
                faces[f][i * 4 + 2] = (uint8_t)(0xf0 - f * 0x10);
                faces[f][i * 4 + 3] = 0xff;
            }
        GfxTextureDesc d = {
            .target = GFX_TEXTURE_CUBE, .vk_format = VK_FORMAT_R8G8B8A8_UNORM,
            .width = 2, .height = 2, .depth = 1, .mip_levels = 1, .array_layers = 6,
            .samples = 1,
        };
        GfxTextureSubresource s[F];
        for (int f = 0; f < F; f++)
            s[f] = (GfxTextureSubresource){ 0, (uint32_t)f, faces[f], N };
        GfxTexture *t = gfx_backend_texture_create_desc(&d, s, F);
        if (!t) {
            vk_log("texture formats selftest: cube create failed");
            ok = 0;
        } else {
            ok &= vk_selftest_read_match(t, 0, 4, faces[4], N, "cube face4");
            gfx_backend_texture_destroy(t);
        }
    }

    /* 3D texture: two 2x2 slices, verify the first slice round-trips. */
    {
        uint8_t vol[2 * 2 * 2 * 4];
        for (int z = 0; z < 2; z++)
            for (int i = 0; i < 2 * 2; i++) {
                int o = (z * 4 + i) * 4;
                vol[o + 0] = z ? 0x30 : 0x80;
                vol[o + 1] = z ? 0x60 : 0x40;
                vol[o + 2] = z ? 0x90 : 0x20;
                vol[o + 3] = 0xff;
            }
        GfxTextureDesc d = {
            .target = GFX_TEXTURE_3D, .vk_format = VK_FORMAT_R8G8B8A8_UNORM,
            .width = 2, .height = 2, .depth = 2, .mip_levels = 1, .array_layers = 1,
            .samples = 1,
        };
        GfxTextureSubresource s = { 0, 0, vol, sizeof vol };
        GfxTexture *t = gfx_backend_texture_create_desc(&d, &s, 1);
        if (!t) {
            vk_log("texture formats selftest: 3D create failed");
            ok = 0;
        } else {
            ok &= vk_selftest_read_match(t, 0, 0, vol, sizeof vol, "3D volume");
            gfx_backend_texture_destroy(t);
        }
    }

    /* sRGB upload/readback must preserve the encoded bytes. */
    {
        uint8_t px[2 * 2 * 4];
        for (int i = 0; i < 2 * 2; i++) {
            px[i * 4 + 0] = 0x7f; px[i * 4 + 1] = 0x3a;
            px[i * 4 + 2] = 0xc4; px[i * 4 + 3] = 0xff;
        }
        GfxTextureDesc d = {
            .target = GFX_TEXTURE_2D, .vk_format = VK_FORMAT_R8G8B8A8_SRGB,
            .width = 2, .height = 2, .depth = 1, .mip_levels = 1, .array_layers = 1,
            .samples = 1,
        };
        GfxTextureSubresource s = { 0, 0, px, sizeof px };
        GfxTexture *t = gfx_backend_texture_create_desc(&d, &s, 1);
        if (!t) {
            vk_log("texture formats selftest: sRGB create failed");
            ok = 0;
        } else {
            ok &= vk_selftest_read_match(t, 0, 0, px, sizeof px, "RGBA8 sRGB");
            gfx_backend_texture_destroy(t);
        }
    }

    /* Extra uncompressed formats from the NVN/Ryujinx mapping tables. */
    {
        const struct { VkFormat fmt; const char *name; } extra[] = {
            { VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F" },
            { VK_FORMAT_R32G32B32A32_SFLOAT, "RGBA32F" },
            { VK_FORMAT_B10G11R11_UFLOAT_PACK32, "R11G11B10F" },
            { VK_FORMAT_D32_SFLOAT, "D32F" },
        };
        for (unsigned i = 0; i < sizeof extra / sizeof extra[0]; i++) {
            VkFormatFeatureFlags ef = 0;
            if (!vk_format_can_texture(extra[i].fmt, &ef) ||
                !(ef & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) {
                vk_log("texture formats selftest: %s unsupported, skipped", extra[i].name);
                continue;
            }
            uint32_t bpp = (uint32_t)vk_format_info(extra[i].fmt).bpp;
            uint8_t px[2 * 2 * 16];
            if (!bpp || bpp > 16) continue;
            size_t n = (size_t)2 * 2 * bpp;
            for (size_t b = 0; b < n; b++) px[b] = (uint8_t)(0x23 * (b + 1));
            GfxTextureDesc d = {
                .target = GFX_TEXTURE_2D, .vk_format = (uint32_t)extra[i].fmt,
                .width = 2, .height = 2, .depth = 1, .mip_levels = 1, .array_layers = 1,
                .samples = 1,
            };
            GfxTextureSubresource s = { 0, 0, px, n };
            GfxTexture *t = gfx_backend_texture_create_desc(&d, &s, 1);
            if (!t) {
                vk_log("texture formats selftest: %s create failed", extra[i].name);
                ok = 0;
                continue;
            }
            char label[32];
            snprintf(label, sizeof label, "%s", extra[i].name);
            ok &= vk_selftest_read_match(t, 0, 0, px, n, label);
            gfx_backend_texture_destroy(t);
        }
    }

    /* Block compressed formats the device actually advertises.  BC is not
     * mandatory on Android/Adreno; skip (not fail) when unavailable. */
    {
        static const struct { VkFormat fmt; const char *name; } bc[] = {
            { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1" },
            { VK_FORMAT_BC3_UNORM_BLOCK, "BC3" },
            { VK_FORMAT_BC4_UNORM_BLOCK, "BC4" },
            { VK_FORMAT_BC5_UNORM_BLOCK, "BC5" },
            { VK_FORMAT_BC7_UNORM_BLOCK, "BC7" },
        };
        unsigned tested = 0, skipped = 0;
        for (unsigned i = 0; i < sizeof bc / sizeof bc[0]; i++) {
            VkFormatFeatureFlags bcf = 0;
            if (!vk_format_can_texture(bc[i].fmt, &bcf) ||
                !(bcf & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) {
                skipped++;
                continue;
            }
            uint8_t block[16];
            uint32_t bytes = (uint32_t)vk_format_info(bc[i].fmt).bpp;
            if (!bytes || bytes > sizeof block) { skipped++; continue; }
            for (uint32_t b = 0; b < bytes; b++) block[b] = (uint8_t)(0x11 * (b + 1));
            GfxTextureDesc d = {
                .target = GFX_TEXTURE_2D, .vk_format = (uint32_t)bc[i].fmt,
                .width = 4, .height = 4, .depth = 1, .mip_levels = 1, .array_layers = 1,
                .samples = 1,
            };
            GfxTextureSubresource s = { 0, 0, block, bytes };
            GfxTexture *t = gfx_backend_texture_create_desc(&d, &s, 1);
            if (!t) {
                vk_log("texture formats selftest: %s create failed", bc[i].name);
                ok = 0;
                continue;
            }
            char label[32];
            snprintf(label, sizeof label, "%s block", bc[i].name);
            ok &= vk_selftest_read_match(t, 0, 0, block, bytes, label);
            tested++;
            gfx_backend_texture_destroy(t);
        }
        vk_log("texture formats selftest: BC tested=%u skipped=%u", tested, skipped);
    }

    vk_log("texture formats selftest %s", ok ? "PASS" : "FAIL");
    return ok;
}

int gfx_backend_mrt_selftest(void) {
    if (!gfx_backend_vulkan_init()) {
        vk_log("MRT selftest: Vulkan unavailable");
        return 0;
    }
    GfxRenderTargetLayout layout = { 0 };
    layout.color_count = 2;
    layout.color_formats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    layout.color_formats[1] = VK_FORMAT_R8G8B8A8_UNORM;
    layout.depth_format = (int)g.depth_format;
    GfxOffscreenTarget *t = gfx_backend_offscreen_create_layout(128, 128, &layout);
    if (!t) {
        vk_log("MRT selftest: 2-color+depth target creation failed");
        return 0;
    }
    GfxReflectedPipeline *p = gfx_backend_reflected_pipeline_create_layout(
        (const uint8_t *)triangle_mrt_vert_spv, triangle_mrt_vert_spv_size,
        (const uint8_t *)triangle_mrt_frag_spv, triangle_mrt_frag_spv_size,
        &layout, NULL, 0, NULL, 0, NULL);
    if (!p) {
        vk_log("MRT selftest: pipeline creation failed");
        gfx_backend_offscreen_destroy(t);
        return 0;
    }

    const float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
    GfxCmd cmds[3] = { 0 };
    cmds[0].kind = GFX_CMD_CLEAR_COLOR;
    memcpy(cmds[0].u.clear.rgba, clear, sizeof clear);
    cmds[1].kind = GFX_CMD_CLEAR_DEPTH_STENCIL;
    cmds[1].u.clear_depth.depth = 1.0f;
    cmds[1].u.clear_depth.dmask = 1;
    cmds[1].u.clear_depth.smask = 1;
    cmds[2].kind = GFX_CMD_DRAW;
    cmds[2].u.draw.primitive = 4;
    cmds[2].u.draw.count = 3;
    int drew = 0;
    for (int i = 0; i < 10; i++) {
        if (!gfx_backend_reflected_execute(p, t, cmds, 3, clear)) break;
        drew++;
    }
    uint8_t c0[4] = { 0 }, c1[4] = { 0 };
    gfx_backend_offscreen_read_pixel_index(t, 0, 64, 64, c0);
    gfx_backend_offscreen_read_pixel_index(t, 1, 64, 64, c1);
    int ok = drew && c0[0] > 200 && c0[1] > 40 && c0[1] < 90 && c0[2] < 40 &&
             c1[0] < 60 && c1[1] > 180 && c1[2] > 20 && c1[2] < 80;
    vk_log("MRT selftest %s draws=%d attach0=%02x%02x%02x%02x attach1=%02x%02x%02x%02x "
           "depth=%s colors=%u",
           ok ? "PASS" : "FAIL", drew, c0[0], c0[1], c0[2], c0[3],
           c1[0], c1[1], c1[2], c1[3],
           vk_format_name((VkFormat)layout.depth_format),
           gfx_backend_offscreen_color_count(t));
    const char *shot = getenv("TOTK_ANDROID_SCREENSHOT");
    if (shot && *shot) {
        char path[1024];
        snprintf(path, sizeof path, "%s-mrt0.ppm", shot);
        gfx_backend_offscreen_write_ppm_index(t, 0, path);
        snprintf(path, sizeof path, "%s-mrt1.ppm", shot);
        gfx_backend_offscreen_write_ppm_index(t, 1, path);
    }
    GfxFence *fence = gfx_backend_fence_create();
    int fence_ok = fence && !gfx_backend_fence_wait(fence, 0);
    if (fence_ok) {
        gfx_backend_fence_signal(fence);
        fence_ok = gfx_backend_fence_wait(fence, 1000000);
    }
    fence_ok = fence_ok && gfx_backend_queue_idle();
    vk_log("MRT selftest fence %s", fence_ok ? "OK" : "FAIL");
    gfx_backend_fence_destroy(fence);
    ok = ok && fence_ok;
    gfx_backend_reflected_pipeline_destroy(p);
    gfx_backend_offscreen_destroy(t);
    return ok;
}
