/* Common includes and macros for the Android NVN host. */
#ifndef TOTK_ANDROID_NVN_PRELUDE_H
#define TOTK_ANDROID_NVN_PRELUDE_H
#include "runtime.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <dlfcn.h>
#include <ucontext.h>
#include <fcntl.h>
#include <math.h>
#include "gfx_backend_android.h"
#include "shader_cache_android.h"
#define NVN_MAGIC(a, b, c, d) \
    ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define NVN_POOL_MAP_MAX 32768
#define NVN_POOL_BUCKETS 8192
#define NVN_POOL_EPOCH_MAX 1024
#define NVN_PROG_UBO_MAX 512
#define NVN_CB_META_MAX 128
#define NVN_ORPHAN_MAX 16
#define NVN_RETIRED_MAX 1024
#define NVN_SCENE_QUEUE_MAX 1024
#define NVN_EMPTY_TARGET_MAX 32
#define NVN_ST(T, x) ((T *)(x))
#define NVN_VB_SNAP_CAP   (64u << 10)   /* per stream */
#define NVN_VB_SNAP_TOTAL (128u << 20)  /* live snapshots across recordings */
#define NVN_PROG_PIPE_SLOTS 512
#define PROG_STATE_BUCKETS 4096
#define NVN_E(x) { #x, (void (*)(void))x }
#endif /* TOTK_ANDROID_NVN_PRELUDE_H */
