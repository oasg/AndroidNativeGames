/* State shared by this module group. */
#ifndef TOTK_ANDROID_GFX_STATE_H
#define TOTK_ANDROID_GFX_STATE_H
#include "gfx_types.h"


extern VulkanState g;
extern pthread_mutex_t g_lock;
extern const uint32_t present_vert_spv[] __attribute__((aligned(4)));
extern const size_t present_vert_spv_size;
extern const uint32_t present_frag_spv[] __attribute__((aligned(4)));
extern const size_t present_frag_spv_size;
extern GfxPresentPipe g_present_pipes[GFX_PRESENT_PIPE_MAX];
extern unsigned g_bad_color_pipeline_logs;

#endif
