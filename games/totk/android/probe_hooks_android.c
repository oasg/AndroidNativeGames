/* Weak probe defaults (Wave 0).
 *
 * A lane implements its probe in the file it owns; until then the probe exits
 * with 77 ("not implemented") instead of reporting a false pass.  The loader
 * always links because these definitions are weak.
 */
#include "probe_hooks_android.h"

#include <stdio.h>

#define TOTK_PROBE_STUB(name)                                                   \
    __attribute__((weak)) int name(const char *work_dir) {                      \
        fprintf(stderr, "[totk-probe] %s not implemented (work=%s)\n",          \
                #name, work_dir ? work_dir : "(null)");                         \
        return 77;                                                              \
    }

TOTK_PROBE_STUB(gfx_backend_compute_probe)
TOTK_PROBE_STUB(gfx_backend_copy_probe)
TOTK_PROBE_STUB(nvn_compute_probe)
TOTK_PROBE_STUB(nvn_copy_probe)
