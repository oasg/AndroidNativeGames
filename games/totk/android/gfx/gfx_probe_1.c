/* Module: probe_1. */
#include "gfx_internal.h"

int gfx_backend_copy_probe(const char *work_dir) {
    vk_log("copy probe begin work=%s", work_dir ? work_dir : "(null)");
    if (!gfx_backend_vulkan_init()) {
        vk_log("copy probe FAIL: Vulkan unavailable");
        return 1;
    }
    const uint32_t W = 64, H = 64;
    const size_t BYTES = (size_t)W * H * 4;
    uint8_t *pat_a = malloc(BYTES), *pat_b = malloc(BYTES);
    uint8_t *got_a = malloc(BYTES), *got_b = malloc(BYTES);
    int ok = pat_a && pat_b && got_a && got_b;
    if (!ok) {
        vk_log("copy probe FAIL: host allocation failed");
        free(pat_a); free(pat_b); free(got_a); free(got_b);
        return 1;
    }
    vk_copy_probe_fill(pat_a, W, H, 0x20);
    vk_copy_probe_fill(pat_b, W, H, 0x80);

    GfxTextureDesc desc = {
        .target = GFX_TEXTURE_2D,
        .vk_format = VK_FORMAT_R8G8B8A8_UNORM,
        .width = W, .height = H, .depth = 1,
        .mip_levels = 1, .array_layers = 1, .samples = 1,
    };
    GfxTexture *a = gfx_backend_texture_create_desc(&desc, NULL, 0);
    GfxTexture *b = gfx_backend_texture_create_desc(&desc, NULL, 0);
    if (!a || !b) {
        vk_log("copy probe FAIL: RGBA8 texture creation failed");
        ok = 0;
    }
    if (ok && (!gfx_backend_texture_upload(a, 0, 0, 0, 0, W, H, pat_a, BYTES) ||
               !gfx_backend_texture_upload(b, 0, 0, 0, 0, W, H, pat_b, BYTES))) {
        vk_log("copy probe FAIL: upload failed");
        ok = 0;
    }
    if (ok && !gfx_backend_texture_copy(b, 0, 0, 16, 16, 0,
                                        a, 0, 0, 8, 8, 0, 16, 16, 1)) {
        vk_log("copy probe FAIL: same-format copy failed");
        ok = 0;
    }
    const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    if (ok && !gfx_backend_texture_clear(b, 0, 0, red, 0, 0, 8, 8)) {
        vk_log("copy probe FAIL: sub-region clear failed");
        ok = 0;
    }
    if (ok) {
        memset(got_a, 0, BYTES);
        memset(got_b, 0, BYTES);
        if (!gfx_backend_texture_download(a, 0, 0, 0, 0, W, H, got_a, BYTES) ||
            !gfx_backend_texture_download(b, 0, 0, 0, 0, W, H, got_b, BYTES)) {
            vk_log("copy probe FAIL: download failed");
            ok = 0;
        } else if (memcmp(got_a, pat_a, BYTES)) {
            size_t i = 0;
            while (i < BYTES && got_a[i] == pat_a[i]) i++;
            vk_log("copy probe FAIL: A upload/download mismatch at byte %zu got=%02x want=%02x",
                   i, got_a[i], pat_a[i]);
            ok = 0;
        } else {
            vk_log("copy probe: A upload/download %ux%u exact match", W, H);
        }
    }
    if (ok) {
        for (uint32_t y = 0; y < H && ok; y++) {
            for (uint32_t x = 0; x < W && ok; x++) {
                const uint8_t *want;
                if (x < 8 && y < 8) {
                    want = (const uint8_t[]){ 255, 0, 0, 255 };
                } else if (x >= 16 && x < 32 && y >= 16 && y < 32) {
                    uint32_t sx = x - 16 + 8, sy = y - 16 + 8;
                    want = pat_a + ((size_t)sy * W + sx) * 4;
                } else {
                    want = pat_b + ((size_t)y * W + x) * 4;
                }
                const uint8_t *have = got_b + ((size_t)y * W + x) * 4;
                if (memcmp(have, want, 4)) {
                    vk_log("copy probe FAIL: B[%u,%u] got=%02x%02x%02x%02x "
                           "want=%02x%02x%02x%02x", x, y, have[0], have[1], have[2],
                           have[3], want[0], want[1], want[2], want[3]);
                    ok = 0;
                }
            }
        }
        if (ok) vk_log("copy probe: copied and cleared regions match every byte");
    }

    /* Block-compressed region upload/download.  TOTK renders BC4 textures
     * through an uncompressed block view, so the block path must round-trip. */
    if (ok) {
        const uint32_t BC4_W = 64, BC4_H = 64;
        size_t bc4_size = (size_t)(BC4_W / 4) * (BC4_H / 4) * 8;
        uint8_t *zero = calloc(1, bc4_size);
        GfxTextureDesc bd = {
            .target = GFX_TEXTURE_2D,
            .vk_format = VK_FORMAT_BC4_UNORM_BLOCK,
            .width = BC4_W, .height = BC4_H, .depth = 1,
            .mip_levels = 1, .array_layers = 1, .samples = 1,
        };
        GfxTextureSubresource bs = { 0, 0, zero, bc4_size };
        GfxTexture *bt = zero ? gfx_backend_texture_create_desc(&bd, &bs, 1) : NULL;
        uint8_t blocks[32];
        for (unsigned i = 0; i < sizeof blocks; i++)
            blocks[i] = (uint8_t)(0x40 + i * 3);
        if (!bt || !gfx_backend_texture_upload(bt, 0, 0, 8, 8, 8, 8,
                                               blocks, sizeof blocks)) {
            vk_log("copy probe FAIL: BC4 block upload failed");
            ok = 0;
        } else {
            uint8_t *got = malloc(bc4_size);
            if (!got || !gfx_backend_texture_download(bt, 0, 0, 0, 0, BC4_W, BC4_H,
                                                      got, bc4_size)) {
                vk_log("copy probe FAIL: BC4 block download failed");
                ok = 0;
            } else {
                size_t row = (BC4_W / 4) * 8;
                for (uint32_t r = 0; r < 2 && ok; r++) {
                    if (memcmp(got + (2 + r) * row + 2 * 8, blocks + r * 16, 16)) {
                        vk_log("copy probe FAIL: BC4 block row %u mismatch", r);
                        ok = 0;
                    }
                }
                if (ok) vk_log("copy probe: BC4 2x2 block upload/download exact match");
            }
            free(got);
        }
        if (bt) gfx_backend_texture_destroy(bt);
        free(zero);
    }

    if (ok && !gfx_backend_bc4_sample_selftest()) {
        vk_log("copy probe FAIL: BC4 sampled draw");
        ok = 0;
    }

    /* Format-mismatch blit, only when the device advertises both sides. */
    VkFormat hdr_fmt = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g.phys, hdr_fmt, &fp);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    int hdr_ok = (fp.optimalTilingFeatures & need) == need &&
                 vk_format_can_blit(VK_FORMAT_R8G8B8A8_UNORM, 0);
    if (!hdr_ok) {
        vk_log("copy probe: format-mismatch blit skipped (RGBA8 -> B10G11R11 features=%#x)",
               fp.optimalTilingFeatures);
    } else if (ok) {
        GfxTextureDesc hd = {
            .target = GFX_TEXTURE_2D,
            .vk_format = hdr_fmt,
            .width = W, .height = H, .depth = 1,
            .mip_levels = 1, .array_layers = 1, .samples = 1,
        };
        GfxTexture *hdr = gfx_backend_texture_create_desc(&hd, NULL, 0);
        if (!hdr) {
            vk_log("copy probe: format-mismatch blit skipped (HDR texture creation failed)");
        } else if (!gfx_backend_texture_copy(hdr, 0, 0, 0, 0, 0,
                                             a, 0, 0, 0, 0, 0, W, H, 1)) {
            vk_log("copy probe: format-mismatch blit skipped (backend rejected blit)");
        } else {
            uint8_t *got = malloc(BYTES);
            if (!got) {
                vk_log("copy probe FAIL: HDR readback allocation failed");
                ok = 0;
            } else if (!gfx_backend_texture_download(hdr, 0, 0, 0, 0, W, H, got, BYTES)) {
                vk_log("copy probe FAIL: HDR blit readback failed");
                ok = 0;
            } else {
                const uint32_t probes[3][2] = { {0, 0}, {10, 10}, {63, 63} };
                for (unsigned i = 0; i < 3 && ok; i++) {
                    uint32_t x = probes[i][0], y = probes[i][1];
                    uint32_t packed;
                    memcpy(&packed, got + ((size_t)y * W + x) * 4, 4);
                    float rgb[3];
                    vk_decode_b10g11r11(packed, rgb);
                    const uint8_t *src = pat_a + ((size_t)y * W + x) * 4;
                    float want[3] = { src[0] / 255.0f, src[1] / 255.0f, src[2] / 255.0f };
                    for (unsigned c = 0; c < 3; c++) {
                        float diff = rgb[c] - want[c];
                        if (diff < 0) diff = -diff;
                        if (diff > 0.02f) {
                            vk_log("copy probe FAIL: blit[%u,%u] channel %u got=%.4f "
                                   "want=%.4f", x, y, c, rgb[c], want[c]);
                            ok = 0;
                        }
                    }
                }
                if (ok) vk_log("copy probe: RGBA8 -> B10G11R11 nearest blit verified");
            }
            free(got);
        }
        if (hdr) gfx_backend_texture_destroy(hdr);
    }

    if (a) gfx_backend_texture_destroy(a);
    if (b) gfx_backend_texture_destroy(b);
    free(pat_a); free(pat_b); free(got_a); free(got_b);
    vk_log("copy probe %s", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
