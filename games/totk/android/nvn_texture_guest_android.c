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
