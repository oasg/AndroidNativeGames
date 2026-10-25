/* Minimal nn::account (acc:u0) service for the Android host.
 *
 * One local user exists.  The UID is all zeroes for now so it matches the save
 * data probe; nickname, language and region are supplied by nn::oe/settings
 * overrides.  Command ids are the IAccountServiceForApplication protocol. */
#include "runtime.h"
#include "ipc.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint8_t local_uid[16] = {0};

static unsigned account_logged;
static void account_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    if (account_logged++ < 80)
        fprintf(stderr, "[totk-account] command %u data=%u bytes\n", rq->cmd, rq->data_size);
    switch (rq->cmd) {
    case 0:                                     /* Initialize(ProcessId, ...) */
        return;
    case 1: {                                   /* GetUserExistence(Uid) -> bool */
        uint8_t exists = 1;
        IPC_OUT(rs, exists);
        return;
    }
    case 2:                                     /* ListAllUsers -> count, Uid[] */
    case 3: {                                   /* ListOpenUsers -> count, Uid[] */
        int32_t count = 1;
        IPC_OUT(rs, count);
        ipc_out(rs, local_uid, sizeof local_uid);
        return;
    }
    case 4:                                     /* GetLastOpenedUser -> Uid */
        ipc_out(rs, local_uid, sizeof local_uid);
        return;
    case 150: {                                 /* 1-byte status query */
        uint8_t yes = 1;
        IPC_OUT(rs, yes);
        return;
    }
    case 160:                                   /* 8-byte input, acknowledgement only */
        return;
    default:
        /* Do not report success for a command whose reply shape we do not
         * know; the trace then names the next gap. */
        rs->result = IPC_RESULT_UNKNOWN_COMMAND;
        return;
    }
}

__attribute__((constructor)) static void register_account(void) {
    runtime_ipc_register("acc:u0", account_handler, NULL, NULL);
    runtime_ipc_register("acc:u1", account_handler, NULL, NULL);
    runtime_ipc_register("acc:u2", account_handler, NULL, NULL);
}
