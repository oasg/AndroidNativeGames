/* NVN module: probe_1. */
#include "nvn_internal.h"

int nvn_probe_pair_load(prog_t *p, const uint8_t *vs, size_t vs_size,
                               const uint8_t *ps, size_t ps_size,
                               const char *vs_res, const char *ps_res,
                               uint8_t tag, nvn_probe_pair_t *pair) {
    memset(pair, 0, sizeof *pair);
    for (unsigned stage = 0; stage < 2; stage++) {
        uint8_t *control = calloc(1, 0x880);
        uint8_t *code = calloc(1, 0x100);
        if (!control || !code) { free(control); free(code); goto fail; }
        uint32_t declared = 0x880, c1 = 0, length = 0x100, st = stage;
        memcpy(control + 0x20, &declared, sizeof declared);
        memcpy(control + 0x700, &c1, sizeof c1);
        memcpy(control + 0x704, &length, sizeof length);
        memcpy(control + 0x714, &st, sizeof st);
        memset(code, (uint8_t)(tag + stage * 0x40), length);
        RuntimeShaderIdentity id;
        if (!runtime_shader_identity(control, code, &id)) {
            free(control); free(code); goto fail;
        }
        if (!runtime_shader_cache_store(runtime_shader_cache_root(), &id,
                                        stage ? ps : vs, stage ? ps_size : vs_size,
                                        stage ? ps_res : vs_res)) {
            fprintf(stderr, "[totk-nvn] ERROR store probe shader %s\n",
                    runtime_shader_stage_name(stage));
            free(control); free(code); goto fail;
        }
        uint64_t address = (uint64_t)(uintptr_t)code;
        memcpy(pair->entries[stage], &address, 8);
        memcpy(pair->entries[stage] + 8, &control, 8);
        pair->controls[stage] = control;
        if (!nvn_probe_install_stage(p, stage, stage ? ps : vs,
                                     stage ? ps_size : vs_size,
                                     stage ? ps_res : vs_res, &id))
            goto fail;
    }
    p->nstage = 2;
    return 1;
fail:
    nvn_probe_pair_free(pair);
    return 0;
}

/* Reset descriptor state between probe sub-tests while keeping the NVN render
 * state defaults (a raw memset would disable the colour write mask). */
void nvn_probe_cb_reset(cb_t *cb) {
    if (!cb || !cb->bind_state) return;
    nvn_bind_state_t *b = cb->bind_state;
    memset(b, 0, sizeof *b);
    nvn_render_state_defaults(&b->rs);
}

int nvn_probe_check(const char *what, const uint8_t px[4],
                           int r_lo, int r_hi, int g_lo, int g_hi, int b_lo, int b_hi) {
    int ok = px[0] >= r_lo && px[0] <= r_hi &&
             px[1] >= g_lo && px[1] <= g_hi &&
             px[2] >= b_lo && px[2] <= b_hi;
    fprintf(stderr, "[totk-nvn] state probe %-7s center=%02x%02x%02x%02x ok=%d\n",
            what, px[0], px[1], px[2], px[3], ok);
    return ok;
}
