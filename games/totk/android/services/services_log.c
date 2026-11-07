/* Module: log. */
#include "services_internal.h"

/* lm (LogManager): the SDK's ILogService command 0 is OpenLogger(pid).  The
 * host returns an ILogger whose Log/SetDestination commands go to a silent
 * sink, so nn::lm::detail::ShimLibraryGlobal initializes. */
void logger_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)rs;
    switch (rq->cmd) {
    case 0: /* Log(in buffer) */
    case 1: /* SetDestination(destination) */
        return;
    default:
        ipc_unimplemented(s, rq);
    }
}

uint32_t lm_open_logger(IpcSession *s, IpcResponse *rs) {
    if (s->domain) {
        uint32_t id = ipc_domain_add_object(s, "ILogger", logger_handler, NULL, NULL);
        IPC_OUT_OBJECT(rs, id);
        return id;
    }
    uint32_t h = runtime_ipc_new_session("ILogger", logger_handler, NULL, NULL);
    rs->move[rs->nmove++] = h;
    return h;
}

void lm_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: { /* OpenLogger(pid) -> ILogger */
        uint64_t pid = 0;
        if (rq->data_size >= 8) memcpy(&pid, rq->data, 8);
        fprintf(stderr, "[totk-ipc] lm OpenLogger pid=%#llx domain=%p\n",
                (unsigned long long)pid, (void *)s->domain);
        lm_open_logger(s, rs);
        return;
    }
    case 3: { /* OpenLogger on a domain object -> ILogger */
        fprintf(stderr, "[totk-ipc] lm OpenLogger cmd3 domain=%p\n", (void *)s->domain);
        lm_open_logger(s, rs);
        return;
    }
    default:
        ipc_unimplemented(s, rq);
    }
}

__attribute__((constructor)) static void register_android_services(void) {
    runtime_ipc_register("lm", lm_handler, NULL, NULL);
}
