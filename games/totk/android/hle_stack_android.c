/* Android HLE thunk table.  The assembly thunks call through this array. */
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void *runtime_hle_targets[4096];
extern char runtime_hle_thunks[];
enum { HLE_THUNK_BYTES = 16 };
static unsigned next_thunk;
int runtime_hle_trace;

void *runtime_hle_wrap_named(void *fn, const char *name) {
    (void)name;
    if (!fn) return NULL;
    if (next_thunk >= 4096) {
        fprintf(stderr, "[totk-android] HLE thunk table full\n");
        return fn;
    }
    unsigned i = next_thunk++;
    runtime_hle_targets[i] = fn;
    return runtime_hle_thunks + (size_t)i * HLE_THUNK_BYTES;
}

void *runtime_hle_wrap(void *fn) { return runtime_hle_wrap_named(fn, NULL); }
