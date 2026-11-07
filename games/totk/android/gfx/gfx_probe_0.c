/* Module: probe_0. */
#include "gfx_internal.h"

#include "gfx_compute_spv.h"

/* Run the production target-present copy into a 96x64 RGBA8 image shaped like
 * a landscape surface and read the result back.  This exercises the same
 * vk_present_target_locked() used on the swapchain without requiring a real
 * ANativeWindow, so the CLI can validate format conversion and scaling. */
int vk_present_probe_locked(GfxOffscreenTarget *t, uint32_t scale_mode,
                                   uint8_t **out_pixels) {
    const uint32_t w = 96, h = 64;
    *out_pixels = NULL;
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory bmem = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    uint8_t *pixels = NULL;
    int ok = 0;
    if (!vk_create_device_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, 1,
                                VK_SAMPLE_COUNT_1_BIT,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                0, &img, &mem))
        goto done;
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (!vk_ok(vkCreateImageView(g.dev, &vci, NULL, &view), "vkCreateImageView(present probe)"))
        goto done;
    if (!vk_create_host_buffer((VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               &buf, &bmem))
        goto done;

    VkCommandBuffer cmd = g.cmd;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (!vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(present probe)"))
        goto done;
    if (!vk_present_target_locked(cmd, t, img, view, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_FORMAT_R8G8B8A8_UNORM, (VkExtent2D){ w, h },
                                  scale_mode, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, &fb)) {
        vkEndCommandBuffer(cmd);
        goto done;
    }
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { w, h, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    VkBufferMemoryBarrier bb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buf, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &bb, 0, NULL);
    int submitted = vk_submit_and_wait(cmd, "present probe");
    if (fb) { vkDestroyFramebuffer(g.dev, fb, NULL); fb = VK_NULL_HANDLE; }
    if (!submitted) goto done;
    void *mapped = NULL;
    if (!vk_ok(vkMapMemory(g.dev, bmem, 0, VK_WHOLE_SIZE, 0, &mapped),
               "vkMapMemory(present probe)"))
        goto done;
    pixels = malloc((size_t)w * h * 4);
    if (pixels) memcpy(pixels, mapped, (size_t)w * h * 4);
    vkUnmapMemory(g.dev, bmem);
    if (!pixels) goto done;
    *out_pixels = pixels;
    pixels = NULL;
    ok = 1;

done:
    free(pixels);
    if (fb) vkDestroyFramebuffer(g.dev, fb, NULL);
    if (buf) vkDestroyBuffer(g.dev, buf, NULL);
    if (bmem) vkFreeMemory(g.dev, bmem, NULL);
    if (view) vkDestroyImageView(g.dev, view, NULL);
    if (img) vkDestroyImage(g.dev, img, NULL);
    if (mem) vkFreeMemory(g.dev, mem, NULL);
    return ok;
}

void probe_pixel(const uint8_t *pixels, uint32_t x, uint32_t y, uint8_t out[4]) {
    memcpy(out, pixels + ((size_t)y * 96 + x) * 4, 4);
}

uint32_t vk_compute_probe_value(uint32_t x, uint32_t y, const uint8_t *tex,
                                       uint32_t seed) {
    uint32_t texv = tex[((y & 3u) * 4u + (x & 3u)) * 4u];
    return (x + 1u) * 3u + (y + 1u) * 5u + seed + texv * 7u;
}

void vk_compute_probe_fill_texture(uint8_t *pixels, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *p = pixels + ((size_t)y * w + x) * 4;
            p[0] = (uint8_t)((x + y * 4u) * 16u);
            p[1] = (uint8_t)(x * 32u);
            p[2] = (uint8_t)(y * 64u);
            p[3] = 255;
        }
    }
}

int gfx_backend_compute_probe(const char *work_dir) {
    vk_log("compute probe begin work=%s", work_dir ? work_dir : "(null)");
    if (!gfx_backend_vulkan_init()) {
        vk_log("compute probe FAIL: Vulkan unavailable");
        return 1;
    }
    int ok = 1;
    uint8_t texels[4 * 4 * 4];
    vk_compute_probe_fill_texture(texels, 4, 4);
    GfxTexture *tex = gfx_backend_texture_create_rgba8(4, 4, texels);
    if (!tex) {
        vk_log("compute probe FAIL: sampled texture creation failed");
        return 1;
    }
    GfxBindingSpec specs[3] = {
        { .set = 0, .binding = 0, .kind = GFX_BIND_UBO, .count = 1 },
        { .set = 0, .binding = 1, .kind = GFX_BIND_SSBO, .count = 1 },
        { .set = 0, .binding = 2, .kind = GFX_BIND_COMBINED_IMAGE_SAMPLER, .count = 1 },
    };
    GfxComputePipeline *p = gfx_backend_compute_pipeline_create(
        (const uint8_t *)compute_probe_comp_spv, compute_probe_comp_spv_size, specs, 3);
    if (!p) {
        vk_log("compute probe FAIL: pipeline creation failed");
        gfx_backend_texture_destroy(tex);
        return 1;
    }
    GfxComputeProbeParams params = {
        .width = 8, .height = 8, .seed = 0x12345678u, .pad = 0,
        .scale = { 1.0f, 1.0f, 1.0f, 1.0f },
    };
    uint32_t out[64];
    memset(out, 0, sizeof out);
    if (!gfx_backend_compute_bind_buffer(p, 0, 0, &params, sizeof params) ||
        !gfx_backend_compute_bind_buffer(p, 0, 1, out, sizeof out) ||
        !gfx_backend_compute_bind_texture(p, 0, 2, tex)) {
        vk_log("compute probe FAIL: buffer/texture bind failed");
        ok = 0;
    }
    if (ok && !gfx_backend_compute_dispatch(p, 1, 1, 1)) {
        vk_log("compute probe FAIL: dispatch failed");
        ok = 0;
    }
    if (ok) {
        uint32_t got[64];
        memset(got, 0, sizeof got);
        if (!gfx_backend_compute_read_buffer(p, 0, 1, got, sizeof got)) {
            vk_log("compute probe FAIL: SSBO readback failed");
            ok = 0;
        } else {
            for (uint32_t y = 0; y < 8 && ok; y++) {
                for (uint32_t x = 0; x < 8 && ok; x++) {
                    uint32_t want = vk_compute_probe_value(x, y, texels, params.seed);
                    uint32_t have = got[y * 8 + x];
                    if (want != have) {
                        vk_log("compute probe FAIL: SSBO[%u,%u] got=%08x want=%08x",
                               x, y, have, want);
                        ok = 0;
                    }
                }
            }
            if (ok) vk_log("compute probe: SSBO 8x8 exact match");
        }
    }

    /* Storage image path, only when the device advertises the format. */
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g.phys, VK_FORMAT_R8G8B8A8_UNORM, &fp);
    int storage_supported =
        (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0 &&
        (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0;
    if (!storage_supported) {
        vk_log("compute probe: storage image case skipped (RGBA8 optimal=%#x)",
               fp.optimalTilingFeatures);
    } else {
        GfxTextureDesc d = {
            .target = GFX_TEXTURE_2D,
            .vk_format = VK_FORMAT_R8G8B8A8_UNORM,
            .width = 8, .height = 8, .depth = 1,
            .mip_levels = 1, .array_layers = 1, .samples = 1,
        };
        GfxTexture *img = gfx_backend_texture_create_desc(&d, NULL, 0);
        GfxBindingSpec specs4[4] = {
            { .set = 0, .binding = 0, .kind = GFX_BIND_UBO, .count = 1 },
            { .set = 0, .binding = 1, .kind = GFX_BIND_SSBO, .count = 1 },
            { .set = 0, .binding = 2, .kind = GFX_BIND_COMBINED_IMAGE_SAMPLER, .count = 1 },
            { .set = 0, .binding = 3, .kind = GFX_BIND_STORAGE_IMAGE, .count = 1 },
        };
        GfxComputePipeline *p2 = gfx_backend_compute_pipeline_create(
            (const uint8_t *)compute_probe_image_comp_spv,
            compute_probe_image_comp_spv_size, specs4, 4);
        if (!img || !p2) {
            vk_log("compute probe FAIL: storage image texture/pipeline creation failed");
            ok = 0;
        } else {
            uint32_t out2[64];
            memset(out2, 0, sizeof out2);
            if (!gfx_backend_compute_bind_buffer(p2, 0, 0, &params, sizeof params) ||
                !gfx_backend_compute_bind_buffer(p2, 0, 1, out2, sizeof out2) ||
                !gfx_backend_compute_bind_texture(p2, 0, 2, tex) ||
                !gfx_backend_compute_bind_texture(p2, 0, 3, img)) {
                vk_log("compute probe FAIL: storage image bind failed");
                ok = 0;
            } else if (!gfx_backend_compute_dispatch(p2, 1, 1, 1)) {
                vk_log("compute probe FAIL: storage image dispatch failed");
                ok = 0;
            } else {
                uint8_t pixels[8 * 8 * 4];
                memset(pixels, 0, sizeof pixels);
                if (!gfx_backend_texture_read_pixels(img, 0, 0, pixels, sizeof pixels)) {
                    vk_log("compute probe FAIL: storage image readback failed");
                    ok = 0;
                } else {
                    for (uint32_t y = 0; y < 8 && ok; y++) {
                        for (uint32_t x = 0; x < 8 && ok; x++) {
                            uint32_t v = vk_compute_probe_value(x, y, texels, params.seed);
                            uint8_t want[4] = {
                                (uint8_t)(v & 255u), (uint8_t)((v >> 8) & 255u),
                                (uint8_t)((v >> 16) & 255u), (uint8_t)((v >> 24) & 255u),
                            };
                            if (memcmp(pixels + ((size_t)y * 8 + x) * 4, want, 4)) {
                                uint8_t *g = pixels + ((size_t)y * 8 + x) * 4;
                                vk_log("compute probe FAIL: image[%u,%u] got=%02x%02x%02x%02x "
                                       "want=%02x%02x%02x%02x", x, y, g[0], g[1], g[2], g[3],
                                       want[0], want[1], want[2], want[3]);
                                ok = 0;
                            }
                        }
                    }
                    if (ok) vk_log("compute probe: storage image 8x8 exact match");
                }
            }
        }
        if (p2) gfx_backend_compute_pipeline_destroy(p2);
        if (img) gfx_backend_texture_destroy(img);
    }

    gfx_backend_compute_pipeline_destroy(p);
    gfx_backend_texture_destroy(tex);
    vk_log("compute probe %s", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

void vk_copy_probe_fill(uint8_t *p, uint32_t w, uint32_t h, uint32_t seed) {
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *q = p + ((size_t)y * w + x) * 4;
            for (uint32_t c = 0; c < 4; c++)
                q[c] = (uint8_t)(seed + x * 3u + y * 5u + c * 7u);
        }
    }
}

void bc4_probe_constant_block(uint8_t *b, uint8_t v) {
    b[0] = v;
    b[1] = v;
    memset(b + 2, 0, 6);
}

void bc4_probe_half_block(uint8_t *b, uint8_t lo, uint8_t hi) {
    b[0] = lo;
    b[1] = hi;
    memset(b + 2, 0, 6);
    for (unsigned i = 8; i < 16; i++) {
        unsigned bit = i * 3;
        b[2 + bit / 8] |= (uint8_t)(1u << (bit % 8));
    }
}

int bc4_probe_expect(GfxOffscreenTarget *t, uint32_t x, uint32_t y,
                            uint8_t r, const char *what) {
    uint8_t px[4] = { 0, 0, 0, 0 };
    if (!gfx_backend_offscreen_read_pixel(t, x, y, px)) {
        vk_log("ERROR BC4 sample %s: read pixel (%u,%u) failed", what, x, y);
        return 0;
    }
    if (px[0] != r || px[1] != 0 || px[2] != 0 || px[3] != 255) {
        vk_log("ERROR BC4 sample %s at (%u,%u): got=%02x%02x%02x%02x "
               "want=%02x0000ff", what, x, y, px[0], px[1], px[2], px[3], r);
        return 0;
    }
    return 1;
}
