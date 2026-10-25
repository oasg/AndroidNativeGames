/* Minimal nn::vi service for the Android host.
 *
 * The first goal is the initialization protocol, not a renderer.  vi:m is a
 * root service whose GetDisplayService commands return an
 * IApplicationDisplayService object; the object then carries the display,
 * layer and vsync calls.  Every command keeps the real CMIF shape so a missing
 * reply is visible as the next failure instead of a faked success. */
#include "runtime.h"
#include "ipc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t display;
    uint64_t next_layer;
} ViState;

static unsigned vi_logged;
static int vi_trace(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_ANDROID_TRACE_VI"); v = e && *e && strcmp(e, "0"); }
    return v;
}
static void vi_log(const char *what, uint32_t cmd, const IpcRequest *rq) {
    if (vi_logged++ < 120)
        fprintf(stderr, "[totk-vi] %s command %u data=%u bytes\n", what, cmd, rq->data_size);
    if (!vi_trace()) return;
    fprintf(stderr, "[totk-vi]   type=%u nx=%u na=%u nb=%u nw=%u nc=%u copy=%u move=%u\n",
            rq->type, rq->nx, rq->na, rq->nb, rq->nw, rq->nc, rq->ncopy, rq->nmove);
    for (unsigned i = 0; i * 4 + 4 <= rq->data_size && i < 24; i++)
        fprintf(stderr, "[totk-vi]   data[%u]=%08x\n", i, IPC_ARG(rq, uint32_t, i * 4));
    for (unsigned i = 0; i < rq->nx; i++)
        fprintf(stderr, "[totk-vi]   x%u type=%#x addr=%#llx size=%#llx\n", i, rq->x[i].index,
                (unsigned long long)rq->x[i].addr, (unsigned long long)rq->x[i].size);
    for (unsigned i = 0; i < rq->na; i++)
        fprintf(stderr, "[totk-vi]   a%u addr=%#llx size=%#llx\n", i,
                (unsigned long long)rq->a[i].addr, (unsigned long long)rq->a[i].size);
    for (unsigned i = 0; i < rq->nb; i++)
        fprintf(stderr, "[totk-vi]   b%u addr=%#llx size=%#llx\n", i,
                (unsigned long long)rq->b[i].addr, (unsigned long long)rq->b[i].size);
    for (unsigned i = 0; i < rq->nw; i++)
        fprintf(stderr, "[totk-vi]   w%u addr=%#llx size=%#llx\n", i,
                (unsigned long long)rq->w[i].addr, (unsigned long long)rq->w[i].size);
    for (unsigned i = 0; i < rq->nc; i++)
        fprintf(stderr, "[totk-vi]   c%u addr=%#llx size=%#llx\n", i,
                (unsigned long long)rq->c[i].addr, (unsigned long long)rq->c[i].size);
}

static void vi_side_service_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    vi_log("vi side service", rq->cmd, rq);
    rs->result = IPC_RESULT_UNKNOWN_COMMAND;
}

/* IApplicationDisplayService.  Command ids are the Horizon vi protocol ids;
 * only the calls the 1.0.0 boot path uses are answered with data. */
static void vi_application_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    ViState *st = s->state;
    vi_log("vi application", rq->cmd, rq);
    switch (rq->cmd) {
    case 5: {                                   /* OpenDisplay(DisplayName) */
        IPC_OUT(rs, st->display);
        return;
    }
    case 6: {                                   /* OpenDefaultDisplay */
        IPC_OUT(rs, st->display);
        return;
    }
    case 9: {                                   /* GetDisplayResolution(u64) */
        int64_t width = 1920, height = 1080;
        IPC_OUT(rs, width);
        IPC_OUT(rs, height);
        return;
    }
    case 10: {                                  /* OpenLayer(name, buffer) */
        uint64_t layer = ++st->next_layer;
        IPC_OUT(rs, layer);
        return;
    }
    case 12: {                                  /* CreateStrayLayer */
        uint64_t layer = ++st->next_layer;
        uint32_t size = 0x1000;
        IPC_OUT(rs, layer);
        IPC_OUT(rs, size);
        return;
    }
    case 20:                                    /* GetDisplayVsyncEvent(u64) */
    case 21:                                    /* ...ForDebug */
    case 5202:
    case 5203: {
        rs->move[rs->nmove++] = runtime_event_new();
        return;
    }
    case 100:                                   /* GetRelayService */
    case 101:                                   /* GetSystemDisplayService */
    case 102: {                                 /* GetManagerDisplayService */
        uint32_t h = runtime_ipc_new_session("IApplicationDisplayService",
                                            vi_side_service_handler, NULL, NULL);
        rs->move[rs->nmove++] = h;
        return;
    }
    case 1010: {                                /* OpenLayer (boot path) */
        uint64_t layer = ++st->next_layer;
        IPC_OUT(rs, layer);
        return;
    }
    default:
        rs->result = IPC_RESULT_UNKNOWN_COMMAND;
        return;
    }
}

static void vi_application_close(void *p) { free(p); }

static void vi_root_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    vi_log("vi root", rq->cmd, rq);
    switch (rq->cmd) {
    case 0:                                     /* GetDisplayService(u32) */
    case 1:                                     /* GetDisplayServiceWithProxyNameExchange */
    case 2: {
        ViState *st = calloc(1, sizeof *st);
        if (!st) runtime_fail("vi service state");
        uint32_t h = runtime_ipc_new_session("IApplicationDisplayService",
                                            vi_application_handler, st, vi_application_close);
        rs->move[rs->nmove++] = h;
        return;
    }
    default:
        rs->result = IPC_RESULT_UNKNOWN_COMMAND;
        return;
    }
}

__attribute__((constructor)) static void register_vi(void) {
    runtime_ipc_register("vi:m", vi_root_handler, NULL, NULL);
    runtime_ipc_register("vi:s", vi_root_handler, NULL, NULL);
    runtime_ipc_register("vi:u", vi_root_handler, NULL, NULL);
}
