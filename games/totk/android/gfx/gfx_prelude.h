/* Common includes and macros for the Android host. */
#ifndef TOTK_ANDROID_GFX_PRELUDE_H
#define TOTK_ANDROID_GFX_PRELUDE_H
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>
#include <android/native_window.h>
#include "gfx_backend_android.h"
#include "probe_hooks_android.h"
#include "triangle_spv.h"
#include <pthread.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <android/log.h>
#include <stdlib.h>
#include <string.h>
#define VK_MAX_SWAPCHAIN_IMAGES 8
#define GFX_PRESENT_PIPE_MAX 8
#define GFX_REFLECT_MAX_BINDINGS 64
#define GFX_REFLECT_RUNTIME_ARRAY 32
#define GFX_DESC_MAX_EPOCHS 512
#define REFLECT_ALIGN 256
#define GFX_COMPUTE_MAX_BINDINGS 64
#endif /* TOTK_ANDROID_GFX_PRELUDE_H */
