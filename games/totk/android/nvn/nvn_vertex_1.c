/* NVN module: vertex_1. */
#include "nvn_internal.h"

/* Merge the per-draw vertex snapshots of one segment into a single stream per
 * binding and offset every translated draw's first/base_vertex.  The guest
 * binds a different small ring window per glyph/quad draw; replaying the
 * segment with the last draw's bind state would render every draw from that
 * one window.  Only all-or-nothing merges are attempted: if any bound stream
 * lacks a snapshot for any draw (a large scene stream, or a budget refusal),
 * the caller keeps the previous single-state behaviour. */
unsigned nvn_merge_draw_vertex_snapshots(const nvn_cmd_t *cmds, uint32_t ncmds,
                                                nvn_bind_state_t *bs,
                                                GfxCmd *translated, uint32_t tcount,
                                                uint32_t tw, uint32_t th,
                                                uint8_t *out_data[16],
                                                size_t out_size[16]) {
    if (!cmds || !ncmds || !bs || !translated) return 0;
    const nvn_cmd_t **draws = calloc(ncmds, sizeof *draws);
    if (!draws) return 0;
    uint32_t ndraw = 0;
    for (uint32_t i = 0; i < ncmds; i++)
        if (cmds[i].kind == NVN_CMD_DRAW || cmds[i].kind == NVN_CMD_DRAW_INDEXED)
            draws[ndraw++] = &cmds[i];
    if (!ndraw) { free(draws); return 0; }

    int merge_stream[16] = {0};
    unsigned ncand = 0, nmissing = 0;
    for (int s = 0; s < 16; s++) {
        if (!(bs->vb_mask & (1u << s)) || !bs->vb[s].address || !bs->vb[s].size)
            continue;
        unsigned have = 0;
        for (uint32_t k = 0; k < ndraw; k++) {
            size_t n = 0;
            if (nvn_draw_vb_find(draws[k], s, &n)) have++;
        }
        if (!have) { nmissing++; continue; }
        if (have != ndraw) {
            /* Mixed snapshot coverage: merging would leave some draws reading
             * the last draw's window.  Keep the whole segment on the old path. */
            free(draws);
            return 0;
        }
        merge_stream[s] = 1;
        ncand++;
    }
    /* A segment that mixes a snapshot-covered stream with an unsnapshotted
     * (large scene) stream cannot be replayed from one call safely. */
    if (!ncand || nmissing) { free(draws); return 0; }

    /* Decisive record-vs-replay evidence for the first few merged segments:
     * the bytes copied at draw time next to the bytes still at the guest
     * address when the deferred replay runs. */
    if (nvn_trace_inputs()) {
        static unsigned cmp_logs;
        if (cmp_logs < 8) {
            cmp_logs++;
            for (int s = 0; s < 16; s++) {
                if (!merge_stream[s]) continue;
                size_t rn = 0;
                const uint8_t *rec = nvn_draw_vb_find(draws[0], s, &rn);
                uint32_t rw[4] = { 0 }, pw[4] = { 0 };
                if (rec) memcpy(rw, rec, sizeof rw < rn ? sizeof rw : rn);
                size_t pn = bs->vb[s].size < sizeof pw ? bs->vb[s].size : sizeof pw;
                if (pn && nvn_guest_range_ok(bs->vb[s].address, pn))
                    memcpy(pw, (const void *)(uintptr_t)bs->vb[s].address, pn);
                fprintf(stderr, "[totk-vb-cmp] %ux%u stream=%d draws=%u"
                                " record=%08x %08x %08x %08x (%zu B)"
                                " replay=%08x %08x %08x %08x @%#llx size=%zu\n",
                        tw, th, s, ndraw, rw[0], rw[1], rw[2], rw[3], rn,
                        pw[0], pw[1], pw[2], pw[3],
                        (unsigned long long)bs->vb[s].address, bs->vb[s].size);
            }
        }
    }

    uint32_t *offset = calloc(ndraw, sizeof *offset);
    if (!offset) { free(draws); return 0; }
    uint32_t total_verts = 0;
    for (uint32_t k = 0; k < ndraw; k++) {
        const nvn_cmd_t *c = draws[k];
        uint32_t need = c->kind == NVN_CMD_DRAW
                      ? c->u.draw.first + c->u.draw.count
                      : nvn_cb_index_vertex_count(c);
        for (int s = 0; s < 16; s++) {
            if (!merge_stream[s]) continue;
            size_t n = 0;
            (void)nvn_draw_vb_find(c, s, &n);
            uint32_t stride = bs->vs[s] ? (uint32_t)bs->vs[s] : 1;
            uint32_t v = (uint32_t)((n + stride - 1) / stride);
            if (v > need) need = v;
        }
        offset[k] = total_verts;
        total_verts += need;
    }

    /* Bound the merged copy; fall back rather than allocate hundreds of MiB. */
    for (int s = 0; s < 16; s++) {
        if (!merge_stream[s]) continue;
        uint32_t stride = bs->vs[s] ? (uint32_t)bs->vs[s] : 1;
        if ((size_t)total_verts * stride > (64u << 20)) {
            free(offset);
            free(draws);
            return 0;
        }
    }

    int ok = 1;
    for (int s = 0; s < 16 && ok; s++) {
        if (!merge_stream[s]) continue;
        uint32_t stride = bs->vs[s] ? (uint32_t)bs->vs[s] : 1;
        size_t bytes = (size_t)total_verts * stride;
        uint8_t *buf = bytes ? calloc(1, bytes) : NULL;
        if (!buf) { ok = 0; break; }
        for (uint32_t k = 0; k < ndraw && ok; k++) {
            size_t n = 0;
            const uint8_t *src = nvn_draw_vb_find(draws[k], s, &n);
            if (!src) { ok = 0; break; }
            size_t room = (size_t)(offset[k] > total_verts ? 0 : total_verts - offset[k]);
            room *= stride;
            if (n > room) n = room;
            memcpy(buf + (size_t)offset[k] * stride, src, n);
        }
        if (!ok) { free(buf); break; }
        out_data[s] = buf;
        out_size[s] = bytes;
    }
    if (!ok) {
        for (int s = 0; s < 16; s++) {
            if (!merge_stream[s]) continue;
            free(out_data[s]);
            out_data[s] = NULL;
            out_size[s] = 0;
        }
        free(offset);
        free(draws);
        return 0;
    }
    /* Only now that every stream is built is the bind state redirected, so a
     * failed allocation never leaves it pointing at freed memory. */
    for (int s = 0; s < 16; s++) {
        if (!merge_stream[s]) continue;
        bs->vb[s].address = (uint64_t)(uintptr_t)out_data[s];
        bs->vb[s].size = out_size[s];
        bs->vb_mask |= 1u << s;
    }

    /* Remap the translated draw list in record order. */
    uint32_t k = 0;
    for (uint32_t i = 0; i < tcount && k < ndraw; i++) {
        if (translated[i].kind == GFX_CMD_DRAW) {
            translated[i].u.draw.first += offset[k];
            k++;
        } else if (translated[i].kind == GFX_CMD_DRAW_INDEXED) {
            translated[i].u.draw_indexed.base_vertex += offset[k];
            k++;
        }
    }
    free(offset);
    free(draws);
    return ncand;
}
