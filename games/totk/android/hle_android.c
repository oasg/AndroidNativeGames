/* Minimal HLE registry for the first Android SDK-start probe.
 *
 * Guest-defined symbols are resolved first.  Undefined imports without a real
 * host implementation get a loud logging stub, so relocation can finish and
 * the first failing service call is visible in the log. */
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>

static uint64_t hle_stub(void) { return 0; }

void *runtime_override(const char *name) {
    (void)name;
    return NULL;
}

void *runtime_observer(const char *name) {
    (void)name;
    return NULL;
}

void *runtime_hle_stub(const char *name) {
    static unsigned logged;
    if (logged++ < 400)
        fprintf(stderr, "[totk-android] unimplemented import -> stub: %s\n", name);
    return (void *)hle_stub;
}
