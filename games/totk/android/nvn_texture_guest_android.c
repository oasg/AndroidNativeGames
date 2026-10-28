/* Guest NVN texture storage bridge.
 *
 * nvn_deswizzle.c is the shared Tegra GOB block-linear decoder from the macOS
 * route; it needs the desktop nvn_texdesc/nvn_tex.c layout functions.  Keep
 * that include boundary in this file so nvn_android.c's own compact tex_desc
 * stays independent and no global/static names collide.
 */
#include "nvn_tex.h"
#include "nvn_deswizzle.h"
#include <stdint.h>
#include <string.h>

static nvn_texdesc desc(int target, int format, int flags, int samples,
                        uint32_t width, uint32_t height, uint32_t depth,
                        uint32_t levels, uint32_t stride, int pkg_block_h) {
    nvn_texdesc d;
    memset(&d, 0, sizeof d);
    d.target = target; d.format = format; d.flags = flags; d.samples = samples;
    d.width = width; d.height = height; d.depth = depth; d.levels = levels;
    d.stride = stride; d.pkg_block_h = pkg_block_h;
    return d;
}

int runtime_guest_texture_deswizzle(const void *storage, void *linear, size_t pitch,
                                    int target, int format, int flags, int samples,
                                    uint32_t width, uint32_t height, uint32_t depth,
                                    uint32_t levels, uint32_t stride, int pkg_block_h) {
    nvn_texdesc d = desc(target, format, flags, samples, width, height, depth, levels, stride, pkg_block_h);
    return nvn_deswizzle(&d, 0, 0, storage, linear, pitch);
}

int runtime_guest_texture_swizzle(void *storage, const void *linear, size_t pitch,
                                  int target, int format, int flags, int samples,
                                  uint32_t width, uint32_t height, uint32_t depth,
                                  uint32_t levels, uint32_t stride, int pkg_block_h) {
    nvn_texdesc d = desc(target, format, flags, samples, width, height, depth, levels, stride, pkg_block_h);
    return nvn_swizzle(&d, 0, 0, linear, pitch, storage);
}

/* Geometry of one mip level: block-row bytes, block rows, depth slices and
 * bytes per block.  Lets the upload bridge size each subresource without
 * duplicating the Tegra layout math.  Returns 0 on success, -1 when the
 * target/format combination has no linear representation. */
int runtime_guest_texture_level_geom(int target, int format, int flags, int samples,
                                     uint32_t width, uint32_t height, uint32_t depth,
                                     uint32_t levels, uint32_t stride, int pkg_block_h,
                                     unsigned level, uint32_t *row_bytes, uint32_t *rows,
                                     uint32_t *slices, uint32_t *bpb) {
    nvn_texdesc d = desc(target, format, flags, samples, width, height, depth, levels, stride, pkg_block_h);
    if (row_bytes) *row_bytes = 0;
    if (rows) *rows = 0;
    if (slices) *slices = 0;
    if (bpb) *bpb = 0;
    if (nvn_tex_is_linear(&d)) {
        int bw, bh;
        int b = nvn_fmt_bpb(d.format, &bw, &bh);
        if (!b || level || samples > 1) return -1;
        if (row_bytes) *row_bytes = (uint32_t)(((d.width + bw - 1) / bw) * b);
        if (rows) *rows = d.target == 0 ? 1 : (d.height + bh - 1) / bh;
        if (slices) *slices = 1;
        if (bpb) *bpb = (uint32_t)b;
        return 0;
    }
    uint64_t base = 0;
    unsigned th = 0, td = 0;
    if (nvn_deswizzle_geom(&d, level, 0, row_bytes, rows, slices, &base, &th, &td, bpb))
        return -1;
    return 0;
}

/* Deswizzle one (level, layer) region, optionally starting at depth slice z
 * and copying z_count slices.  For layered 2D/cube targets z is always 0 and
 * z_count is 1; for 3D the caller walks the level's depth slices.  The output
 * is tightly packed block rows (pitch bytes per row). */
int runtime_guest_texture_deswizzle_at(const void *storage, void *linear, size_t pitch,
                                       int target, int format, int flags, int samples,
                                       uint32_t width, uint32_t height, uint32_t depth,
                                       uint32_t levels, uint32_t stride, int pkg_block_h,
                                       unsigned level, unsigned layer,
                                       unsigned z, unsigned z_count) {
    if (!storage || !linear) return -1;
    nvn_texdesc d = desc(target, format, flags, samples, width, height, depth, levels, stride, pkg_block_h);
    if (nvn_tex_is_linear(&d)) {
        if (level || layer || z || z_count != 1) return -1;
        return nvn_deswizzle(&d, 0, 0, storage, linear, pitch);
    }
    uint32_t row_bytes = 0, rows = 0, slices = 0, bpb = 0;
    uint64_t base = 0;
    unsigned th = 0, td = 0;
    if (nvn_deswizzle_geom(&d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb))
        return -1;
    if (!row_bytes || !rows || !z_count || z >= slices) return -1;
    if (z_count > slices - z) z_count = slices - z;
    if (pitch < row_bytes) return -1;
    const uint8_t *src = (const uint8_t *)storage + base;
    uint8_t *dst = linear;
    for (unsigned s = 0; s < z_count; s++) {
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t *row = dst + (size_t)y * pitch;
            for (uint32_t x = 0; x < row_bytes; x += 16) {
                uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
                const uint8_t *p = src + nvn_bl_offset(x, y, z + s, row_bytes, rows, th, td);
                memcpy(row + x, p, n);
            }
        }
        dst += (size_t)rows * pitch;
    }
    return 0;
}
