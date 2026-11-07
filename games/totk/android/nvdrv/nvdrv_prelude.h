/* Common includes and macros for the Android host. */
#ifndef TOTK_ANDROID_NVDRV_PRELUDE_H
#define TOTK_ANDROID_NVDRV_PRELUDE_H
#include "runtime.h"
#include "ipc.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define MAP_NORESERVE 0
#define NVDRV_MAX_FD 64
#define NVDRV_MAX_MAP 128
#define NVDRV_GUEST_AT 0x500000000ull
#define NVDRV_GUEST_SIZE (1ull << 30)
#endif /* TOTK_ANDROID_NVDRV_PRELUDE_H */
