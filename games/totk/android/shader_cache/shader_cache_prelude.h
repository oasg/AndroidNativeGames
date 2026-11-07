/* Common includes and macros for the Android host. */
#ifndef TOTK_ANDROID_SHADER_CACHE_PRELUDE_H
#define TOTK_ANDROID_SHADER_CACHE_PRELUDE_H
#include "shader_cache_android.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "sha256.h"
#define SHADER_SPV_MAX (16u << 20)
#define SHADER_SPH_MAGIC 0x12345678u
#define SHADER_LOW_PTR 0x100000ull
#endif /* TOTK_ANDROID_SHADER_CACHE_PRELUDE_H */
