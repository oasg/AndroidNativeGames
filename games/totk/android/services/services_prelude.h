/* Common includes and macros for the Android host. */
#ifndef TOTK_ANDROID_SERVICES_PRELUDE_H
#define TOTK_ANDROID_SERVICES_PRELUDE_H
#include "runtime.h"
#include "ipc.h"
#include "runtime_tables.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define MAP_FIXED_NOREPLACE MAP_FIXED
#define MAP_NORESERVE 0
#define GUEST_LIMIT (1ull << 39)
#define LOW_AT 0x300000000ull
#define STACK_OFF 0x10000000ull
#define STACK_SIZE 0x10000000ull
#define HEAP_OFF (STACK_OFF + STACK_SIZE + 0x10000000ull)
#define HEAP_CAP (4ull << 30)
#define LOW_SIZE (HEAP_OFF + HEAP_CAP)
#define ALIAS_AT 0x7000000000ull
#define ALIAS_MAX (64ull << 30)
#define APP_POOL 0xcd500000ull
#define MAX_COMMITS 1024
#define MAX_EXTRA 8
#endif /* TOTK_ANDROID_SERVICES_PRELUDE_H */
