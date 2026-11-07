/* Common includes and macros for the Android host. */
#ifndef TOTK_ANDROID_LOADER_PRELUDE_H
#define TOTK_ANDROID_LOADER_PRELUDE_H
#include "runtime.h"
#include "ipc.h"
#include "sha256.h"
#include "gfx_backend_android.h"
#include "probe_hooks_android.h"
#include "shader_cache_android.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include "runtime_tables.h"
#define TLS_CANARY 0x544c53434f4eULL   /* "TLSOK" */
#define REFLECT_SPV_MAX_BOUND (1u << 20)
#endif /* TOTK_ANDROID_LOADER_PRELUDE_H */
