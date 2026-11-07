/* NVN module: queue_1. */
#include "nvn_internal.h"

/* Render one retired offscreen segment from its immutable snapshot.  Each
 * segment targets the texture it actually recorded draws for, so the 1920x1080
 * clear-only target of a UI/atlas buffer is never treated as its scene pass. */
int nvn_submit_scene_snapshot(uint64_t handle, uint32_t seg_index) {
#include "impl/submit_scene_segment_1.inc"
#include "impl/submit_scene_segment_2.inc"
}
